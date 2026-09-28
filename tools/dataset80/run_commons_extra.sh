#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/../.."
PY=/home/oooa/msdl/bin/python
F=tools/dataset80/commons_fetch.py
N=${1:-600}
run() { echo "### $1 <- $2"; timeout 1200 $PY $F "$1" "$2" "$N"; }
run narcissus "Category:Narcissus (plant)"
