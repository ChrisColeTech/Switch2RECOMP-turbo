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
