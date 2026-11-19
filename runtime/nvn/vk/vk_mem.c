// GPU memory: pool mirrors, sparse backing, the staging ring, dirty-page uploads, the per-frame budget (vk_mem.h; docs/m5-vulkan.md §3).
// nxnative's Metal backend wraps each pool zero-copy (newBufferWithBytesNoCopy, unified memory); a discrete GPU needs copies, and importing guest memory
// as host memory is what research/01 lesson 10 rules out. Behaviour of pool lookups follows nvn_mtl.m (mtl_pool_add, pool_find); the rest is new.
#include "vk_mem.h"
#include "vk_submit.h"
#include "vk_flight.h"
#include "vk_perf.h"
#include "dmsh_abi.h"   // gfx/: DMSH_GMEM_MAP_*, the layout of the memory pool table
#include "nvn_dirty.h"   // gfx/: the dirty-page tracker the recording layer shares (nxnative's, ported by track A)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

void dm_guest_write_prepare(void *addr, size_t len);   // host/crash.c: unprotect before the host writes guest memory

enum { PAGE = 4096, GRAN = 2u << 20, HOT_STREAK = 3, HOT_COOL_FRAMES = 240, MAXPOOL = 16384, RUN_MAX = 1024, MAXWIN = 32 };
// A pool larger than the device's largest buffer (AMD: 2 GiB; the game's biggest pool is 3.4 GiB) gets several buffers, windows over it. With sparse
// binding and aliasing the windows overlap and share the same memory, so any range up to the overlap fits in one of them; otherwise they are side by side.
typedef struct {
    uintptr_t lo, hi, base;       // the pool [lo, hi) as given; base = lo rounded down to a page
    uint64_t size, npages;        // bytes the buffer covers (from base, whole pages)
    const void *key;
    VkDeviceAddress da[MAXWIN];   // the windows' device addresses, asked for when the memory pool table first lists them (vkm_gmem_table)
    VkBuffer buf[MAXWIN]; uint64_t win_lo[MAXWIN], win_n[MAXWIN]; int nwin;   // window k covers [win_lo, win_lo + win_n) of the pool, from base
    int sparse;
    vk_alloc *gran; uint64_t ngran, bound;   // sparse: one allocation per bound granule, bound into every window that covers it
    vk_alloc mem[MAXWIN];         // not sparse: one allocation per window
    const void *span;             // the dirty tracker's range (registered by the recording layer right after gfx_pool_add)
    uint64_t *up;                 // per page: the tracker's stamp when its device copy was taken (0 = never copied)
    uint32_t *last_dirty; uint8_t *streak, *hot;   // hot pages: written in HOT_STREAK consecutive frames, left writable and copied at the first use of each submission
    uint32_t *hot_at;             // per page: the submission that copied it (a 4 MB pool bound by a thousand draws was 16 GB of copies a frame)
    uint8_t *committed;           // per page: seen committed and readable once (pool_sync asks VirtualQuery only for pages not known yet)
    _Atomic uint8_t *gpu_pending;   // per page: GPU writes recorded whose bytes have not reached guest memory yet (the device copy is the newer one)
    uint32_t **shadow;            // per page written by the GPU: guest memory as the device has it. While a write-back is on its way only the words the CPU changed since go to the device
    uint64_t hot_n, cursor;       // vkm_sync_all's position
    uint64_t up_b, up_never, up_hot;   // bytes uploaded since the last report: all, of pages never copied, of hot pages
    uint32_t nvn_flags;           // the game's NVNmemoryPoolFlags (vkm_pool_flags): CPU_NO_ACCESS = the CPU never reads or writes it
    // Write-back on demand (below): per page, 1 = the device's copy is the page and guest memory is inaccessible; GPU writes seen in a row; the frame before which it stays
    // eager; how often the CPU came for it. lz_seq: the submission (vks_cur_seq) that last skipped a copy back into this pool
    uint8_t *lz, *lz_n, *lz_fail; uint32_t *lz_hold; uint64_t lz_pages; _Atomic uint64_t lz_seq;
} pool_t_;

static pool_t_ *g_pool[MAXPOOL]; static int g_npool;
static _Atomic uint32_t g_pool_gen = 1;   // moves whenever a pool comes or goes (vkm_use's remembered buffers and offsets are then stale)
static SRWLOCK g_lk = SRWLOCK_INIT;
static _Atomic uint32_t g_bind_gen;       // moves when a pool that is not sparse gets its memory (it has a device address from then on: vkm_gmem_table)
static uint64_t g_frame = 1;
static _Atomic uint64_t s_wb_words, s_wb_kept, s_wb_stale, s_wb_merged, s_wb_cpu_newer;   // cpu_newer: GPU words landed over a value the CPU wrote while the GPU work ran (wr_land; metrics column wb_cpu_newer)
uint64_t vkm_wb_cpu_newer(void) { return atomic_load(&s_wb_cpu_newer); }   // metrics (vk_perf.c)
static VkBufferUsageFlags g_usage;
static VkDeviceSize g_gran = GRAN, g_max_buffer;
static int g_alias;   // overlapping windows: sparse binding with aliasing (NXRT_VK_NO_ALIAS=1 turns them off, for tests)

static struct {
    VkBuffer buf; vk_alloc mem; uint8_t *map; VkDeviceSize size, head, tail; int wrapped;
    struct { uint64_t value; int owner; VkDeviceSize begin, end; VkBuffer ovf; vk_alloc ovf_mem; } q[4096]; unsigned qh, qt;   // allocations in order; value 0 until owner commits
    SRWLOCK lk;
} g_st;

static uint64_t g_budget = 64ull << 20, g_budget_used;

// ---------- Helper threads for the large copies and compares ----------
// Mario Kart World's compute passes write six buffers of 0.5 to 4.7 MB every frame, and each such write costs two walks over guest memory: the pages' contents as the
// device has them, copied aside when the write is recorded (on the translation thread, 3,700 pages a frame), and the compare of what the GPU left when it lands (on the
// thread that completes submissions, 14.7 MB a frame against the copy made before the shader ran). The first was 2 of the translation thread's 12 ms a frame, the second
// 3 ms that the game's wait for the frame's fence ended later. Both are per page and the pages are independent, so above PAR_MIN pages they are split among the caller
// and three helper threads. One loop at a time: a caller that finds the helpers busy does its pages alone. NXRT_VK_PAR=0: always alone, as before
enum { PAR_MIN = 96, PAR_CHUNK = 32, PAR_THREADS = 3 };
typedef void (*par_fn)(void *ctx, uint64_t lo, uint64_t hi);
static struct { SRWLOCK lk, one; CONDITION_VARIABLE cv, done; par_fn fn; void *ctx; uint64_t n; _Atomic uint64_t next; int active, started; uint64_t gen; } g_par = {SRWLOCK_INIT, SRWLOCK_INIT};
static void par_work(void) {
    for (;;) { uint64_t i = atomic_fetch_add(&g_par.next, PAR_CHUNK); if (i >= g_par.n) return; uint64_t e = i + PAR_CHUNK; g_par.fn(g_par.ctx, i, e < g_par.n ? e : g_par.n); }
}
static unsigned __stdcall par_main(void *arg) {
    (void)arg; uint64_t seen = 0;
    for (;;) {
        AcquireSRWLockExclusive(&g_par.lk);
        while (g_par.gen == seen) SleepConditionVariableSRW(&g_par.cv, &g_par.lk, INFINITE, 0);
        seen = g_par.gen; g_par.active++;
        ReleaseSRWLockExclusive(&g_par.lk);
        par_work();
        AcquireSRWLockExclusive(&g_par.lk);
        if (!--g_par.active) WakeAllConditionVariable(&g_par.done);
        ReleaseSRWLockExclusive(&g_par.lk);
    }
    return 0;
}
static void par_for(uint64_t n, par_fn fn, void *ctx) {   // fn over [0, n) in pieces; returns when all of it is done
    static int on = -1; if (on < 0) on = vkc_int("par", 1);
    if (n < PAR_MIN || !on || !TryAcquireSRWLockExclusive(&g_par.one)) { fn(ctx, 0, n); return; }
    AcquireSRWLockExclusive(&g_par.lk);
    if (!g_par.started) { g_par.started = 1; InitializeConditionVariable(&g_par.cv); InitializeConditionVariable(&g_par.done);
        for (int i = 0; i < PAR_THREADS; i++) { uintptr_t th = _beginthreadex(NULL, 0, par_main, NULL, 0, NULL); if (th) CloseHandle((HANDLE)th); } }
    g_par.fn = fn; g_par.ctx = ctx; g_par.n = n; atomic_store(&g_par.next, 0);   // next last: a helper still leaving the loop before sees either the old end or all of the new loop
    g_par.gen++;
    WakeAllConditionVariable(&g_par.cv);
    ReleaseSRWLockExclusive(&g_par.lk);
    par_work();
    AcquireSRWLockExclusive(&g_par.lk);
    while (g_par.active) SleepConditionVariableSRW(&g_par.done, &g_par.lk, INFINITE, 0);
    ReleaseSRWLockExclusive(&g_par.lk);
    ReleaseSRWLockExclusive(&g_par.one);
}
_Atomic uint64_t g_t_bind, g_n_bind;   // time in sparse binds, with vk_sparse.inc: they wait for the queue
static int g_seen_on = 1;   // gamectl set use_seen 0: every use looks again (a same-run test of vkm_use)
static _Atomic uint64_t s_up_bytes, s_up_pages, s_up_runs, s_up_ns, s_sync, s_st_bytes, s_st_ovf, s_st_waits, s_wb_bytes, s_uncommitted;

