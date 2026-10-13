#include "ffmpeg_h264_session.h"
#include "../media/ffmpeg_decode.h"
#include "d3d11_cast_capabilities.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <new>
#include <string>
#include <thread>

#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <shlwapi.h>
#include <windows.h>
#include <winternl.h>

namespace {

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

D3D11CastCapabilityCheck check(HRESULT result) noexcept {
    return D3D11CastCapabilityCheck{
        true,
        SUCCEEDED(result),
        static_cast<std::int32_t>(result),
    };
}

std::uint64_t luid_value(LUID luid) noexcept {
    return (static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(luid.HighPart)) << 32U)
        | static_cast<std::uint32_t>(luid.LowPart);
}

HRESULT adapter_description(
        ID3D11Device* device,
        D3D11CastCapabilityReport& report) {
    IDXGIDevice* dxgi_device{};
    IDXGIAdapter* adapter{};
    auto hr = device
        ? device->QueryInterface(IID_PPV_ARGS(&dxgi_device)) : E_POINTER;
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC description{};
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&description);
    if (SUCCEEDED(hr)) {
        report.adapter_luid = luid_value(description.AdapterLuid);
        report.vendor_id = description.VendorId;
        report.device_id = description.DeviceId;
        report.feature_level = static_cast<std::uint32_t>(
            device->GetFeatureLevel());
    }
    release(adapter);
    release(dxgi_device);
    return hr;
}

std::uint32_t windows_build_number() noexcept {
    using RtlGetVersionFunction = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto function = module ? reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion")) : nullptr;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return function && function(&version) == 0 ? version.dwBuildNumber : 0;
}

HRESULT create_nv12_texture(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        UINT bind_flags,
        ID3D11Texture2D** texture) {
    if (!device || !texture) return E_POINTER;
    *texture = nullptr;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bind_flags;
    return device->CreateTexture2D(&description, nullptr, texture);
}

HRESULT probe_plane_srvs(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height) {
    const auto probe_bindings = [=](UINT bind_flags) {
        ID3D11Texture2D* texture{};
        auto hr = create_nv12_texture(
            device, width, height,
            bind_flags | D3D11_BIND_SHADER_RESOURCE, &texture);
        ID3D11ShaderResourceView* y{};
        ID3D11ShaderResourceView* uv{};
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        view.Format = DXGI_FORMAT_R8_UNORM;
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(texture, &view, &y);
        }
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(texture, &view, &uv);
        }
        release(uv);
        release(y);
        release(texture);
        return hr;
    };
    auto hr = probe_bindings(D3D11_BIND_DECODER);
    if (SUCCEEDED(hr)) {
        hr = probe_bindings(D3D11_BIND_RENDER_TARGET);
    }
    return hr;
}

HRESULT probe_plane_uavs(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        UINT extra_bind_flags,
        ID3D11Texture2D** retained_texture = nullptr) {
    if (retained_texture) *retained_texture = nullptr;
    ID3D11Texture2D* texture{};
    auto hr = create_nv12_texture(device, width, height,
        D3D11_BIND_UNORDERED_ACCESS | extra_bind_flags, &texture);
    ID3D11UnorderedAccessView* y{};
    ID3D11UnorderedAccessView* uv{};
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    view.Format = DXGI_FORMAT_R8_UNORM;
    if (SUCCEEDED(hr)) {
        hr = device->CreateUnorderedAccessView(texture, &view, &y);
    }
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    if (SUCCEEDED(hr)) {
        hr = device->CreateUnorderedAccessView(texture, &view, &uv);
    }
    release(uv);
    release(y);
    if (SUCCEEDED(hr) && retained_texture) {
        *retained_texture = texture;
        texture = nullptr;
    }
    release(texture);
    return hr;
}

HRESULT probe_encoder_copy(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        ID3D11Texture2D** retained_encoder_texture = nullptr) {
    if (retained_encoder_texture) *retained_encoder_texture = nullptr;
    ID3D11Texture2D* compose{};
    ID3D11Texture2D* encoder{};
    auto hr = create_nv12_texture(
        device, width, height, D3D11_BIND_UNORDERED_ACCESS, &compose);
    if (SUCCEEDED(hr)) {
        hr = create_nv12_texture(
            device, width, height, D3D11_BIND_VIDEO_ENCODER, &encoder);
    }
    ID3D11DeviceContext* context{};
    if (SUCCEEDED(hr)) device->GetImmediateContext(&context);
    if (SUCCEEDED(hr) && !context) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        context->CopyResource(encoder, compose);
        hr = device->GetDeviceRemovedReason();
    }
    release(context);
    release(compose);
    if (SUCCEEDED(hr) && retained_encoder_texture) {
        *retained_encoder_texture = encoder;
        encoder = nullptr;
    }
    release(encoder);
    return hr;
}

