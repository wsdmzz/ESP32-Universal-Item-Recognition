#!/bin/bash
# 补抓薄弱类别：空类 + 只有 ~50 张的日用品/水果类
cd "$(dirname "$0")/../.." || exit 1
PY=/home/oooa/msdl/bin/python
LOG=/tmp/commons_boost.log
CAP=${1:-450}
: > "$LOG"
run() {
  cls="$1"; cat="$2"
  echo "### $cls <- $cat" >> "$LOG"
  timeout 1500 "$PY" tools/dataset80/commons_fetch.py "$cls" "$cat" "$CAP" >> "$LOG" 2>&1
  sleep 5
}
run narcissus      "Category:Narcissus"
run blossom_cherry "Category:Prunus serrulata"
run hydrangea      "Category:Hydrangea macrophylla"
run jasmine        "Category:Jasminum"
run hibiscus       "Category:Hibiscus"
run towel          "Category:Towels"
run umbrella       "Category:Umbrellas"
run backpack       "Category:Backpacks"
run phone          "Category:Mobile phones"
run keyboard       "Category:Computer keyboards"
run mouse          "Category:Computer mice"
run remote         "Category:Television remote controls"
run table          "Category:Tables"
run corn           "Category:Maize"
run lemon          "Category:Lemon"
run strawberry     "Category:Strawberries"
echo "ALL DONE" >> "$LOG"
