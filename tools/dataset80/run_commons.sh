#!/usr/bin/env bash
# 批量抓取 Wikimedia Commons 补缺类别（可重复运行，增量去重）
set -u
cd "$(dirname "$0")/../.."
PY=/home/oooa/msdl/bin/python
F=tools/dataset80/commons_fetch.py
N=${1:-600}
run() { echo "### $1 <- $2"; timeout 1200 $PY $F "$1" "$2" "$N"; }
run watermelon     "Category:Citrullus lanatus"
run cherry         "Category:Cherries"
run blueberry      "Category:Blueberries"
run lychee         "Category:Lychees"
run peach          "Category:Prunus persica"
run onion          "Category:Onions"
run lettuce        "Category:Lactuca sativa"
run spinach        "Category:Spinacia oleracea"
run garlic         "Category:Garlic"
run ginger         "Category:Zingiber officinale"
run mushroom       "Category:Agaricus bisporus"
run orchid         "Category:Orchidaceae"
run peony          "Category:Paeonia"
run lotus          "Category:Nelumbo nucifera"
run blossom_cherry "Category:Cherry blossom"
run jasmine        "Category:Jasminum"
run hydrangea      "Category:Hydrangea"
run lavender       "Category:Lavandula"
run hibiscus       "Category:Hibiscus"
run lily           "Category:Lilium"
run chrysanthemum  "Category:Chrysanthemum"
run rose           "Category:Rosa"
run chair          "Category:Chairs"
run sofa           "Category:Couches"
run bed            "Category:Beds"
run book           "Category:Books"
run scissors       "Category:Scissors"
run watch          "Category:Wristwatches"
run glasses        "Category:Glasses"
run spoon          "Category:Eating spoons"
run fork           "Category:Eating forks"