HRESULT probe_nv12_video_processor(
        ID3D11Device* device,
        const D3D11CastCapabilityProbeOptions& options) {
    ID3D11VideoDevice* video_device{};
    auto hr = device->QueryInterface(IID_PPV_ARGS(&video_device));
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate.Numerator = options.frame_rate_num;
    content.InputFrameRate.Denominator = options.frame_rate_den;
    content.InputWidth = options.width;
    content.InputHeight = options.height;
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = options.width;
    content.OutputHeight = options.height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ID3D11VideoProcessorEnumerator* enumerator{};
    if (SUCCEEDED(hr)) {
        hr = video_device->CreateVideoProcessorEnumerator(
            &content, &enumerator);
    }
    UINT support{};
    if (SUCCEEDED(hr)) {
        hr = enumerator->CheckVideoProcessorFormat(
            DXGI_FORMAT_NV12, &support);
    }
    constexpr UINT required = D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT
        | D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT;
    if (SUCCEEDED(hr) && (support & required) != required) hr = E_FAIL;
    UINT bgra_support{};
    if (SUCCEEDED(hr)) {
        hr = enumerator->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &bgra_support);
    }
    if (SUCCEEDED(hr)
            && (bgra_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
                == 0) {
        hr = E_FAIL;
    }
    ID3D11VideoProcessor* processor{};
    if (SUCCEEDED(hr)) {
        hr = video_device->CreateVideoProcessor(enumerator, 0, &processor);
    }
    release(processor);
    release(enumerator);
    release(video_device);
    return hr;
}

std::wstring source_url(const wchar_t* source) {
    if (!source || !*source) return {};
    std::wstring value(source);
    if (value.starts_with(L"http://") || value.starts_with(L"https://")) {
        return value;
    }
    DWORD length = 4096;
    std::wstring url(length, L'\0');
    if (FAILED(UrlCreateFromPathW(
            value.c_str(), url.data(), &length, 0))) {
        return value;
    }
    url.resize(length);
    return url;
}

HRESULT same_adapter(ID3D11Device* left, ID3D11Device* right) {
    if (!left || !right) return E_POINTER;
    D3D11CastCapabilityReport left_report;
    D3D11CastCapabilityReport right_report;
    auto hr = adapter_description(left, left_report);
    if (SUCCEEDED(hr)) hr = adapter_description(right, right_report);
    if (SUCCEEDED(hr) && left_report.adapter_luid != right_report.adapter_luid) {
        hr = DXGI_ERROR_INVALID_CALL;
    }
    return hr;
}

HRESULT probe_native_nv12_source(ID3D11Device* device, const wchar_t* source) {
    if (!device || !source || !*source) return E_INVALIDARG;
    FfmpegVideoDecoder decoder;
    if (!decoder.open(source, MfD3D11VideoPixelFormat::nv12, false, device)) return E_FAIL;
    FfmpegVideoFrame frame;
    if (decoder.read(frame) != FfmpegReadStatus::frame) return E_FAIL;
    if (!frame.hardware_texture) {
        // Software fallback will upload the converted NV12 pixels into the
        // same device; the report describes the usable source output.
        return frame.pixels.empty() ? E_FAIL : S_OK;
    }
    auto* texture = static_cast<ID3D11Texture2D*>(frame.hardware_texture);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010) return E_FAIL;
    ID3D11Device* owner{};
    texture->GetDevice(&owner);
    const auto result = same_adapter(device, owner);
    release(owner);
    return result;
}
HRESULT probe_h264_surface(ID3D11Device* device,ID3D11Texture2D* texture,
        const D3D11CastCapabilityProbeOptions& options,
        D3D11CastCapabilityCheck* tracked_release,bool* retained_asynchronously) {
    if (!device || !texture) return E_POINTER;
    if (tracked_release) *tracked_release={};
    if (retained_asynchronously) *retained_asynchronously=false;
    MfD3D11H264EncoderConfig config;
    config.width=options.width; config.height=options.height;
    config.frame_rate_num=options.frame_rate_num; config.frame_rate_den=options.frame_rate_den;
    config.gop_size_frames=options.frame_rate_num/options.frame_rate_den;
    config.input_format=MfH264EncoderInputFormat::nv12;
    texture->AddRef();
    auto token=std::shared_ptr<void>(texture,[](void* value) { static_cast<ID3D11Texture2D*>(value)->Release(); });
    std::weak_ptr<void> released=token;
    auto session=std::make_unique<FfmpegH264Session>();
    bool succeeded=session->open(device,config,true,true);
    if (succeeded) succeeded=session->send(texture,token,0,
        static_cast<std::int64_t>(10000000ULL*options.frame_rate_den/options.frame_rate_num),true)>=0;
    if (succeeded && retained_asynchronously) *retained_asynchronously=session->input_retained_async();
    if (succeeded) succeeded=session->receive({})>=0;
    session.reset(); token.reset();
    const auto result=succeeded && released.expired() ? S_OK : E_FAIL;
    if (tracked_release) *tracked_release=check(result);
    return result;
}
}  // namespace

