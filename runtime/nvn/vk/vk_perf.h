// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
// Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
#pragma once
#include <stdint.h>
enum { VKPERF_PRESENT, VKPERF_REPLAY };
typedef struct {
    uint64_t time, frame, interval;
    uint64_t cpu, acquire, fence, queue, enqueue, sparse, wait, texture, pipeline, pool, gpu, writeback, astc, cache, wb_bytes, dropped;
} vkperf_row;
extern int vkperf_enabled;
void vkperf_init(void);
void vkperf_push(int kind, const vkperf_row *row);
// Graphics work dropped or degraded on an error path, counted always (one relaxed add, only on those paths) and written per
// second into the metrics row (SWR_METRICS, kernel.c), so a black region can be traced to what was dropped in that second
#include <stdatomic.h>
enum { VKC_TEX_NEW, VKC_TEX_NOFMT, VKC_TEX_CREATE_FAIL, VKC_TEX_NOMEM, VKC_TEX_UNADDRESSABLE, VKC_TEX_UNREADABLE, VKC_VIEW_FAIL,
       VKC_SHADER_UNUSABLE, VKC_DRAW_PRIM, VKC_DRAW_NO_MESH, VKC_DRAW_RENDER_SKIP, VKC_IND_INST0, VKC_APPEND_RESET, VKC_SBUF_SIZE0, VKC_GMEM_LEARN, VKC_TF_CAPTURE, VKC_N };
extern _Atomic uint64_t vkc_count[VKC_N];
#define VKC(k) atomic_fetch_add_explicit(&vkc_count[k], 1, memory_order_relaxed)
