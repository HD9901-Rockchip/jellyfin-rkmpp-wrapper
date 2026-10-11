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
#
# Fix 3) Cap 4K transcodes at 1080p:
#   RK3399's encoder (VEPu2) is a 1080p-class block: encoding 3840x2160 H264
#   runs at ~5 fps (0.2x realtime), so any "4K quality" transcode just stalls
#   the client in an endless buffer/restart loop. 1080p output runs at ~38 fps
#   (1.5x realtime) from a 4K source. Clients that ask for a 4K transcode get
#   1080p instead; Direct-Play and remux sessions never reach this wrapper
#   path and are untouched. Two graph shapes are handled:
#     a) simple:  -vf "...vpp_rkrga=w=3840:h=2160:format=nv12..." -> 1920x1080
#     b) overlay: sub canvas "vpp_rkrga=w=3840:h=2160:format=bgra" -> 1080p,
#        and a 1080p scale is inserted on [main] before the overlay.

REAL=/usr/lib/jellyfin-ffmpeg/ffmpeg.bin
args=()
skip=0

for a in "$@"; do
  if [ "$skip" = "1" ]; then skip=0; continue; fi
  if [ "$a" = "-afbc" ]; then skip=1; continue; fi
  case "$a" in
    *overlay_rkrga*)
      a=$(printf '%s' "$a" | sed -E 's/(overlay_rkrga=[^;]*):afbc=[0-9]+/\1/g')
      case "$a" in
        *w=3840*)
          a=$(printf '%s' "$a" | sed -E '
            s/vpp_rkrga=w=3840:h=2160:format=bgra/vpp_rkrga=w=1920:h=1080:format=bgra/g;
            s/(\[0:0\][^;]*)\[main\]/\1,vpp_rkrga=w=1920:h=1080:format=nv12:afbc=0[main]/g;
            s/hwupload=derive_device=rkmpp,vpp_rkrga=w=1920:h=1080:format=bgra:afbc=1\[sub\]/hwupload=derive_device=rkmpp[sub]/g
          ')
          ;;
      esac
      ;;
    *vpp_rkrga=w=3840*)
      a=$(printf '%s' "$a" | sed -E 's/vpp_rkrga=w=3840:h=2160/vpp_rkrga=w=1920:h=1080/g')
      ;;
  esac
  args+=("$a")
done

exec "$REAL" "${args[@]}"
