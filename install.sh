#!/bin/bash
# Install the Jellyfin rkmpp wrapper + patched librga for RK3399.
# Must run as root on the target board (Armbian/Debian with jellyfin-ffmpeg).
set -e

FFDIR="${FFDIR:-/usr/lib/jellyfin-ffmpeg}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" != "0" ]; then
  echo "ERROR: run as root (sudo $0)" >&2
  exit 1
fi

if [ ! -e "$FFDIR/ffmpeg" ]; then
  echo "ERROR: $FFDIR/ffmpeg not found - is jellyfin-ffmpeg installed?" >&2
  exit 1
fi

# 1. Preserve the real binary once
if [ ! -f "$FFDIR/ffmpeg.bin" ]; then
  cp -a "$FFDIR/ffmpeg" "$FFDIR/ffmpeg.bin"
  echo "==> real binary preserved as $FFDIR/ffmpeg.bin"
else
  echo "==> $FFDIR/ffmpeg.bin already exists, keeping it"
fi

# 2. Install the wrapper (v2: hardware overlay_rkrga, strips afbc only)
install -m 755 "$SCRIPT_DIR/jellyfin-rkmpp-wrapper.sh" "$FFDIR/ffmpeg"
echo "==> wrapper installed as $FFDIR/ffmpeg"

# 3. Patched librga ("rga split overlay" two-pass hardware blending).
#    Without it, burned-in subtitles come out green/corrupted again.
LIB_SRC="$SCRIPT_DIR/librga.so.2.1.0.rga-split"
if [ -f "$LIB_SRC" ]; then
  if [ ! -f "$FFDIR/lib/librga.so.2.1.0.orig" ]; then
    cp -a "$FFDIR/lib/librga.so.2.1.0" "$FFDIR/lib/librga.so.2.1.0.orig"
    echo "==> original librga backed up as librga.so.2.1.0.orig"
  fi
  install -m 644 "$LIB_SRC" "$FFDIR/lib/librga.so.2.1.0"
  echo "==> patched librga installed"
else
  echo "WARNING: $LIB_SRC not found - subtitles will burn in GREEN!" >&2
  echo "         put librga.so.2.1.0.rga-split next to install.sh and re-run." >&2
fi

# 4. Optional: CJK fallback font for libass subtitle rendering.
#    Without it, burned-in Chinese subtitles render as tofu boxes.
FONT_SRC="$SCRIPT_DIR/DroidSansFallbackFull.ttf"
if [ -f "$FONT_SRC" ]; then
  mkdir -p /usr/share/fonts/truetype/droid
  install -m 644 "$FONT_SRC" /usr/share/fonts/truetype/droid/
  fc-cache -f >/dev/null 2>&1 || true
  echo "==> CJK fallback font installed"
else
  echo "==> (no DroidSansFallbackFull.ttf next to the script; skipped)"
  echo "    Chinese subtitles will render as boxes if the system has no CJK font."
fi

# 5. Restart Jellyfin so future transcodes pick up the wrapper
systemctl restart jellyfin 2>/dev/null || true
echo "==> done. Jellyfin restarted."
