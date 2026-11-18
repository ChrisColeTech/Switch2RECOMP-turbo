/* The NVN layer's view of the runtime: what pokopia-native's host (nxrt) gave hle_nvn.c, gfx/ and vk/, on our kernel and services.
   Guest threads are real host threads here and the SDK runs for real (docs/PLAN_NATIVE_GPU.md D1), so there is no guest core to give back
   around a host wait and no SDK function to replace: only logging, clocks, guest calls, the fault hooks and the names. */
#include "dm.h"
#include "hle_registry.h"
#include "ctl_gfx.h"
#include <stdarg.h>

/* runtime/nvn_hook.c: the part that sees rt.h */
void swr_nvn_log(const char *s);
void swr_nvn_fatal(const char *s);
void swr_nvn_frame(void);
uint64_t swr_nvn_slot_addr(unsigned index);
const char *swr_nvn_log_dir(void);
void swr_nvn_vi_crop(int c[4]);
NxwGuestCpu *swr_nvn_cur_cpu(void);
uint64_t swr_nvn_guest_call(uint64_t va, int n, const uint64_t *args);
uint64_t swr_nvn_guest_ns(void);
void swr_nvn_symbol(uint64_t a, char *out, size_t n);
int swr_nvn_quiet_faults(void);

uint64_t g_t0_ns;
struct dm_flags g_flag;
_Thread_local _Atomic int dm_host_work;
dm_gfx_ctl_ops dm_gfx_ctl;
__thread void **nvn_guard_cur;

void nxrt_logf(const char *fmt, ...) {
    char b[1600]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b - 2, fmt, a); va_end(a);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n] = '\n'; b[n + 1] = 0; swr_nvn_log(b);
}
void nxrt_die(const char *fmt, ...) {
    char b[1600]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    swr_nvn_fatal(b); ExitProcess(1);
}

uint64_t now_ns(void) { return plat_now_ns(); }
uint64_t guest_ns(void) { return swr_nvn_guest_ns(); }
void dm_guest_frame_tick(void) { swr_nvn_frame(); }
void dm_vi_layer_crop(int c[4]) { swr_nvn_vi_crop(c); }
NxwGuestCpu *nxrt_cur_cpu(void) { return swr_nvn_cur_cpu(); }
uint64_t nxrt_guest_call(uint64_t va, int n, const uint64_t *args) { return swr_nvn_guest_call(va, n, args); }
void dm_backtrace(uint64_t fp) { LOG("guest frame pointer %#llx", (unsigned long long)fp); }
module_t *module_of(const void *p) { (void)p; return NULL; }
void nxrt_symbolize(uint64_t a, char *out, size_t n) { swr_nvn_symbol(a, out, n); }
void nxrt_log_summary(void) {}
const char *nxrt_log_path(void) { return swr_nvn_log_dir(); }
uint32_t nxrt_hle_calls(const char *name) { (void)name; return 0; }

/* Names: a registry index is a slot of the synthetic range (nvn_hook.c) */
const nxrt_hle_entry *nxrt_hle_find(const char *name, int kind) {
    (void)kind;
    static struct { const char *k; int i; } *h; enum { HN = 4096 };
    if (!h) {
        void *t = calloc(HN, sizeof *h);
        for (unsigned i = 0; i < nxrt_hle_count; i++) {
            uint32_t v = 2166136261u; for (const char *c = nxrt_hle_table[i].name; *c; c++) v = (v ^ (uint8_t)*c) * 16777619u;
            __typeof__(h) e = (__typeof__(h))t + (v & (HN - 1));
            while (e->k) { e++; if (e == (__typeof__(h))t + HN) e = t; }
            e->k = nxrt_hle_table[i].name; e->i = (int)i;
        }
        h = t;
    }
    uint32_t v = 2166136261u; for (const char *c = name; *c; c++) v = (v ^ (uint8_t)*c) * 16777619u;
    for (uint32_t k = v & (HN - 1);; k = (k + 1) & (HN - 1)) { if (!h[k].k) return NULL; if (!strcmp(h[k].k, name)) return &nxrt_hle_table[h[k].i]; }
}
uint64_t nxrt_token_of(const char *name) {
    const nxrt_hle_entry *e = nxrt_hle_find(name, 0);
    return e ? swr_nvn_slot_addr((unsigned)(e - nxrt_hle_table)) : 0;
}
void nxrt_stub_log(const char *name, const char *file, uint8_t *seen, const NxwGuestCpu *cpu) {
    if (*seen) return;
    *seen = 1; LOG("[RED] NVN stub called: %s (%s), from %#llx", name, file, (unsigned long long)cpu->x[30]);
}

