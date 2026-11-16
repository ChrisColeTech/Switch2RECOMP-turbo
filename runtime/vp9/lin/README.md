# libvpx for Linux

`libvpx.a` is libvpx 1.14.0 built from Ubuntu source package `libvpx_1.14.0-1ubuntu2.3`. It is a static,
position-independent x86_64 ELF library containing the VP9 decoder only. Ubuntu's complete patch series was applied,
including its fixes for CVE-2024-5197, CVE-2025-5283, and CVE-2026-2447.

## Provenance

The source package was downloaded over HTTPS from Ubuntu's primary archive:

`https://archive.ubuntu.com/ubuntu/pool/main/libv/libvpx/`

| File | SHA-256 |
| --- | --- |
| `libvpx_1.14.0-1ubuntu2.3.dsc` | `284e824d29a73ce6a19b3533ecbde9576bf9325add0b21f2764cb1b29f5aa167` |
| `libvpx_1.14.0.orig.tar.xz` | `11150687870e4c94479abafe5d1ed38790e68dc7bbe08880fd5fb55138e4c6f3` |
| `libvpx_1.14.0-1ubuntu2.3.debian.tar.xz` | `9605b124232fe21980e2f1a4e7d86b2cedd0f72ac7961a02171eb90a7d762c34` |

`dpkg-source` applied the quilt series `0001-Relax-ABI-check.patch`, `0002-Do-not-undefine-_FORTIFY_SOURCE.patch`,
`CVE-2024-5197-pre1.patch`, `CVE-2024-5197-1.patch`, `CVE-2024-5197-2.patch`, `CVE-2024-5197-3.patch`,
`CVE-2025-5283.patch`, and `CVE-2026-2447.patch`. No prebuilt libvpx binary was used.

## Reproducing the build

The checked-in archive was built on Ubuntu 24.04 (x86_64) with:

- GCC 13.3.0 (`gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`, the Ubuntu 24.04 default) and GNU Binutils 2.42
  (`2.42-4ubuntu2.8`)
- NASM 2.16.01 (Ubuntu `nasm`), GNU Make 4.3, dpkg-source 1.22.6 (`dpkg-dev`)

Starting in an empty working directory:

```sh
for f in libvpx_1.14.0-1ubuntu2.3.dsc libvpx_1.14.0.orig.tar.xz libvpx_1.14.0-1ubuntu2.3.debian.tar.xz; do
  curl -fLO "https://archive.ubuntu.com/ubuntu/pool/main/libv/libvpx/$f"
done
sha256sum libvpx_1.14.0-1ubuntu2.3.dsc libvpx_1.14.0.orig.tar.xz libvpx_1.14.0-1ubuntu2.3.debian.tar.xz
dpkg-source --no-check -x libvpx_1.14.0-1ubuntu2.3.dsc libvpx-source
mkdir libvpx-build && cd libvpx-build
../libvpx-source/configure --target=x86_64-linux-gcc --enable-static --disable-shared --enable-pic \
  --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder --disable-examples --disable-tools \
  --disable-docs --disable-unit-tests --enable-multithread --as=nasm
make -j8
cp libvpx.a /path/to/Switch2RECOMP/runtime/vp9/lin/libvpx.a
```

Two builds from fresh directories produced identical bytes; the archive records no build paths. The result is
979,332 bytes with SHA-256 `2a2fa82e5141041dc6b18a5e7ce763707e23fde91cc34552e47ac0fbc2b2e545`.

## Verification

```sh
sha256sum runtime/vp9/lin/libvpx.a
objdump -f runtime/vp9/lin/libvpx.a
nm -g --defined-only runtime/vp9/lin/libvpx.a
```

Every archive member reports `file format elf64-x86-64`. The global symbol table defines `vpx_codec_vp9_dx`,
`vpx_codec_dec_init_ver`, `vpx_codec_decode`, `vpx_codec_get_frame`, and `vpx_codec_destroy`, and does not define
the VP9 encoder entry point `vpx_codec_vp9_cx`. `switchrecomp.py --build-runtime --target linux --backend x11`
links it and the runtime then contains `WebM Project VP9 Decoder v1.14.0`.

libvpx is BSD-3-Clause with an additional patent grant; see the unmodified `../LICENSE-libvpx.txt` and
`../PATENTS-libvpx.txt`. The headers in `../include/vpx/` match this patched source.
