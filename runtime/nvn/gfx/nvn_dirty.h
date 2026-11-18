// from nxnative dm/gfx/nvn/nvn_dirty.h (MIT, (c) seedds). The nvn_dirty_lazy_* functions are this engine's.
// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);
void nvn_dirty_protect_span(const void *span, const void *addr, size_t len);   // the same through a kept handle (nvn_dirty_span): no search over every range              // Set the covered pages read-only
uint64_t nvn_dirty_now(void);                                      // Current generation: take the content's stamp before nvn_dirty_protect and before reading it
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp); // Covered pages were written after the stamp -> 1
const void *nvn_dirty_span(const void *addr, size_t len);          // The tracked range holding the span (a handle to keep; NULL = untracked)
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp);   // nvn_dirty_since through a kept handle (rechecked, scanned again only if it no longer holds the span)
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // Currently live slots, slot table high-water mark, total number of registrations
// Write-back on demand (nvn_dirty.c): pages the GPU writes and the CPU does not touch are made inaccessible; the first access brings the device's bytes in
void nvn_dirty_lazy_register(const void *(*fetch)(uintptr_t addr, size_t len), void (*done)(uintptr_t addr, size_t len, int fetched));   // fetch: the device's bytes of the pages, valid until done
int nvn_dirty_lazy_set(const void *span, const void *addr, size_t len);   // 1: the pages (all protected, in one range only) are the device's from now on
void nvn_dirty_lazy_in(const void *addr, size_t len, int fetch);          // the lazy pages of the range come in now (fetch 0: without their bytes, nothing was skipped)
void nvn_dirty_lazy_stats(uint64_t *pages, uint64_t *ins);                 // lazy pages now; pages brought in so far
uint64_t nvn_dirty_lazy_pages(void);
