# Building And Running An NSP In Place

Switch2RECOMP accepts standard Switch 2 application NSPs whose Program NCA contains a plaintext PartitionFS ExeFS
and plaintext IVFC RomFS. ExeFS is copied into the native build; the large RomFS remains in the original NSP.

## Keys

Use only keys dumped from your own console. In the Qt launcher choose **Keys > Install keys** and select `prod.keys`.
The launcher stores a private copy in its application-data folder and passes its path to the build. It does not put
keys or decrypted content keys in the toolkit, game build, logs, or releases.

Command-line builds can set `SWR_PROD_KEYS`:

```bat
set "SWR_PROD_KEYS=D:\Keys\prod.keys"
scripts\build-from-nsp.bat "D:\Games\Game.nsp" --outdir "D:\Games\Game-build" -o Game
```

## Runtime layout

The build writes ordinary ExeFS files and a small `Game_data/romfs.nsp` text marker. The marker records the original
package path and the validated plaintext RomFS byte range. At launch, the runtime opens that range directly. Moving
or disconnecting the NSP requires rebuilding the marker or restoring the same path.

No whole NCA or RomFS is extracted. The original NSP is read-only and is never modified.

## Supported packages

The first implementation deliberately supports the package layout verified with Mario Kart World 1.8.0:

- top-level PFS0 NSP;
- one NCA3 Program content archive;
- plaintext PartitionFS ExeFS;
- plaintext IVFC RomFS;
- no rights-managed section data, sparse storage, compression, or BKTR patch section.

The NCA header remains encrypted and is inspected with checksum-pinned hactool 1.4.0 using the installed keys.
Packages with encrypted filesystem sections or BKTR update-only data fail before code analysis with an explicit
message. An update-only NSP needs a matching base NSP and virtual BKTR composition; it is not silently treated as a
standalone game.

## Verification

`python tests/test_nsp.py` covers bounded PFS0/NCA metadata, ExeFS, RomFS traversal, and BKTR rejection using synthetic
data. `sh tests/run_nsp.sh` checks the native runtime marker/range reader under sanitizers.

The Mario Kart World 1.8.0 validation build analyzed 244,016 functions and 13,924,869 instructions, compiled 460 C
units, staged seven ExeFS files, and read a 22.4 GB RomFS directly from the original NSP. Native Vulkan ran past 2,100
presented frames and reached about 60 FPS after disabling an obsolete system validation layer. Set
`NXRT_VK_VALIDATION=1` only for diagnostics with a current Vulkan SDK.
