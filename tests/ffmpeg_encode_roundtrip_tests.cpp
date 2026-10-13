#include "../apps/show_host/cast/mf_d3d11_h264_encoder.h"
#include "../apps/show_host/cast/mf_aac_encoder.h"
#include "../apps/show_host/cast/aac_adts_packetizer.h"
#include "../apps/show_host/cast/mpeg_ts_muxer.h"
#include "../apps/show_host/media/ffmpeg_decode.h"
#include "../apps/show_host/audio/decoded_buffer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <d3d10_1.h>
#include <d3d11.h>

void expect(bool value,const char* message) { if (!value) { std::cerr<<message<<'\n'; std::exit(1); } }
void roundtrip(ID3D11Device* device,ID3D11Texture2D* texture,const std::shared_ptr<void>& retention,
        MfH264EncoderPreference preference,const std::filesystem::path& path) {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::uint8_t> transport;
    MpegTsMuxer muxer;
    int video_frames{},keyframes{},audio_frames{};
    std::int64_t video_pts=-1,audio_pts=-1;
    bool valid=true;
    MfD3D11H264Encoder video;
    MfAacEncoder audio;
    MfD3D11H264EncoderConfig config;
    config.width=640; config.height=360; config.frame_rate_num=30; config.frame_rate_den=1;
    config.gop_size_frames=300; config.bitrate=1500000;
    config.input_format=MfH264EncoderInputFormat::nv12; config.preference=preference;
    expect(video.start(device,config,[&](const std::uint8_t* data,std::size_t size,std::int64_t pts,bool key) {
        std::lock_guard lock(mutex);
        valid=valid && pts>video_pts;
        video_pts=pts; ++video_frames; if (key) ++keyframes;
        muxer.append_video_access_unit(data,size,pts,key,transport); ready.notify_all();
    },nullptr,texture,retention),"start FFmpeg H.264 encoder");
    const auto backend=video.backend();
    expect(audio.start(MfAacEncoderConfig{},[&](const std::uint8_t* data,std::size_t size,std::int64_t pts) {
        std::vector<std::uint8_t> adts;
        const auto packetized=AacAdtsPacketizer::append_frame({48000,2,2},data,size,adts);
        std::lock_guard lock(mutex);
        valid=valid && pts>audio_pts && packetized;
        audio_pts=pts; ++audio_frames;
        if (packetized) muxer.append_audio_frame(adts.data(),adts.size(),pts,transport);
        ready.notify_all();
    }),"start FFmpeg native AAC encoder");
    std::vector<std::int16_t> pcm(1600*2);
    for (int index=0;index<30;++index) {
        for (int sample=0;sample<1600;++sample) {
            const auto value=static_cast<std::int16_t>(8000*std::sin(2*3.141592653589793*440*(index*1600+sample)/48000));
            pcm[sample*2]=pcm[sample*2+1]=value;
        }
        expect(audio.submit_pcm(pcm.data(),1600,index*1600),"queue non-AAC-aligned PCM chunks");
        expect(video.submit_nv12_texture(texture,retention,static_cast<std::int64_t>(index)*10000000/30,
            10000000/30,index==0 || index==15),"queue GPU input with requested IDR");
        std::unique_lock lock(mutex);
        expect(ready.wait_for(lock,std::chrono::seconds(3),[&] { return video_frames>=index+1; }),"encoded H.264 output timeout");
    }
    video.stop(); audio.stop();
    expect(valid && video_frames==30 && keyframes>=2 && audio_frames>=47,"monotonic timestamps, drain and keyframes");
    expect(transport.size()%188==0,"MPEG-TS packet alignment");
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(transport.data()),transport.size()); output.close();
    FfmpegVideoDecoder decoder;
    expect(decoder.open(path.wstring(),MfD3D11VideoPixelFormat::nv12),"open encoded MPEG-TS video");
    int decoded{};
    for (;;) {
        FfmpegVideoFrame frame;
        const auto result=decoder.read(frame);
        if (result==FfmpegReadStatus::end_of_stream) break;
        expect(result==FfmpegReadStatus::frame,"decode emitted H.264 stream");
        expect(frame.format.width==640 && frame.format.height==360,"encoded resolution");
        expect(!frame.pixels.empty() && std::abs(static_cast<int>(frame.pixels[0])-160)<12,"encoded pixels preserve luminance");
        ++decoded;
    }
    expect(decoded==30,"decode all emitted video frames");
    DecodedAudioBuffer samples;
    expect(ffmpeg_decode_audio(path.wstring(),samples),"decode emitted AAC-LC stream");
    expect(samples.sample_rate==48000 && samples.channels==2 && samples.frame_count()>=48000
        && samples.frame_count()<=51200,"AAC frame sizing and bounded priming/padding");
    double energy{};
    for (const auto sample:samples.samples) { expect(std::isfinite(sample),"finite decoded AAC"); energy+=sample*sample; }
    expect(energy>100,"encoded audio preserves the tone");
    std::cout << "MPEG-TS roundtrip: backend=" << (backend==MfH264EncoderBackend::hardware ? "hardware" : "software")
        << " video=" << decoded << " keyframes=" << keyframes << " AAC=" << audio_frames << '\n';
}
int wmain(int argc,wchar_t** argv) {
    expect(argc==2,"pass output directory");
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ID3D11Device* device{}; ID3D11DeviceContext* context{}; D3D_FEATURE_LEVEL level{};
    expect(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,
        &device,&level,&context)),"D3D11 encoder device");
    ID3D10Multithread* multithread{};
    expect(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&multithread))),"protected D3D11 context");
    multithread->SetMultithreadProtected(TRUE); multithread->Release();
    std::vector<std::uint8_t> pixels(640*360*3/2,128); std::fill_n(pixels.begin(),640*360,160);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=640; desc.Height=360; desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_NV12; desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(),640,0}; ID3D11Texture2D* texture{};
    expect(SUCCEEDED(device->CreateTexture2D(&desc,&initial,&texture)),"immutable test texture");
    auto retention=std::shared_ptr<void>(texture,[](void* value) { static_cast<ID3D11Texture2D*>(value)->Release(); });
    roundtrip(device,texture,retention,MfH264EncoderPreference::software_only,directory/L"software.ts");
    roundtrip(device,texture,retention,MfH264EncoderPreference::automatic,directory/L"automatic.ts");
    retention.reset(); context->Release(); device->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