// ---------- Staging ring ----------
static void st_reclaim(void) {   // caller holds g_st.lk
    uint64_t done = atomic_load(&g_vk.tl_done);
    while (g_st.qt != g_st.qh) {
        unsigned i = g_st.qt % 4096;
        if (!g_st.q[i].value || g_st.q[i].value > done) break;
        if (g_st.q[i].ovf) { vkDestroyBuffer(g_vk.dev, g_st.q[i].ovf, NULL); vk_free_mem(&g_st.q[i].ovf_mem); g_st.q[i].ovf = VK_NULL_HANDLE; }
        else { if (g_st.q[i].end < g_st.tail) g_st.wrapped = 0;   // the first allocation made after the wrap is gone: what is in use is one run again, [tail, head). Left set, the ring gave nothing more until it was empty
               g_st.tail = g_st.q[i].end; }
        g_st.qt++;
    }
    if (g_st.qt == g_st.qh) { g_st.head = g_st.tail = 0; g_st.wrapped = 0; }
}
// Called by vk_submit at each commit: that slot's allocations now live until its timeline value
static _Atomic uint32_t g_sub_epoch = 1;   // moves at each commit
int g_vkm_same_epoch;                      // a commit in the middle of an NVN submission (vks_split): the same submission as far as guest memory goes
void vkm_committed(int owner, uint64_t value) {
    if (!g_vkm_same_epoch) atomic_fetch_add(&g_sub_epoch, 1);
    AcquireSRWLockExclusive(&g_st.lk);
    for (unsigned k = g_st.qt; k != g_st.qh; k++) if (!g_st.q[k % 4096].value && g_st.q[k % 4096].owner == owner) g_st.q[k % 4096].value = value;
    ReleaseSRWLockExclusive(&g_st.lk);
}
int vks_owner(void);
void *vkm_staging(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    int owner = vks_owner();
    if (owner < 0) { VKRED("staging asked for outside a submission"); return NULL; }
    if (!align) align = 16;
    n = (n + 15) & ~(VkDeviceSize)15;
    AcquireSRWLockExclusive(&g_st.lk);
    for (int attempt = 0; attempt < 64; attempt++) {
        st_reclaim();
        VkDeviceSize a = (g_st.head + align - 1) / align * align;
        // One entry for a submission's run of allocations: each had its own, and a submission with more than 4095 of them (Ocarina of Time's first scene: a thousand
        // descriptor sets a frame, each with its uniform blocks) filled the list. From there every request took a 32 MB buffer of its own and its entry was written over
        // a live one: 100 to 400 MB of device memory lost a second, 24 GB in a minute and a half, and ring space handed out again while the GPU still read it
        if (g_st.qh != g_st.qt) { unsigned l = (g_st.qh - 1) % 4096;
            if (!g_st.q[l].value && g_st.q[l].owner == owner && !g_st.q[l].ovf && g_st.q[l].end == g_st.head && (g_st.wrapped ? a + n <= g_st.tail : a + n <= g_st.size)) {
                g_st.q[l].end = a + n; g_st.head = a + n;
                ReleaseSRWLockExclusive(&g_st.lk);
                atomic_fetch_add(&s_st_bytes, n);
                *buf = g_st.buf; *off = a; return g_st.map + a;
            } }
        if (g_st.qh - g_st.qt >= 4095) goto wait;
        int ok = 0;
        if (!g_st.wrapped) {   // free space: [head, size) then [0, tail)
            if (a + n <= g_st.size) ok = 1;
            else if (n <= g_st.tail) { a = 0; g_st.wrapped = 1; ok = 1; }
        } else if (a + n <= g_st.tail) ok = 1;
        if (ok) {
            unsigned i = g_st.qh++ % 4096;
            g_st.q[i].value = 0; g_st.q[i].owner = owner; g_st.q[i].begin = a; g_st.q[i].end = a + n; g_st.q[i].ovf = VK_NULL_HANDLE;
            g_st.head = a + n;
            ReleaseSRWLockExclusive(&g_st.lk);
            atomic_fetch_add(&s_st_bytes, n);
            *buf = g_st.buf; *off = a; return g_st.map + a;
        }
        if (n > g_st.size / 2) break;   // too big for the ring: a buffer of its own
    wait:;
        // the ring is full: wait for the oldest committed allocation's submission; when only the recording's own remain, overflow
        uint64_t v = g_st.qt != g_st.qh ? g_st.q[g_st.qt % 4096].value : 0;
        if (!v) break;
        ReleaseSRWLockExclusive(&g_st.lk);
        atomic_fetch_add(&s_st_waits, 1);
        vks_wait(v, UINT64_MAX);
        AcquireSRWLockExclusive(&g_st.lk);
    }
    // Overflow: a host buffer for this submission alone, released with it. One chunk of at least 32 MB serves the rest of the submission: a buffer and an allocation per request
    // was 1,800 allocations a second, and frames of 10 s, while MKW loads
    static struct { VkBuffer b; uint8_t *map; VkDeviceSize used, size; unsigned qi; int owner; } oc;
    if (oc.b && oc.owner == owner && g_st.qt <= oc.qi && oc.qi != g_st.qh && g_st.q[oc.qi % 4096].ovf == oc.b && !g_st.q[oc.qi % 4096].value) {
        VkDeviceSize a2 = (oc.used + align - 1) / align * align;
        if (a2 + n <= oc.size) { oc.used = a2 + n; ReleaseSRWLockExclusive(&g_st.lk); atomic_fetch_add(&s_st_bytes, n); *buf = oc.b; *off = a2; return oc.map + a2; }
    }
    { static uint64_t said; uint64_t tn = GetTickCount64(); if (tn - said > 2000) { said = tn; unsigned i0 = g_st.qt % 4096; uint64_t held = 0; unsigned novf = 0;
        for (unsigned k = g_st.qt; k != g_st.qh; k++) if (g_st.q[k % 4096].ovf) { novf++; held += g_st.q[k % 4096].ovf_mem.size; }
        VKLOG("staging overflow: %llu bytes asked by slot %d; the ring holds %u allocations (%u overflow buffers, %llu MB), head %llu tail %llu%s; the oldest: slot %d value %llu (GPU done %llu, assigned %llu)", (unsigned long long)n, owner, g_st.qh - g_st.qt, novf,
              (unsigned long long)(held >> 20), (unsigned long long)g_st.head, (unsigned long long)g_st.tail, g_st.wrapped ? " wrapped" : "", g_st.qt != g_st.qh ? g_st.q[i0].owner : -1, g_st.qt != g_st.qh ? (unsigned long long)g_st.q[i0].value : 0ull,
              (unsigned long long)atomic_load(&g_vk.tl_done), (unsigned long long)atomic_load(&g_vk.tl_assigned)); } }
    if (g_st.qh - g_st.qt >= 4095) { ReleaseSRWLockExclusive(&g_st.lk); VKRED("staging: the list of allocations is full (%u, all of submissions not yet done): %llu bytes are not given", g_st.qh - g_st.qt, (unsigned long long)n); return NULL; }
    VkDeviceSize chunk = n > (32u << 20) ? n : (32u << 20);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, chunk, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b = VK_NULL_HANDLE; vk_alloc m;
    void *p = NULL;
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &b) == VK_SUCCESS) {
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
        if (!vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m)) {
            vkBindBufferMemory(g_vk.dev, b, m.mem, m.off); p = m.map;
            unsigned i = g_st.qh++ % 4096;
            g_st.q[i].value = 0; g_st.q[i].owner = owner; g_st.q[i].begin = g_st.q[i].end = 0; g_st.q[i].ovf = b; g_st.q[i].ovf_mem = m;
            oc.b = b; oc.map = p; oc.used = n; oc.size = chunk; oc.qi = g_st.qh - 1; oc.owner = owner;
        } else { vkDestroyBuffer(g_vk.dev, b, NULL); b = VK_NULL_HANDLE; }
    }
    ReleaseSRWLockExclusive(&g_st.lk);
    if (!p) { VKRED("staging: no host memory for %llu bytes", (unsigned long long)n); return NULL; }
    atomic_fetch_add(&s_st_ovf, 1); atomic_fetch_add(&s_st_bytes, n);
    *buf = b; *off = 0; return p;
}
uint64_t vkm_budget_left(void) { return g_budget_used >= g_budget ? 0 : g_budget - g_budget_used; }
void vkm_budget_take(uint64_t n) { g_budget_used += n; }

// ---------- Pools ----------
static int pool_cmp(const void *a, const void *b) { uintptr_t x = (*(pool_t_ *const *)a)->lo, y = (*(pool_t_ *const *)b)->lo; return x < y ? -1 : x > y; }
static pool_t_ *pool_find(uintptr_t a, uint64_t len) {   // caller holds g_lk (shared): binary search on start, then a scan for overlapping pools
    int lo = 0, hi = g_npool - 1, hit = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pool[m]->lo <= a) { hit = m; lo = m + 1; } else hi = m - 1; }
    if (hit >= 0 && a + len <= g_pool[hit]->hi) return g_pool[hit];
    for (int i = 0; i < g_npool; i++) if (a >= g_pool[i]->lo && a + len <= g_pool[i]->hi) return g_pool[i];
    return NULL;
}

static int win_of(const pool_t_ *p, uint64_t off) { int k = p->nwin - 1; while (k > 0 && p->win_lo[k] > off) k--; return k; }   // the last window starting at or before off
static int win_for(const pool_t_ *p, uint64_t off, uint64_t n) { int k = win_of(p, off); return off + n <= p->win_lo[k] + p->win_n[k] ? k : -1; }   // the window holding all of it, or -1

static void pool_free(pool_t_ *p) {
    for (int k = 0; k < p->nwin; k++) if (p->buf[k]) vkDestroyBuffer(g_vk.dev, p->buf[k], NULL);
    if (p->sparse) { for (uint64_t g = 0; g < p->ngran; g++) vk_free_mem(&p->gran[g]); free(p->gran); }
    else for (int k = 0; k < p->nwin; k++) vk_free_mem(&p->mem[k]);
    free(p->up); free(p->hot_at); free(p->last_dirty); free(p->streak); free(p->hot); free((void *)p->gpu_pending); free(p->committed);
    free(p->lz); free(p->lz_n); free(p->lz_fail); free(p->lz_hold); free(p);
}
static void pool_free_cb(void *arg) { pool_free(arg); }

void vkm_pool_add(const void *mem, uint64_t size) {
    if (!g_vk.up || !mem || !size) return;
    pool_t_ *p = calloc(1, sizeof *p);
    p->lo = (uintptr_t)mem; p->hi = p->lo + size; p->key = mem;
    p->base = p->lo & ~(uintptr_t)(PAGE - 1);
    p->size = ((p->hi - p->base) + PAGE - 1) & ~(uint64_t)(PAGE - 1); p->npages = p->size / PAGE;
    p->up = calloc(p->npages, sizeof *p->up); p->last_dirty = calloc(p->npages, sizeof *p->last_dirty); p->streak = calloc(p->npages, 1); p->hot = calloc(p->npages, 1); p->hot_at = calloc(p->npages, sizeof *p->hot_at); p->gpu_pending = calloc(p->npages, 1);
    p->committed = calloc(p->npages, 1);
    // MKW has thousands of small pools. Sparse, each first use is a queue bind that waits for the GPU, with a 2 MB granule for a pool of 16 KB: frames of seconds while it loads. A small pool is a plain
    // buffer that gets its memory at first use
    p->sparse = g_vk.has.sparse_buffer && p->size >= (8u << 20);
    VkDeviceSize bsize = p->sparse ? (p->size + g_gran - 1) / g_gran * g_gran : p->size, w = bsize, ov = 0;
    if (g_max_buffer && bsize > g_max_buffer) {   // windows start on granules (or pages), so a granule never straddles two
        w = p->sparse ? g_max_buffer / g_gran * g_gran : g_max_buffer / PAGE * PAGE;
        if (p->sparse && g_alias) { ov = w / 4 < (256ull << 20) ? w / 4 : (256ull << 20); ov = ov / g_gran * g_gran; }
    }
    for (uint64_t at = 0; w > ov && p->nwin < MAXWIN; at += w - ov) {
        p->win_lo[p->nwin] = at; p->win_n[p->nwin] = bsize - at < w ? bsize - at : w; p->nwin++;
        if (at + w >= bsize) break;
    }
    if (!p->nwin || p->win_lo[p->nwin - 1] + p->win_n[p->nwin - 1] < bsize) {
        VKRED("memory pool %p+%#llx does not fit in %d buffers of the device's largest size (%#llx): not mirrored", mem, (unsigned long long)size, MAXWIN, (unsigned long long)g_max_buffer);
        p->nwin = 0; pool_free(p); return;
    }
    if (nvn_dirty_lazy_pages()) nvn_dirty_lazy_in((const void *)p->base, (size_t)p->size, 1);   // pages another pool handed to the device: this pool's copy is taken from guest memory
    VkBufferCreateFlags fl = p->sparse ? VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | (ov ? VK_BUFFER_CREATE_SPARSE_ALIASED_BIT : 0) : 0;
    int ok = p->up && p->hot && p->committed;
    for (int k = 0; ok && k < p->nwin; k++) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, fl, p->win_n[k], g_usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        ok = vkCreateBuffer(g_vk.dev, &bci, NULL, &p->buf[k]) == VK_SUCCESS;
        if (ok) vk_name((uint64_t)p->buf[k], VK_OBJECT_TYPE_BUFFER, "nvn memory pool");
    }
    if (!ok) { VKRED("cannot create the buffer of memory pool %p+%#llx", mem, (unsigned long long)size); pool_free(p); return; }
    if (p->sparse) { p->ngran = bsize / g_gran; p->gran = calloc(p->ngran, sizeof *p->gran); for (uint64_t g = 0; g < p->ngran; g++) p->gran[g].block = -1; }
    else if (p->size >= (8u << 20)) {   // no sparse binding on this device, a large pool: all of it now
        for (int k = 0; k < p->nwin; k++) {
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, p->buf[k], &mr);
            if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &p->mem[k])) { VKRED("no device memory for memory pool %p+%#llx", mem, (unsigned long long)size); pool_free(p); return; }
            vkBindBufferMemory(g_vk.dev, p->buf[k], p->mem[k].mem, p->mem[k].off);
        }
        p->bound = p->size;
    }
    if (p->nwin > 1) VKLOG("memory pool %p+%#llx: %d buffers of up to %llu MB%s (the device's largest buffer is %llu MB)", mem, (unsigned long long)size, p->nwin,
                           (unsigned long long)(w >> 20), ov ? ", overlapping by the same memory" : ", side by side", (unsigned long long)(g_max_buffer >> 20));
    AcquireSRWLockExclusive(&g_lk);
    if (g_npool < MAXPOOL) { g_pool[g_npool++] = p; qsort(g_pool, (size_t)g_npool, sizeof *g_pool, pool_cmp); p = NULL; atomic_fetch_add(&g_pool_gen, 1); }
    ReleaseSRWLockExclusive(&g_lk);
    if (p) { VKRED("too many memory pools (%d)", MAXPOOL); pool_free(p); return; }
    static int nlog; if (nlog++ < 32) VKLOG("memory pool %p+%#llx: device-local mirror%s", mem, (unsigned long long)size, g_vk.has.sparse_buffer ? ", backed in 2 MB granules on first use" : "");
}

// The NVN layer tells each pool's flags right after adding it (any backend; a pool this one does not mirror is ignored)
void vkm_pool_flags(const void *mem, uint32_t flags) {
    AcquireSRWLockShared(&g_lk);
    for (int i = 0; i < g_npool; i++) if (g_pool[i]->key == mem) { g_pool[i]->nvn_flags = flags; break; }
    ReleaseSRWLockShared(&g_lk);
}
static _Atomic uint64_t s_wb_noacc_b;   // write-back bytes into pools the CPU cannot access (measured first; NXRT_VK_WB_NOACCESS=0 skips them)
static int wb_skip(uint64_t addr, uint64_t len, int count) {   // 1: a GPU write into a CPU_NO_ACCESS pool, which the CPU never reads: no copy back
    AcquireSRWLockShared(&g_lk); pool_t_ *p = pool_find((uintptr_t)addr, len ? len : 1); int noacc = p && (p->nvn_flags & 1); ReleaseSRWLockShared(&g_lk);
    if (!noacc) return 0;
    if (count) atomic_fetch_add(&s_wb_noacc_b, len);
    static int keep = -1; if (keep < 0) keep = vkc_int("wb_noaccess", 1);
    return !keep;
}
uint64_t vkm_wb_noaccess_bytes(void) { return atomic_load(&s_wb_noacc_b); }
void vkm_pool_del(const void *mem) {
    pool_t_ *p = NULL;
    if (nvn_dirty_lazy_pages()) {   // what the device alone has of it goes back to guest memory while the pool can still be read
        uintptr_t b = 0; uint64_t n = 0;
        AcquireSRWLockShared(&g_lk);
        for (int i = 0; i < g_npool; i++) if (g_pool[i]->key == mem) { if (g_pool[i]->lz_pages) { b = g_pool[i]->base; n = g_pool[i]->size; } break; }
        ReleaseSRWLockShared(&g_lk);
        if (n) nvn_dirty_lazy_in((const void *)b, (size_t)n, 1);
    }
    AcquireSRWLockExclusive(&g_lk);
    for (int i = 0; i < g_npool; i++) if (g_pool[i]->key == mem) { p = g_pool[i]; memmove(&g_pool[i], &g_pool[i + 1], (size_t)(g_npool - i - 1) * sizeof *g_pool); g_npool--; atomic_fetch_add(&g_pool_gen, 1); break; }
    ReleaseSRWLockExclusive(&g_lk);
    if (p) vk_retire(pool_free_cb, p);   // submissions recorded so far may still read it
}

