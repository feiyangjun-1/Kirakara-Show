#include "mf_aac_encoder.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
namespace {
constexpr std::size_t kPendingChunkLimit=32;
bool valid_config(const MfAacEncoderConfig& c) {
    return (c.sample_rate==44100 || c.sample_rate==48000) && (c.channels==1 || c.channels==2)
        && (c.average_bytes_per_second==12000 || c.average_bytes_per_second==16000
            || c.average_bytes_per_second==20000 || c.average_bytes_per_second==24000);
}
struct PendingPcm { std::vector<std::int16_t> samples; std::size_t frames{}; std::uint64_t start_frame{}; };
class AacSession {
public:
    ~AacSession() { avcodec_free_context(&codec); av_frame_free(&frame); av_packet_free(&packet); }
    bool open(const MfAacEncoderConfig& config) {
        const std::string_view build(avcodec_configuration());
        if (build.find("--enable-gpl")!=build.npos || build.find("--enable-nonfree")!=build.npos) return false;
        const auto* encoder=avcodec_find_encoder_by_name("aac");
        if (!encoder) return false;
        codec=avcodec_alloc_context3(encoder);
        if (!codec) return false;
        codec->sample_rate=config.sample_rate;
        av_channel_layout_default(&codec->ch_layout,config.channels);
        codec->sample_fmt=AV_SAMPLE_FMT_FLTP; codec->time_base={1,static_cast<int>(config.sample_rate)};
        codec->bit_rate=static_cast<std::int64_t>(config.average_bytes_per_second)*8;
        codec->profile=AV_PROFILE_AAC_LOW; codec->thread_count=1;
        if (avcodec_open2(codec,encoder,nullptr)<0 || codec->frame_size!=1024) return false;
        frame=av_frame_alloc(); packet=av_packet_alloc();
        if (!frame || !packet) return false;
        frame->format=codec->sample_fmt; frame->sample_rate=codec->sample_rate; frame->nb_samples=1024;
        if (av_channel_layout_copy(&frame->ch_layout,&codec->ch_layout)<0) return false;
        return av_frame_get_buffer(frame,0)>=0;
    }
    int receive(const MfAacEncoder::OutputCallback& output) {
        for (;;) {
            const auto result=avcodec_receive_packet(codec,packet);
            if (result==AVERROR(EAGAIN) || result==AVERROR_EOF) return 0;
            if (result<0) return result;
            // ADTS cannot signal priming skip samples; avoid negative transport PTS.
            const auto samples=std::max<std::int64_t>(0,packet->pts+codec->initial_padding);
            const auto pts=av_rescale_q(samples,codec->time_base,AVRational{1,90000});
            if (output) output(packet->data,packet->size,pts);
            av_packet_unref(packet);
        }
    }
    int send(const std::int16_t* samples,std::int64_t position,const MfAacEncoder::OutputCallback& output) {
        auto result=av_frame_make_writable(frame);
        if (result<0) return result;
        for (int channel=0; channel<codec->ch_layout.nb_channels; ++channel) {
            auto* plane=reinterpret_cast<float*>(frame->data[channel]);
            for (int index=0; index<1024; ++index)
                plane[index]=samples[index*codec->ch_layout.nb_channels+channel]/32768.0f;
        }
        frame->pts=position;
        result=avcodec_send_frame(codec,frame);
        if (result==AVERROR(EAGAIN)) {
            result=receive(output);
            if (result>=0) result=avcodec_send_frame(codec,frame);
        }
        return result<0 ? result : receive(output);
    }
    int finish(const MfAacEncoder::OutputCallback& output) {
        const auto result=avcodec_send_frame(codec,nullptr);
        return result<0 && result!=AVERROR_EOF ? result : receive(output);
    }
    AVCodecContext* codec{}; AVFrame* frame{}; AVPacket* packet{};
};
}
struct MfAacEncoder::Impl {
    mutable std::mutex mutex;
    std::condition_variable state_changed,startup_ready;
    std::thread worker;
    MfAacEncoderConfig config;
    OutputCallback output;
    std::deque<PendingPcm> pending;
    bool stop_requested{},startup_complete{},startup_succeeded{},running{};
    ~Impl() { stop(); }
    bool start(const MfAacEncoderConfig& requested,OutputCallback callback) {
        if (!valid_config(requested) || !callback) return false;
        std::unique_lock lock(mutex);
        if (worker.joinable()) return running;
        config=requested; output=std::move(callback); pending.clear();
        stop_requested=startup_complete=startup_succeeded=running=false;
        try { worker=std::thread([this] { worker_main(); }); } catch (...) { output={}; return false; }
        startup_ready.wait(lock,[&] { return startup_complete; });
        if (startup_succeeded) return true;
        lock.unlock(); worker.join(); lock.lock(); output={}; return false;
    }
    void stop() {
        { std::lock_guard lock(mutex); stop_requested=true; }
        state_changed.notify_all(); if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex); pending.clear(); running=false; output={};
    }
    bool is_running() const { std::lock_guard lock(mutex); return running && !stop_requested; }
    bool submit_pcm(const std::int16_t* data,std::size_t frames,std::uint64_t start) {
        std::lock_guard lock(mutex);
        if (!running || stop_requested || !data || !frames || pending.size()>=kPendingChunkLimit
                || frames>std::numeric_limits<std::size_t>::max()/config.channels
                || frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                || start>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())-frames) return false;
        PendingPcm chunk; chunk.frames=frames; chunk.start_frame=start;
        chunk.samples.assign(data,data+frames*config.channels); pending.push_back(std::move(chunk));
        state_changed.notify_one(); return true;
    }
    void worker_main() {
        auto session=std::make_unique<AacSession>();
        const bool ready=session->open(config);
        { std::lock_guard lock(mutex); startup_complete=true; startup_succeeded=running=ready; }
        startup_ready.notify_one(); if (!ready) return;
        std::vector<std::int16_t> assembled;
        std::uint64_t expected{},next_frame{};
        bool first=true,failed=false;
        const auto packet_samples=static_cast<std::size_t>(1024)*config.channels;
        const auto emit=[this](const std::uint8_t* data,std::size_t bytes,std::int64_t pts) {
            OutputCallback callback; { std::lock_guard lock(mutex); callback=output; }
            if (callback) callback(data,bytes,pts);
        };
        for (;;) {
            PendingPcm next;
            {
                std::unique_lock lock(mutex);
                state_changed.wait(lock,[&] { return stop_requested || !pending.empty(); });
                if (stop_requested && pending.empty()) break;
                next=std::move(pending.front()); pending.pop_front();
            }
            if (first) { next_frame=expected=next.start_frame; first=false; }
            if (next.start_frame<expected) continue;
            const auto gap=next.start_frame-expected;
            if (gap>static_cast<std::uint64_t>(config.sample_rate)*2) {
                if (session->finish(emit)<0) { failed=true; break; }
                session=std::make_unique<AacSession>();
                if (!session->open(config)) { failed=true; break; }
                assembled.clear(); next_frame=next.start_frame;
            } else if (gap) assembled.insert(assembled.end(),gap*config.channels,0);
            assembled.insert(assembled.end(),next.samples.begin(),next.samples.end());
            expected=next.start_frame+next.frames;
            std::size_t consumed{};
            while (assembled.size()-consumed>=packet_samples) {
                if (session->send(assembled.data()+consumed,next_frame,emit)<0) { failed=true; break; }
                consumed+=packet_samples; next_frame+=1024;
            }
            assembled.erase(assembled.begin(),assembled.begin()+consumed);
            if (failed) break;
        }
        if (!failed && !first) {
            if (!assembled.empty()) {
                assembled.resize(packet_samples,0); static_cast<void>(session->send(assembled.data(),next_frame,emit));
            }
            static_cast<void>(session->finish(emit));
        }
        std::lock_guard lock(mutex); running=false;
    }
};
MfAacEncoder::MfAacEncoder() : impl_(std::make_unique<Impl>()) {}
MfAacEncoder::~MfAacEncoder()=default;
bool MfAacEncoder::start(const MfAacEncoderConfig& config,OutputCallback output) { return impl_->start(config,std::move(output)); }
void MfAacEncoder::stop() { impl_->stop(); }
bool MfAacEncoder::is_running() const noexcept { return impl_->is_running(); }
bool MfAacEncoder::submit_pcm(const std::int16_t* pcm,std::size_t frames,std::uint64_t start) { return impl_->submit_pcm(pcm,frames,start); }
