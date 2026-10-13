#include "ffmpeg_h264_session.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <dxgi.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

namespace {
template<class T> void release(T*& value) { if (value) { value->Release(); value = nullptr; } }
bool allowed_build() {
    const std::string_view config(avcodec_configuration());
    return config.find("--enable-gpl") == config.npos && config.find("--enable-nonfree") == config.npos;
}
unsigned int vendor(ID3D11Device* device) {
    IDXGIDevice* dxgi{};
    IDXGIAdapter* adapter{};
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))
            && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->GetDesc(&desc);
    release(adapter); release(dxgi);
    return desc.VendorId;
}
struct TextureOwner {
    ID3D11Texture2D* texture{};
    std::shared_ptr<void> lease;
    std::shared_ptr<std::atomic_bool> released;
    ~TextureOwner() { texture->Release(); released->store(true, std::memory_order_release); }
};
}

struct FfmpegH264Session::Impl {
    AVCodecContext* codec{};
    AVBufferRef* frames{};
    AVBSFContext* filter{};
    AVPacket* packet{};
    AVPacket* filtered{};
    ID3D11DeviceContext* context{};
    ID3D11Texture2D* staging{};
    bool gpu{};
    std::string encoder_name;
    std::shared_ptr<std::atomic_bool> last_released;
    ~Impl() {
        avcodec_free_context(&codec);
        av_buffer_unref(&frames);
        av_bsf_free(&filter);
        av_packet_free(&packet);
        av_packet_free(&filtered);
        release(staging); release(context);
    }
    bool open(ID3D11Device* device, const MfD3D11H264EncoderConfig& config,
            const char* name, bool hardware, bool gpu_input) {
        const auto* encoder = avcodec_find_encoder_by_name(name);
        if (!encoder) return false;
        codec = avcodec_alloc_context3(encoder);
        if (!codec) return false;
        gpu = gpu_input;
        encoder_name = name;
        codec->width = config.width;
        codec->height = config.height;
        codec->time_base = {static_cast<int>(config.frame_rate_den), static_cast<int>(config.frame_rate_num)};
        codec->framerate = av_inv_q(codec->time_base);
        codec->bit_rate = config.bitrate;
        codec->gop_size = config.gop_size_frames;
        codec->max_b_frames = 0;
        codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codec->thread_count = hardware ? 1 : std::max(1U, std::min(4U, std::thread::hardware_concurrency()));
        codec->pix_fmt = gpu ? AV_PIX_FMT_D3D11 : hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        codec->color_range = AVCOL_RANGE_MPEG;
        codec->colorspace = AVCOL_SPC_BT709;
        codec->color_primaries = AVCOL_PRI_BT709;
        codec->color_trc = AVCOL_TRC_BT709;
        if (hardware) {
            auto* reference = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!reference) return false;
            auto* hardware_context = reinterpret_cast<AVHWDeviceContext*>(reference->data);
            auto* d3d = static_cast<AVD3D11VADeviceContext*>(hardware_context->hwctx);
            device->AddRef();
            d3d->device = device;
            if (av_hwdevice_ctx_init(reference) < 0) { av_buffer_unref(&reference); return false; }
            if (encoder_name == "h264_qsv") {
                AVBufferRef* qsv{};
                const auto result = av_hwdevice_ctx_create_derived(&qsv, AV_HWDEVICE_TYPE_QSV, reference, 0);
                av_buffer_unref(&reference);
                if (result < 0) return false;
                reference = qsv;
            }
            codec->hw_device_ctx = reference;
            if (gpu) {
                frames = av_hwframe_ctx_alloc(reference);
                if (!frames) return false;
                auto* frame_context = reinterpret_cast<AVHWFramesContext*>(frames->data);
                frame_context->format = AV_PIX_FMT_D3D11;
                frame_context->sw_format = AV_PIX_FMT_NV12;
                frame_context->width = config.width;
                frame_context->height = config.height;
                frame_context->initial_pool_size = 0;
                if (av_hwframe_ctx_init(frames) < 0) return false;
                codec->hw_frames_ctx = av_buffer_ref(frames);
                if (!codec->hw_frames_ctx) return false;
            }
        }
        AVDictionary* options{};
        if (encoder_name == "h264_nvenc") {
            av_dict_set(&options, "preset", "p1", 0);
            av_dict_set(&options, "tune", "ull", 0);
            av_dict_set(&options, "rc", "cbr", 0);
            av_dict_set(&options, "zerolatency", "1", 0);
            av_dict_set(&options, "forced-idr", "1", 0);
            av_dict_set(&options, "delay", "0", 0);
        } else if (encoder_name == "h264_amf") {
            av_dict_set(&options, "usage", "ultralowlatency", 0);
            av_dict_set(&options, "quality", "speed", 0);
            av_dict_set(&options, "rc", "cbr", 0);
            av_dict_set(&options, "forced_idr", "1", 0);
            av_dict_set(&options, "async_depth", "1", 0);
        } else if (encoder_name == "h264_qsv") {
            av_dict_set(&options, "async_depth", "1", 0);
            av_dict_set(&options, "look_ahead", "0", 0);
            av_dict_set(&options, "preset", "veryfast", 0);
            av_dict_set(&options, "forced_idr", "1", 0);
        } else {
            codec->profile = AV_PROFILE_H264_MAIN;
            av_dict_set(&options, "rc_mode", "bitrate", 0);
            av_dict_set(&options, "allow_skip_frames", "0", 0);
        }
        const auto opened = avcodec_open2(codec, encoder, &options);
        av_dict_free(&options);
        if (opened < 0) return false;
        if (!gpu) {
            device->GetImmediateContext(&context);
            if (!context) return false;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = config.width; desc.Height = config.height;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
        }
        const auto* annex_b = av_bsf_get_by_name("h264_mp4toannexb");
        if (!annex_b || av_bsf_alloc(annex_b, &filter) < 0
                || avcodec_parameters_from_context(filter->par_in, codec) < 0) return false;
        filter->time_base_in = codec->time_base;
        if (av_bsf_init(filter) < 0) return false;
        packet = av_packet_alloc(); filtered = av_packet_alloc();
        return packet && filtered;
    }
};