// The window of p holding [addr, addr+len): buffer and offset, or 0 when the range spans two windows that do not overlap that far (logged once)
static int pool_window(pool_t_ *p, uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    uint64_t o = addr - p->base; int k = win_for(p, o, len ? len : 1);
    if (k < 0) {
        static _Atomic int said; if (!atomic_exchange(&said, 1))
            VKRED("a range %#llx+%#llx spans two of memory pool %p's buffers (the device's largest buffer is %llu MB): it reads zeros (later ones are not logged)",
                  (unsigned long long)addr, (unsigned long long)len, p->key, (unsigned long long)(g_max_buffer >> 20));
        return 0;
    }
    if (buf) *buf = p->buf[k];
    if (off) *off = o - p->win_lo[k];
    return 1;
}

uint64_t vkm_room(uint64_t addr) {
    uint64_t n = 0;
    AcquireSRWLockShared(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, 1);
    if (p) { uint64_t o = addr - p->base; int k = win_of(p, o); uint64_t wend = p->win_lo[k] + p->win_n[k], pend = p->hi - p->base; n = (wend < pend ? wend : pend) - o; }
    ReleaseSRWLockShared(&g_lk);
    return n;
}

int vkm_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    AcquireSRWLockShared(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len ? len : 1);
    int ok = p && pool_window(p, addr, len, buf, off);
    ReleaseSRWLockShared(&g_lk);
    return ok;
}

// Sparse backing for [o, o+n) of the buffer: unbound granules get memory and are bound before anything is recorded against them (a bind is a queue
// operation; it is waited for here, which happens once per 2 MB granule)
static int pool_bind(pool_t_ *p, uint64_t o, uint64_t n) {
    if (!p->sparse) {
        if (p->bound) return 0;
        static SRWLOCK lk = SRWLOCK_INIT; int rc = 0;
        AcquireSRWLockExclusive(&lk);
        if (!p->bound) for (int k = 0; k < p->nwin && !rc; k++) {
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, p->buf[k], &mr);
            if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &p->mem[k])) { VKRED("no device memory for memory pool %p+%#llx", p->key, (unsigned long long)p->size); rc = -1; }
            else vkBindBufferMemory(g_vk.dev, p->buf[k], p->mem[k].mem, p->mem[k].off);
        }
        if (!rc) { p->bound = p->size; atomic_fetch_add(&g_bind_gen, 1); }
        ReleaseSRWLockExclusive(&lk);
        return rc;
    }
    uint64_t g0 = o / g_gran, g1 = (o + n + g_gran - 1) / g_gran;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, p->buf[0], &mr);
    VkMemoryRequirements gr = {g_gran, mr.alignment, mr.memoryTypeBits};
    for (;;) {
        uint64_t fresh[32]; int nf = 0, rc = 0;   // granules backed in this round; each is bound into every window that covers it (two where windows overlap)
        for (uint64_t g = g0; g < g1 && g < p->ngran && nf < 32; g++) {
            if (p->gran[g].mem) continue;
            { static uint64_t failed; extern uint64_t now_ns(void); if (failed && now_ns() - failed < 100000000ull) { rc = -1; break; }   // not 18,000 times a second
              if (vk_alloc_mem(&gr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &p->gran[g])) { failed = now_ns(); VKRED("no device memory to back pool %p at %#llx", p->key, (unsigned long long)(g * g_gran)); rc = -1; break; } }
            fresh[nf++] = g;
            p->bound += g_gran;
        }
        if (!nf) return rc;
        VkSparseMemoryBind binds[64]; VkSparseBufferMemoryBindInfo bis[MAXWIN]; int nb = 0, nbi = 0;
        for (int k = 0; k < p->nwin; k++) {
            int first = nb;
            for (int i = 0; i < nf; i++) {
                uint64_t at = fresh[i] * g_gran;
                if (at >= p->win_lo[k] && at < p->win_lo[k] + p->win_n[k] && nb < 64)
                    binds[nb++] = (VkSparseMemoryBind){at - p->win_lo[k], g_gran, p->gran[fresh[i]].mem, p->gran[fresh[i]].off, 0};
            }
            if (nb > first) bis[nbi++] = (VkSparseBufferMemoryBindInfo){p->buf[k], (uint32_t)(nb - first), binds + first};
        }
        VkBindSparseInfo si = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, (uint32_t)nbi, bis, 0, NULL, 0, NULL, 0, NULL};
        uint64_t tb = now_ns();
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; vkCreateFence(g_vk.dev, &fci, NULL, &f);
        AcquireSRWLockExclusive(&g_vk.qlock);
        VkResult r = vkQueueBindSparse(g_vk.q, 1, &si, f);
        ReleaseSRWLockExclusive(&g_vk.qlock);
        if (r == VK_SUCCESS) vkWaitForFences(g_vk.dev, 1, &f, VK_TRUE, UINT64_MAX); else { VKRED("vkQueueBindSparse failed (%d)", r); vk_lost("a sparse bind", r); rc = -1; }
        vkDestroyFence(g_vk.dev, f, NULL);
        atomic_fetch_add(&g_t_bind, now_ns() - tb); atomic_fetch_add(&g_n_bind, 1);
        if (rc) return rc;
    }
}

// Guest memory is read with a guard: a page that is not mapped any more (a pool the game freed without finalizing) reads as zeros instead of crashing
static int guest_copy(void *dst, const void *src, size_t n) {
    NVN_GUARDED(memset(dst, 0, n); return -1);
    memcpy(dst, src, n); NVN_GUARD_END(); return 0;
}

// A page copied before and written again: hot after HOT_STREAK writes in a row, with at most hot_gap frames between them: a double-buffered upload writes each buffer every other frame,
// and at 2560x1440 a movie's two took 1,470 write faults a frame without this
static void page_rewritten(pool_t_ *p, uint64_t pg) {
    if (p->last_dirty[pg] == (uint32_t)g_frame) return;
    static int gap = -1; if (gap < 0) gap = vkc_int("hot_gap", 2);
    p->streak[pg] = p->last_dirty[pg] + 1 + (uint32_t)gap >= (uint32_t)g_frame ? (uint8_t)(p->streak[pg] < 255 ? p->streak[pg] + 1 : 255) : 1;
    p->last_dirty[pg] = (uint32_t)g_frame;
    if (p->streak[pg] >= HOT_STREAK && !p->hot[pg]) { p->hot[pg] = 1; p->hot_n++; }
}
// Whether page pg's device copy is behind guest memory: never copied, hot, or written (a fault stamped it) since its copy; untracked pages always are
static inline int page_needs(pool_t_ *p, uint64_t pg) {
    if (p->lz && p->lz[pg]) return 0;   // handed to the device (write-back on demand): guest memory is not looked at
    if (atomic_load(&p->gpu_pending[pg])) return 0;   // a GPU write in flight: the device copy is ahead until its bytes are written back
    if (p->hot[pg]) return p->hot_at[pg] != atomic_load(&g_sub_epoch);
    if (!p->up[pg] || !p->span) return 1;
    return nvn_dirty_since_span(p->span, (const void *)(p->base + pg * PAGE), PAGE, p->up[pg]);
}
// A clean group of 64 pages at once: one generation scan from the group's oldest copy
static int group_clean(pool_t_ *p, uint64_t g0) {
    if (!p->span || g0 + 64 > p->npages) return 0;
    uint64_t lo = UINT64_MAX;
    for (uint64_t k = g0; k < g0 + 64; k++) { if (!p->up[k] || p->hot[k] || atomic_load(&p->gpu_pending[k])) return 0; if (p->up[k] < lo) lo = p->up[k]; }
    return !nvn_dirty_since_span(p->span, (const void *)(p->base + g0 * PAGE), 64 * PAGE, lo);
}

typedef struct { VkBufferCopy r[256]; int n; VkBuffer src, dst; } copy_batch;
static void batch_flush(copy_batch *b, VkCommandBuffer cb) { if (b->n) vkCmdCopyBuffer(cb, b->src, b->dst, (uint32_t)b->n, b->r); b->n = 0; }
// [off, off+n) of the pool from src at so: one copy per window it crosses (windows that overlap share their memory, so one of them is enough)
static void batch_put(copy_batch *b, pool_t_ *p, VkCommandBuffer cb, VkBuffer src, VkDeviceSize so, uint64_t off, uint64_t n) {
    while (n) {
        int k = win_of(p, off); uint64_t end = p->win_lo[k] + p->win_n[k], m = off + n <= end ? n : end - off;
        if (b->n == 256 || (b->n && (b->src != src || b->dst != p->buf[k]))) batch_flush(b, cb);
        b->src = src; b->dst = p->buf[k]; b->r[b->n++] = (VkBufferCopy){so, off - p->win_lo[k], m};
        so += m; off += m; n -= m;
    }
}
static void pool_fill_zero(pool_t_ *p, VkCommandBuffer cb, uint64_t off, uint64_t n) {
    while (n) { int k = win_of(p, off); uint64_t end = p->win_lo[k] + p->win_n[k], m = off + n <= end ? n : end - off; vkCmdFillBuffer(cb, p->buf[k], off - p->win_lo[k], m, 0); off += m; n -= m; }
}

// Copies the pages among [p0, p1) of pool p whose device copy is behind into cb, until budget bytes; *stop_at (optional) is where it stopped.
// Order per run (nxnative's): take the stamp, write-protect, then read guest memory: a write after the protection faults and is seen at the next check
static uint64_t pool_sync(pool_t_ *p, uint64_t p0, uint64_t p1, VkCommandBuffer cb, uint64_t budget, uint64_t *stop_at) {
    if (!p->span) p->span = nvn_dirty_span((const void *)p->lo, p->hi - p->lo);
    if (p1 > p->npages) p1 = p->npages;
    copy_batch b; b.n = 0; b.src = b.dst = VK_NULL_HANDLE;
    uint64_t queued = 0, t0 = now_ns(), pg = p0;
    while (pg < p1 && queued < budget) {
        if (!(pg & 63) && pg + 64 <= p1 && group_clean(p, pg)) { pg += 64; continue; }
        if (!page_needs(p, pg)) {
            if (atomic_load(&p->gpu_pending[pg]) && (p->hot[pg] ? p->hot_at[pg] != atomic_load(&g_sub_epoch) : p->span && p->up[pg] && nvn_dirty_since_span(p->span, (const void *)(p->base + pg * PAGE), PAGE, p->up[pg]))) {
                atomic_fetch_add(&s_wb_stale, 1);
                if (p->shadow && p->shadow[pg] && vkc_int("pending_merge", 1)) {   // the words the CPU changed go to the device, the GPU's stay (Eden's OnCachedWrite, by comparison instead of a trap)
                    uint64_t stamp = nvn_dirty_now();
                    if (!p->hot[pg] && p->span) nvn_dirty_protect_span(p->span, (const void *)(p->base + pg * PAGE), PAGE);
                    uint32_t now[PAGE / 4], *s = p->shadow[pg]; guest_copy(now, (const void *)(p->base + pg * PAGE), PAGE);
                    for (uint32_t i = 0; i < PAGE / 4; ) {
                        if (now[i] == s[i]) { i++; continue; }
                        uint32_t e = i; while (e < PAGE / 4 && now[e] != s[e]) e++;
                        VkBuffer sb; VkDeviceSize so; uint8_t *dst = vkm_staging((uint64_t)(e - i) * 4, 16, &sb, &so);
                        if (!dst) break;
                        memcpy(dst, now + i, (size_t)(e - i) * 4); memcpy(s + i, now + i, (size_t)(e - i) * 4);
                        batch_put(&b, p, cb, sb, so, pg * PAGE + 4ull * i, (uint64_t)(e - i) * 4);
                        atomic_fetch_add(&s_wb_merged, e - i); i = e;
                    }
                    p->up[pg] = stamp; p->hot_at[pg] = atomic_load(&g_sub_epoch);
                }
            }
            pg++; continue; }
        uint64_t run0 = pg;
        while (pg < p1 && pg - run0 < RUN_MAX && page_needs(p, pg)) { if (p->up[pg] && !p->hot[pg]) page_rewritten(p, pg); pg++; }
        // Pages the game never touched are not committed (the heap commits lazily): nothing to read or protect there. A use reads them as zeros and
        // keeps asking (they stay "never copied"); the background sync steps over the whole uncommitted region. VirtualQuery walks the address space's
        // regions, which the tracker's protections split finely: in gameplay it was 70 % of the render thread's time. Pages seen committed are remembered
        // (a pool's memory stays committed while the pool exists) and only a run that starts on a page not known yet is asked about
        uint64_t known = run0; while (known < pg && p->committed[known]) known++;
        MEMORY_BASIC_INFORMATION mi;
        if (known > run0) pg = known;   // the known part now, the rest in the next run
        else if (VirtualQuery((const void *)(p->base + run0 * PAGE), &mi, sizeof mi)) {
            uint64_t rend = ((uintptr_t)mi.BaseAddress + mi.RegionSize - p->base) / PAGE;
            int readable = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (rend < pg) pg = rend > run0 ? rend : run0 + 1;
            if (!readable) {
                if (budget == UINT64_MAX) { if (pool_bind(p, run0 * PAGE, (pg - run0) * PAGE)) break; batch_flush(&b, cb); pool_fill_zero(p, cb, run0 * PAGE, (pg - run0) * PAGE); }
                else if (rend > pg) pg = rend < p1 ? rend : p1;
                atomic_fetch_add(&s_uncommitted, pg - run0);
                continue;
            }
            memset(p->committed + run0, 1, (size_t)(pg - run0));
        }
        uint64_t off = run0 * PAGE, n = (pg - run0) * PAGE;
        if (pool_bind(p, off, n)) break;
        p->up_b += n; for (uint64_t k = run0; k < pg; k++) { if (!p->up[k]) p->up_never += PAGE; else if (p->hot[k]) p->up_hot += PAGE; }
        uint64_t stamp = nvn_dirty_now();
        for (uint64_t q0 = run0; q0 < pg; ) {   // protect the pages that are not hot
            if (p->hot[q0]) { q0++; continue; }
            uint64_t q1 = q0; while (q1 < pg && !p->hot[q1]) q1++;
            if (p->span) nvn_dirty_protect_span(p->span, (const void *)(p->base + q0 * PAGE), (size_t)((q1 - q0) * PAGE));
            q0 = q1;
        }
        VkBuffer sb; VkDeviceSize so;
        uint8_t *dst = vkm_staging(n, 16, &sb, &so);
        if (!dst) { pg = run0; break; }
        guest_copy(dst, (const void *)(p->base + off), (size_t)n);
        { uint32_t ep = atomic_load(&g_sub_epoch); for (uint64_t k = run0; k < pg; k++) { p->up[k] = stamp; p->hot_at[k] = ep; } }
        batch_put(&b, p, cb, sb, so, off, n);
        queued += n;
        atomic_fetch_add(&s_up_pages, pg - run0); atomic_fetch_add(&s_up_runs, 1);
    }
    batch_flush(&b, cb);
    if (stop_at) *stop_at = pg;
    atomic_fetch_add(&s_up_bytes, queued); atomic_fetch_add(&s_up_ns, now_ns() - t0);
    return queued;
}

