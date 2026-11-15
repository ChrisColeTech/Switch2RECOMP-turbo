#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
testbin=$(mktemp -d)
trap 'rm -rf "$testbin"' EXIT
python3 - "$testbin/game.nup" <<'PY'
import sys
sys.path.insert(0, 'tests')
from test_nup import minimal_nso, write_nup
write_nup(sys.argv[1], {'Data/raw.bin': b'raw data', 'Data/packed.bin': bytes(range(256)) * 1024},
          {'main': minimal_nso(), 'main.npdm': b'not-meta'}, compressed={'Data/packed.bin'},
          trailing_auxiliary=True)
PY
zstd=runtime/edenvk/deps/zstd/lib
cc -O1 -g -fsanitize=address,undefined -Iruntime -I"$zstd" -DZSTD_DISABLE_ASM=1 -DXXH_NAMESPACE=ZSTD_ \
  tests/test_nup_runtime.c runtime/nup.c \
  "$zstd/common/entropy_common.c" "$zstd/common/error_private.c" "$zstd/common/fse_decompress.c" \
  "$zstd/common/xxhash.c" "$zstd/common/zstd_common.c" "$zstd/decompress/huf_decompress.c" \
  "$zstd/decompress/zstd_ddict.c" "$zstd/decompress/zstd_decompress.c" "$zstd/decompress/zstd_decompress_block.c" \
  -o "$testbin/nup"
ASAN_OPTIONS=detect_leaks=1 "$testbin/nup" "$testbin/game.nup"