/* Fault hooks: the write-protect tracker (nvn_dirty.c), and nvn_guard.h's guarded reads */
enum { MAX_HOOKS = 8 };
static int (*g_hook[MAX_HOOKS])(void *, int, void *); static int g_nhook;
static void (*g_prep[MAX_HOOKS])(void *, size_t); static int g_nprep;
static int (*g_wp)(void *);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { if (g_nhook < MAX_HOOKS) g_hook[g_nhook++] = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { if (g_nprep < MAX_HOOKS) g_prep[g_nprep++] = fn; }
void dm_fault_wp_register(int (*fn)(void *)) { g_wp = fn; }
int dm_fault_is_wp(void *a) { return g_wp && g_wp(a); }
void dm_guest_write_prepare(void *a, size_t n) { for (int i = 0; i < g_nprep; i++) g_prep[i](a, n); }

#ifdef __linux__
/* Linux: the same order from a SIGSEGV handler; anything else goes on to the runtime's own handler */
static struct sigaction g_old_segv;
static void segv_nvn(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv; void *a = si->si_addr;
    int wr = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    for (int i = 0; i < g_nhook; i++) if (g_hook[i](a, wr, ucv)) return;
    void **b = nvn_guard_cur;
    if (b) { uc->uc_mcontext.gregs[REG_RBP] = (greg_t)b[0]; uc->uc_mcontext.gregs[REG_RIP] = (greg_t)b[1]; uc->uc_mcontext.gregs[REG_RSP] = (greg_t)b[2]; return; }
    if (g_old_segv.sa_flags & SA_SIGINFO) { if (g_old_segv.sa_sigaction) g_old_segv.sa_sigaction(sig, si, ucv); return; }
    if (g_old_segv.sa_handler == SIG_DFL || g_old_segv.sa_handler == SIG_IGN) { signal(sig, SIG_DFL); return; }
    g_old_segv.sa_handler(sig);
}
void nvn_host_init(void) { g_t0_ns = now_ns(); struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = segv_nvn; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK; sigemptyset(&sa.sa_mask); sigaction(SIGSEGV, &sa, &g_old_segv); }
#else
LONG plat_lazy_try(EXCEPTION_POINTERS *e);   /* plat.c: memory committed on first touch */
/* In this order: the write-protect tracker, lazily committed memory, then a guarded read gives up. Registered first, so before the fatal handler */
static LONG WINAPI veh_nvn(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    void *a = (void *)e->ExceptionRecord->ExceptionInformation[1];
    int wr = e->ExceptionRecord->ExceptionInformation[0] == 1;
    for (int i = 0; i < g_nhook; i++) if (g_hook[i](a, wr, e->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    void **b = nvn_guard_cur;
    if (!b) return EXCEPTION_CONTINUE_SEARCH;
    if (plat_lazy_try(e) == EXCEPTION_CONTINUE_EXECUTION) return EXCEPTION_CONTINUE_EXECUTION;
    e->ContextRecord->Rbp = (DWORD64)b[0]; e->ContextRecord->Rip = (DWORD64)b[1]; e->ContextRecord->Rsp = (DWORD64)b[2];
    return EXCEPTION_CONTINUE_EXECUTION;
}
/* SWR_HOST_PROF=<start s>,<seconds>[,<file>]: samples the NVN translation thread (the one that replays the game's command buffers, g_host_focus_tid) 2000 times
   a second for that window and writes where it was: the instruction address when it is in this program, else the module it is in and the nearest return address into
   this program on its stack (which of our calls led there). tools/host_prof.py turns the file into a table by function (x86_64-w64-mingw32-nm on the runtime).
   Costs nothing unless set. The thread is suspended for a few microseconds per sample; its own time is otherwise unchanged */
extern volatile DWORD g_host_focus_tid;
extern volatile DWORD g_host_focus_tid2;   /* the thread that completes submissions (vk_submit.c): SWR_HOST_PROF_THREAD=completion samples it instead */
static DWORD WINAPI host_prof_main(LPVOID arg) {
    (void)arg; double start = 0, secs = 10; char path[1200] = "";
    sscanf(getenv("SWR_HOST_PROF"), "%lf,%lf,%1199[^\n]", &start, &secs, path);
    if (!path[0]) snprintf(path, sizeof path, "%s.hostprof.txt", swr_nvn_log_dir());
    HMODULE self = GetModuleHandleW(NULL); int mi_ = 0; uintptr_t lo = (uintptr_t)self, hi = lo;
    uintptr_t tlo = lo, thi = lo;   /* the code section: a return address is a stack word that points into it */
    { const IMAGE_DOS_HEADER *d = (const IMAGE_DOS_HEADER *)self; const IMAGE_NT_HEADERS *n = (const IMAGE_NT_HEADERS *)((const char *)self + d->e_lfanew); hi = lo + n->OptionalHeader.SizeOfImage; (void)mi_;
      const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(n); tlo = lo + sec[0].VirtualAddress; thi = tlo + sec[0].Misc.VirtualSize; }
    const char *which = getenv("SWR_HOST_PROF_THREAD"); volatile DWORD *tidp = which && which[0] == 'c' ? &g_host_focus_tid2 : &g_host_focus_tid;
    while ((now_ns() - g_t0_ns) / 1e9 < start || !*tidp) Sleep(50);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, *tidp);
    if (!th) { LOG("host profile: cannot open the translation thread"); return 0; }
    enum { MAXS = 1 << 16 }; struct { uintptr_t ip, ret; } *smp = calloc(MAXS, sizeof *smp); int n = 0;
    uint64_t t_end = now_ns() + (uint64_t)(secs * 1e9); FILETIME c0, e0, k0, u0, k1, u1; GetThreadTimes(th, &c0, &e0, &k0, &u0);
    while (now_ns() < t_end && n < MAXS) {
        uint64_t t = now_ns(); while (now_ns() - t < 480000) YieldProcessor();   /* ~0.5 ms, without the 1 ms sleep granularity */
        if (SuspendThread(th) == (DWORD)-1) break;
        CONTEXT cx; cx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(th, &cx)) { smp[n].ip = (uintptr_t)cx.Rip; smp[n].ret = 0;
            if (cx.Rip < lo || cx.Rip >= hi) { const uintptr_t *sp = (const uintptr_t *)cx.Rsp; MEMORY_BASIC_INFORMATION mb;   /* the first stack word that points into our code */
                if (VirtualQuery(sp, &mb, sizeof mb) && mb.State == MEM_COMMIT) { uintptr_t end = (uintptr_t)mb.BaseAddress + mb.RegionSize; for (int k = 0; k < 512 && (uintptr_t)(sp + k + 1) <= end; k++) if (sp[k] >= tlo && sp[k] < thi) { smp[n].ret = sp[k]; break; } } }
            n++; }
        ResumeThread(th);
    }
    GetThreadTimes(th, &c0, &e0, &k1, &u1); CloseHandle(th);
    double cpu = ((((uint64_t)u1.dwHighDateTime << 32 | u1.dwLowDateTime) - ((uint64_t)u0.dwHighDateTime << 32 | u0.dwLowDateTime)) + (((uint64_t)k1.dwHighDateTime << 32 | k1.dwLowDateTime) - ((uint64_t)k0.dwHighDateTime << 32 | k0.dwLowDateTime))) / 1e7;
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "# host profile of the NVN translation thread: %d samples in %.1f s, thread CPU %.2f s, image %llx..%llx\n", n, secs, cpu, (unsigned long long)lo, (unsigned long long)hi);
        for (int i = 0; i < n; i++) { char mod[260] = "-"; uintptr_t rel = smp[i].ip - lo;   /* in another module: the address from that module's start, and its path (its exports name the function) */
            if (smp[i].ip < lo || smp[i].ip >= hi) { HMODULE m; rel = smp[i].ip; if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)smp[i].ip, &m)) { GetModuleFileNameA(m, mod, sizeof mod); rel = smp[i].ip - (uintptr_t)m; } else snprintf(mod, sizeof mod, "?"); }
            fprintf(f, "%llx %llx %s\n", (unsigned long long)rel, (unsigned long long)(smp[i].ret ? smp[i].ret - lo : 0), mod); }
        fclose(f); LOG("host profile: %d samples written to %s (thread CPU %.2f s of %.1f s)", n, path, cpu, secs); }
    free(smp); return 0;
}
void nvn_host_init(void) { g_t0_ns = now_ns(); AddVectoredExceptionHandler(1, veh_nvn); if (getenv("SWR_HOST_PROF")) CloseHandle(CreateThread(NULL, 0, host_prof_main, NULL, 0, NULL)); }
#endif
void dm_gfx_shot_at(const char *path) { if (dm_gfx_ctl.shot_at) dm_gfx_ctl.shot_at(path); }

