#!/bin/bash
# Jellyfin ffmpeg wrapper for RK3399 (rkmpp)
#
# Fix 1) Strip "-afbc <val>":
#   Jellyfin hardcodes "-afbc rga" for the rkmpp hwaccel. The RK3399 decoder
#   CAN emit AFBC buffers, but the RGA 2D engine on this SoC cannot CONSUME
#   them, so every scale_rkrga / vpp_rkrga / overlay_rkrga filter fails with:
#     "Output format 'nv12' with AFBC modifier is not supported by RGA2"
#
# Fix 2) Rewrite the overlay_rkrga subtitle-burn graph into software overlay:
#   When a subtitle must be burned in (Android clients always request this),
#   Jellyfin builds:
#     ...subtitles=...,hwupload=derive_device=rkmpp[sub];
#     [0:0]setparams=...[main];
#     [main][sub]overlay_rkrga=...:format=nv12:afbc=1
#   The RGA2 hardware BGRA->NV12 blend corrupts chroma, producing a fully
#   green-tinted picture (web clients use the simple vpp_rkrga path and are
#   unaffected). Replacement chain, verified on RK3399:
#     scale_rkrga=format=nv12:afbc=0  (hw, also bridges hwdownload:
#        raw decoder frames cannot be downloaded, scale_rkrga frames can)
#     -> hwdownload,format=nv12       (CPU copy)
#     -> overlay (software blend with BGRA subtitle layer)
#     -> format=nv12 -> hwupload=derive_device=rkmpp (back to hw encode)
#
# Install: see README.md

REAL=/usr/lib/jellyfin-ffmpeg/ffmpeg.bin
args=()
skip=0

for a in "$@"; do
  if [ "$skip" = "1" ]; then skip=0; continue; fi
  if [ "$a" = "-afbc" ]; then skip=1; continue; fi
  case "$a" in
    *overlay_rkrga*)
      a=$(printf '%s' "$a" | sed \
        -e 's/,hwupload=derive_device=rkmpp\[sub\]/[sub]/g' \
        -e 's/\[0:0\]\([^;\[]*\)\[main\]/[0:0]\1,scale_rkrga=format=nv12:afbc=0,hwdownload,format=nv12[main]/g' \
        -e 's/\[main\]\[sub\]overlay_rkrga=[^;]*/[main][sub]overlay=eof_action=pass:repeatlast=0,format=nv12,hwupload=derive_device=rkmpp/g')
      ;;
  esac
  args+=("$a")
done

exec "$REAL" "${args[@]}"
