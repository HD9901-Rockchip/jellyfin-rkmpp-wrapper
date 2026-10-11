#!/bin/bash
# Install the Jellyfin rkmpp wrapper for RK3399.
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

# 2. Install the wrapper in place of the real binary
install -m 755 "$SCRIPT_DIR/jellyfin-rkmpp-wrapper.sh" "$FFDIR/ffmpeg"
echo "==> wrapper installed as $FFDIR/ffmpeg"

# 3. Optional: CJK fallback font for libass subtitle rendering.
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

# 4. Restart Jellyfin so future transcodes pick up the wrapper
systemctl restart jellyfin 2>/dev/null || true
echo "==> done. Jellyfin restarted."
