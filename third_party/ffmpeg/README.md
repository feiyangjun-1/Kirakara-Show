# FFmpeg 编解码依赖

自有代码保留 MIT。默认 SDK 是锁定版本的 Windows x64 FFmpeg 8.1 LGPL 共享构建，
不是 GPL 构建。该 SDK 使用 `--enable-version3`，适用 LGPL-3.0-or-later。
附带 GPLv3 正文是因为 LGPLv3 引用它，不代表本项目采用 GPL。

运行 `tools/prepare_ffmpeg.ps1` 准备 SDK，或通过 `KIRAKARA_FFMPEG_ROOT` 指向自己的
精简 LGPL 共享 SDK。导入库只链接 DLL；构建和运行时都会拒绝启用 GPL/nonfree 的构建。
解码仅保留 H.264、H.265、VP9、AV1、AAC、Opus；编码仅保留 H.264、AAC。
CMake 枚举实际 DLL 中的编解码器，拒绝缺少所需实现或包含其他格式的 SDK。
播放入口同时检查格式白名单，WAV/PCM、MP3、FLAC 等文件不再支持。
AV1 软件回退使用 `libdav1d`，精简 SDK 保留其 8/10 位支持。

视频默认在渲染器同一 D3D11 device 上尝试 D3D11VA，输出 NV12 或 P010 硬件帧。
输出给 Stage 的 BGRA、给 Cast 的 NV12 通过 GPU 复制或 VideoProcessor 转换生成。
当设备或 codec profile 不支持硬解时，自动回退软件解码和纹理上传。
所有宿主音视频解码均走 FFmpeg，音频经 swresample 转成现有播放器所需的立体声浮点 PCM。
H.264/AV1/HEVC/VP9 的本机播放使用独立媒体时钟，不再要求 Media Engine 解码同一个视频；
未指定外部音轨时会提取视频中的音轨（包括 Opus），用于本机播放和 Cast。
DirectSound、现有变调和音轨切换继续使用处理后的 PCM；音频仍会整轨预解码到内存。
本次切换适用于 `show_host.dll`；独立演示程序保留各自的媒体流程。

## 投屏编码

- H.264：优先使用同一 D3D11 设备上的 FFmpeg NVENC / AMF 硬编，直接引用 GPU NV12 纹理。
- 兼容路径：NVENC / AMF 可使用有界 NV12 回读；Intel QSV 当前使用同设备派生的 QSV 上下文和 NV12 回读。
- 软件回退：`libopenh264`（Cisco BSD-2-Clause），不使用 GPL `libx264`，维持 LGPL SDK。
- AAC：使用 FFmpeg 原生 `aac` 编码器，输出 AAC-LC。PCM 任意分块会聚合为 1024 样本；停止时排空延迟输出。
- 码流：H.264 Annex B + AAC ADTS，继续使用现有 MPEG-TS 封装、连续传输 PTS 和强制 IDR 请求。
- FFmpeg 的帧引用与 AVBuffer 释放回调持有纹理和池租约；同步完成的硬编无需伪装成异步 MFT。

本机已验证 NVIDIA GPU 直接输入、硬编回读、OpenH264 软件回退和 H.264/AAC MPEG-TS 编解码回读。
AMF/QSV 路径还需要对应显卡实机验证。AAC 的 priming/padding 在 ADTS 中不能携带跳过样本元数据，
传输原点保持非负，音频会有约一帧的编码延迟。OpenH264 不跳帧，码率为目标值，复杂画面可能短时超过目标。

## 分发材料

准备脚本从锁定 SHA-256 的源代码构建 DLL，而不再下载通用全功能 SDK。
需要 Git Bash、Python（含 pip）、CMake 和 Ninja；Visual Studio Build Tools 内置的 CMake/Ninja
会自动发现。便携 w64devkit 和固定版本 Meson 仅安装到项目 `.deps`。
默认 SDK 位于 `.deps/ffmpeg-minimal-sdk`，与原通用 SDK 分开保存。
不生成 FFmpeg/FFprobe 命令行程序，不构建 avfilter/avdevice 或其他格式编解码器。

准备脚本保存 FFmpeg、OpenH264、dav1d、oneVPL、AMF 头文件、NV 编码头文件的
固定来源、SHA-256、对应源码、构建脚本、实际 configure 参数和许可证。引擎构建把这些材料放在
DLL 同目录的 `third_party/ffmpeg`；Desktop 安装时一起复制。
附带 MinGW-w64 运行库声明与 GCC Runtime Library Exception，覆盖编译器运行库材料。

源码压缩包只排除 OpenH264 的二进制测试素材及未参与构建的 AMF 样例/第三方目录，
保留实际使用的源代码、头文件、许可证和构建文件；原始完整档案的地址与校验值保存在版本锁中。
保持 DLL 可被用户替换，发布说明和关于页面应声明 FFmpeg 的使用与源码获取位置。

来源：[FFmpeg 许可与合规清单](https://ffmpeg.org/legal.html)、
[FFmpeg 源码](https://github.com/FFmpeg/FFmpeg)。
