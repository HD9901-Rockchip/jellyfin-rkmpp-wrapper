#!/bin/bash
# Uninstall the wrapper and restore the real jellyfin-ffmpeg binary.
set -e

FFDIR="${FFDIR:-/usr/lib/jellyfin-ffmpeg}"

if [ "$(id -u)" != "0" ]; then
  echo "ERROR: run as root (sudo $0)" >&2
  exit 1
fi

if [ -f "$FFDIR/ffmpeg.bin" ]; then
  mv -f "$FFDIR/ffmpeg.bin" "$FFDIR/ffmpeg"
  chmod 755 "$FFDIR/ffmpeg"
  echo "==> real binary restored"
else
  echo "ERROR: $FFDIR/ffmpeg.bin not found - nothing to restore" >&2
  exit 1
fi

systemctl restart jellyfin 2>/dev/null || true
echo "==> done. Jellyfin restarted."
