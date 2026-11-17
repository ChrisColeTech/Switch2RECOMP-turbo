// SwitchRecomp - Copyright 2026 MZ Labs
// Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
// Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen.
#include "vk_internal.h"
#include "vk_perf.h"
#include "vk_replay.h"
#include "nxs_rt.h"
#include "nvn_texio.h"
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
_Atomic uint64_t vkc_count[VKC_N];
static const char *const vkc_name[VKC_N] = {"tex_new", "tex_no_format", "tex_create_failed", "tex_no_memory", "tex_unaddressable", "tex_unreadable",
    "view_failed", "shader_unusable", "draw_prim_unsupported", "draw_no_mesh_shader", "draw_render_skip", "ind_draw_no_instances", "append_counter_reset", "sbuf_size0"};
// the graphics columns of the metrics row (kernel.c metrics_tick): per second, except tex_order
uint64_t vkm_wb_cpu_newer(void);
static void metrics_gfx(FILE *f, int header) {
    static vkr_stats_t ls; static nxs_counts ln; static uint64_t lc[VKC_N], lwn;
    if (header == 2) { vkr_stats(&ls); nxs_counts_get(&ln); for (int i = 0; i < VKC_N; i++) lc[i] = atomic_load(&vkc_count[i]); lwn = vkm_wb_cpu_newer(); return; }
    if (header) {
        fputs(",draws,draws_skipped,skip_no_shader,skip_no_pipeline,skip_no_target,skip_state,dispatches,shader_missing,shader_arrived,shader_failed,shader_stale", f);
        for (int i = 0; i < VKC_N; i++) fprintf(f, ",%s", vkc_name[i]);
        fputs(",wb_cpu_newer,tex_order", f); return;
    }
    vkr_stats_t s; vkr_stats(&s); nxs_counts n; nxs_counts_get(&n);
    fprintf(f, ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu", (unsigned long long)(s.draws - ls.draws), (unsigned long long)(s.draws_skipped - ls.draws_skipped),
            (unsigned long long)(s.skip_no_shader - ls.skip_no_shader), (unsigned long long)(s.skip_no_pipeline - ls.skip_no_pipeline), (unsigned long long)(s.skip_no_target - ls.skip_no_target),
            (unsigned long long)(s.skip_state - ls.skip_state), (unsigned long long)(s.dispatches - ls.dispatches), (unsigned long long)(n.missing_keys - ln.missing_keys),
            (unsigned long long)(n.arrived - ln.arrived), (unsigned long long)(n.failed - ln.failed), (unsigned long long)(n.stale - ln.stale));
    for (int i = 0; i < VKC_N; i++) { uint64_t c = atomic_load_explicit(&vkc_count[i], memory_order_relaxed); fprintf(f, ",%llu", (unsigned long long)(c - lc[i])); lc[i] = c; }
    { uint64_t wn = vkm_wb_cpu_newer(); fprintf(f, ",%llu", (unsigned long long)(wn - lwn)); lwn = wn; }
    fprintf(f, ",%s", nvn_texio_order() == S2TEX_TX1 ? "tx1" : "switch2");
    ls = s; ln = n;
}
void vkperf_init(void) {
    { extern void (*g_metrics_gfx)(FILE *, int); g_metrics_gfx = metrics_gfx; }
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
