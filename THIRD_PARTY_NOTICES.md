# Third-Party Notices

Kirakara Show 自有代码使用仓库根目录中的 MIT License。以下随源码分发的第三方组件保留其
各自版权和许可证。

## Signalsmith Stretch

- Project: Signalsmith Stretch
- Upstream: <https://github.com/Signalsmith-Audio/signalsmith-stretch>
- Copyright: Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.
- License: MIT
- Local license: [third_party/signalsmith-stretch/LICENSE.txt](third_party/signalsmith-stretch/LICENSE.txt)

## Signalsmith Linear

- Project: Signalsmith Linear
- Upstream: <https://github.com/Signalsmith-Audio/linear>
- Copyright: Copyright (c) 2025 Signalsmith Audio
- License: MIT
- Local license: [third_party/signalsmith-linear/LICENSE.txt](third_party/signalsmith-linear/LICENSE.txt)

第三方许可证原文以各组件目录中的 `LICENSE.txt` 为准。

## FFmpeg

- 用途：宿主所有音视频解码，以及投屏 H.264 / AAC 编码。
- Project: [FFmpeg](https://ffmpeg.org/)
- 默认 SDK：FFmpeg 8.1 LGPL 共享构建，许可证 LGPL-3.0-or-later。
- 固定版本、来源、构建参数和分发要求见 [FFmpeg 依赖说明](third_party/ffmpeg/README.md)。
- SDK 中第三方库保留各自许可证。FFmpeg DLL 与其许可及源码材料随宿主一起分发，
  自有代码仍为 MIT。精简 SDK 附带 dav1d、OpenH264、oneVPL、AMF/NV SDK 头文件
  的许可及对应源码材料。

软件 H.264 编码回退通过 FFmpeg 使用 Cisco OpenH264（BSD-2-Clause）。版本锁、源码和许可材料
由 FFmpeg 准备脚本保存，FFmpeg 共享库整体仍遵循 LGPL-3.0-or-later。
