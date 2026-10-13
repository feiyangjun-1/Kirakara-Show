#include <cstdio>
#include <cstring>
#include <initializer_list>
extern "C" {
#include <libavcodec/avcodec.h>
}

int main() {
    const auto* config = avcodec_configuration();
    if (std::strstr(config, "--enable-gpl") || std::strstr(config, "--enable-nonfree")
            || !std::strstr(config, "--enable-shared")) return 1;
    void* iterator = nullptr;
    while (const auto* codec = av_codec_iterate(&iterator)) {
        if (av_codec_is_decoder(codec)) {
            switch (codec->id) {
            case AV_CODEC_ID_H264: case AV_CODEC_ID_HEVC: case AV_CODEC_ID_VP9:
            case AV_CODEC_ID_AV1: case AV_CODEC_ID_AAC: case AV_CODEC_ID_OPUS: break;
            default: std::fprintf(stderr, "Unexpected decoder: %s\n", codec->name); return 2;
            }
            std::printf("decoder: %s\n", codec->name);
        }
        if (av_codec_is_encoder(codec)) {
            if (codec->id != AV_CODEC_ID_H264 && codec->id != AV_CODEC_ID_AAC) {
                std::fprintf(stderr, "Unexpected encoder: %s\n", codec->name); return 3;
            }
            std::printf("encoder: %s\n", codec->name);
        }
    }
    for (const auto* name : {"h264", "hevc", "vp9", "av1", "libdav1d", "aac", "opus"}) {
        if (!avcodec_find_decoder_by_name(name)) {
            std::fprintf(stderr, "Missing decoder: %s\n", name); return 4;
        }
    }
    for (const auto* name : {"h264", "hevc", "vp9", "av1"}) {
        const auto* codec = avcodec_find_decoder_by_name(name);
        bool d3d11 = false;
        for (int index = 0; const auto* hw = avcodec_get_hw_config(codec, index); ++index) {
            d3d11 |= hw->device_type == AV_HWDEVICE_TYPE_D3D11VA
                && (hw->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX);
        }
        if (!d3d11) {
            std::fprintf(stderr, "Missing D3D11 device decoder: %s\n", name); return 6;
        }
    }
    for (const auto* name : {"libopenh264", "h264_nvenc", "h264_amf", "h264_qsv", "aac"}) {
        if (!avcodec_find_encoder_by_name(name)) {
            std::fprintf(stderr, "Missing encoder: %s\n", name); return 5;
        }
    }
    return 0;
}