/* The clock and sleep of plat.h */
#ifdef __linux__
uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
uint64_t plat_realtime_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
void plat_sleep_ns(uint64_t ns) { struct timespec t = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)}; while (nanosleep(&t, &t) && errno == EINTR); }
void plat_yield(void) { sched_yield(); }
#else
uint64_t plat_now_ns(void) {   // called tens of thousands of times a frame: one multiply when the counter's rate divides a second (10 MHz on current Windows), else two divisions
    static LARGE_INTEGER f; static uint64_t mul; if (!f.QuadPart) { QueryPerformanceFrequency(&f); mul = 1000000000ull % (uint64_t)f.QuadPart ? 0 : 1000000000ull / (uint64_t)f.QuadPart; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    if (mul) return (uint64_t)c.QuadPart * mul;
    return (uint64_t)c.QuadPart / (uint64_t)f.QuadPart * 1000000000ull + (uint64_t)c.QuadPart % (uint64_t)f.QuadPart * 1000000000ull / (uint64_t)f.QuadPart;
}
uint64_t plat_realtime_ns(void) { FILETIME t; GetSystemTimePreciseAsFileTime(&t); return ((((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) - 116444736000000000ull) * 100; }
void plat_sleep_ns(uint64_t ns) {
    static __thread HANDLE h; if (!h) h = CreateWaitableTimerExW(NULL, NULL, 2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, 0x1F0003);
    LARGE_INTEGER d; d.QuadPart = -(LONGLONG)(ns / 100);
    if (h && d.QuadPart && SetWaitableTimer(h, &d, 0, NULL, NULL, 0)) WaitForSingleObject(h, INFINITE); else Sleep((DWORD)(ns / 1000000));
}
void plat_yield(void) { SwitchToThread(); }
#endif

/* The game profile keys that differ by title (game_profile.h; nxnative dm/games/<id>/game.tsv) */
extern uint64_t g_title_id;
int dm_game_tex_rewrite(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("NXRT_TEX_REWRITE");
        v = e && *e ? atoi(e) != 0 : g_title_id == 0x0400486024DEC000ull || g_title_id == 0x0100F2C0115B6000ull;
        if (v) LOG("game profile: textures the CPU rewrites after creating them are read again (tex_flush_upload, tex_dirty_upload)"); }
    return v;
}
/* The Switch 2's NVN answers, from the game's sdk (nxnative dm/host/loader.c) */
static uint64_t g_sdk_base, g_sdk_size;
void nvn_host_sdk(uint64_t base, uint64_t size) { g_sdk_base = base; g_sdk_size = size; }
int dm_sdk_ounce(void) {
    static int ounce = -1;
    if (ounce >= 0) return ounce;
    ounce = 0;
    const char *b = (const char *)(uintptr_t)g_sdk_base, *e = b + g_sdk_size;
    for (const char *q = b; q + 11 <= e; q++) if (*q == 's' && !memcmp(q, "spec: Ounce", 11)) { ounce = 1; break; }
    if (ounce) LOG("sdk: built for the Switch 2 (spec: Ounce)");
    return ounce;
}
// The game's NintendoSDK major, from its sdk's "NintendoSdk_nnSdk-<major>_<minor>_<micro>-Release". It picks the NVN API the device reports (hle_nvn.c nvn_api): SDK 10 and older are
// API 53, and games check it and create no device on a mismatch. Mario Kart 8 Deluxe (SDK 4.4.0) requires major == 53 and minor >= 105 before it creates its graphics system and the
// global it locks right after (main+0x1353408); told 55.15 it skipped both and crashed on that NULL. 19 when the sdk has no such string, the value this used to return for every game
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 19;
    static const char k[] = "NintendoSdk_nnSdk-";
    const char *b = (const char *)(uintptr_t)g_sdk_base, *e = b + g_sdk_size;
    for (const char *q = b; q + sizeof k < e; q++)
        if (*q == 'N' && !memcmp(q, k, sizeof k - 1) && q[sizeof k - 1] >= '0' && q[sizeof k - 1] <= '9') {
            int v = 0; for (const char *d = q + sizeof k - 1; d < e && *d >= '0' && *d <= '9' && v < 1000; d++) v = v * 10 + (*d - '0');
            major = v; break;
        }
    LOG("sdk: NintendoSDK %d", major);
    return major;
}
// The constants NVN2's own nvnDeviceGetInteger answers: the driver is compiled into the game's sdk, and its API and GPU code versions move with the SDK (20.5.19: API 231.30, GPU code minor 17;
// 21.4.4: 231.39 and 20). Games check them: Splatoon Raiders requires minor >= 39 and otherwise creates no graphics system (main+0x31b86b0). The driver's switch over the items loads w0 with the
// major first, then jumps through a byte table: mov w0, #231 / adr / adrp + add (the table) / ldrb / add, lsl #2 / br, with cmp wN, #<last item> before it. A case that is a constant is mov w0, #v; b.
// Returns 1 with *out set when item has such a case; 0 for a case that reads something else (most items go through the driver's table lookup or read the GPU's description), or no such switch
int dm_sdk_nvn2_item(unsigned item, int *out) {
    static const uint8_t *table; static const uint32_t *cases; static unsigned last; static int found = -1;
    if (found < 0) {
        found = 0;
        {
            const uint32_t *t = (const uint32_t *)(uintptr_t)g_sdk_base; size_t n = g_sdk_size / 4;
            for (size_t p = 8; p + 9 < n && !found; p++) if (t[p] == 0x52801CE0) {   // mov w0, #231
                const uint8_t *tb = NULL; const uint32_t *adr = NULL; int lim = -1, ldrb = 0, br = 0;
                for (size_t q = p - 8; q <= p + 8; q++) {
                    uint32_t w = t[q];
                    if ((w & 0xFFC0001F) == 0x7100001F) lim = (w >> 10) & 0xFFF;   // cmp wN, #imm
                    else if ((w & 0x9F000000) == 0x90000000 && (t[q + 1] & 0xFFC00000) == 0x91000000) {   // adrp + add
                        int64_t pg = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); if (pg & 0x100000) pg -= 0x200000;
                        tb = (const uint8_t *)(((uintptr_t)&t[q] & ~(uintptr_t)0xFFF) + (uintptr_t)(pg * 4096) + ((t[q + 1] >> 10) & 0xFFF));
                    } else if ((w & 0x9F000000) == 0x10000000) {   // adr
                        int64_t off = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); if (off & 0x100000) off -= 0x200000;
                        adr = (const uint32_t *)((const char *)&t[q] + off);
                    } else if ((w & 0xFFE0FC00) == 0x38606800) ldrb = 1;   // ldrb wK, [xT, xI]
                    else if ((w & 0xFFFFFC1F) == 0xD61F0000) br = 1;       // br
                }
                if (!tb || !adr || lim <= 1 || !ldrb || !br) continue;
                const char *ro = (const char *)(uintptr_t)g_sdk_base, *ro_end = ro + g_sdk_size;
                if ((const char *)tb < ro || (const char *)tb + lim >= ro_end) continue;
                table = tb; cases = adr; last = (unsigned)lim; found = 1;
            }
        }
        if (dm_sdk_ounce() && !found) LOG("[RED] sdk: NVN2's nvnDeviceGetInteger switch not found; the device answers the newest versions known");
    }
    if (!found || item > last) return 0;
    const uint32_t *c = cases + table[item];
    if ((c[0] & 0xFFE0001F) != 0x52800000 || (c[1] & 0xFC000000) != 0x14000000) return 0;   // mov w0, #imm16; b
    *out = (int)((c[0] >> 5) & 0xFFFF);
    return 1;
}

