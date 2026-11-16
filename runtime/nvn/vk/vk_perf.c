// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
// Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
#include "vk_internal.h"
#include "vk_perf.h"
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// one producer per ring: present and serialized replay. Never block a game thread
// on the disk. A slow writer loses telemetry, not frames; losses are explicit.
enum { PERF_RING = 4096, PERF_LIMIT = 1000000 };
static struct { vkperf_row row[PERF_RING]; _Atomic unsigned in, out; _Atomic uint64_t lost; } g_ring[2];
static FILE *g_file;
static uint64_t g_clock, g_epoch;
int vkperf_enabled;

void vkperf_push(int kind, const vkperf_row *row) {
    unsigned in = atomic_load(&g_ring[kind].in), out = atomic_load(&g_ring[kind].out);
    if (in - out == PERF_RING || in >= PERF_LIMIT) { atomic_fetch_add(&g_ring[kind].lost, 1); return; }
    g_ring[kind].row[in % PERF_RING] = *row;
    atomic_store(&g_ring[kind].in, in + 1);
}
static unsigned __stdcall writer(void *arg) {
    (void)arg; SetThreadDescription(GetCurrentThread(), L"vk frame telemetry");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    uint64_t lost = 0;
    for (;;) {
        for (int k = 0; k < 2; k++) {
            unsigned out = atomic_load(&g_ring[k].out), in = atomic_load(&g_ring[k].in);
            while (out != in) {
                vkperf_row r = g_ring[k].row[out++ % PERF_RING];
                atomic_store(&g_ring[k].out, out);
                fprintf(g_file, "%s,%llu,%llu,%llu", k ? "replay" : "present", (unsigned long long)(g_epoch + r.time - g_clock), (unsigned long long)r.frame, (unsigned long long)r.interval);
                const uint64_t v[] = {r.cpu,r.acquire,r.fence,r.queue,r.enqueue,r.sparse,r.wait,r.texture,r.pipeline,r.pool,r.gpu,r.writeback,r.astc,r.cache,r.wb_bytes,r.dropped};
                for (unsigned i = 0; i < sizeof v / sizeof *v; i++) fprintf(g_file, ",%llu", (unsigned long long)v[i]);
                fputc('\n', g_file);
            }
        }
        uint64_t n = atomic_load(&g_ring[0].lost) + atomic_load(&g_ring[1].lost);
        if (n != lost) { VKLOG("frame telemetry: %llu records lost (ring full or million-row limit)", (unsigned long long)n); lost = n; }
        // bounded loss on ExitProcess (the window close path), no frame-thread flush
        if (fflush(g_file) || ferror(g_file)) { VKRED("frame telemetry write failed"); return 0; }
        Sleep(250);
    }
}
void vkperf_init(void) {
    const char *p = getenv("SWR_FRAME_CSV"); if (!p || !*p) return;
    // do not overwrite earlier evidence
    g_file = fopen(p, "wx"); if (!g_file) { VKRED("cannot create frame telemetry %s", p); return; }
    setvbuf(g_file, NULL, _IOFBF, 65536);
    struct timespec ts; timespec_get(&ts, TIME_UTC); g_clock = now_ns(); g_epoch = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    fputs("kind,unix_ns,frame,interval_ns,cpu_ns,acquire_ns,fence_ns,queue_ns,enqueue_ns,sparse_ns,wait_ns,texture_ns,pipeline_ns,pool_ns,gpu_ns,writeback_ns,astc_ns,cache_ns,writeback_bytes,dropped\n", g_file);
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, writer, NULL, 0, NULL);
    if (!h) { fclose(g_file); g_file = NULL; return; }
    CloseHandle(h); vkperf_enabled = 1;
    VKLOG("frame telemetry: %s; bounded rings, background buffered writer; present call completion, not scanout", p);
}
