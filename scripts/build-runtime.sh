#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$root/scripts/_env.sh"
if [ -n "${SWR_CC:-}" ]; then cc=$SWR_CC
elif [ -x "$root/gcc/bin/x86_64-conda-linux-gnu-gcc" ]; then cc=$root/gcc/bin/x86_64-conda-linux-gnu-gcc
else cc=gcc
fi
"$python" "$root/switchrecomp.py" --build-runtime --target linux --backend x11 --cc "$cc" "$@"
printf '%s\n' '[RUNTIME] SUCCESS - bin/runtime_linux_x11'
