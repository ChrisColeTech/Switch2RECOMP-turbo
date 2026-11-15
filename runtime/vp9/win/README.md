# libvpx for Windows

`libvpx.a` is libvpx 1.14.0 built from Ubuntu source package `libvpx_1.14.0-1ubuntu2.3`. It is a static
x86_64-w64-mingw32 library containing the VP9 decoder only. Ubuntu's complete patch series was applied, including
its fixes for CVE-2024-5197, CVE-2025-5283, and CVE-2026-2447.

## Provenance

The source package was downloaded over HTTPS from Ubuntu's primary archive:

`https://archive.ubuntu.com/ubuntu/pool/main/libv/libvpx/`

| File | SHA-256 |
| --- | --- |
| `libvpx_1.14.0-1ubuntu2.3.dsc` | `284e824d29a73ce6a19b3533ecbde9576bf9325add0b21f2764cb1b29f5aa167` |
| `libvpx_1.14.0.orig.tar.xz` | `11150687870e4c94479abafe5d1ed38790e68dc7bbe08880fd5fb55138e4c6f3` |
| `libvpx_1.14.0-1ubuntu2.3.debian.tar.xz` | `9605b124232fe21980e2f1a4e7d86b2cedd0f72ac7961a02171eb90a7d762c34` |

The `.dsc` is clearsigned by Ubuntu and records the same SHA-256 hashes for both source payloads. Its quilt series
contains `0001-Relax-ABI-check.patch`, `0002-Do-not-undefine-_FORTIFY_SOURCE.patch`, the four CVE-2024-5197
patches, `CVE-2025-5283.patch`, and `CVE-2026-2447.patch`. No prebuilt libvpx binary was used.

## Reproducing the build

The checked-in archive was built on Windows with:

- GCC 16.2.0 (`conda-forge gcc 16.2.0-7`) and GNU Binutils 2.46.1 from `gcc/Library/bin`
- GNU Make 4.4.1 from the signed MSYS2 package `make-4.4.1-2-x86_64.pkg.tar.zst` (SHA-256
  `2408af61717dae87b00c855b132769a125c708907fc94a46bb16dae076113e5c`)
- NASM 2.16.03 from `https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip`
  (SHA-256 `3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6`)
- Git for Windows Bash 5.2.37 and Perl 5.38.2

Starting in an empty PowerShell working directory, download the three source files named above, the MSYS2 Make
package, and the NASM zip, verify them with `Get-FileHash -Algorithm SHA256`, then run:

```powershell
curl.exe -fLO https://archive.ubuntu.com/ubuntu/pool/main/libv/libvpx/libvpx_1.14.0-1ubuntu2.3.dsc
curl.exe -fLO https://archive.ubuntu.com/ubuntu/pool/main/libv/libvpx/libvpx_1.14.0.orig.tar.xz
curl.exe -fLO https://archive.ubuntu.com/ubuntu/pool/main/libv/libvpx/libvpx_1.14.0-1ubuntu2.3.debian.tar.xz
curl.exe -fLO https://repo.msys2.org/msys/x86_64/make-4.4.1-2-x86_64.pkg.tar.zst
curl.exe -fLO https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip
Get-FileHash -Algorithm SHA256 libvpx_1.14.0-1ubuntu2.3.dsc, `
  libvpx_1.14.0.orig.tar.xz, libvpx_1.14.0-1ubuntu2.3.debian.tar.xz, `
  make-4.4.1-2-x86_64.pkg.tar.zst, nasm-2.16.03-win64.zip
& "D:\Projects\Switch2RECOMP-main\gcc\Library\bin\zstd.exe" -d -f `
  "make-4.4.1-2-x86_64.pkg.tar.zst" -o "make-4.4.1-2-x86_64.pkg.tar"
New-Item -ItemType Directory -Force -Path "tools\make", "tools\nasm" | Out-Null
& "C:\Program Files\Git\usr\bin\tar.exe" -xf "make-4.4.1-2-x86_64.pkg.tar" -C "tools\make"
Expand-Archive -LiteralPath "nasm-2.16.03-win64.zip" -DestinationPath "tools\nasm" -Force
& "C:\Program Files\Git\bin\bash.exe" -lc @'
set -euo pipefail
tar -xf libvpx_1.14.0.orig.tar.xz
tar -xf libvpx_1.14.0-1ubuntu2.3.debian.tar.xz -C libvpx-1.14.0
cd libvpx-1.14.0
while IFS= read -r patch; do git apply "debian/patches/$patch"; done < debian/patches/series
cd ..
export PATH="/d/Projects/Switch2RECOMP-main/gcc/Library/bin:$PWD/tools/nasm/nasm-2.16.03:$PWD/tools/make/usr/bin:$PATH"
mkdir build-win
cd build-win
CROSS=x86_64-w64-mingw32- ../libvpx-1.14.0/configure \
  --target=x86_64-win64-gcc --enable-static --disable-shared --disable-vp8 \
  --disable-vp9-encoder --enable-vp9-decoder --disable-examples --disable-tools \
  --disable-docs --disable-unit-tests --disable-multithread --as=nasm
make -j8
'@
Copy-Item -LiteralPath "build-win\libvpx.a" `
  -Destination "D:\Projects\Switch2RECOMP-main\runtime\vp9\win\libvpx.a"
```

The resulting archive is 1,010,710 bytes with SHA-256
`441569ee34a9731973b0ea670faa4a2133daedb3905ce81bd42d10c9820a5066`.

## Verification

Run these commands from the repository root:

```powershell
Get-FileHash -Algorithm SHA256 runtime\vp9\win\libvpx.a
gcc\Library\bin\x86_64-w64-mingw32-objdump.exe -f runtime\vp9\win\libvpx.a
gcc\Library\bin\x86_64-w64-mingw32-nm.exe -g --defined-only runtime\vp9\win\libvpx.a
```

Every archive member reports `file format pe-x86-64`. The global symbol table defines `vpx_codec_vp9_dx`,
`vpx_codec_dec_init_ver`, `vpx_codec_decode`, `vpx_codec_get_frame`, and `vpx_codec_destroy`, and does not define
the VP9 encoder entry point `vpx_codec_vp9_cx`.

The repository runtime was linked successfully with:

```powershell
python\python.exe switchrecomp.py --build-runtime --target win64 --backend win32 `
  --cc gcc\Library\bin\x86_64-w64-mingw32-gcc.exe
```

This produced `bin/runtime_win64_win32.exe` as `pei-x86-64`. Its symbol/string tables contain
`vpx_codec_vp9_dx`, `vpx_codec_decode`, `vp9_decode_frame`, `WebM Project VP9 Decoder v1.14.0`, and the runtime's
libvpx error text, confirming that `SWR_HAVE_VP9` was defined and the static decoder was linked.

libvpx is BSD-3-Clause with an additional patent grant; see the unmodified `../LICENSE-libvpx.txt` and
`../PATENTS-libvpx.txt`. The headers in `../include/vpx/` match this patched source. The decoder is intentionally
single-threaded because this mingw-w64 toolchain uses the Win32 threading model while libvpx multithreading requires
pthreads for this target.
