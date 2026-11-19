Switch2Recomp 0.4.1
See CHANGELOG.txt for how to install and the music settings.

SwitchRecomp - static recompiler for Nintendo Switch games (ARM64 -> C -> native)
================================================================================

Same idea as GBRecomp / PSXRecomp: the game CODE (exefs) is translated to C once
and compiled to a native Windows program. The game DATA (romfs: models, textures,
music) is not compiled: the game reads it from GAME_data\romfs or directly from
the original NUP package.

Use only your own copy of the game. No games, keys or firmware are included.

License: GNU AGPL v3 (LICENSE.txt) with the additional terms in NOTICE.txt.
Free to use, modify and share. Any copy, fork or derived work has to stay
under the same license, publish its source, credit "SwitchRecomp by MZ Labs",
keep NOTICE.txt and keep the SwitchRECOMP startup screen (logo and copyright
notice) that the recompiler and every recompiled game show when they start.

--------------------------------------------------------------------------------
1) RECOMPILE A GAME
--------------------------------------------------------------------------------
  First double-click Install.bat once (needs internet, no admin rights): it downloads Python
  (python\) and GCC (gcc\) into this folder. TCC comes in the package. If you forget, the
  recompiler opens Install.bat by itself the first time you recompile.

  a) Dump the game from your own console and extract it with hactool and your
     own keys:
        hactool -k prod.keys --exefsdir=exefs --romfsdir=romfs  game.nca
     With an update, extract the combined romfs:
        hactool -k prod.keys --basenca=base.nca --exefsdir=exefs --romfsdir=romfs  update.nca
     You end up with a folder that has exefs\ and romfs\ inside.

     Switch 2 indexed NUP and application NSP dumps can instead be used directly,
     also an NSP that holds the game together with its update (1G+1U) and its DLC
     (1G+1U+1D). RomFS stays in the package; see docs\NUP_IN_PLACE.md,
     docs\NSP_IN_PLACE.md and docs\COMBINED_NSP.md. Install prod.keys from the Qt
     launcher's Keys menu.
        scripts\build-from-nup.bat "D:\Games\Game.nup" --outdir "D:\Games\Game-build"

  b) Open SwitchRecomp.exe. It looks like the Switch HOME menu and works with
     the mouse (also keyboard: arrows + Enter / Esc, or a controller).
     - The first tile, "Recompile a game": pick the game folder (the one with
       exefs\ and romfs\). The game is written to the games\ folder and shows
       up in the row with its icon.
     - Click a game to start it. Right click (or the "-/+ Options" hint) opens
       its options: start, update its runtime, recompile its code, open its
       folder, its icon (pick any picture, or go back to the game's own), and
       the game settings (TV mode, resolution, shadows...).
     - Games are found in the games folder and its subfolders (one folder per
       game is fine). Settings > Games folder changes where that is.
     - Games without an icon get the one from their _data folder or from the
       dump (icon_AmericanEnglish.dat in control\) automatically.
     - Round buttons:  Update all games  puts the runtime of this package in
                                         every game already made (only the
                                         .exe changes; saves, options and the
                                         game code stay). A number on it says
                                         how many games have an older runtime.
                       Games folder      Settings (compiler, theme)   Close
     - Compiler (Settings):  GCC  takes longer to compile, the game runs faster
                             TCC  compiles in seconds, the game runs slower
       GCC comes from Install.bat (gcc\ folder). Without it TCC is used.

  Command line:
     python\python.exe switchrecomp.py C:\path\to\game -o MyGame
     scripts\build-from-nup.bat C:\path\to\game.nup --outdir C:\Games\MyGame-build -o MyGame
     scripts\build-from-nsp.bat C:\path\to\game.nsp --outdir C:\Games\MyGame-build -o MyGame
     python\python.exe switchrecomp.py homebrew.nro
     Options: --native (optimize for your CPU)  -O 3  --jobs N  --cc tcc

  A big game takes several minutes with GCC the first time. Only once: if the
  code did not change, the next run reuses what was already compiled.

  Output:
     MyGame.exe          the runtime (kernel, services, GPU -> OpenGL, video)
     MyGame_code.dll     the game code translated to x86-64
     MyGame_data\        exefs\ + romfs\ or romfs.nup + save data
     MyGame.ini          options

  The exe and the dll must come from the same game version. If you update the
  game, recompile.

--------------------------------------------------------------------------------
2) PLAY
--------------------------------------------------------------------------------
  Requirements: Windows 10/11 64-bit, GPU with OpenGL 4.3 or newer (keep the
  driver up to date). Linux and the Steam Deck work through Proton.

  Controls
    Xbox / XInput controller: same as the Switch one, by position.
    PlayStation (DualSense / DualShock 4, native) and other Windows game
      controllers: no controller-emulation software required. Buttons are
      mapped by position too (Circle = A, Cross = B, Triangle = X, Square = Y).
    button_layout=xbox in the .ini maps by Xbox label instead (bottom = A,
      right = B, left = X, top = Y: Cross = A, Circle = B on PlayStation).
    Keyboard: Arrows = D-Pad, WASD = left stick, IJKL = right stick,
              X/Space = A, Z = B, C = X, V = Y, Q = L, E = R, 1 = ZL, 3 = ZR,
              Enter = +, Backspace = -
    F1 help   F3 screenshot   F4 FPS limit 60 / unlimited   F5 diagnostic trace
    F7 record .mp4 video   F8 full resolution PNG   F11 or Alt+Enter fullscreen
    Esc quit

  Options (MyGame.ini, rewritten on every start with all the available keys):
    docked=1               TV mode (1080p) instead of handheld (720p)
    resolution=1           internal resolution scale (0.25 .. 4)
    fps_limit=1            1 = 60 FPS like the console, 0 = unlimited
    shadows=1              0 skips shadow maps (faster on weak GPUs)
    lsfg=1                 frame generation with YOUR OWN copy of Lossless
                           Scaling (its Lossless.dll is read at run time, it is
                           never included or copied)
    log=2                  log everything (for bug reports)
    renderer=opengl        opengl (default), vulkan, or eden (Eden's Vulkan GPU)
    button_layout=switch   controller face buttons: switch = by position like
                           the Switch, xbox = by Xbox label (Cross = A)
    button_map=            optional per-button overrides, physical:guest pairs,
                           e.g. south:a east:b (see the comments in the .ini)

  renderer=vulkan translates the game's shaders when the game is built
  (switchrecomp.py --no-shaders skips that), and any shader the build did
  not find while it runs. It needs Python 3 (Install.bat downloads it) and
  the .NET 10 runtime (https://dotnet.microsoft.com/download/dotnet/10.0).
  A shader translated while the game runs appears about a second and a
  half after its first use; after that it comes from <game>_nvn\cache.
  On Linux it also needs, once:
      sudo sysctl vm.mmap_min_addr=0

  If something goes wrong: send MyGame.log (next to the exe). The F5 trace
  goes to the captures\ folder. performance.txt summarizes slow frames.

--------------------------------------------------------------------------------
Thanks
--------------------------------------------------------------------------------
  Includes fixes from the SwitchRecomp fork by Mythrax-ZS (contributed under
  Apache 2.0, those parts keep that notice): GPU
  memory mapping and nvdrv limits, atomic loads/stores with real acquire/release
  ordering, IPC and file system bounds checks, save data types and SIMD
  rounding fixes.

--------------------------------------------------------------------------------
Third-party components
--------------------------------------------------------------------------------
  OpenH264 (Cisco, BSD 2-clause)        game videos   runtime\oh264\LICENSE
  Opus (Xiph.Org, BSD 3-clause)         game audio    runtime\opus\LICENSE
  Hind, Barlow Semi Condensed (OFL)    tool text     tool\fonts\OFL-*.txt
  SDL2, SDL2_ttf, SDL2_image (zlib)    tool          (linked into SwitchRecomp.exe,
    with FreeType, libpng, libjpeg-turbo and zlib under their own licenses)
  SPIRV-Cross (Khronos, Apache 2.0)     LSFG shaders  runtime\spvc\LICENSE-SPIRV-Cross.txt
  texture2ddecoder (MIT)                BCn textures  runtime\bcn\LICENSE_texture2ddecoder.txt
  TCC (LGPL)                            tcc\COPYING.txt
  Python (PSF license)                  downloaded by Install.bat, python\LICENSE.txt
  GCC / MinGW-w64 (GPL with the GCC runtime library exception), downloaded from conda-forge
    by Install.bat (tool\get_deps.py)

  The AGPL v3 in LICENSE.txt covers the SwitchRecomp code. Each component
  above keeps its own license.
