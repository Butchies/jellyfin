#!/bin/bash
# Baut den Decoder-Kern des Butchies-Web-Players (FFmpeg -> WebAssembly) in zwei Varianten:
#   bmccore-mt.js/.wasm  mit Threads (schnell, braucht SharedArrayBuffer = https + COOP/COEP)
#   bmccore-st.js/.wasm  ohne Threads (Notlösung, z. B. bei http://)
set -euo pipefail
FFVER=${FFVER:-7.1.1}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/out}
mkdir -p "$OUT"
[ -d ffmpeg-$FFVER ] || curl -fsSL https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz | tar -xJ

COMMON="--target-os=none --arch=x86_32 --enable-cross-compile --disable-asm --disable-inline-asm --disable-x86asm
  --disable-stripping --disable-programs --disable-doc --disable-debug --disable-runtime-cpudetect --disable-autodetect
  --disable-network --disable-everything --disable-avdevice --disable-avfilter --disable-postproc
  --enable-avformat --enable-avcodec --enable-swresample --enable-swscale
  --enable-protocol=file
  --enable-demuxer=matroska,mov,avi,mpegts,mpegps,flv,asf,ogg,mp3,aac,ac3,eac3,dts,truehd,flac,wav
  --enable-decoder=hevc,h264,mpeg4,mpeg2video,mpeg1video,vc1,wmv3,vp8,vp9,msmpeg4v3,aac,aac_latm,ac3,eac3,dca,truehd,mlp,mp1,mp2,mp3,flac,opus,vorbis,alac,wmav2,pcm_s16le,pcm_s24le,pcm_s16be,pcm_dvd,pcm_bluray
  --enable-parser=hevc,h264,mpeg4video,mpegvideo,vc1,vp8,vp9,aac,aac_latm,ac3,dca,mlp,mpegaudio,flac,opus,vorbis
  --enable-bsf=hevc_mp4toannexb,h264_mp4toannexb,vp9_superframe_split
  --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm --objcc=emcc --dep-cc=emcc"

EXPORTS='["_bmc_open","_bmc_info","_bmc_step","_bmc_seek","_bmc_set_audio","_bmc_v_width","_bmc_v_height","_bmc_v_bits","_bmc_v_data","_bmc_v_linesize","_bmc_v_pts","_bmc_v_matrix","_bmc_v_range","_bmc_a_data","_bmc_a_samples","_bmc_a_pts","_malloc","_free"]'
RUNTIME='["HEAPU8","HEAPF32","UTF8ToString"]'

build() {
  local name=$1 thr=$2
  local src=ffmpeg-$FFVER
  local bdir=build-$name
  rm -rf $bdir && mkdir $bdir && cd $bdir
  local CF="-O3 -msimd128" LF="" TH="--disable-pthreads --disable-w32threads --disable-os2threads"
  if [ "$thr" = 1 ]; then CF="$CF -pthread"; LF="-pthread"; TH="--enable-pthreads --disable-w32threads --disable-os2threads"; fi
  emconfigure ../$src/configure $COMMON $TH --extra-cflags="$CF" --extra-ldflags="$LF" >/dev/null
  emmake make -j"$(nproc)" >/dev/null
  local EXTRA="-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB"
  if [ "$thr" = 1 ]; then EXTRA="-pthread -sPTHREAD_POOL_SIZE=8 -sINITIAL_MEMORY=768MB"; fi
  emcc $CF -I. -I../$src "$HERE/player.c" libavformat/libavformat.a libavcodec/libavcodec.a libswscale/libswscale.a \
       libswresample/libswresample.a libavutil/libavutil.a \
       -o "$OUT/bmccore-$name.js" $EXTRA -sMODULARIZE=1 -sEXPORT_NAME=BmcCore -sENVIRONMENT=worker \
       -sEXPORTED_FUNCTIONS="$EXPORTS" -sEXPORTED_RUNTIME_METHODS="$RUNTIME" -sSTACK_SIZE=1MB -sFILESYSTEM=0
  cd ..
}
build st 0
build mt 1
ls -la "$OUT"
