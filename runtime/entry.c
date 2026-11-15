/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* executable entry point */
int swr_main(int argc, char** argv);
#if defined(_WIN32)
#include <windows.h>
#include <string.h>
int shp_main(void);

/* Hybrid systems otherwise put this unknown executable on the integrated GPU. These exports are the documented
   NVIDIA/AMD application hints and do nothing on systems without switchable graphics. */
__declspec(dllexport) DWORD NvOptimusEnablement=1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance=1;

/* --shader-helper: this copy only compiles shaders for the game (shader_proc.c) */
int main(int argc, char** argv){ if(argc>1 && !strcmp(argv[1],"--shader-helper")) return shp_main(); return swr_main(argc,argv); }
#else
int main(int argc, char** argv){ return swr_main(argc,argv); }
#endif
