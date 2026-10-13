#include "mf_d3d11_h264_encoder.h"
#include "ffmpeg_h264_session.h"
#include "cast_nv12_surface_pool.h"
#include "cast_pipeline_diagnostics.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <d3d11.h>
#include <objbase.h>
extern "C" {
#include <libavutil/error.h>
}
namespace {
constexpr std::size_t kPendingTextureLimit = 2;
template<class T> void release(T*& value) {
    if (value) { value->Release(); value=nullptr; }
}
bool valid_config(const MfD3D11H264EncoderConfig& config) {
    return config.width > 0 && config.height > 0
        && (config.width & 1U) == 0 && (config.height & 1U) == 0
        && config.frame_rate_num > 0 && config.frame_rate_den > 0
        && config.bitrate > 0 && config.gop_size_frames > 0;
}

struct PendingTexture {
    ID3D11Texture2D* texture{};
    std::shared_ptr<void> retention;
    std::int64_t timestamp{};
    std::int64_t duration{};
    bool discontinuity{};
    bool force_keyframe{};
};

void release_pending(PendingTexture& pending) {
    pending.retention.reset();
    pending = {};
}

}  // namespace

struct MfD3D11H264Encoder::Impl {
    mutable std::mutex submission_mutex;
    mutable std::mutex mutex;
    std::condition_variable state_changed;
    std::condition_variable startup_ready;
    std::thread worker;
    ID3D11Device* device{};
    MfD3D11H264EncoderConfig config;
    OutputCallback output;
    D3D11BgraToNv12Converter converter;
    CastNv12SurfacePool surface_pool;
    CastPipelineDiagnostics* diagnostics{};
    std::deque<PendingTexture> pending;
    ID3D11Texture2D* cached_bgra_texture{};
    ID3D11Texture2D* cached_nv12_texture{};
    std::shared_ptr<void> cached_nv12_retention;
    ID3D11Texture2D* startup_nv12_texture{};
    std::shared_ptr<void> startup_nv12_retention;
    bool surface_pool_needs_copy{};
    bool stop_requested{};
    bool startup_complete{};
    bool startup_succeeded{};
    bool startup_retained_async{};
    bool running{};
    bool first_input{true};
    MfH264EncoderBackend active_backend{MfH264EncoderBackend::none};
    MfH264ForceKeyframeSupport keyframe_support{
        MfH264ForceKeyframeSupport::unknown};

    ~Impl() { stop(); }

    void clear_pending_locked() {
        for (auto& texture : pending) release_pending(texture);
        pending.clear();
    }

    void clear_startup_sample_locked() {
        release(startup_nv12_texture);
        startup_nv12_retention.reset();
    }

    void clear_conversion_cache_locked() {
        release(cached_bgra_texture);
        cached_nv12_texture = nullptr;
        cached_nv12_retention.reset();
    }

    void cache_conversion_locked(
            ID3D11Texture2D* bgra,
            ID3D11Texture2D* nv12,
            std::shared_ptr<void> retention) {
        clear_conversion_cache_locked();
        if (!bgra || !nv12 || !retention) return;
        bgra->AddRef();
        cached_bgra_texture = bgra;
        cached_nv12_texture = nv12;
        cached_nv12_retention = std::move(retention);
    }

    void enqueue_locked(
            ID3D11Texture2D* nv12,
            std::shared_ptr<void> retention,
            std::int64_t timestamp,
            std::int64_t duration,
            bool& dropped_stale_input,
            bool force_keyframe) {
        bool dropped_keyframe_request{};
        while (pending.size() >= kPendingTextureLimit) {
            dropped_keyframe_request = dropped_keyframe_request
                || pending.front().force_keyframe;
            release_pending(pending.front());
            pending.pop_front();
            dropped_stale_input = true;
            if (diagnostics) {
                diagnostics->increment(
                    CastPipelineCounter::encoder_drops);
            }
        }
        // Preserve a program-switch request when backpressure discards the
        // frame that carried it. The earliest surviving input gets the bit;
        // this keeps the request tied to a real queued frame and avoids a
        // process-wide flag accidentally targeting an older hold frame.
        if (dropped_keyframe_request) {
            if (!pending.empty()) {
                pending.front().force_keyframe = true;
            } else {
                force_keyframe = true;
            }
        }
        pending.push_back(PendingTexture{
            nv12,
            std::move(retention),
            static_cast<std::int64_t>(timestamp),
            static_cast<std::int64_t>(duration),
            first_input || dropped_stale_input,
            force_keyframe});
        first_input = false;
    }