FfmpegH264Session::FfmpegH264Session() : impl_(std::make_unique<Impl>()) {}
FfmpegH264Session::~FfmpegH264Session() = default;
bool FfmpegH264Session::open(void* borrowed, const MfD3D11H264EncoderConfig& config,
        bool hardware, bool gpu_input) {
    auto* device = static_cast<ID3D11Device*>(borrowed);
    if (!device || !allowed_build()) return false;
    const auto id = vendor(device);
    const char* name = !hardware ? "libopenh264" : id == 0x10de ? "h264_nvenc"
        : id == 0x1002 ? "h264_amf" : id == 0x8086 ? "h264_qsv" : nullptr;
    if (!name || (gpu_input && id == 0x8086)) return false; // QSV uses bounded NV12 readback.
    impl_ = std::make_unique<Impl>();
    return impl_->open(device, config, name, hardware, gpu_input);
}
bool FfmpegH264Session::gpu_input() const { return impl_->gpu; }
bool FfmpegH264Session::input_retained_async() const {
    return impl_->last_released && !impl_->last_released->load(std::memory_order_acquire);
}
const std::string& FfmpegH264Session::name() const { return impl_->encoder_name; }

int FfmpegH264Session::send(void* borrowed, std::shared_ptr<void> retention,
        std::int64_t timestamp, std::int64_t duration, bool keyframe) {
    auto& state = *impl_;
    if (!state.codec || !borrowed || !retention) return AVERROR(EINVAL);
    auto* texture = static_cast<ID3D11Texture2D*>(borrowed);
    AVFrame* frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    frame->format = state.codec->pix_fmt;
    frame->width = state.codec->width; frame->height = state.codec->height;
    frame->pts = av_rescale_q(timestamp, AVRational{1,10000000}, state.codec->time_base);
    frame->duration = av_rescale_q(duration, AVRational{1,10000000}, state.codec->time_base);
    frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    int result = 0;
    if (state.gpu) {
        auto released = std::make_shared<std::atomic_bool>(false);
        auto* owner = new TextureOwner{texture, std::move(retention), released};
        texture->AddRef();
        frame->buf[0] = av_buffer_create(reinterpret_cast<std::uint8_t*>(texture), sizeof(void*),
            [](void* opaque, std::uint8_t*) { delete static_cast<TextureOwner*>(opaque); }, owner, 0);
        if (!frame->buf[0]) { delete owner; result = AVERROR(ENOMEM); }
        frame->data[0] = reinterpret_cast<std::uint8_t*>(texture);
        frame->hw_frames_ctx = av_buffer_ref(state.frames);
        state.last_released = std::move(released);
        if (!frame->hw_frames_ctx) result = AVERROR(ENOMEM);
    } else {
        result = av_frame_get_buffer(frame, 32);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (result >= 0) {
            state.context->CopyResource(state.staging, texture);
            if (FAILED(state.context->Map(state.staging, 0, D3D11_MAP_READ, 0, &mapped))) result = AVERROR(EIO);
        }
        if (result >= 0) {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
            for (int row=0; row<frame->height; ++row)
                std::memcpy(frame->data[0]+row*frame->linesize[0], source+row*mapped.RowPitch, frame->width);
            source += mapped.RowPitch * frame->height;
            for (int row=0; row<frame->height/2; ++row) {
                if (frame->format == AV_PIX_FMT_NV12)
                    std::memcpy(frame->data[1]+row*frame->linesize[1], source+row*mapped.RowPitch, frame->width);
                else for (int column=0; column<frame->width/2; ++column) {
                    frame->data[1][row*frame->linesize[1]+column] = source[row*mapped.RowPitch+column*2];
                    frame->data[2][row*frame->linesize[2]+column] = source[row*mapped.RowPitch+column*2+1];
                }
            }
            state.context->Unmap(state.staging, 0);
        }
    }
    if (result >= 0) result = avcodec_send_frame(state.codec, frame);
    av_frame_free(&frame);
    return result;
}

int FfmpegH264Session::receive(const MfD3D11H264Encoder::OutputCallback& output) {
    auto& state = *impl_;
    for (;;) {
        auto result = avcodec_receive_packet(state.codec, state.packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 0;
        if (result < 0) return result;
        result = av_bsf_send_packet(state.filter, state.packet);
        if (result < 0) return result;
        for (;;) {
            result = av_bsf_receive_packet(state.filter, state.filtered);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) return result;
            const auto pts = av_rescale_q(state.filtered->pts, state.filter->time_base_out, AVRational{1,90000});
            if (output) output(state.filtered->data, state.filtered->size, pts,
                (state.filtered->flags & AV_PKT_FLAG_KEY) != 0);
            av_packet_unref(state.filtered);
        }
    }
}
int FfmpegH264Session::flush(const MfD3D11H264Encoder::OutputCallback& output) {
    const auto result = avcodec_send_frame(impl_->codec, nullptr);
    return result < 0 && result != AVERROR_EOF ? result : receive(output);
}
