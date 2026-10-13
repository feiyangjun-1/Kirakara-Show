# FFmpeg 投屏编码

宿主的 H.264 视频与 AAC 音频编码已迁移到 FFmpeg，保留现有 H.264/AAC MPEG-TS 输出、
连续传输时间戳、关键帧请求、有限队列和 GPU 纹理输入接口。历史 `mf_` 文件和类型名称仅为调用兼容。

## 编码路径

| 输出 | 首选 | 回退 |
|---|---|---|
| H.264 | 同一 D3D11 设备上的 NVENC / AMF，直接 GPU NV12 输入 | 有限 NV12 回读后硬编；Intel QSV 当前使用此路径；最终回退 OpenH264 |
| AAC-LC | FFmpeg 原生 `aac` | 无 Media Foundation 编码回退 |

未使用 FFmpeg 的 `h264_mf` / `aac_mf`，也未引入 GPL `libx264`。原生 AAC 和 FFmpeg
封装仍为 LGPL，软件 H.264 回退依赖 BSD-2-Clause 的 Cisco OpenH264。默认 SDK 的来源、
固定源码版本与分发材料见 [FFmpeg 依赖说明](../third_party/ffmpeg/README.md)。

NVENC/AMF/QSV 输入使用显式 I/IDR 请求、禁用 B 帧和低延迟设置。纹理与池租约由 AVFrame/AVBuffer
引用持有；如果编码器同步完成，不要求伪造异步持有能力。AAC 把任意 PCM 分块聚合为 1024 样本，
填补短暂缺口，停止时补齐最后一帧并排空编码延迟。ADTS 不携带 priming skip 信息，音频约有一帧编码延迟。

## 本机验证（2026-10-13）

- 40 项完整回归通过，包括 H.264/AAC MPEG-TS 编解码回读、软件回退、暂停/跳转、变调和 C ABI。
- NVIDIA GPU 直接输入硬编与显式回读硬编各输出 60 帧，强制 IDR 和租约释放验证通过。
- MPEG-TS 回读验证硬编/软件各 30 帧 H.264、至少两个关键帧、48 个 AAC 帧，图像亮度和音频信号正确。
- 真实 1080p H.264/H.265 的完整投屏验证通过：冷启动、快速切歌、暂停/恢复、重连及热接管。
- 连续采样编码约 59.7 fps，最大最近 H.264/AAC PTS 差约 10.7 ms，强制关键帧请求均产生对应输出。
- AMF、Intel QSV 需要对应显卡进一步实机验证。OpenH264 保留不跳帧策略，码率是目标值，复杂画面可能短时超出目标。

探针 `kirakara_show_mf_h264_encoder_probe` 支持 `--software`、`--readback`、`--adapter=名称` 和
`--output=路径`；自动回读测试为 `kirakara_show_ffmpeg_encode_roundtrip_tests`。
宿主已移除 MF 编码器的创建、MFT 枚举以及无用途的 Media Foundation 初始化。
