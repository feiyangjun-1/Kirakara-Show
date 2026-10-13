#pragma once
#include "mf_d3d11_h264_encoder.h"
#include <memory>
#include <string>

// Shared by the production worker and capability probe. No MF encoder is used.
class FfmpegH264Session {
public:
    FfmpegH264Session();
    ~FfmpegH264Session();
    bool open(void* device, const MfD3D11H264EncoderConfig& config,
              bool hardware, bool gpu_input);
    int send(void* nv12_texture, std::shared_ptr<void> retention,
             std::int64_t timestamp, std::int64_t duration, bool keyframe);
    int receive(const MfD3D11H264Encoder::OutputCallback& output);
    int flush(const MfD3D11H264Encoder::OutputCallback& output);
    bool gpu_input() const;
    bool input_retained_async() const;
    const std::string& name() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