// A range synced in this submission, with no page written since, needs no second look: the same few buffers are bound by thousands of draws (5,000 descriptor sets a frame), and looking took 11 ms a frame
// Only the recording thread gets this far (vks_upload_cb is NULL elsewhere), so the table is that thread's alone. A hit also gives the buffer and offset, so a range
// used again (thousands of descriptor sets bind the same few) skips the pool lookup and its lock: gamectl set use_fast 0 (NXRT_VK_USE_FAST=0) looks them up every time
static struct { uint64_t addr, len, gen; uint32_t ep, pgen, host; VkBuffer buf; VkDeviceSize off; pool_t_ *pool; } g_seen[8192];   // pool: whose pages to look at when guest memory has changed since
static uint32_t g_seen_host = 1;   // moves when the replay writes guest memory itself (vkm_host_wrote)
void vkm_host_wrote(void) { g_seen_host++; }
static int g_use_fast = -1;
static uint64_t s_use_hit, s_use_calls, s_use_synced;   // the recording thread's vkm_use calls, fast-path hits, and those that ran pool_sync
void vkm_use_counts(uint64_t *calls, uint64_t *hits, uint64_t *synced) { *calls = s_use_calls; *hits = s_use_hit; *synced = s_use_synced; }
uint32_t vkm_pool_gen(void) { return atomic_load(&g_pool_gen); }
uint64_t vkm_up_ns(void) { return atomic_load(&s_up_ns); }
static uint64_t s_use_cyc; static int g_use_ph = -1;   // NXRT_VK_PHASES: cycles inside vkm_use (the replay thread's only)
uint64_t vkm_use_cycles(void) { return s_use_cyc; }
static void gpu_pending_drain(void);
static int vkm_use_(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);
int vkm_use(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    if (g_use_ph < 0) g_use_ph = vkc_int("phases", 0);
    if (!g_use_ph) return vkm_use_(addr, len, buf, off);
    uint64_t t0 = __builtin_ia32_rdtsc(); int r = vkm_use_(addr, len, buf, off); if (vks_owner() >= 0) s_use_cyc += __builtin_ia32_rdtsc() - t0; return r;
}
static int vkm_use_(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    if (g_use_fast < 0) g_use_fast = vkc_int("use_fast", 3);
    s_use_calls++;
    unsigned h = (unsigned)(((addr >> 4) * 0x9E3779B1u ^ len * 0x85EBCA6Bu) >> 7) & 8191;
    if (g_use_fast && g_seen_on && g_seen[h].addr == addr && g_seen[h].len == len && g_seen[h].pgen == atomic_load(&g_pool_gen) && g_seen[h].buf) {
        uint64_t now = nvn_dirty_now(); uint32_t ep = atomic_load(&g_sub_epoch);
        // the same range synced before: still current if nothing was written anywhere since (in this submission), or if none of its own pages needs a copy: page_needs
        // looks at each page's own stamp, and a hot page needs one copy per submission. Game threads write all the time, and there are 6 submissions a frame: the
        // whole-memory test alone missed most uses. NXRT_VK_USE_FAST=2: only within a submission (as at first)
        // A range used once in a submission stays as it was copied for the rest of that submission (use_fast 3, the default): the game may not write memory a submitted
        // command buffer still reads, and what its threads write meanwhile is the next submission's (the console's GPU reads it whenever it gets there). Looking again
        // at every use meant each page's stamp and pending count for 16,000 uses a frame in a Mario Kart World race, most of them cache misses: 0.8 ms of the 12.7 ms
        // the translation thread takes per frame. gamectl set use_fast 1: every use looks at its pages again, as before
        int ok = g_seen[h].ep == ep && (g_use_fast >= 3 ? g_seen[h].host == g_seen_host : g_seen[h].gen == now);
        if (!ok && g_seen[h].ep != ep && g_use_fast == 2) g_seen[h].pool = NULL;
        if (!ok && g_seen[h].pool) {   // what pool_sync would look at: each page against the stamp of its own copy (pgen above: the pool is still there)
            pool_t_ *p = g_seen[h].pool; uint64_t p0 = (addr - p->base) / PAGE, p1 = (addr + (len ? len : 1) - p->base + PAGE - 1) / PAGE;
            ok = p1 <= p->npages; for (uint64_t pg = p0; pg < p1 && ok; pg++) ok = !page_needs(p, pg);
        }
        if (ok && vks_upload_cb()) {
            s_use_hit++;
            g_seen[h].gen = now; g_seen[h].ep = ep; g_seen[h].host = g_seen_host;
            if (buf) *buf = g_seen[h].buf;
            if (off) *off = g_seen[h].off;
            return 1;
        }
    }
    VkBuffer b_; VkDeviceSize o_; if (!buf) buf = &b_; if (!off) off = &o_;
    uint32_t pgen = atomic_load(&g_pool_gen);
    AcquireSRWLockShared(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len ? len : 1);
    int ok = p && pool_window(p, addr, len, buf, off);
    ReleaseSRWLockShared(&g_lk);
    if (!ok) return 0;
    VkCommandBuffer cb = vks_upload_cb();
    if (!cb) return 1;
    uint32_t ep = atomic_load(&g_sub_epoch); uint64_t gen = nvn_dirty_now();
    if (g_seen_on && g_seen[h].addr == addr && g_seen[h].len == len && g_seen[h].ep == ep && g_seen[h].gen == gen && g_seen[h].host == g_seen_host) { g_seen[h].pgen = pgen; g_seen[h].buf = *buf; g_seen[h].off = *off; g_seen[h].pool = p; return 1; }
    gpu_pending_drain();
    atomic_fetch_add(&s_sync, 1); s_use_synced++;
    pool_sync(p, (addr - p->base) / PAGE, (addr + (len ? len : 1) - p->base + PAGE - 1) / PAGE, cb, UINT64_MAX, NULL);
    g_seen[h].addr = addr; g_seen[h].len = len; g_seen[h].ep = ep; g_seen[h].gen = nvn_dirty_now(); g_seen[h].pgen = pgen; g_seen[h].buf = *buf; g_seen[h].off = *off; g_seen[h].host = g_seen_host;
    g_seen[h].pool = p;
    return 1;
}

// Every pool's pages behind guest memory, round robin from where the last call stopped, within the frame's upload budget and a scan limit
static uint64_t g_scan_pages = 262144;   // NXRT_VK_SCAN_PAGES: pages looked at per call (1 GB)
static int g_rr;
uint64_t vkm_sync_all(void) {
    VkCommandBuffer cb = vks_upload_cb();
    if (!cb) return 0;
    uint64_t total = 0, scanned = 0;
    AcquireSRWLockShared(&g_lk);
    for (int k = 0; k < g_npool && vkm_budget_left() && scanned < g_scan_pages; k++) {
        int i = (g_rr + k) % g_npool;
        pool_t_ *p = g_pool[i];
        uint64_t from = p->cursor < p->npages ? p->cursor : 0, to = from + (g_scan_pages - scanned), stop = from;
        uint64_t n = pool_sync(p, from, to, cb, vkm_budget_left(), &stop);
        vkm_budget_take(n); total += n; scanned += stop - from;
        p->cursor = stop >= p->npages ? 0 : stop;
        if (p->cursor) { g_rr = i; break; }   // continue in this pool next time
        g_rr = (i + 1) % g_npool;
    }
    ReleaseSRWLockShared(&g_lk);
    atomic_fetch_add(&s_sync, 1);
    return total;
}

// GPU writes in flight over [a, a+len): +1 when recorded, -1 when their bytes have reached guest memory. The pages are copied again from guest memory after
// that (the write back stamps them), which also brings in anything the CPU wrote beside the GPU's bytes
static uint64_t s_gp_calls, s_gp_pages, s_gp_first, s_gp_cyc;   // statistics (the translation thread's, unlocked)
typedef struct { pool_t_ *p; const uint64_t *pg; } shadow_ctx;
static void shadow_copy(void *c_, uint64_t lo, uint64_t hi) { shadow_ctx *c = c_; for (uint64_t k = lo; k < hi; k++) guest_copy(c->p->shadow[c->pg[k]], (const void *)(c->p->base + c->pg[k] * PAGE), PAGE); }
static void gpu_pending_(uint64_t a, uint64_t len, int d) {
    AcquireSRWLockShared(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)a, len);
    if (p) {
        uint64_t p0 = (a - p->base) / PAGE, p1 = (a + len - 1 - p->base) / PAGE + 1; if (p1 > p->npages) p1 = p->npages;
        s_gp_pages += p1 > p0 ? p1 - p0 : 0;
        if (d > 0) {   // a page's first write in flight: the use that came before it has just made the device current, and guest memory as it is now is what the device has
            static uint64_t list[2048];   // the translation thread's alone (with synchronous replay, whoever holds the graphics lock)
            if (!p->shadow) p->shadow = calloc(p->npages, sizeof *p->shadow);
            for (uint64_t pg = p0; pg < p1 && p->shadow; ) {
                uint64_t n = 0;
                for (; pg < p1 && n < 2048; pg++) if (!atomic_load(&p->gpu_pending[pg])) { if (!p->shadow[pg]) p->shadow[pg] = malloc(PAGE); if (p->shadow[pg]) list[n++] = pg; }
                s_gp_first += n;
                if (n) { shadow_ctx c = {p, list}; par_for(n, shadow_copy, &c); }
            }
        }
        for (uint64_t pg = p0; pg < p1; pg++) atomic_fetch_add(&p->gpu_pending[pg], (uint8_t)d);
    }
    ReleaseSRWLockShared(&g_lk);
}
static void gpu_pending(uint64_t a, uint64_t len, int d) { uint64_t t0 = __builtin_ia32_rdtsc(); gpu_pending_(a, len, d); s_gp_cyc += __builtin_ia32_rdtsc() - t0; s_gp_calls++; }

// A write-back that has landed takes its pages' counts down again. That happens on the thread that completes submissions, while the translation thread counts up and
// reads the same bytes at every use: the cache lines went back and forth between the two cores, and the one "lock add" in gpu_pending was 4.6 % of the translation
// thread in a Mario Kart World race (3,600 pages a frame, 230 ns each). So the completion thread only queues the range, and the translation thread takes the counts
// down itself, before it counts up or looks (gpu_pending_drain). A count stays up a moment longer: the device copy counts as the newer one until then, as it did.
// NXRT_VK_PENDING_DIRECT=1: as before
static struct { uint64_t a, len; } g_pdq[8192]; static _Atomic unsigned g_pdq_n; static SRWLOCK g_pdq_lk = SRWLOCK_INIT;
static void gpu_pending_done(uint64_t a, uint64_t len) {
    static int direct = -1; if (direct < 0) direct = vkc_int("pending_direct", 0);
    if (!direct) {
        AcquireSRWLockExclusive(&g_pdq_lk);
        unsigned n = atomic_load_explicit(&g_pdq_n, memory_order_relaxed);
        if (n < 8192) { g_pdq[n].a = a; g_pdq[n].len = len; atomic_store(&g_pdq_n, n + 1); ReleaseSRWLockExclusive(&g_pdq_lk); return; }
        ReleaseSRWLockExclusive(&g_pdq_lk);   // full (the translation thread is far behind): directly
    }
    gpu_pending(a, len, -1);
}
static void gpu_pending_drain(void) {
    if (!atomic_load_explicit(&g_pdq_n, memory_order_relaxed)) return;
    static struct { uint64_t a, len; } loc[8192];   // the translation thread's alone (with synchronous replay, whoever holds the graphics lock)
    AcquireSRWLockExclusive(&g_pdq_lk);
    unsigned n = atomic_load_explicit(&g_pdq_n, memory_order_relaxed); memcpy(loc, g_pdq, n * sizeof *loc); atomic_store(&g_pdq_n, 0);
    ReleaseSRWLockExclusive(&g_pdq_lk);
    for (unsigned i = 0; i < n; i++) gpu_pending(loc[i].a, loc[i].len, -1);
}

