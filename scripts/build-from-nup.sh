#!/bin/sh
set -eu
if [ "$#" -lt 1 ]; then
  echo 'Usage: scripts/build-from-nup.sh GAME.nup [switchrecomp options]' >&2
  echo 'Example: scripts/build-from-nup.sh /games/Game.nup --outdir /games/Game-build' >&2
  exit 2
fi
[ -f "$1" ] || { echo "[NUP BUILD] FAILED - file not found: $1" >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python=${SWR_PYTHON:-python3}
if [ -n "${SWR_CC:-}" ]; then cc=$SWR_CC
elif [ -x "$root/gcc/bin/x86_64-conda-linux-gnu-gcc" ]; then cc=$root/gcc/bin/x86_64-conda-linux-gnu-gcc
else cc=gcc
fi
package=$1
shift
"$python" "$root/switchrecomp.py" "$package" --target linux --backend x11 --cc "$cc" "$@"
printf '%s\n' "[NUP BUILD] SUCCESS - RomFS remains in $package"