D3D11CastCapabilityReport D3D11CastCapabilityProbe::run(
        void* d3d11_device,
        const D3D11CastCapabilityProbeOptions& options) {
    D3D11CastCapabilityReport report;
    auto* device = static_cast<ID3D11Device*>(d3d11_device);
    if (!device || options.width == 0 || options.height == 0
            || options.frame_rate_num == 0 || options.frame_rate_den == 0) {
        report.device_healthy = check(E_INVALIDARG);
        return select_d3d11_cast_backend(report);
    }

    report.windows_build = windows_build_number();
    auto hr = adapter_description(device, report);
    if (SUCCEEDED(hr)) hr = device->GetDeviceRemovedReason();
    report.device_healthy = check(hr);
    report.even_420_geometry = check(
        (options.width & 1U) == 0 && (options.height & 1U) == 0
            ? S_OK : E_INVALIDARG);
    report.nv12_plane_srvs = check(
        probe_plane_srvs(device, options.width, options.height));
    report.nv12_plane_uavs = check(
        probe_plane_uavs(device, options.width, options.height, 0));

    ID3D11Texture2D* direct_encoder_texture{};
    hr = probe_plane_uavs(device, options.width, options.height,
        D3D11_BIND_VIDEO_ENCODER, &direct_encoder_texture);
    report.uav_video_encoder_bind = check(hr);
    report.nv12_video_processor = check(
        probe_nv12_video_processor(device, options));

    if (options.decoder_source && *options.decoder_source) {
        report.native_nv12_source = check(
            probe_native_nv12_source(device, options.decoder_source));
    }
    if (options.probe_hardware_encoder) {
        D3D11CastCapabilityCheck tracked_release;
        bool retained_asynchronously{};
        hr = probe_h264_surface(
            device, direct_encoder_texture, options,
            &tracked_release, &retained_asynchronously);
        report.combined_uav_encoder_input = check(hr);
        if (SUCCEEDED(hr)) {
            report.dxgi_h264_encoder = check(hr);
            report.tracked_sample_release = tracked_release;
            report.encoder_retains_samples_async = retained_asynchronously;
        } else {
            ID3D11Texture2D* copied_encoder_texture{};
            const auto copy_result = probe_encoder_copy(
                device, options.width, options.height,
                &copied_encoder_texture);
            report.compose_to_encoder_copy = check(copy_result);
            const auto encoder_result = SUCCEEDED(copy_result)
                ? probe_h264_surface(
                    device, copied_encoder_texture, options,
                    &tracked_release, &retained_asynchronously)
                : copy_result;
            report.dxgi_h264_encoder = check(encoder_result);
            report.tracked_sample_release = tracked_release;
            report.encoder_retains_samples_async = retained_asynchronously;
            release(copied_encoder_texture);
        }
    } else if (!report.uav_video_encoder_bind.passed()) {
        ID3D11Texture2D* copied_encoder_texture{};
        report.compose_to_encoder_copy = check(probe_encoder_copy(
            device, options.width, options.height,
            &copied_encoder_texture));
        release(copied_encoder_texture);
    }
    release(direct_encoder_texture);
    return select_d3d11_cast_backend(report);
}
