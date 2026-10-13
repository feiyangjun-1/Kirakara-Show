# 精简 FFmpeg 编解码与体积验证

2026-10-14，Windows x64 Release；与此前通用 LGPL SDK 使用同一 FFmpeg revision。

解码仅保留 H.264、H.265（HEVC）、VP9、AV1、AAC、Opus；编码仅保留 H.264、AAC。
H.264 编码实现为 NVENC、AMF、QSV 和 OpenH264 软件回退；AAC 输出 AAC-LC。
视频默认优先同设备 D3D11VA 硬解，设备或 profile 不支持时回退软件。
AV1 软件回退保留 dav1d 的 8/10 位支持。

## 实际体积

单位 MiB = 1,048,576 字节。以下不包含 Flutter 应用及其其他运行库。

| 内容 | 原通用 SDK | 精简 SDK |
|---|---:|---:|
| 宿主 + 五个 FFmpeg DLL，未压缩 | 124.99 | 10.98 |
| 同上并附声明、许可证、构建信息，未压缩 | 125.04 | 11.47 |
| 运行包 ZIP，附声明与许可证 | 53.81 | 4.28 |
| 运行包与对应源码归档一起 ZIP | 131.67 | 40.23 |

六个 DLL 总体积从 131,059,200 字节降至 11,513,344 字节，减少 91.2%。
其中精简 avcodec 为 7,545,856 字节，avformat 为 605,696 字节。
不构建 avfilter/avdevice、FFmpeg/FFprobe 程序或清单外的编解码器。
对应源码仍占用独立体积，不能把纯运行包 ZIP 当作包含源码的完整分发包。

## 验证

- CMake 直接枚举 SDK DLL：7 个解码实现对应上述 6 种格式，5 个编码实现对应 H.264/AAC。
  通用全功能 SDK 被检查器拒绝。另检查四种视频的 D3D11 设备上下文接口，防止误裁剪后静默软件回退。
- 全部 40 项 CTest 通过，包括硬编/软件 H.264 与 AAC MPEG-TS 编解码回读、强制 IDR、延迟输出排空。
- H.264 8 位、H.265/VP9/AV1 8/10 位样片，BGRA 和 NV12 输出各解出 60/60 个硬件帧；
  同时验证纯软件路径、纹理内容、跳转、EOF 及无硬解设备回退。
  H.264 Hi10P 使用软件回退。
- 验证 AAC 和 Opus 的宿主播放，以及 WAV/PCM、FLAC、MP3 的拒绝行为。
- NVIDIA RTX 5070 Ti Laptop GPU 上，真实 H.264/H.265 视频与 AAC 外部音轨通过完整投屏 smoke。
  10 秒采样约 59.8 fps，无编码丢帧、无解码欠载，错过输出时隙 2 次；
  最大最近 H.264/AAC PTS 差约 10.667 ms。接收端为本机 HTTP 客户端，不代表电视端延迟。
  AMF/QSV 仍需对应显卡实机验证。

## 构建与分发

运行 `tools/prepare_ffmpeg.ps1` 从固定来源和 SHA-256 校验的源码构建
`.deps/ffmpeg-minimal-sdk`，CMake 默认使用该目录；旧通用 SDK 可独立保留。
播放入口也执行格式白名单检查。PCM 仍作为重采样、混音、变调及编码输入的内部样本格式，
不提供 WAV/PCM 文件解码。

构建脚本、版本锁、许可证和实际使用的对应源码随运行库放在 `third_party/ffmpeg`。
源码归档去除 OpenH264 二进制测试素材和未参与构建的 AMF 样例/第三方目录，
保留原始完整档案下载地址、原始 SHA-256 及分发归档 SHA-256。
自有源码继续使用 MIT；共享 FFmpeg 构建保持 LGPL-3.0-or-later。