// ---------- GPU writes back to guest memory ----------
typedef struct { uint64_t addr, len; VkBuffer buf; vk_alloc mem; const uint8_t *cpu; uint32_t fill; int kind; uint64_t base_at, after_at, ring_end; } wb_t;   // kind 0 readback, 1 known bytes, 2 fill, 3 the words a dispatch changed
// A dispatch's written range, before and after, in a ring of cached memory (kind 3). A storage buffer is written in part: the shader's bytes are the words that differ, and only they reach guest
// memory, so what the CPU wrote in the same range while the GPU worked stays (nxnative's unified memory has no such copy; Eden's buffer cache keeps the same two apart by the byte)
enum { WR_SIZE = 256u << 20 };
static VkBuffer g_wr; static vk_alloc g_wr_mem; static uint64_t g_wr_head; static _Atomic uint64_t g_wr_tail; static int g_wr_state;
static int wr_init(void) {
    if (g_wr_state) return g_wr_state > 0;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, WR_SIZE, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    g_wr_state = -1;
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_wr)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_wr, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &g_wr_mem) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_wr_mem)) return 0;
    vkBindBufferMemory(g_vk.dev, g_wr, g_wr_mem.mem, g_wr_mem.off);
    g_wr_state = 1; return 1;
}
// Room for n bytes: *at its offset in the ring. Monotonic head and tail; a piece does not wrap
static int wr_take(uint64_t n, uint64_t *at) {
    if (!wr_init() || n > WR_SIZE / 4) return 0;
    uint64_t h = g_wr_head, o = h % WR_SIZE;
    if (o + n > WR_SIZE) h += WR_SIZE - o, o = 0;
    if (h + n - atomic_load(&g_wr_tail) > WR_SIZE) return 0;
    g_wr_head = h + n; *at = o; return 1;
}
static uint64_t g_wr_end(void) { return g_wr_head; }
static void wr_shadow(uint32_t **shadow, uint64_t off, const uint32_t *a, const uint32_t *b, uint64_t n, int skip) {
    if (!skip) {
        for (uint64_t i = 0; i < n; i++) { uint64_t at = off + 4*i; uint32_t *s = shadow[at/PAGE];
            if (s) { if (a[i] != b[i]) s[(at%PAGE)/4] = a[i]; } else i += (PAGE-at%PAGE)/4-1; }
        return;
    }
    // MKW writes back about 16 MB a frame but changes only ~170 KB. The old
    // shadow pass visited every word again, even pages the comparison just skipped.
    for (uint64_t i = 0; i < n; ) {
        uint64_t at = off + 4*i, count = (PAGE-at%PAGE)/4; if (count > n-i) count = n-i;
        uint32_t *s = shadow[at/PAGE];
        if (s && memcmp(a+i, b+i, (size_t)count*4)) {
            s += (at%PAGE)/4;
            for (uint64_t j = 0; j < count; j++) if (a[i+j] != b[i+j]) s[j] = a[i+j];
        }
        i += count;
    }
}
// The words that differ between base and after reach guest memory, page by page; a page the shader left alone is not touched (a touch sends it to the device again)
typedef struct { const uint32_t *a, *b; uint32_t *g; uint64_t n, off; uint32_t **shadow; } land_ctx;
static void wr_land_pages(void *c_, uint64_t k0, uint64_t k1) {   // guest pages k0..k1 of the range, counted from its first (w->addr is a multiple of 4 here)
    land_ctx *c = c_; const uint32_t *a = c->a, *b = c->b; uint32_t *g = c->g; uintptr_t g0 = (uintptr_t)g; uint64_t changed = 0, kept = 0, newer = 0;
    for (uint64_t k = k0; k < k1; k++) {
        uintptr_t lo = ((g0 / PAGE) + k) * PAGE, hi = lo + PAGE;
        uint64_t i = lo > g0 ? (lo - g0) / 4 : 0, e = (hi - g0) / 4; if (e > c->n) e = c->n;
        if (i >= e || !memcmp(a + i, b + i, (size_t)(e - i) * 4)) continue;
        dm_guest_write_prepare(g + i, (size_t)(e - i) * 4);
        uint64_t at = c->off + 4 * i; uint32_t *sh = c->shadow ? c->shadow[at / PAGE] : NULL; if (sh) sh += (at % PAGE) / 4;   // the shadow: what the device has, for the words the shader changed
        for (uint64_t x = i; x < e; x++) if (a[x] != b[x]) { if (g[x] != b[x] && g[x] != a[x]) newer++; g[x] = a[x]; changed++; if (sh) sh[x - i] = a[x]; } else if (g[x] != a[x]) kept++;
    }
    atomic_fetch_add(&s_wb_words, changed); atomic_fetch_add(&s_wb_kept, kept); atomic_fetch_add(&s_wb_cpu_newer, newer);
}
static void wr_land(wb_t *w) {
    const uint32_t *b = (const uint32_t *)((const uint8_t *)g_wr_mem.map + w->base_at), *a = (const uint32_t *)((const uint8_t *)g_wr_mem.map + w->after_at);
    uint32_t *g = (uint32_t *)(uintptr_t)w->addr; uint64_t n = w->len / 4, changed = 0, kept = 0, newer = 0;
    static int skip = -1; if (skip < 0) skip = vkc_int("wb_shadow_skip", 1);
    if (skip && !(w->addr & 3) && n) {   // guest memory and the shadow in one pass per page, large ranges among the helper threads
        AcquireSRWLockShared(&g_lk);
        pool_t_ *p = pool_find((uintptr_t)w->addr, w->len);
        land_ctx c = {a, b, g, n, p ? w->addr - p->base : 0, p ? p->shadow : NULL};
        par_for((w->addr + 4 * n - 1) / PAGE - w->addr / PAGE + 1, wr_land_pages, &c);
        ReleaseSRWLockShared(&g_lk);
    } else {
        for (uint64_t i = 0; i < n; ) {
            uint64_t e = ((((uintptr_t)(g + i)) | (PAGE - 1)) + 1 - (uintptr_t)g) / 4; if (e > n) e = n;   // to the end of the guest page
            if (!memcmp(a + i, b + i, (size_t)(e - i) * 4)) { i = e; continue; }
            dm_guest_write_prepare(g + i, (size_t)(e - i) * 4);
            for (; i < e; i++) if (a[i] != b[i]) { if (g[i] != b[i] && g[i] != a[i]) newer++; g[i] = a[i]; changed++; } else if (g[i] != a[i]) kept++;
        }
        atomic_fetch_add(&s_wb_words, changed); atomic_fetch_add(&s_wb_kept, kept); atomic_fetch_add(&s_wb_cpu_newer, newer);
        AcquireSRWLockShared(&g_lk);
        { pool_t_ *p = pool_find((uintptr_t)w->addr, w->len);
          if (p && p->shadow) wr_shadow(p->shadow, w->addr-p->base, a, b, n, skip); }
        ReleaseSRWLockShared(&g_lk);
    }
    atomic_store(&g_wr_tail, w->ring_end);
    if (w->len & 3) { uint8_t *d = (uint8_t *)(uintptr_t)w->addr; const uint8_t *ab = (const uint8_t *)a, *bb = (const uint8_t *)b; dm_guest_write_prepare(d + (w->len & ~3ull), 3); for (uint64_t i = w->len & ~3ull; i < w->len; i++) if (ab[i] != bb[i]) d[i] = ab[i]; }
}
// The whole range as the device has it (a draw's storage buffer): a page that came back unchanged is not written, so it is not sent to the device again
typedef struct { const uint8_t *a; uint8_t *g; uint64_t len; } land4_ctx;
static void land4_pages(void *c_, uint64_t k0, uint64_t k1) {
    land4_ctx *c = c_; uintptr_t g0 = (uintptr_t)c->g;
    for (uint64_t k = k0; k < k1; k++) {
        uintptr_t lo = ((g0 / PAGE) + k) * PAGE, hi = lo + PAGE;
        uint64_t i = lo > g0 ? lo - g0 : 0, e = hi - g0; if (e > c->len) e = c->len;
        if (i < e && memcmp(c->a + i, c->g + i, (size_t)(e - i))) { dm_guest_write_prepare(c->g + i, (size_t)(e - i)); memcpy(c->g + i, c->a + i, (size_t)(e - i)); }
    }
}
// NXRT_VK_WBLOG=<bytes>: every write-back of at least that many bytes in the log, when recorded (rec) and when it lands
// (land: bytes of guest memory it changed, only when some changed; rec only with NXRT_VK_WBLOG_REC=1), up to 100000 lines. To see what the GPU's copies back put into guest memory
static int wblog_min(void) { static int v = -1; if (v < 0) v = vkc_int("wblog", 0); return v; }
static void wblog(const char *what, uint64_t addr, uint64_t len, int kind, uint64_t changed) {
    static volatile LONG n; if (!wblog_min() || len < (uint64_t)wblog_min() || (what[0] == 'r' && !vkc_int("wblog_rec", 0)) || (what[0] == 'l' && !changed) || InterlockedIncrement(&n) > 100000) return;
    VKLOG("wblog: %s frame %llu addr %#llx len %#llx kind %d changed %llu", what, (unsigned long long)g_frame, (unsigned long long)addr, (unsigned long long)len, kind, (unsigned long long)changed); }
