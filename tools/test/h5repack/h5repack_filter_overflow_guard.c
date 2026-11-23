/*
 * Interposes calloc for h5repack's pack_info_t records and places an
 * inaccessible page immediately after each one. Preserving seven or more
 * filters indexes past the six-slot array in that record; the first access
 * falls on this page and the process dies with SIGSEGV. A repack that stays
 * inside the record runs to completion.
 *
 * Loaded with LD_PRELOAD. Not part of h5repack.
 */
#define _GNU_SOURCE

#include "h5repack.h"

#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct guarded {
    void  *map;
    size_t map_len;
    void  *user;
};

#define MAX_GUARDED 128

/* Caller buffer count presented to H5Pget_filter2 for the out-of-range slot.
 * Must be <= 256 (H5Pget_filter2 rejects a larger caller count) and large
 * enough that the file's client-data values extend past pack_info_t. */
enum { GUARD_CD_NELMTS = 80 };

_Static_assert(GUARD_CD_NELMTS <= 256, "H5Pget_filter2 rejects a caller count above 256");
_Static_assert(offsetof(pack_info_t, filter) + H5_REPACK_MAX_NFILTERS * sizeof(filter_info_t) +
                       offsetof(filter_info_t, cd_values) +
                       GUARD_CD_NELMTS * sizeof(unsigned) >
                   sizeof(pack_info_t),
               "client-data copy must extend past pack_info_t");

static struct guarded guarded[MAX_GUARDED];
static int           nguarded;
static int           guard_failed;

static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void (*real_free)(void *);
static void (*asan_poison)(const volatile void *, size_t);
static void (*asan_unpoison)(const volatile void *, size_t);
static int asan_resolved;

static char   bootstrap[8192];
static size_t bootstrap_used;
static int    resolving;

static void
resolve_libc(void)
{
    if (real_calloc != NULL)
        return;
    if (resolving)
        return;
    resolving   = 1;
    real_calloc  = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    real_free    = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    real_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    resolving    = 0;
    if (!asan_resolved) {
        asan_poison   = (void (*)(const volatile void *, size_t))dlsym(RTLD_DEFAULT, "__asan_poison_memory_region");
        asan_unpoison = (void (*)(const volatile void *, size_t))dlsym(RTLD_DEFAULT, "__asan_unpoison_memory_region");
        asan_resolved = 1;
    }
}

static void *
bootstrap_alloc(size_t bytes)
{
    size_t aligned = (bootstrap_used + 15u) & ~(size_t)15u;

    if (bytes > sizeof(bootstrap) || aligned > sizeof(bootstrap) - bytes)
        return NULL;
    bootstrap_used = aligned + bytes;
    return bootstrap + aligned;
}

static int
in_bootstrap(const void *ptr)
{
    const char *p = (const char *)ptr;

    return p >= bootstrap && p < bootstrap + sizeof(bootstrap);
}

static struct guarded *
find_guarded(const void *ptr)
{
    int i;

    for (i = 0; i < nguarded; i++) {
        if (guarded[i].user == ptr)
            return &guarded[i];
    }
    return NULL;
}

static void
drop_guarded(struct guarded *slot)
{
    int index = (int)(slot - guarded);

    if (munmap(slot->map, slot->map_len) != 0)
        guard_failed = 1;
    guarded[index] = guarded[nguarded - 1];
    nguarded--;
}

/*
 * Slot 6 of the filter array begins at the tail of pack_info_t and its
 * client-data count lies past the allocation. When AddressSanitizer is loaded,
 * that count is left readable and set to a value the copy accepts, and the
 * bytes between the allocation and that count are poisoned so the parameter
 * write is the access that faults. Otherwise the page after the record is
 * inaccessible and the out-of-range read faults.
 */
