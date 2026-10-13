#!/usr/bin/env bash
set -euxo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROOT_WINDOWS=$(cygpath -m "$ROOT")
DEPS=$ROOT/.deps
export PATH="/usr/bin:$DEPS/toolchain/w64devkit/bin:$DEPS/python-build/bin:$PATH"
export PYTHONPATH="$DEPS/python-build"
export CC=gcc CXX=g++ AR=ar
PREFIX=$DEPS/minimal-prefix
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
mkdir -p "$PREFIX"
export TMPDIR="$DEPS/minimal-tmp"
mkdir -p "$TMPDIR"
SRC=$DEPS/minimal-sources
if [ "${1:-}" != "ffmpeg" ]; then
DAV1D=$SRC/dav1d-9711965b60bb692ae24004659acf61f5c7d9ed61
if [ ! -f "$DEPS/dav1d-minimal-build/build.ninja" ]; then
meson setup "$DEPS/dav1d-minimal-build" "$DAV1D" --prefix="$ROOT_WINDOWS/.deps/minimal-prefix" --libdir=lib --buildtype=release --default-library=static -Denable_tools=false -Denable_tests=false -Denable_examples=false
fi
meson compile -C "$DEPS/dav1d-minimal-build" -j 8
meson install -C "$DEPS/dav1d-minimal-build"
make -C "$SRC/openh264-8b2d28faade10d74d99ac80e199aef664c9c5a3b" -j8 OS=mingw_nt ARCH=x86_64 BUILDTYPE=Release DEBUGSYMBOLS=False PREFIX="$PREFIX" CC=gcc CXX=g++ AR=ar SHELL=/usr/bin/bash install-static
make -C "$SRC/nv-codec-headers-eddcea9e27f6b772057c9b3f87de2cc1737faffc" PREFIX="$PREFIX" install
mkdir -p "$PREFIX/include/AMF"
cp -R "$SRC/AMF-6277e353fd625121a8f627b1d0540323ef372a49/amf/public/include/"* "$PREFIX/include/AMF/"
cmake -S "$SRC/libvpl-674d015bcb294bc39fa276e99a652ea045423e82" -B "$DEPS/vpl-minimal-build" -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ROOT_WINDOWS/.deps/minimal-prefix" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_DISPATCHER=ON -DBUILD_DEV=ON -DBUILD_PREVIEW=OFF -DBUILD_TOOLS=OFF -DBUILD_TOOLS_ONEVPL_EXPERIMENTAL=OFF -DINSTALL_EXAMPLE_CODE=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF
cmake --build "$DEPS/vpl-minimal-build" -j8
cmake --install "$DEPS/vpl-minimal-build"
fi
mkdir -p "$DEPS/ffmpeg-minimal-build"
cd "$DEPS/ffmpeg-minimal-build"
bash "$SRC/FFmpeg-46d899d7a02bc63237472cc4897258d5ddf274ac/configure" \
 --prefix="$ROOT_WINDOWS/.deps/ffmpeg-minimal-sdk" --target-os=mingw32 --arch=x86_64 \
 --cc=gcc --cxx=g++ --ar=ar --nm=nm --enable-shared --disable-static \
 --disable-everything --disable-autodetect --disable-doc --disable-programs --disable-debug \
 --disable-avfilter --disable-avdevice --enable-small --enable-version3 \
 --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
 --enable-libdav1d --enable-libopenh264 --enable-libvpl --enable-amf --enable-ffnvcodec \
 --enable-d3d11va --enable-nvenc --enable-schannel \
 --enable-decoder=h264,hevc,vp9,av1,libdav1d,aac,opus \
 --enable-encoder=libopenh264,h264_nvenc,h264_amf,h264_qsv,aac \
 --enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2,vp9_d3d11va,vp9_d3d11va2,av1_d3d11va,av1_d3d11va2 \
 --enable-parser=h264,hevc,vp9,av1,aac,opus \
 --enable-demuxer=mov,matroska,mpegts,aac,ogg,avi,h264,hevc,av1 \
 --enable-muxer=mpegts --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata \
 --enable-protocol=file,http,https,tcp,tls,crypto,data \
 --extra-cflags="-I$PREFIX/include -O2" --extra-ldflags="-L$PREFIX/lib -static-libgcc -static-libstdc++" --extra-libs="-lstdc++" \
 --pkg-config-flags=--static
# Native GNU Make cannot resolve MSYS /drive paths in included makefiles.
sed -i "s|$ROOT|$ROOT_WINDOWS|g" Makefile ffbuild/config.mak
make -j8
make install
