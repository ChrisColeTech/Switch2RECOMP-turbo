# Copyright 2026 ChrisColeTech
# SPDX-License-Identifier: MIT
# source after setting root; do not install tools or change PATH here.
if [ -n "${SWR_PYTHON:-}" ]; then
  "$SWR_PYTHON" -c 'import sys; sys.exit(sys.version_info < (3, 8))' || return 1
else
  for candidate in "$root/.venv/bin/python3" "$root/venv/bin/python3" "$root/python/bin/python3" python3; do
    if "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
      SWR_PYTHON=$candidate
      break
    fi
  done
fi
[ -n "${SWR_PYTHON:-}" ] || { echo 'Python missing: provide a venv, bundled Python, or SWR_PYTHON.' >&2; return 1; }
export SWR_PYTHON
python=$SWR_PYTHON
