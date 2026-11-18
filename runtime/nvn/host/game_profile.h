/* The keys of nxnative's game profile the graphics layer reads, with MKW's values (dm/games/0400C3F00006E000/game.tsv).
   ROADMAP G4 turns this into per-game data. */
#pragma once
#define DM_GAME_ID "0400C3F00006E000"
#define DM_GAME_NAME "MKW"
#define DM_GAME_APP_POOL 0x240000000ull
#define DM_GAME_RENDER_COND 1
/* tex_flush_upload / tex_dirty_upload (nxnative's profile keys): the CPU writes a texture's storage after creating it for GPU use, and the texture is read again at the
   flush that covers it (and, dirty: when its pages were written). On for the titles nxnative's profiles turn it on for (nvn_host.c dm_game_tex_rewrite): Ocarina of Time
   and Tears of the Kingdom write their terrain's height, blend and hole maps this way. NXRT_TEX_REWRITE=0/1 overrides */
int dm_game_tex_rewrite(void);
#define DM_GAME_TEX_FLUSH_UPLOAD dm_game_tex_rewrite()
#define DM_GAME_TEX_DIRTY_UPLOAD dm_game_tex_rewrite()
