#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$#" -lt 1 ]; then
  echo "usage: scripts/build-from-nsp.sh GAME.nsp [switchrecomp options]" >&2
  echo "set SWR_PROD_KEYS to your prod.keys path" >&2
  exit 2
fi
test -f "$1" || { echo "[NSP BUILD] FAILED - file not found: $1" >&2; exit 1; }
python3 "$root/switchrecomp.py" "$@" --target linux --backend x11
echo "[NSP BUILD] SUCCESS - RomFS remains in $1"