static _Atomic uint64_t s_wb_ns;
void vkm_perf(uint64_t *ns, uint64_t *bytes) { *ns = atomic_load(&s_wb_ns); *bytes = atomic_load(&s_wb_bytes); }
static void wb_land_(void *arg) {
    wb_t *w = arg;
    if (wblog_min() && w->kind != 3) { uint64_t ch = 0; const uint8_t *src = w->kind == 4 ? (const uint8_t *)g_wr_mem.map + w->after_at : w->kind == 0 ? (const uint8_t *)w->mem.map : w->kind == 1 ? (const uint8_t *)w->cpu : NULL;
        if (src) { const uint8_t *g = (const uint8_t *)(uintptr_t)w->addr; for (uint64_t i = 0; i < w->len; i++) ch += src[i] != g[i]; } wblog("land", w->addr, w->len, w->kind, ch); }
    if (w->kind == 3) { wr_land(w); gpu_pending_done(w->addr, w->len); atomic_fetch_add(&s_wb_bytes, w->len); free(w); return; }
    if (w->kind == 4) {   // the whole range, page by page: a page that came back unchanged is not written, so it is not sent to the device again
        land4_ctx c = {(const uint8_t *)g_wr_mem.map + w->after_at, (uint8_t *)(uintptr_t)w->addr, w->len};
        if (w->len) par_for((w->addr + w->len - 1) / PAGE - w->addr / PAGE + 1, land4_pages, &c);
        atomic_store(&g_wr_tail, w->ring_end); gpu_pending_done(w->addr, w->len); atomic_fetch_add(&s_wb_bytes, w->len); free(w); return; }
    dm_guest_write_prepare((void *)(uintptr_t)w->addr, (size_t)w->len);
    uint8_t *d = (uint8_t *)(uintptr_t)w->addr;
    if (w->kind == 0) memcpy(d, w->mem.map, (size_t)w->len);
    else if (w->kind == 1) memcpy(d, w->cpu, (size_t)w->len);
    else for (uint64_t i = 0; i < w->len; i++) d[i] = (uint8_t)(w->fill >> 8 * (i & 3));
    gpu_pending_done(w->addr, w->len);
    atomic_fetch_add(&s_wb_bytes, w->len);
    if (w->buf) { vkDestroyBuffer(g_vk.dev, w->buf, NULL); vk_free_mem(&w->mem); }
    free((void *)w->cpu); free(w);
}
static void wb_land(void *arg) {
    uint64_t t = vkperf_enabled ? now_ns() : 0;
    wb_land_(arg);
    if (t) atomic_fetch_add(&s_wb_ns, now_ns() - t);
}
// NXRT_VK_WB_OFF=1 (TEST ONLY, wrong results possible): no GPU write goes back to guest memory. Measures the most that fewer write-backs could gain
static int wb_off(void) { static int v = -1; if (v < 0) v = vkc_int("wb_off", 0); return v; }
// ---------- Write-back on demand ----------
// Every GPU write into a pool was copied back to guest memory when its submission completed: the device's bytes into a ring, a compare with guest memory, the changed
// pages written there, and those pages sent to the device again at their next use (a write to guest memory marks them). Donkey Kong Bananza's terrain does that for
// 630 chunks a frame, a 64-byte indirect draw record and a 15,000-byte index list each, about 10 MB a frame in each direction, and its CPU never reads one of them.
// nxnative has no such copies (the GPU's memory is the game's: docs/reference/memory.md); with a discrete GPU the answer is to copy only what the CPU asks for.
//
// A page that GPU writes keep coming to (LZ_STREAK of them), that has no copy on its way, is not hot and was not written by the CPU since its last upload is handed
// to the device: the tracker makes it inaccessible (nvn_dirty.c PG_LAZY), and from then on writes to it are not copied back and nothing reads it from guest memory
// (page_needs). The first CPU access, by the game or the host, faults; the tracker calls lz_fetch, which reads the page back from the device after every submission
// that skipped a copy into it has reached the queue, and the access goes on with the device's bytes. That page then stays eager for LZ_HOLD frames, doubled each
// time it happens again (a buffer the CPU does read is tried once in a long while, and costs one wait for the GPU each time).
// What a submission still being recorded writes is not in a fetch made by the thread recording it: that thread reads guest memory as the submissions before left it,
// which is what it read before (copies landed at completion). Any other thread waits for that submission to be committed (at most LZ_WAIT_NS).
// A range written in part into lazy pages is copied back in its other parts only (lz_split). NXRT_VK_WB_LAZY=0 (gamectl set wb_lazy 0 before the first write): off.
enum { LZ_STREAK = 48, LZ_HOLD = 600, LZ_FAIL_MAX = 5, LZ_RUNS = 128 };
#define LZ_WAIT_NS 300000000ull
typedef struct { uint64_t a, n; } lz_run;
static SRWLOCK g_lz_lk = SRWLOCK_INIT;   // lazy pages come and go under it; a fetch holds it until its pages are in
static _Atomic uint64_t s_lz_set, s_lz_in, s_lz_skip_b, s_lz_fetch_ns;
uint64_t vks_cur_seq(void); uint64_t vks_seq_value(uint64_t seq, int *mine);
static struct { VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkBuffer buf; vk_alloc mem; uint64_t size; int bad; } g_lzrb;   // the fetch's own command buffer and host buffer (under g_lz_lk)
static __thread int t_lz_held;
static int lzrb_ready(uint64_t n) {
    if (g_lzrb.bad) return 0;
    if (!g_lzrb.pool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_vk.qfam};
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_lzrb.pool) != VK_SUCCESS || vkCreateFence(g_vk.dev, &fci, NULL, &g_lzrb.fence) != VK_SUCCESS) { g_lzrb.bad = 1; return 0; }
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_lzrb.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (vkAllocateCommandBuffers(g_vk.dev, &ai, &g_lzrb.cb) != VK_SUCCESS) { g_lzrb.bad = 1; return 0; }
    }
    if (n <= g_lzrb.size) return 1;
    if (g_lzrb.buf) { vkDestroyBuffer(g_vk.dev, g_lzrb.buf, NULL); vk_free_mem(&g_lzrb.mem); g_lzrb.buf = VK_NULL_HANDLE; g_lzrb.size = 0; }   // idle: every fetch waits for its copy
    uint64_t sz = n < (1u << 20) ? (1u << 20) : (n + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_lzrb.buf) != VK_SUCCESS) { g_lzrb.buf = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_lzrb.buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &g_lzrb.mem) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &g_lzrb.mem)) { vkDestroyBuffer(g_vk.dev, g_lzrb.buf, NULL); g_lzrb.buf = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(g_vk.dev, g_lzrb.buf, g_lzrb.mem.mem, g_lzrb.mem.off); g_lzrb.size = sz;
    return 1;
}
// The pool that has page x (a page's start) as the device's alone. Caller holds g_lk
static pool_t_ *pool_page(uintptr_t x) {
    int lo = 0, hi = g_npool - 1, hit = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pool[m]->base <= x) { hit = m; lo = m + 1; } else hi = m - 1; }
    for (int i = hit; i >= 0 && i > hit - 4; i--) { pool_t_ *q = g_pool[i]; if (x >= q->base && x < q->base + q->size && q->lz && q->lz[(x - q->base) / PAGE]) return q; }
    for (int i = 0; i < g_npool; i++) { pool_t_ *q = g_pool[i]; if (x >= q->base && x < q->base + q->size && q->lz && q->lz[(x - q->base) / PAGE]) return q; }
    return NULL;
}
// The tracker's fetch: the device's bytes of the lazy pages [a, a+n), valid until lz_done. Any thread, also inside a fault. NULL: not available (the pages come in as guest memory has them)
static const void *lz_fetch(uintptr_t a, size_t n) {
    uint64_t t0 = now_ns(), v = 1;
    for (;;) {   // the last submission that skipped a copy into these pools has to be committed, then handed to the queue
        AcquireSRWLockExclusive(&g_lz_lk); AcquireSRWLockShared(&g_lk);
        uint64_t seq = 0; int mine = 0;
        for (uintptr_t x = a; x < a + n; ) { pool_t_ *q = pool_page(x); if (!q) { x += PAGE; continue; }
            uint64_t sq = atomic_load(&q->lz_seq); if (sq > seq) seq = sq; x = q->base + q->size; }
        v = seq ? vks_seq_value(seq, &mine) : 1;
        if (v || mine || now_ns() - t0 > LZ_WAIT_NS) break;
        ReleaseSRWLockShared(&g_lk); ReleaseSRWLockExclusive(&g_lz_lk);
        Sleep(0);
    }
    while (v > 1 && atomic_load(&g_vk.tl_submitted) < v && now_ns() - t0 < LZ_WAIT_NS) { vks_flush(); if (atomic_load(&g_vk.tl_submitted) < v) Sleep(0); }
    int ok = lzrb_ready(n), nc = 0;
    if (ok) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        vkResetCommandBuffer(g_lzrb.cb, 0); vkBeginCommandBuffer(g_lzrb.cb, &bi);
        vk_mem_barrier(g_lzrb.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        for (uintptr_t x = a; x < a + n; ) {
            pool_t_ *q = pool_page(x);
            if (!q || !q->bound) { nc = 0; break; }   // a page with no device copy to read: none of it (guest memory stays as it is)
            uint64_t off = x - q->base, m = q->size - off; if (m > a + n - x) m = a + n - x;
            for (uint64_t left = m, o = off; left; ) { int k = win_of(q, o); uint64_t end = q->win_lo[k] + q->win_n[k], c = o + left <= end ? left : end - o;
                VkBufferCopy r = {o - q->win_lo[k], (x - a) + (o - off), c}; vkCmdCopyBuffer(g_lzrb.cb, q->buf[k], g_lzrb.buf, 1, &r); nc++; o += c; left -= c; }
            x += m;
        }
        vk_mem_barrier(g_lzrb.cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        vkEndCommandBuffer(g_lzrb.cb);
        if (nc) {
            VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, g_lzrb.cb, 0};
            VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cbi, 0, NULL};
            AcquireSRWLockExclusive(&g_vk.qlock); VkResult r = vkQueueSubmit2(g_vk.q, 1, &si, g_lzrb.fence); ReleaseSRWLockExclusive(&g_vk.qlock);
            ok = r == VK_SUCCESS && vkWaitForFences(g_vk.dev, 1, &g_lzrb.fence, VK_TRUE, 10000000000ull) == VK_SUCCESS;
            if (r == VK_SUCCESS && !ok) { VKRED("write-back on demand: the device did not give %#llx+%#llx back in 10 s", (unsigned long long)a, (unsigned long long)n); g_lzrb.bad = 1; }
            else if (r == VK_SUCCESS) vkResetFences(g_vk.dev, 1, &g_lzrb.fence);
        } else ok = 0;
    }
    ReleaseSRWLockShared(&g_lk);
    atomic_fetch_add(&s_lz_fetch_ns, now_ns() - t0);
    if (!ok) { ReleaseSRWLockExclusive(&g_lz_lk); return NULL; }
    t_lz_held = 1;
    return g_lzrb.mem.map;
}
static void lz_done(uintptr_t a, size_t n, int fetched) {
    if (!t_lz_held) AcquireSRWLockExclusive(&g_lz_lk);
    t_lz_held = 0; (void)fetched;
    AcquireSRWLockShared(&g_lk);
    uint64_t pages = 0;
    for (uintptr_t x = a; x < a + n; x += PAGE) { pool_t_ *q = pool_page(x); if (!q) continue;
        uint64_t pg = (x - q->base) / PAGE; if (!q->lz[pg]) continue;
        q->lz[pg] = 0; q->lz_n[pg] = 0; if (q->lz_fail[pg] < LZ_FAIL_MAX) q->lz_fail[pg]++;
        q->lz_hold[pg] = (uint32_t)g_frame + ((uint32_t)LZ_HOLD << q->lz_fail[pg]); q->lz_pages--; pages++; }
    ReleaseSRWLockShared(&g_lk);
    ReleaseSRWLockExclusive(&g_lz_lk);
    atomic_fetch_add(&s_lz_in, pages);
    static _Atomic int said; if (atomic_fetch_add(&said, 1) < 24)
        VKLOG("write-back on demand: thread %lu touched %#llx: %llu pages read back from the device (frame %llu); they are copied back after every write again for a while",
              (unsigned long)GetCurrentThreadId(), (unsigned long long)a, (unsigned long long)pages, (unsigned long long)g_frame);
}
static int lz_on(void) { static int v = -1; if (v < 0) { v = vkc_int("wb_lazy", 1); if (v) nvn_dirty_lazy_register(lz_fetch, lz_done); } return v; }
static int pool_alone(const pool_t_ *p, uintptr_t a, uintptr_t e) {   // no other pool has a device copy of these pages. Caller holds g_lk
    for (int i = 0; i < g_npool; i++) { const pool_t_ *q = g_pool[i]; if (q != p && a < q->base + q->size && e > q->base) return 0; }
    return 1;
}
// Pages [r0, r1) of p become the device's. Caller holds g_lz_lk and g_lk
static int lz_make(pool_t_ *p, uint64_t r0, uint64_t r1) {
    uintptr_t a = p->base + r0 * PAGE; size_t n = (size_t)((r1 - r0) * PAGE);
    if (!pool_alone(p, a, a + n) || !nvn_dirty_lazy_set(p->span, (const void *)a, n)) return 0;
    for (uint64_t pg = r0; pg < r1; pg++)   // written between the look and the hand-over: taken back, it goes the usual way
        if (nvn_dirty_since_span(p->span, (const void *)(p->base + pg * PAGE), PAGE, p->up[pg])) { nvn_dirty_lazy_in((const void *)a, n, 0); return 0; }
    for (uint64_t pg = r0; pg < r1; pg++) p->lz[pg] = 1;
    p->lz_pages += r1 - r0; atomic_fetch_add(&s_lz_set, r1 - r0);
    return 1;
}
// The parts of a GPU write [addr, addr+len) that still go back to guest memory: -1 = all of it, as one piece (no page of it is lazy), else their number in runs (0: none).
// may_set: the write counts towards handing its pages over, and pages that qualify are handed over now. The recording thread only
static int lz_split(uint64_t addr, uint64_t len, int may_set, lz_run *runs) {
    if (!lz_on() || !len) return -1;
    if (may_set) gpu_pending_drain();
    int nr = -1, any = 0, over = 0; uint64_t back = 0;
    AcquireSRWLockExclusive(&g_lz_lk); AcquireSRWLockShared(&g_lk);
    pool_t_ *p = pool_find((uintptr_t)addr, len);
    if (p && p->span && (p->lz || may_set)) {
        if (!p->lz) { p->lz = calloc(p->npages, 1); p->lz_n = calloc(p->npages, 1); p->lz_fail = calloc(p->npages, 1); p->lz_hold = calloc(p->npages, sizeof *p->lz_hold);
            if (!p->lz || !p->lz_n || !p->lz_fail || !p->lz_hold) { free(p->lz); free(p->lz_n); free(p->lz_fail); free(p->lz_hold); p->lz = p->lz_n = p->lz_fail = NULL; p->lz_hold = NULL; } }
        uint64_t p0 = (addr - p->base) / PAGE, p1 = (addr + len - 1 - p->base) / PAGE + 1;
        if (p->lz && p1 <= p->npages) {
            nr = 0;
            for (uint64_t pg = p0; pg < p1 && !over; ) {
                if (p->lz[pg]) { pg++; any = 1; continue; }
                uint64_t r0 = pg; int elig = may_set;
                for (; pg < p1 && !p->lz[pg]; pg++) {
                    if (!may_set) continue;
                    if (p->lz_n[pg] < 255) p->lz_n[pg]++;
                    if (elig && (p->lz_n[pg] < LZ_STREAK || (uint32_t)g_frame < p->lz_hold[pg] || atomic_load(&p->gpu_pending[pg]) || p->hot[pg] || !p->up[pg] ||
                                 nvn_dirty_since_span(p->span, (const void *)(p->base + pg * PAGE), PAGE, p->up[pg]))) elig = 0;
                }
                if (elig && lz_make(p, r0, pg)) { any = 1; continue; }
                if (nr == LZ_RUNS) { over = 1; break; }
                uint64_t lo = p->base + r0 * PAGE, hi = p->base + pg * PAGE; if (lo < addr) lo = addr; if (hi > addr + len) hi = addr + len;
                runs[nr].a = lo; runs[nr].n = hi - lo; back += hi - lo; nr++;
            }
            if (!any || over) nr = -1;
            else { uint64_t sq = vks_cur_seq(); if (sq) atomic_store(&p->lz_seq, sq); atomic_fetch_add(&s_lz_skip_b, len - back); }
        }
    }
    ReleaseSRWLockShared(&g_lk); ReleaseSRWLockExclusive(&g_lz_lk);
    if (over) nvn_dirty_lazy_in((const void *)(uintptr_t)addr, (size_t)len, 1);   // in too many pieces: all of it eager again
    return nr;
}

