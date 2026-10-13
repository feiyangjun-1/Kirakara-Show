#include "../apps/show_host/cast/mf_d3d11_h264_encoder.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>

int wmain(int argc,wchar_t** argv) {
    bool software=false,readback=false;
    std::wstring adapter_filter;
    std::filesystem::path output_path;
    for (int i=1;i<argc;++i) {
        const std::wstring arg(argv[i]);
        if (arg==L"--software") software=true;
        else if (arg==L"--readback") readback=true;
        else if (arg.starts_with(L"--adapter=")) adapter_filter=arg.substr(10);
        else if (arg.starts_with(L"--output=")) output_path=arg.substr(9);
    }
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    IDXGIFactory* factory{};
    IDXGIAdapter* selected{};
    if (!adapter_filter.empty() && SUCCEEDED(CreateDXGIFactory(IID_PPV_ARGS(&factory)))) {
        for (UINT index=0;;++index) {
            IDXGIAdapter* candidate{};
            if (factory->EnumAdapters(index,&candidate)==DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC desc{}; candidate->GetDesc(&desc);
            if (std::wstring(desc.Description).find(adapter_filter)!=std::wstring::npos) { selected=candidate; break; }
            candidate->Release();
        }
        factory->Release();
        if (!selected) return 2;
    }
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    D3D_FEATURE_LEVEL level{};
    const auto created=D3D11CreateDevice(selected,selected ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,
        D3D11_SDK_VERSION,&device,&level,&context);
    if (selected) selected->Release();
    if (FAILED(created)) return 3;
    ID3D10Multithread* threading{};
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&threading)))) { threading->SetMultithreadProtected(TRUE); threading->Release(); }
    MfD3D11H264EncoderConfig config;
    config.width=640; config.height=360; config.frame_rate_num=30; config.frame_rate_den=1;
    config.bitrate=1500000; config.gop_size_frames=300;
    config.input_format=MfH264EncoderInputFormat::nv12;
    config.preference=software ? MfH264EncoderPreference::software_only
        : readback ? MfH264EncoderPreference::hardware_readback_only : MfH264EncoderPreference::hardware_only;
    std::vector<unsigned char> pixels(640*360*3/2,128);
    std::fill_n(pixels.begin(),640*360,160);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=640; desc.Height=360; desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_NV12; desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(),640,0};
    ID3D11Texture2D* texture{};
    if (FAILED(device->CreateTexture2D(&desc,&initial,&texture))) return 4;
    auto token=std::shared_ptr<void>(texture,[](void* pointer) { static_cast<ID3D11Texture2D*>(pointer)->Release(); });
    std::weak_ptr<void> lifetime=token;
    std::mutex mutex;
    std::condition_variable ready;
    int frames{},keys{};
    std::int64_t last_pts=-1;
    bool valid=true,forced=false;
    std::ofstream file;
    if (!output_path.empty()) file.open(output_path,std::ios::binary);
    MfD3D11H264Encoder encoder;
    if (!encoder.start(device,config,[&](const std::uint8_t* bytes,std::size_t size,std::int64_t pts,bool key) {
        std::lock_guard lock(mutex);
        valid=valid && size>4 && bytes[0]==0 && bytes[1]==0 && (bytes[2]==1 || (bytes[2]==0 && bytes[3]==1)) && pts>last_pts;
        last_pts=pts; ++frames; if (key) ++keys;
        if (key && pts>=90000) forced=true;
        if (file) file.write(reinterpret_cast<const char*>(bytes),size);
        ready.notify_all();
    },nullptr,texture,token)) return 5;
    std::cout << "FFmpeg backend=" << (encoder.backend()==MfH264EncoderBackend::hardware ? "hardware" : "software")
        << " input=" << (readback || software ? "readback" : "GPU") << std::endl;
    for (int frame=0;frame<60;++frame) {
        if (!encoder.submit_nv12_texture(texture,token,static_cast<std::int64_t>(frame)*10000000/30,
                10000000/30,frame==0 || frame==30)) return 6;
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock,std::chrono::seconds(3),[&] { return frames>=frame+1; })) return 7;
    }
    encoder.stop(); token.reset();
    context->Release(); device->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    std::cout << "frames=" << frames << " keys=" << keys << " forced_IDR=" << forced
        << " lease_released=" << lifetime.expired() << std::endl;
    return valid && forced && lifetime.expired() ? 0 : 8;
}
