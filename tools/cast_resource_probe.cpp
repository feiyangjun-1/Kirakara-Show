// Compare full host playback with identical media/settings across DLL builds.
#include "../apps/show_host/show_host_api.h"
#include <winsock2.h>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

struct GpuCounters {
    PDH_HQUERY query{};
    PDH_HCOUNTER engines{}, dedicated{}, shared{};
    std::wstring prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    bool available{};
    GpuCounters() {
        available = PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS
            && PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines) == ERROR_SUCCESS;
        if (query) {
            PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &dedicated);
            PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &shared);
            PdhCollectQueryData(query);
        }
    }
    ~GpuCounters() { if (query) PdhCloseQuery(query); }
    double sum(PDH_HCOUNTER counter, const wchar_t* kind = nullptr) {
        if (!counter) return 0.0;
        DWORD bytes{}, count{};
        const auto format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
        if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, nullptr) != PDH_MORE_DATA) return 0.0;
        std::vector<unsigned char> buffer(bytes);
        auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, values) != ERROR_SUCCESS) return 0.0;
        double total{};
        for (DWORD index = 0; index < count; ++index) {
            const std::wstring_view name(values[index].szName);
            if (!name.starts_with(prefix) || (kind && name.find(kind) == name.npos)) continue;
            const auto& value = values[index].FmtValue;
            if ((value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA)
                    && std::isfinite(value.doubleValue)) total += value.doubleValue;
        }
        return total;
    }
};

double process_cpu_seconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto integer = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (integer(kernel) + integer(user)) / 10000000.0;
}

#include <winsock2.h>
#include <ws2tcpip.h>
#include <unordered_map>
#include <numeric>
#include <algorithm>