    bool start(
            void* borrowed_device,
            const MfD3D11H264EncoderConfig& requested_config,
            OutputCallback requested_output,
            CastPipelineDiagnostics* requested_diagnostics,
            void* borrowed_startup_nv12_texture,
            std::shared_ptr<void> requested_startup_retention) {
        auto* requested_device = static_cast<ID3D11Device*>(borrowed_device);
        auto* requested_startup_texture =
            static_cast<ID3D11Texture2D*>(borrowed_startup_nv12_texture);
        if (!requested_device || !valid_config(requested_config)
                || !requested_output) {
            return false;
        }
        if ((requested_startup_texture != nullptr)
                != static_cast<bool>(requested_startup_retention)) {
            return false;
        }
        if (requested_startup_texture
                && requested_config.input_format
                    != MfH264EncoderInputFormat::nv12) {
            return false;
        }
        std::unique_lock submission_lock(submission_mutex);
        std::unique_lock lock(mutex);
        if (worker.joinable()) return running && device == requested_device;
        surface_pool_needs_copy = false;
        if (requested_config.input_format
                == MfH264EncoderInputFormat::bgra8) {
            if (!converter.configure(
                    requested_device,
                    requested_config.width,
                    requested_config.height,
                    requested_config.frame_rate_num,
                    requested_config.frame_rate_den,
                    requested_diagnostics)) {
                return false;
            }
            CastNv12SurfacePoolConfig pool_config;
            pool_config.width = requested_config.width;
            pool_config.height = requested_config.height;
            pool_config.capacity = 8;
            pool_config.create_plane_uavs = false;
            pool_config.video_processor_output = true;
            pool_config.encoder_compatible = true;
            pool_config.separate_encoder_texture = false;
            if (!surface_pool.configure(
                    requested_device, pool_config, requested_diagnostics)) {
                pool_config.separate_encoder_texture = true;
                if (!surface_pool.configure(
                        requested_device,
                        pool_config,
                        requested_diagnostics)) {
                    converter.reset();
                    return false;
                }
            }
            surface_pool_needs_copy = pool_config.separate_encoder_texture;
            for (std::size_t index = 0;
                    index < surface_pool.capacity(); ++index) {
                if (!converter.prepare_output_texture(
                        surface_pool.compose_texture_at(index))) {
                    converter.reset();
                    surface_pool.reset();
                    return false;
                }
            }
        }
        requested_device->AddRef();
        device = requested_device;
        config = requested_config;
        output = std::move(requested_output);
        diagnostics = requested_diagnostics;
        if (requested_startup_texture) requested_startup_texture->AddRef();
        startup_nv12_texture = requested_startup_texture;
        startup_nv12_retention = std::move(requested_startup_retention);
        stop_requested = false;
        startup_complete = false;
        startup_succeeded = false;
        startup_retained_async = false;
        running = false;
        first_input = true;
        active_backend = MfH264EncoderBackend::none;
        keyframe_support = MfH264ForceKeyframeSupport::unknown;
        clear_pending_locked();
        clear_conversion_cache_locked();
        try {
            worker = std::thread([this] { worker_main(); });
        } catch (...) {
            release(device);
            converter.reset();
            surface_pool.reset();
            clear_startup_sample_locked();
            output = {};
            diagnostics = nullptr;
            return false;
        }

        startup_ready.wait(lock, [&] { return startup_complete; });
        if (startup_succeeded) return true;
        stop_requested = true;
        lock.unlock();
        state_changed.notify_all();
        worker.join();
        lock.lock();
        release(device);
        converter.reset();
        surface_pool.reset();
        clear_startup_sample_locked();
        output = {};
        diagnostics = nullptr;
        return false;
    }

    void stop() {
        std::lock_guard submission_lock(submission_mutex);
        {
            std::lock_guard lock(mutex);
            if (!worker.joinable()) {
                clear_pending_locked();
                clear_conversion_cache_locked();
                release(device);
                converter.reset();
                surface_pool.reset();
                clear_startup_sample_locked();
                output = {};
                diagnostics = nullptr;
                running = false;
                active_backend = MfH264EncoderBackend::none;
                keyframe_support = MfH264ForceKeyframeSupport::unknown;
                return;
            }
            stop_requested = true;
        }
        state_changed.notify_all();
        worker.join();
        std::lock_guard lock(mutex);
        clear_pending_locked();
        clear_conversion_cache_locked();
        release(device);
        converter.reset();
        surface_pool.reset();
        clear_startup_sample_locked();
        output = {};
        diagnostics = nullptr;
        running = false;
        active_backend = MfH264EncoderBackend::none;
        keyframe_support = MfH264ForceKeyframeSupport::unknown;
        startup_complete = false;
        startup_succeeded = false;
        startup_retained_async = false;
        stop_requested = false;
    }

