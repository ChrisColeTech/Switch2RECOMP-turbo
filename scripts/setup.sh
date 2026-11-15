#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$root/Install.sh"
printf '%s\n' '[SETUP] SUCCESS - build dependencies are ready'
