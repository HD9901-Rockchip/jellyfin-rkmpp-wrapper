#!/bin/bash
# Jellyfin ffmpeg wrapper for RK3399 (rkmpp)
#
# Fix 1) Strip "-afbc <val>":
#   Jellyfin hardcodes "-afbc rga" for the rkmpp hwaccel. The RK3399 decoder
#   CAN emit AFBC buffers, but the RGA 2D engine on this SoC cannot CONSUME
#   them, so every scale_rkrga / vpp_rkrga / overlay_rkrga filter fails with:
#     "Output format 'nv12' with AFBC modifier is not supported by RGA2"
#
# Fix 2) Strip the per-filter "afbc=N" parameter from overlay_rkrga:
#   When a subtitle must be burned in (Android clients always request this),
#   Jellyfin builds:
#     ...subtitles=...,hwupload=derive_device=rkmpp[sub];
#     [0:0]setparams=...[main];
#     [main][sub]overlay_rkrga=...:format=nv12:afbc=1
#   The graph itself is fine now — the earlier green-tinted output came from
#   an RGA2 hardware limitation: a single pass that converts YUV->RGB (main),
#   blends an RGB layer and converts back RGB->YUV silently drops the final
#   RGB->YUV step, writing raw RGB into the YUV planes. librga 1.10.4 with
#   the "rga split overlay" patch (shipped in this image) works around it by
#   running the job as two hardware passes, so subtitle compositing stays on
#   the RGA engine with no CPU cost. Only the AFBC output request has to be
#   removed, since RGA2 cannot emit AFBC either.
#
#   NOTE: this wrapper REQUIRES the patched librga. Without it the burned-in
#   subtitles come out green/corrupted again.

REAL=/usr/lib/jellyfin-ffmpeg/ffmpeg.bin
args=()
skip=0

for a in "$@"; do
  if [ "$skip" = "1" ]; then skip=0; continue; fi
  if [ "$a" = "-afbc" ]; then skip=1; continue; fi
  case "$a" in
    *overlay_rkrga*)
      a=$(printf '%s' "$a" | sed -E 's/(overlay_rkrga=[^;]*):afbc=[0-9]+/\1/g')
      ;;
  esac
  args+=("$a")
done

exec "$REAL" "${args[@]}"