class StreamClient {
public:
    SOCKET connection{INVALID_SOCKET};
    std::thread reader;
    std::atomic<std::uint64_t> bytes{},packets{};
    bool start(unsigned short port) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2,2),&data)) return false;
        connection=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port);
        inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
        if (connect(connection,reinterpret_cast<sockaddr*>(&address),sizeof(address))) return false;
        const char request[]="GET /cast/stage.ts HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (send(connection,request,sizeof(request)-1,0)<0) return false;
        reader=std::thread([this] {
            char data[65536];
            for (;;) {
                const auto count=recv(connection,data,sizeof(data),0);
                if (count<=0) break;
                bytes.fetch_add(count,std::memory_order_relaxed);
                packets.fetch_add(1,std::memory_order_relaxed);
            }
        });
        return true;
    }
    ~StreamClient() {
        if (connection!=INVALID_SOCKET) { shutdown(connection,SD_BOTH); closesocket(connection); }
        if (reader.joinable()) reader.join();
        WSACleanup();
    }
};
int wmain(int argc,wchar_t** argv) {
    if (argc!=5) { std::cerr<<"cast probe video audio output_json sample_seconds\n"; return 2; }
    auto host=show_host_create();
    if (!host) return 3;
    if (!show_host_load_with_clock(host,argv[1],L"",argv[2],L"",SHOW_CLOCK_VIDEO_MASTER)) return 4;
    show_host_set_volume(host,0);
    show_host_set_cast_pipeline_trace_enabled(host,true);
    const auto startup_begin=std::chrono::steady_clock::now();
    if (!show_host_start_cast_stream(host,0)) return 5;
    const auto startup_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-startup_begin).count();
    StreamClient client;
    if (!client.start(show_host_get_cast_stream_port(host))) return 6;
    show_host_play(host);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    ShowHostCastCapabilityReport capability{}; capability.struct_size=sizeof(capability);
    if (!show_host_get_cast_capability_report(host,&capability) || !capability.backend_active) return 7;
    GpuCounters gpu;
    ShowHostCastPipelineStats before{}; before.struct_size=sizeof(before);
    show_host_get_cast_pipeline_stats(host,&before);
    ShowHostCastPipelineTraceState initial_trace{}; initial_trace.struct_size=sizeof(initial_trace);
    show_host_get_cast_pipeline_trace_state(host,&initial_trace);
    const auto first_bytes=client.bytes.load();
    const auto cpu_start=process_cpu_seconds();
    const auto start=std::chrono::steady_clock::now();
    const int seconds=std::max(5,_wtoi(argv[4]));
    double working{},committed{},decode{},encode{},three_d{},dedicated{},shared{};
    int samples{};
    for (int index=0;index<seconds*2;++index) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (gpu.query) PdhCollectQueryData(gpu.query);
        decode+=gpu.sum(gpu.engines,L"engtype_VideoDecode");
        encode+=gpu.sum(gpu.engines,L"engtype_VideoEncode");
        three_d+=gpu.sum(gpu.engines,L"engtype_3D");
        dedicated+=gpu.sum(gpu.dedicated); shared+=gpu.sum(gpu.shared);
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
        working+=memory.WorkingSetSize; committed+=memory.PrivateUsage; ++samples;
    }
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    const auto cpu=process_cpu_seconds()-cpu_start;
    const auto emitted_bytes=client.bytes.load()-first_bytes;
    ShowHostCastPipelineStats after{}; after.struct_size=sizeof(after);
    show_host_get_cast_pipeline_stats(host,&after);
    const bool playing=show_host_get_state(host)==SHOW_STATE_PLAYING;
    ShowHostCastPipelineTraceState trace_state{}; trace_state.struct_size=sizeof(trace_state);
    show_host_get_cast_pipeline_trace_state(host,&trace_state);
    std::vector<ShowHostCastPipelineTraceRecord> records(trace_state.capacity);
    const auto count=show_host_read_cast_pipeline_trace(host,records.data(),records.size(),initial_trace.latest_sequence);
    std::unordered_map<std::uint64_t,std::uint64_t> inputs;
    std::vector<double> latencies;
    for (std::uint32_t index=0;index<count;++index) {
        const auto& event=records[index];
        if (event.event==SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT)
            inputs[event.correlation_id]=event.qpc-event.duration_qpc;
        else if (event.event==SHOW_CAST_EVENT_ENCODER_OUTPUT) {
            const auto input=inputs.find(event.correlation_id);
            if (input!=inputs.end() && event.qpc>=input->second)
                latencies.push_back((event.qpc-input->second)*1000.0/after.qpc_frequency);
        }
    }
    std::sort(latencies.begin(),latencies.end());
    const auto average=latencies.empty()?0.0:std::accumulate(latencies.begin(),latencies.end(),0.0)/latencies.size();
    const auto p95=latencies.empty()?0.0:latencies[std::min(latencies.size()-1,static_cast<std::size_t>(latencies.size()*0.95))];
    const auto encoded=after.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count-before.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count;
    show_host_stop_cast_stream(host); show_host_destroy(host);
    const double mib=1024.0*1024.0;
    std::ofstream out{std::filesystem::path(argv[3])};
    out<<"{\n"
       <<"\"cpu_percent_machine\":"<<cpu/elapsed*100/GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)<<",\n"
       <<"\"cpu_core_equivalents\":"<<cpu/elapsed<<",\n"
       <<"\"working_set_mib\":"<<working/samples/mib<<",\n"
       <<"\"private_bytes_mib\":"<<committed/samples/mib<<",\n"
       <<"\"gpu_video_encode_percent\":"<<encode/samples<<",\n"
       <<"\"gpu_video_decode_percent\":"<<decode/samples<<",\n"
       <<"\"gpu_3d_percent\":"<<three_d/samples<<",\n"
       <<"\"gpu_dedicated_mib\":"<<dedicated/samples/mib<<",\n"
       <<"\"gpu_shared_mib\":"<<shared/samples/mib<<",\n"
       <<"\"encoder_fps\":"<<encoded/elapsed<<",\n"
       <<"\"encoder_latency_mean_ms\":"<<average<<",\n"
       <<"\"encoder_latency_p95_ms\":"<<p95<<",\n"
       <<"\"matched_latency_frames\":"<<latencies.size()<<",\n"
       <<"\"actual_transport_mbps\":"<<emitted_bytes*8.0/elapsed/1000000<<",\n"
       <<"\"encoder_drops\":"<<after.encoder_drops-before.encoder_drops<<",\n"
       <<"\"missed_slots\":"<<after.missed_output_slots-before.missed_output_slots<<",\n"
       <<"\"decoder_underflows\":"<<after.decoder_underflows-before.decoder_underflows<<",\n"
       <<"\"startup_ms\":"<<startup_ms<<",\n"
       <<"\"backend\":"<<capability.backend<<",\n"
       <<"\"requires_encoder_copy\":"<<(capability.requires_encoder_copy?"true":"false")<<",\n"
       <<"\"gpu_counters_available\":"<<(gpu.available?"true":"false")<<",\n"
       <<"\"playing_at_end\":"<<(playing?"true":"false")<<",\n"
       <<"\"elapsed_seconds\":"<<elapsed<<"\n}\n";
    return playing && encoded>seconds*40 && emitted_bytes>0 && !latencies.empty()?0:8;
}