static void gpu_wrote_(uint64_t addr, uint64_t len) {
    VkBuffer pb; VkDeviceSize po;
    VkCommandBuffer cb = vks_cb();
    if (!cb || !len || !vkm_addr(addr, len, &pb, &po)) return;
    { uint64_t at;   // through the ring: a buffer and an allocation for each of 400 write-backs a frame cost more than the copies
      if (vkc_int("wb_ring", 1) && wr_take((len + 15) & ~15ull, &at)) {
          wb_t *r = calloc(1, sizeof *r); r->addr = addr; r->len = len; r->kind = 4; r->after_at = at; r->ring_end = g_wr_end();
          vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
          VkBufferCopy c = {po, at, len}; vkCmdCopyBuffer(cb, pb, g_wr, 1, &c);
          vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
          gpu_pending_drain(); gpu_pending(addr, len, 1); vks_post_fn(wb_land, r); return; } }
    wb_t *w = calloc(1, sizeof *w); w->addr = addr; w->len = len;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, len, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    vkCreateBuffer(g_vk.dev, &bci, NULL, &w->buf);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, w->buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &w->mem) &&
        vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, &w->mem)) { vkDestroyBuffer(g_vk.dev, w->buf, NULL); free(w); return; }
    vkBindBufferMemory(g_vk.dev, w->buf, w->mem.mem, w->mem.off);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy c = {po, 0, len};
    vkCmdCopyBuffer(cb, pb, w->buf, 1, &c);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    gpu_pending_drain(); gpu_pending(addr, len, 1);
    vks_post_fn(wb_land, w);
}
static void gpu_wrote_runs(uint64_t addr, uint64_t len, int nr, const lz_run *rs) { if (nr < 0) gpu_wrote_(addr, len); else for (int i = 0; i < nr; i++) gpu_wrote_(rs[i].a, rs[i].n); }
void vkm_gpu_wrote(uint64_t addr, uint64_t len) {
    if (wb_off()) return;
    wblog("rec", addr, len, 4, 0);
    if (wb_skip(addr, len, 1)) return;
    lz_run rs[LZ_RUNS]; gpu_wrote_runs(addr, len, lz_split(addr, len, 1, rs), rs);
}
// A dispatch's range before the shader runs: returns the ring offset + 1, 0 when it is not kept (vkm_gpu_wrote_base falls back to the whole range)
uint64_t vkm_wb_base(uint64_t addr, uint64_t len) {
    VkBuffer pb; VkDeviceSize po; uint64_t at;
    VkCommandBuffer cb = vks_cb();
    if (wb_off() || wb_skip(addr, len, 0)) return 0;
    { lz_run rs[LZ_RUNS]; if (!lz_split(addr, len, 0, rs)) return 0; }   // every page of it is the device's: nothing will be copied back
    if (vkc_int("wb_whole", 0) || !cb || !len || !vkm_addr(addr, len, &pb, &po) || !wr_take((len + 15) & ~15ull, &at)) return 0;
    VkBufferCopy c = {po, at, len}; vkCmdCopyBuffer(cb, pb, g_wr, 1, &c);
    return at + 1;
}
void vkm_gpu_wrote_base(uint64_t addr, uint64_t len, uint64_t base) {
    if (wb_off()) return;
    wblog("rec", addr, len, 3, 0);
    if (wb_skip(addr, len, 1)) return;
    lz_run rs[LZ_RUNS]; int nr = lz_split(addr, len, 1, rs);
    if (!nr) return;
    VkBuffer pb; VkDeviceSize po; uint64_t at;
    VkCommandBuffer cb = vks_cb();
    if (!base || !cb || !vkm_addr(addr, len, &pb, &po) || !wr_take((len + 15) & ~15ull, &at)) { gpu_wrote_runs(addr, len, nr, rs); return; }
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy c = {po, at, len}; vkCmdCopyBuffer(cb, pb, g_wr, 1, &c);
    vk_mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    gpu_pending_drain();
    if (nr < 0) { rs[0].a = addr; rs[0].n = len; nr = 1; }
    for (int i = 0; i < nr; i++) {   // the parts that are not the device's alone, each from the same two copies
        wb_t *w = calloc(1, sizeof *w); w->addr = rs[i].a; w->len = rs[i].n; w->kind = 3; w->base_at = base - 1 + (rs[i].a - addr); w->after_at = at + (rs[i].a - addr); w->ring_end = g_wr_end();
        gpu_pending(w->addr, w->len, 1);
        vks_post_fn(wb_land, w);
    }
}
// Writes whose bytes the CPU knows (inline uniform updates, fills): the GPU writes the mirror in order, guest memory gets the same bytes at completion
void vkm_cpu_also(uint64_t addr, uint64_t len, const void *data, int fill, uint32_t value) {
    lz_run rs[LZ_RUNS]; int nr = lz_split(addr, len, 0, rs);
    if (nr < 0) { rs[0].a = addr; rs[0].n = len; nr = 1; }
    gpu_pending_drain();
    for (int i = 0; i < nr; i++) {
        unsigned ph = (unsigned)((rs[i].a - addr) & 3) * 8;
        wb_t *w = calloc(1, sizeof *w); w->addr = rs[i].a; w->len = rs[i].n; w->kind = fill ? 2 : 1; w->fill = ph ? value >> ph | value << (32 - ph) : value;
        if (!fill) { uint8_t *c = malloc((size_t)w->len); memcpy(c, (const uint8_t *)data + (rs[i].a - addr), (size_t)w->len); w->cpu = c; }
        gpu_pending(w->addr, w->len, 1);
        vks_post_fn(wb_land, w);
    }
}
void vkm_lazy_stats(uint64_t *pages, uint64_t *set, uint64_t *in, uint64_t *skip_bytes, uint64_t *fetch_ns) {
    *pages = nvn_dirty_lazy_pages(); *set = atomic_load(&s_lz_set); *in = atomic_load(&s_lz_in); *skip_bytes = atomic_load(&s_lz_skip_b); *fetch_ns = atomic_load(&s_lz_fetch_ns);
}

