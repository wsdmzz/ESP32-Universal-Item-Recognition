#!/usr/bin/env bash
# 把一张图片推到 4 寸出题屏上。
#   用法: tools/show_photo.sh 图片.jpg [http://192.168.8.194]
#   支持 jpg/jpeg（板子上用 ROM 里的 TJpgDec 软解，等比缩放居中，黑边补齐）。
#   PNG/BMP 先转一下: convert a.png -quality 90 a.jpg
set -euo pipefail
F="${1:?用法: tools/show_photo.sh 图片.jpg [设备IP]}"
IP="${2:-http://192.168.8.194}"
TMP=""
case "${F,,}" in
  *.jpg|*.jpeg) SRC="$F" ;;
  *) TMP="$(mktemp --suffix=.jpg)"; python3 - "$F" "$TMP" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
im.save(sys.argv[2], quality=92)
PY
  SRC="$TMP" ;;
esac
echo "-> POST ${SRC} ($(stat -c%s "$SRC") bytes) 到 ${IP}/show"
curl -sS -m 60 --data-binary "@${SRC}" -H 'Content-Type: image/jpeg' "${IP}/show"
echo
[ -n "$TMP" ] && rm -f "$TMP"
exit 0