static void *
guard_calloc(size_t bytes)
{
    long      page;
    size_t    map_len;
    size_t    cd_off;
    void     *map;
    uintptr_t base;
    uintptr_t guard;
    uintptr_t user;
    int       use_poison;

    if (nguarded >= MAX_GUARDED) {
        guard_failed = 1;
        return NULL;
    }

    page = sysconf(_SC_PAGESIZE);
    if (page < 1)
        return NULL;

    cd_off = offsetof(pack_info_t, filter) + (size_t)H5_REPACK_MAX_NFILTERS * sizeof(filter_info_t) +
             offsetof(filter_info_t, cd_nelmts);
    use_poison = asan_poison != NULL && asan_unpoison != NULL && cd_off > bytes;

    map_len = (size_t)page + bytes + (size_t)page + (use_poison ? cd_off : 0);
    map_len = (map_len + (size_t)page - 1u) & ~((size_t)page - 1u);
    map     = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
        return NULL;

    base = (uintptr_t)map;
    if (use_poison) {
        /* Leave room after the record for the poisoned tail and the seeded count. */
        user = (base + (uintptr_t)page + 15u) & ~(uintptr_t)15u;
        if (user + cd_off + sizeof(size_t) > base + map_len) {
            munmap(map, map_len);
            return NULL;
        }
        memset((void *)user, 0, bytes);
        *(size_t *)((char *)user + cd_off) = GUARD_CD_NELMTS;
        asan_poison((char *)user + bytes, cd_off - bytes);
        asan_unpoison((char *)user + cd_off, sizeof(size_t));
    }
    else {
        guard = (base + map_len) - (uintptr_t)page;
        user  = (guard - bytes) & ~(uintptr_t)15u;
        if (user < base) {
            munmap(map, map_len);
            return NULL;
        }
        if (mprotect((void *)guard, (size_t)page, PROT_NONE) != 0) {
            munmap(map, map_len);
            return NULL;
        }
        memset((void *)user, 0, bytes);
    }

    guarded[nguarded].map     = map;
    guarded[nguarded].map_len = map_len;
    guarded[nguarded].user    = (void *)user;
    nguarded++;

    if (getenv("GUARD_DEBUG") != NULL) {
        fprintf(stderr, "guard pack_info_t %zu bytes at %p poison=%d cd_off=%zu\n", bytes, (void *)user,
                use_poison, cd_off);
    }
    return (void *)user;
}

__attribute__((visibility("default"), no_sanitize("address"))) void *
calloc(size_t nmemb, size_t size)
{
    size_t bytes;

    if (nmemb != 0 && size > (size_t)-1 / nmemb)
        return NULL;
    bytes = nmemb * size;

    if (real_calloc == NULL) {
        if (resolving)
            return bootstrap_alloc(bytes);
        resolve_libc();
        if (real_calloc == NULL)
            return bootstrap_alloc(bytes);
    }

    if (bytes == sizeof(pack_info_t)) {
        void *guarded_ptr = guard_calloc(bytes);

        if (guarded_ptr != NULL)
            return guarded_ptr;
        guard_failed = 1;
    }
    return real_calloc(nmemb, size);
}

__attribute__((visibility("default"), no_sanitize("address"))) void
free(void *ptr)
{
    struct guarded *slot;

    if (ptr == NULL || in_bootstrap(ptr))
        return;
    if (real_free == NULL)
        resolve_libc();

    slot = find_guarded(ptr);
    if (slot != NULL) {
        drop_guarded(slot);
        return;
    }
    if (real_free != NULL)
        real_free(ptr);
}

__attribute__((visibility("default"), no_sanitize("address"))) void *
realloc(void *ptr, size_t size)
{
    struct guarded *slot;

    if (real_realloc == NULL)
        resolve_libc();
    if (ptr == NULL)
        return real_realloc(NULL, size);

    slot = find_guarded(ptr);
    if (slot == NULL)
        return real_realloc(ptr, size);

    /*
     * pack_info_t itself is not resized. Keep the guard if a caller does
     * resize one of these records.
     */
    {
        void *next = size == sizeof(pack_info_t) ? guard_calloc(size) : real_calloc(1, size);

        if (next == NULL)
            return NULL;
        memcpy(next, ptr, size < sizeof(pack_info_t) ? size : sizeof(pack_info_t));
        drop_guarded(slot);
        return next;
    }
}

__attribute__((constructor)) static void
guard_loaded(void)
{
    const char *marker = getenv("GUARD_MARKER");
    FILE       *out;

    resolve_libc();
    if (marker == NULL)
        return;
    out = fopen(marker, "w");
    if (out == NULL)
        return;
    fprintf(out, "pack_info_t %zu\n", sizeof(pack_info_t));
    fclose(out);
}