// ---------- The memory pool table: raw global memory ----------
// A shader that reads or writes memory at an address it loaded from memory (not at a bound buffer's) cannot be given a buffer for it: the translator (nxnative's
// GlobalToStorage.TryGenerateRawStorageOp, docs/reference/shaders.md "Global memory at an address loaded from memory"; MIT, (c) seedds) makes such an access look the
// address up in a table of the memory pools the host binds as a storage buffer, add that pool's delta and go through a physical storage buffer pointer. The table
// (dmsh_abi.h DMSH_GMEM_MAP_*): 8192 entries of {start, end, delta, hint}, 64 bits each, sorted by start; entry 0 is empty and unused entries start at ~0. Here the
// delta is the pool mirror's device address minus the guest address. Donkey Kong Bananza's terrain depends on it: gsys's vertex shader c9a56f796ea4a4cd writes
// {count 0, 1 instance} every frame into each chunk's indirect draw record, whose address it reads from a list (docs/NXNATIVE_GAP_ANALYSIS.md).
// A pool is listed once its buffer has memory (sparse: always; the unbacked parts read undefined and drop writes, as Vulkan defines). The table is written anew when
// a pool comes, goes or gets memory, into the next of GM_TABLES buffers, so that work already submitted keeps the table it was recorded with.
// The write log (DMSH_GMEM_LOG_*): word 0 set when the set was full, then a hash set of 1024 guest page numbers a store or atomic went to. One log for all draws,
// taken and cleared once a frame: the replay learns from it which pages such shaders write and has them copied back to guest memory (vk_replay.c gm_frame).
// Both buffers are in device memory: a shader looks the table up at every access and logs every store (Ocarina of Time's movie shaders: 384 loads and 384 stores
// each, over whole pictures). Kept in host memory first, those accesses went across the bus and one dispatch ran into the GPU's time limit (device lost). So the
// table is written through the staging ring in the submission's upload command buffer, and the log is copied to a small host buffer and cleared there once a frame,
// before the frame's first use of it; the copy is read when its submission completes (gm_log_land).
enum { GM_TABLES = 4, GM_BYTES = DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY, GM_LOG_WORDS = 1 + (1 << DMSH_GMEM_LOG_LOG2) };
static struct { VkBuffer buf[GM_TABLES], log, log_rb; vk_alloc mem[GM_TABLES], logmem, rbmem; int state, cur, log_used; uint32_t pgen, bgen, entries; uint64_t log_frame; } g_gm;   // state: 0 not tried, 1 ready, 2 unavailable
static SRWLOCK g_gm_lk = SRWLOCK_INIT; static uint32_t g_gm_pend[GM_LOG_WORDS]; static int g_gm_npend, g_gm_full;   // logged pages landed, not taken yet
static int gm_buffer(VkDeviceSize n, VkBuffer *b, vk_alloc *m, const char *name, int host) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, b) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, *b, &mr);
    if (host ? (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, m) || !m->map) : vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, m)) return -1;
    vkBindBufferMemory(g_vk.dev, *b, m->mem, m->off); if (host) memset(m->map, 0, (size_t)n); vk_name((uint64_t)*b, VK_OBJECT_TYPE_BUFFER, name);
    return 0;
}
static int gm_ready(void) {
    if (g_gm.state) return g_gm.state == 1;
    g_gm.state = 2;
    if (!g_vk.f12.bufferDeviceAddress) { VKRED("raw global memory: the device has no buffer device addresses; shaders that read or write through loaded pointers get an empty table"); return 0; }
    for (int i = 0; i < GM_TABLES; i++) if (gm_buffer(GM_BYTES, &g_gm.buf[i], &g_gm.mem[i], "raw global memory: pool table", 0)) { VKRED("raw global memory: no memory for the pool table"); return 0; }
    if (gm_buffer(GM_LOG_WORDS * 4, &g_gm.log, &g_gm.logmem, "raw global memory: write log", 0) || gm_buffer(GM_LOG_WORDS * 4, &g_gm.log_rb, &g_gm.rbmem, "raw global memory: write log copy", 1)) { VKRED("raw global memory: no memory for the write log"); return 0; }
    g_gm.state = 1; g_gm.cur = -1;
    return 1;
}
// The table for the submission being recorded (the recording thread only); VK_NULL_HANDLE when the device cannot do it
VkBuffer vkm_gmem_table(VkDeviceSize *size) {
    *size = GM_BYTES;
    if (!gm_ready()) return VK_NULL_HANDLE;
    uint32_t pg = atomic_load(&g_pool_gen), bg = atomic_load(&g_bind_gen);
    if (g_gm.cur >= 0 && pg == g_gm.pgen && bg == g_gm.bgen) return g_gm.buf[g_gm.cur];
    VkCommandBuffer up = vks_upload_cb(); VkBuffer sb; VkDeviceSize so;
    uint64_t *t = up ? vkm_staging(GM_BYTES, 16, &sb, &so) : NULL;
    if (!t) return g_gm.cur >= 0 ? g_gm.buf[g_gm.cur] : VK_NULL_HANDLE;   // outside a submission: the table as it was
    int nx = (g_gm.cur + 1) % GM_TABLES; uint32_t n = 1, left = 0; uint64_t last_end = 0;
    t[0] = t[1] = t[2] = t[3] = 0;
    AcquireSRWLockShared(&g_lk);
    for (int i = 0; i < g_npool; i++) { pool_t_ *p = g_pool[i];
        if (!p->sparse && !p->bound) continue;
        for (int k = 0; k < p->nwin; k++) {
            uint64_t lo = p->base + p->win_lo[k], hi = k + 1 < p->nwin ? p->base + p->win_lo[k + 1] : p->base + p->size;
            if (lo < last_end) lo = last_end;   // pools the game laid over each other: the first one has the overlap
            if (lo >= hi) continue;
            if (n == DMSH_GMEM_MAP_ENTRIES) { left++; continue; }
            if (!p->da[k]) { VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, p->buf[k]}; p->da[k] = vkGetBufferDeviceAddress(g_vk.dev, &ai); }
            t[4 * n] = lo; t[4 * n + 1] = hi; t[4 * n + 2] = p->da[k] - (p->base + p->win_lo[k]); t[4 * n + 3] = 0; n++; last_end = hi;
        }
    }
    ReleaseSRWLockShared(&g_lk);
    for (uint32_t i = n; i < DMSH_GMEM_MAP_ENTRIES; i++) { t[4 * i] = ~0ull; t[4 * i + 1] = 0; t[4 * i + 2] = 0; t[4 * i + 3] = 0; }
    // The hints (dmsh_abi.h DMSH_GMEM_HINT_SHIFT; nxnative nvn_mtl.m gmem_map, MIT, (c) seedds): a 2 MiB guest chunk wholly inside an entry is named in the hint word of
    // entry (chunk & mask), so a shader finds its pool with one load instead of the search's thirteen. A chunk whose place is taken, or an address past the tag's reach, searches
    for (uint32_t i = 1; i < n; i++) {
        uint64_t c0 = (t[4 * i] + ((1ull << DMSH_GMEM_HINT_SHIFT) - 1)) >> DMSH_GMEM_HINT_SHIFT, c1 = t[4 * i + 1] >> DMSH_GMEM_HINT_SHIFT;
        for (uint64_t c = c0; c < c1 && c < (1ull << 19); c++) { uint64_t *h = &t[4 * (c & (DMSH_GMEM_MAP_ENTRIES - 1)) + 3]; if (!(uint32_t)*h) *h = (c * DMSH_GMEM_MAP_ENTRIES | i) & 0xffffffffu; }
    }
    VkBufferCopy c = {so, 0, GM_BYTES}; vkCmdCopyBuffer(up, sb, g_gm.buf[nx], 1, &c);
    if (left) { static int said; if (!said++) VKRED("raw global memory: %u more memory pools than the table's %d entries; reads through loaded pointers into them give 0 (missing geometry)", left, DMSH_GMEM_MAP_ENTRIES); }
    { static int said; if (!said++) VKLOG("raw global memory: a shader reads or writes through pointers it loaded from memory; the memory pool table has %u ranges (written again when pools change)", n - 1); }
    g_gm.cur = nx; g_gm.pgen = pg; g_gm.bgen = bg; g_gm.entries = n - 1;
    return g_gm.buf[nx];
}
static void gm_log_land(void *arg) {   // the submission that copied the log out has completed: its pages wait for vkm_gmem_log_take
    (void)arg; const uint32_t *w = g_gm.rbmem.map;
    AcquireSRWLockExclusive(&g_gm_lk);
    if (w[0]) g_gm_full = 1;
    for (int i = 1; i < GM_LOG_WORDS; i++) if (w[i]) { int k = 0; while (k < g_gm_npend && g_gm_pend[k] != w[i]) k++; if (k == g_gm_npend && g_gm_npend < GM_LOG_WORDS) g_gm_pend[g_gm_npend++] = w[i]; }
    ReleaseSRWLockExclusive(&g_gm_lk);
}
VkBuffer vkm_gmem_log(VkDeviceSize *size) {
    *size = GM_LOG_WORDS * 4;
    if (!gm_ready()) return VK_NULL_HANDLE;
    VkCommandBuffer up = vks_upload_cb();
    if (up && g_gm.log_frame != g_frame) {   // once a frame, ahead of the frame's first use: last frame's entries out, the log empty
        g_gm.log_frame = g_frame;
        vk_mem_barrier(up, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (g_gm.log_used) { VkBufferCopy c = {0, 0, GM_LOG_WORDS * 4}; vkCmdCopyBuffer(up, g_gm.log, g_gm.log_rb, 1, &c); vks_post_fn(gm_log_land, NULL); }
        vkCmdFillBuffer(up, g_gm.log, 0, GM_LOG_WORDS * 4, 0);
        g_gm.log_used = 1;
    }
    return g_gm.log_used ? g_gm.log : VK_NULL_HANDLE;
}
// The guest pages logged and landed since the last call (at most max). *full: a log overflowed, some pages are not in it
int vkm_gmem_log_take(uint64_t *pages, int max, int *full) {
    *full = 0; if (g_gm.state != 1) return 0;
    AcquireSRWLockExclusive(&g_gm_lk);
    int n = g_gm_npend < max ? g_gm_npend : max; for (int i = 0; i < n; i++) pages[i] = g_gm_pend[i];
    g_gm_npend = 0; *full = g_gm_full; g_gm_full = 0;
    ReleaseSRWLockExclusive(&g_gm_lk);
    return n;
}

// ---------- Frames, statistics ----------
void vkm_stats(vkm_stats_t *s) {
    memset(s, 0, sizeof *s);
    AcquireSRWLockShared(&g_lk);
    for (int i = 0; i < g_npool; i++) { s->pools++; s->pool_bytes += g_pool[i]->hi - g_pool[i]->lo; s->bound_bytes += g_pool[i]->bound; s->sparse_pools += g_pool[i]->sparse; s->hot_pages += g_pool[i]->hot_n; }
    ReleaseSRWLockShared(&g_lk);
    s->up_bytes = atomic_load(&s_up_bytes); s->up_pages = atomic_load(&s_up_pages); s->up_runs = atomic_load(&s_up_runs); s->up_ns = atomic_load(&s_up_ns);
    s->sync_calls = atomic_load(&s_sync); s->staging_bytes = atomic_load(&s_st_bytes); s->staging_overflow = atomic_load(&s_st_ovf); s->staging_waits = atomic_load(&s_st_waits);
    s->writeback_bytes = atomic_load(&s_wb_bytes); s->wb_words = atomic_load(&s_wb_words); s->wb_kept = atomic_load(&s_wb_kept); s->wb_stale = atomic_load(&s_wb_stale); s->wb_merged = atomic_load(&s_wb_merged); s->uncommitted_pages = atomic_load(&s_uncommitted);
    nvn_dirty_stats(&s->faults, &s->preps);
    uint64_t mc, mpg; nvn_dirty_mp_stats(&mc, &mpg, &s->fault_ns);   // the tracker's protection changes (faults and protects)
}

void vkm_frame_tick(uint64_t frame) {
    static vkm_stats_t prev; static uint64_t prev_frame, t_prev;
    gpu_pending_drain();
    g_frame = frame + 1; g_budget_used = 0; g_seen_on = vkc_int("use_seen", 1);
    if (frame % HOT_COOL_FRAMES == 0) {   // hot pages cool down: protected again at their next copy; written again, they come back within HOT_STREAK frames
        AcquireSRWLockShared(&g_lk);
        for (int i = 0; i < g_npool; i++) { pool_t_ *p = g_pool[i]; for (uint64_t k = 0; k < p->npages; k++) if (p->hot[k]) { p->hot[k] = 0; p->up[k] = 0; p->streak[k] = 0; } p->hot_n = 0; }   // copied (and protected) again at the next sync
        ReleaseSRWLockShared(&g_lk);
    }
    if (frame % 60 || frame == prev_frame) return;
    vkm_stats_t s; vkm_stats(&s);
    uint64_t df = frame - prev_frame, now = now_ns(); double secs = t_prev ? (now - t_prev) / 1e9 : 0;
    uint64_t budget = 0, usage = 0; vk_budget(&budget, &usage);
    VKLOG("memory up to frame %llu: %llu pools %.1f MB (backed %.1f MB, %llu sparse); per frame: uploaded %.1f KB in %.1f runs (%.3f ms CPU), %.1f write faults (protection changes %.3f ms), "
          "%.1f host pre-writes, written back %.1f KB (%.0f words changed, %.0f kept for the CPU, %.1f uses while one was on its way, %.0f words sent for them), %.0f MB of uncommitted pool stepped over; hot pages %llu; staging overflow %llu waits %llu; video memory %.0f / %.0f MB%s",
          (unsigned long long)frame, (unsigned long long)s.pools, s.pool_bytes / 1048576.0, s.bound_bytes / 1048576.0, (unsigned long long)s.sparse_pools,
          (s.up_bytes - prev.up_bytes) / 1024.0 / df, (double)(s.up_runs - prev.up_runs) / df, (s.up_ns - prev.up_ns) / 1e6 / df,
          (double)(s.faults - prev.faults) / df, (s.fault_ns - prev.fault_ns) / 1e6 / df, (double)(s.preps - prev.preps) / df,
          (s.writeback_bytes - prev.writeback_bytes) / 1024.0 / df, (double)(s.wb_words - prev.wb_words) / df, (double)(s.wb_kept - prev.wb_kept) / df, (double)(s.wb_stale - prev.wb_stale) / df, (double)(s.wb_merged - prev.wb_merged) / df, (s.uncommitted_pages - prev.uncommitted_pages) * 4096.0 / 1048576.0 / df, (unsigned long long)s.hot_pages, (unsigned long long)s.staging_overflow,
          (unsigned long long)s.staging_waits, usage / 1048576.0, budget / 1048576.0, secs > 0 ? "" : "");
    if (g_use_ph > 0) { static uint64_t pc, pp, pf, pcy; VKLOG("pending counts up to frame %llu, per frame: %.1f calls, %.0f pages, %.0f first (a page copied for its shadow), %.0f kcycles", (unsigned long long)frame, (double)(s_gp_calls - pc) / df, (double)(s_gp_pages - pp) / df, (double)(s_gp_first - pf) / df, (double)(s_gp_cyc - pcy) / df / 1000.0);
      pc = s_gp_calls; pp = s_gp_pages; pf = s_gp_first; pcy = s_gp_cyc; }
    { static uint64_t pset, pin, pskip, pns; uint64_t pages, set, in, skip, ns; vkm_lazy_stats(&pages, &set, &in, &skip, &ns);
      if (set) VKLOG("write-back on demand up to frame %llu: %llu pages are the device's; per frame: %.1f KB not copied back, %.2f pages handed over, %.2f read back for the CPU (%.3f ms waiting)",
                     (unsigned long long)frame, (unsigned long long)pages, (skip - pskip) / 1024.0 / df, (double)(set - pset) / df, (double)(in - pin) / df, (ns - pns) / 1e6 / df);
      pset = set; pin = in; pskip = skip; pns = ns; }
    prev = s; prev_frame = frame; t_prev = now;
    { pool_t_ *top[4] = {0}; AcquireSRWLockShared(&g_lk);
      for (int i = 0; i < g_npool; i++) { pool_t_ *q = g_pool[i]; for (int k = 0; k < 4; k++) if (!top[k] || q->up_b > top[k]->up_b) { for (int j = 3; j > k; j--) top[j] = top[j - 1]; top[k] = q; break; } }
      for (int k = 0; k < 4; k++) if (top[k] && top[k]->up_b >> 20) VKLOG("  pool %#llx+%llu MB: %llu MB uploaded since the last line (never copied %llu, hot %llu), span %d, %llu hot pages", (unsigned long long)top[k]->lo, (unsigned long long)(top[k]->size >> 20),
          (unsigned long long)(top[k]->up_b >> 20), (unsigned long long)(top[k]->up_never >> 20), (unsigned long long)(top[k]->up_hot >> 20), top[k]->span != NULL, (unsigned long long)top[k]->hot_n);
      for (int i = 0; i < g_npool; i++) g_pool[i]->up_b = g_pool[i]->up_never = g_pool[i]->up_hot = 0;
      ReleaseSRWLockShared(&g_lk); }
}

int vkm_init(void) {
    InitializeSRWLock(&g_st.lk);
    const char *e = getenv("NXRT_VK_UPLOAD_MB"); if (e && atoi(e) > 0) g_budget = (uint64_t)atoi(e) << 20;
    e = getenv("NXRT_VK_SCAN_PAGES"); if (e && atoi(e) > 0) g_scan_pages = (uint64_t)atoi(e);
    VkDeviceSize ring = g_budget < (16u << 20) ? (16u << 20) : g_budget > (256u << 20) ? (256u << 20) : g_budget;
    e = getenv("NXRT_VK_STAGING_MB"); if (e && atoi(e) > 0) ring = (VkDeviceSize)atoi(e) << 20;
    g_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if (g_vk.f12.bufferDeviceAddress) g_usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (g_vk.has.xfb) g_usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    if (g_vk.has.cond_render) g_usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    VkPhysicalDeviceMaintenance4Properties m4 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &m4};
    vkGetPhysicalDeviceProperties2(g_vk.pd, &p2);
    g_max_buffer = m4.maxBufferSize;
    e = getenv("NXRT_VK_MAX_BUFFER_MB");   // test switch: pretend the device's largest buffer is smaller (AMD's 2048, to try its pool windows on another card)
    if (e && atoi(e) > 0 && ((VkDeviceSize)atoi(e) << 20) < g_max_buffer) g_max_buffer = (VkDeviceSize)atoi(e) << 20;
    g_alias = g_vk.has.sparse_aliased && !(getenv("NXRT_VK_NO_ALIAS") && atoi(getenv("NXRT_VK_NO_ALIAS")));
    if (g_vk.has.sparse_buffer) {   // the granule is a whole number of the device's sparse blocks
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, GRAN, g_usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkBuffer b; VkMemoryRequirements mr;
        if (vkCreateBuffer(g_vk.dev, &bci, NULL, &b) == VK_SUCCESS) { vkGetBufferMemoryRequirements(g_vk.dev, b, &mr); vkDestroyBuffer(g_vk.dev, b, NULL);
            if (mr.alignment > g_gran) g_gran = mr.alignment; else g_gran = (GRAN + mr.alignment - 1) / mr.alignment * mr.alignment; }
    }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, ring, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &g_st.buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_st.buf, &mr);
    if (vk_alloc_mem(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_st.mem)) { VKRED("no host memory for the staging ring"); return -1; }
    vkBindBufferMemory(g_vk.dev, g_st.buf, g_st.mem.mem, g_st.mem.off);
    g_st.map = g_st.mem.map; g_st.size = ring;
    vk_name((uint64_t)g_st.buf, VK_OBJECT_TYPE_BUFFER, "staging ring");
    { void vk_process_memory_cap(void); vk_process_memory_cap(); }
    VKLOG("memory: staging ring %llu MB, upload budget %llu MB a frame, pools %s (granule %llu KB), largest buffer %.0f GB",
          (unsigned long long)(ring >> 20), (unsigned long long)(g_budget >> 20), g_vk.has.sparse_buffer ? "sparse" : "fully backed", (unsigned long long)(g_gran >> 10), g_max_buffer / 1073741824.0);
    return 0;
}

void vkm_shutdown(void) {
    AcquireSRWLockExclusive(&g_lk);
    for (int i = 0; i < g_npool; i++) pool_free(g_pool[i]);
    g_npool = 0;
    ReleaseSRWLockExclusive(&g_lk);
    AcquireSRWLockExclusive(&g_st.lk);
    for (unsigned k = g_st.qt; k != g_st.qh; k++) { unsigned i = k % 4096; if (g_st.q[i].ovf) { vkDestroyBuffer(g_vk.dev, g_st.q[i].ovf, NULL); vk_free_mem(&g_st.q[i].ovf_mem); } }
    g_st.qt = g_st.qh = 0;
    if (g_st.buf) { vkDestroyBuffer(g_vk.dev, g_st.buf, NULL); vk_free_mem(&g_st.mem); g_st.buf = VK_NULL_HANDLE; }
    ReleaseSRWLockExclusive(&g_st.lk);
}