    bool is_running() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested;
    }

    MfH264EncoderBackend backend() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested
            ? active_backend : MfH264EncoderBackend::none;
    }

    MfH264ForceKeyframeSupport force_keyframe_support() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested
            ? keyframe_support : MfH264ForceKeyframeSupport::unknown;
    }

    bool startup_sample_retained_async() const {
        std::lock_guard lock(mutex);
        return running && !stop_requested && startup_retained_async;
    }

    bool prepare_input_texture(void* composed_bgra_texture) {
        if (!composed_bgra_texture) return false;
        std::lock_guard submission_lock(submission_mutex);
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || config.input_format
                        != MfH264EncoderInputFormat::bgra8) {
                return false;
            }
        }
        return converter.prepare_input_texture(composed_bgra_texture);
    }

    bool submit_texture(
            void* composed_bgra_texture,
            std::int64_t timestamp,
            std::int64_t duration,
            bool force_keyframe) {
        if (!composed_bgra_texture || timestamp < 0 || duration <= 0) {
            return false;
        }
        std::lock_guard submission_lock(submission_mutex);
        bool dropped_stale_input = false;
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || config.input_format
                        != MfH264EncoderInputFormat::bgra8) {
                return false;
            }
        }

        auto surface = surface_pool.try_acquire();
        if (!surface) {
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::encoder_drops);
            }
            return false;
        }
        auto* compose_nv12 = static_cast<ID3D11Texture2D*>(
            surface.compose_texture());
        auto* encoder_nv12 = static_cast<ID3D11Texture2D*>(
            surface.encoder_texture());
        const auto correlation_id = duration > 0
            ? static_cast<std::uint64_t>(timestamp / duration) : 0;
        if (!converter.convert(
                composed_bgra_texture, compose_nv12, correlation_id)) {
            return false;
        }
        if (surface_pool_needs_copy) {
            ID3D11DeviceContext* context{};
            device->GetImmediateContext(&context);
            if (!context) return false;
            const auto copy_started = diagnostics
                ? CastPipelineDiagnostics::qpc_now() : 0;
            context->CopyResource(encoder_nv12, compose_nv12);
            release(context);
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::gpu_copies);
                diagnostics->record_duration(
                    CastPipelineEvent::gpu_copy,
                    correlation_id,
                    copy_started,
                    CastPipelineDiagnostics::qpc_now(),
                    static_cast<std::uint64_t>(timestamp));
            }
        }
        auto retention = surface.retention_token();
        surface.reset();

        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested) {
                return false;
            }
            cache_conversion_locked(
                static_cast<ID3D11Texture2D*>(composed_bgra_texture),
                encoder_nv12,
                retention);
            enqueue_locked(
                encoder_nv12,
                std::move(retention),
                timestamp,
                duration,
                dropped_stale_input,
                force_keyframe);
        }
        state_changed.notify_one();
        return true;
    }

    bool submit_repeated_texture(
            void* unchanged_bgra_texture,
            std::int64_t timestamp,
            std::int64_t duration,
            bool force_keyframe) {
        auto* bgra = static_cast<ID3D11Texture2D*>(
            unchanged_bgra_texture);
        if (!bgra || timestamp < 0 || duration <= 0) return false;
        std::unique_lock submission_lock(submission_mutex);

        bool reused{};
        bool dropped_stale_input{};
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || config.input_format
                        != MfH264EncoderInputFormat::bgra8) {
                return false;
            }
            if (cached_bgra_texture == bgra && cached_nv12_texture
                    && cached_nv12_retention) {
                enqueue_locked(
                    cached_nv12_texture,
                    cached_nv12_retention,
                    timestamp, duration, dropped_stale_input,
                    force_keyframe);
                reused = true;
                if (diagnostics) {
                    diagnostics->increment(
                        CastPipelineCounter::repeated_frames);
                }
            }
        }
        if (!reused) {
            submission_lock.unlock();
            return submit_texture(
                bgra, timestamp, duration, force_keyframe);
        }
        state_changed.notify_one();
        return true;
    }

    bool submit_nv12_texture(
            void* borrowed_nv12_texture,
            std::shared_ptr<void> retention,
            std::int64_t timestamp,
            std::int64_t duration,
            bool force_keyframe) {
        auto* nv12 = static_cast<ID3D11Texture2D*>(borrowed_nv12_texture);
        if (!nv12 || !retention || timestamp < 0 || duration <= 0) {
            return false;
        }
        std::lock_guard submission_lock(submission_mutex);
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested
                    || config.input_format
                        != MfH264EncoderInputFormat::nv12) {
                return false;
            }
        }

        D3D11_TEXTURE2D_DESC desc{};
        nv12->GetDesc(&desc);
        ID3D11Device* texture_device{};
        nv12->GetDevice(&texture_device);
        const bool valid = texture_device == device
            && desc.Format == DXGI_FORMAT_NV12
            && desc.Width == config.width
            && desc.Height == config.height
            && desc.SampleDesc.Count == 1;
        release(texture_device);
        if (!valid) return false;

        bool dropped_stale_input = false;
        {
            std::lock_guard lock(mutex);
            if (!running || stop_requested) return false;
            enqueue_locked(nv12, std::move(retention), timestamp, duration,
                dropped_stale_input, force_keyframe);
        }
        state_changed.notify_one();
        return true;
    }

    void emit_access_unit(
            const std::uint8_t* data,
            std::size_t size,
            std::int64_t pts90k,
            bool keyframe) {
        OutputCallback callback;
        {
            std::lock_guard lock(mutex);
            callback = output;
        }
        if (callback) {
            callback(data, size, pts90k, keyframe);
        }
        if (diagnostics) {
            // Input timestamps are generated in 100 ns units and then scaled
            // to 90 kHz by the MFT. Dividing by a separately truncated frame
            // duration makes a valid frame N appear as N-1. Recover the
            // transport-frame correlation from the exact rational rate and
            // round to the nearest frame instead.
            const auto non_negative_pts =
                std::max<std::int64_t>(0, pts90k);
            const auto correlation_id = static_cast<std::uint64_t>(
                std::llround(
                    static_cast<double>(non_negative_pts)
                        * config.frame_rate_num
                    / (90000.0 * config.frame_rate_den)));
            diagnostics->record(
                CastPipelineEvent::encoder_output,
                correlation_id,
                static_cast<std::uint64_t>(
                    non_negative_pts),
                keyframe
                    ? -static_cast<std::int64_t>(size)
                    : static_cast<std::int64_t>(size));
        }
    }

    void worker_main() {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ID3D11Texture2D* startup_texture{};
        std::shared_ptr<void> startup_retention;
        {
            std::lock_guard lock(mutex);
            startup_texture = std::exchange(startup_nv12_texture, nullptr);
            startup_retention = std::move(startup_nv12_retention);
        }
        std::vector<std::pair<bool,bool>> choices;
        switch (config.preference) {
            case MfH264EncoderPreference::hardware_only: choices={{true,true}}; break;
            case MfH264EncoderPreference::hardware_readback_only: choices={{true,false}}; break;
            case MfH264EncoderPreference::software_only: choices={{false,false}}; break;
            default: choices={{true,true},{true,false},{false,false}};
        }
        std::unique_ptr<FfmpegH264Session> session;
        MfH264EncoderBackend selected_backend=MfH264EncoderBackend::none;
        bool retained_async{};
        for (const auto& [hardware,gpu] : choices) {
            auto candidate=std::make_unique<FfmpegH264Session>();
            if (!candidate->open(device,config,hardware,gpu)) continue;
            if (startup_texture) {
                const auto duration=static_cast<std::int64_t>(10000000ULL*config.frame_rate_den/config.frame_rate_num);
                if (candidate->send(startup_texture,startup_retention,0,duration,true)<0) continue;
                retained_async=candidate->input_retained_async();
                if (candidate->receive({})<0) continue;
            }
            selected_backend=hardware ? MfH264EncoderBackend::hardware : MfH264EncoderBackend::software;
            session=std::move(candidate);
            break;
        }
        release(startup_texture);
        startup_retention.reset();
        {
            std::lock_guard lock(mutex);
            startup_succeeded=static_cast<bool>(session);
            startup_complete=true;
            startup_retained_async=retained_async && startup_succeeded;
            running=startup_succeeded;
            active_backend=selected_backend;
            keyframe_support=running ? MfH264ForceKeyframeSupport::supported : MfH264ForceKeyframeSupport::unknown;
        }
        startup_ready.notify_one();
        const auto emit=[this](const std::uint8_t* data,std::size_t size,std::int64_t pts,bool key) {
            emit_access_unit(data,size,pts,key);
        };
        bool failed{};
        bool first_production=true;
        while (session) {
            PendingTexture next;
            {
                std::unique_lock lock(mutex);
                state_changed.wait(lock,[&] { return stop_requested || !pending.empty(); });
                if (stop_requested && pending.empty()) break;
                next=std::move(pending.front());
                pending.pop_front();
            }
            const auto started=diagnostics ? CastPipelineDiagnostics::qpc_now() : 0;
            const bool request_keyframe=next.force_keyframe || first_production;
            auto result=session->send(next.texture,next.retention,next.timestamp,next.duration,request_keyframe);
            if (result==AVERROR(EAGAIN)) {
                result=session->receive(emit);
                if (result>=0) result=session->send(next.texture,next.retention,next.timestamp,next.duration,request_keyframe);
            }
            if (diagnostics) {
                auto flags=next.discontinuity ? cast_encoder_input_diagnostic::discontinuity : 0LL;
                if (next.force_keyframe) flags|=cast_encoder_input_diagnostic::keyframe_requested
                    | (result>=0 ? cast_encoder_input_diagnostic::keyframe_applied : cast_encoder_input_diagnostic::keyframe_failed);
                const auto correlation=static_cast<std::uint64_t>(std::llround(
                    static_cast<double>(next.timestamp)*config.frame_rate_num/(10000000.0*config.frame_rate_den)));
                diagnostics->record_duration(CastPipelineEvent::encoder_process_input,correlation,
                    started,CastPipelineDiagnostics::qpc_now(),next.timestamp,flags);
            }
            release_pending(next);
            if (result<0 || session->receive(emit)<0) { failed=true; break; }
            first_production=false;
        }
        if (session && !failed) static_cast<void>(session->flush(emit));
        session.reset();
        {
            std::lock_guard lock(mutex);
            running=false;
            clear_pending_locked();
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
};
MfD3D11H264Encoder::MfD3D11H264Encoder()
        : impl_(std::make_unique<Impl>()) {}

MfD3D11H264Encoder::~MfD3D11H264Encoder() = default;

bool MfD3D11H264Encoder::start(
        void* d3d11_device,
        const MfD3D11H264EncoderConfig& config,
        OutputCallback output,
        CastPipelineDiagnostics* diagnostics,
        void* startup_nv12_texture,
        std::shared_ptr<void> startup_retention) {
    return impl_ && impl_->start(
        d3d11_device, config, std::move(output), diagnostics,
        startup_nv12_texture, std::move(startup_retention));
}

void MfD3D11H264Encoder::stop() {
    if (impl_) impl_->stop();
}

bool MfD3D11H264Encoder::is_running() const noexcept {
    return impl_ && impl_->is_running();
}

MfH264EncoderBackend MfD3D11H264Encoder::backend() const noexcept {
    return impl_ ? impl_->backend() : MfH264EncoderBackend::none;
}

MfH264ForceKeyframeSupport
MfD3D11H264Encoder::force_keyframe_support() const noexcept {
    return impl_ ? impl_->force_keyframe_support()
        : MfH264ForceKeyframeSupport::unknown;
}

bool MfD3D11H264Encoder::startup_sample_retained_async() const noexcept {
    return impl_ && impl_->startup_sample_retained_async();
}

bool MfD3D11H264Encoder::prepare_input_texture(
        void* composed_bgra_texture) {
    return impl_ && impl_->prepare_input_texture(composed_bgra_texture);
}

bool MfD3D11H264Encoder::submit_texture(
        void* composed_bgra_texture,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe) {
    return impl_ && impl_->submit_texture(
        composed_bgra_texture,
        sample_time_100ns,
        sample_duration_100ns,
        force_keyframe);
}

bool MfD3D11H264Encoder::submit_repeated_texture(
        void* unchanged_bgra_texture,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe) {
    return impl_ && impl_->submit_repeated_texture(
        unchanged_bgra_texture,
        sample_time_100ns,
        sample_duration_100ns,
        force_keyframe);
}

bool MfD3D11H264Encoder::submit_nv12_texture(
        void* nv12_texture,
        std::shared_ptr<void> retention,
        std::int64_t sample_time_100ns,
        std::int64_t sample_duration_100ns,
        bool force_keyframe) {
    return impl_ && impl_->submit_nv12_texture(
        nv12_texture,
        std::move(retention),
        sample_time_100ns,
        sample_duration_100ns,
        force_keyframe);
}
