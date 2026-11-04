// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded capability check for the VM primitives the native heap needs.
// Only mappings owned by this process are protected, touched or replaced.
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <initializer_list>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef NXRT_TEST_PS5
extern "C" {
int sceKernelReserveVirtualRange(void**, size_t, int, size_t);
int64_t sceKernelGetDirectMemorySize();
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t*);
int sceKernelMapDirectMemory(void**, size_t, int, int, int64_t, size_t);
int sceKernelReleaseDirectMemory(int64_t, size_t);
int sceKernelEnableDmemAliasing();
}
#endif
#ifndef __FreeBSD__
#include <sys/syscall.h>
#endif

namespace {
sigjmp_buf fault_return;
volatile sig_atomic_t expected_fault = 0;
void fault(int signal) {
    if (expected_fault) siglongjmp(fault_return, 1);
    _exit(128 + signal);
}
bool can_write(volatile unsigned char* address) {
    if (sigsetjmp(fault_return, 1)) { expected_fault = 0; return false; }
    expected_fault = 1;
    *address = 0x5a;
    expected_fault = 0;
    return true;
}
void check(const char* name, bool ok, int& failures) {
    std::printf("%s %s errno=%d\n", name, ok ? "PASS" : "FAIL", errno);
    if (!ok) ++failures;
}
}

int main() {
#ifdef NXRT_TEST_PS5
    for (const char* path : {"/data/pokopia-recomp", "/data/pokopia-recomp/tests"}) {
        if (mkdir(path, 0777) != 0 && errno != EEXIST) return 4;
    }
    char report[160];
    std::snprintf(report, sizeof report, "/data/pokopia-recomp/tests/memory-%lld-%d.log",
                  static_cast<long long>(std::time(nullptr)), static_cast<int>(getpid()));
    if (!std::freopen(report, "w", stdout)) return 5;
    if (dup2(fileno(stdout), fileno(stderr)) < 0) return 6;
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const long page = sysconf(_SC_PAGESIZE);
    std::printf("page_size=%ld\n", page);
    for (int resource : {RLIMIT_AS, RLIMIT_DATA, RLIMIT_RSS}) {
        struct rlimit limit{};
        if (getrlimit(resource, &limit) == 0)
            std::printf("rlimit_%d soft=%llu hard=%llu\n", resource,
                static_cast<unsigned long long>(limit.rlim_cur), static_cast<unsigned long long>(limit.rlim_max));
    }
    if (page <= 0 || page > 65536) return 7;
    const size_t size = static_cast<size_t>(page) * 4;
    int failures = 0;
    struct sigaction handler{}, old_segv{}, old_bus{};
    handler.sa_handler = fault;
    sigemptyset(&handler.sa_mask);
    if (sigaction(SIGSEGV, &handler, &old_segv) != 0) return 8;
    if (sigaction(SIGBUS, &handler, &old_bus) != 0) {
        sigaction(SIGSEGV, &old_segv, nullptr);
        return 9;
    }

#ifdef __FreeBSD__
    int fd = shm_open(SHM_ANON, O_RDWR, 0600);
#else
    int fd = static_cast<int>(syscall(SYS_memfd_create, "pokopia-vm-check", 0));
#endif
    check("anonymous_shared_object", fd >= 0, failures);
    if (fd >= 0) {
        const bool resized = ftruncate(fd, static_cast<off_t>(size)) == 0;
        check("shared_object_resize", resized, failures);
        if (resized) {
            auto* a = static_cast<unsigned char*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
            auto* b = static_cast<unsigned char*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
            check("shared_primary_map", a != MAP_FAILED, failures);
            check("shared_alias_map", b != MAP_FAILED, failures);
            if (a != MAP_FAILED && b != MAP_FAILED) {
                a[0] = 13; b[page] = 29;
                check("aliases_share_writes", b[0] == 13 && a[page] == 29, failures);
                const bool readonly = mprotect(a, static_cast<size_t>(page), PROT_READ) == 0;
                check("shared_protect", readonly, failures);
                if (readonly) {
                    check("primary_write_faults", !can_write(a), failures);
                    check("alias_keeps_write_permission", can_write(b), failures);
                    check("adjacent_host_page_stays_writable", can_write(a + page), failures);
                }
                // Report whether guest 4 KiB protection can be represented by
                // the host. A larger host granule is a porting constraint.
                mprotect(a, size, PROT_READ | PROT_WRITE);
                errno = 0;
                const int result = mprotect(a, 4096, PROT_READ);
                std::printf("protect_4k_rc=%d errno=%d\n", result, errno);
                if (result == 0) {
                    const bool next_guest = can_write(a + 4096);
                    std::printf("independent_4k_protection=%d\n", next_guest ? 1 : 0);
                }
            }
            if (a != MAP_FAILED) munmap(a, size);
            if (b != MAP_FAILED) munmap(b, size);
        }
        close(fd);
    }

    constexpr uintptr_t address = 0x7100000000ULL;
    void* wanted = reinterpret_cast<void*>(address);
    void* fixed = mmap(wanted, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("guest_base_reservation", fixed == wanted, failures);
    if (fixed != MAP_FAILED) {
        if (fixed == wanted) {
            const bool committed = mprotect(fixed, 65536, PROT_READ | PROT_WRITE) == 0;
            check("guest_base_commit", committed, failures);
            if (committed) {
                auto* p = static_cast<unsigned char*>(fixed);
                p[0] = 81;
                void* collision = mmap(wanted, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                check("reservation_does_not_replace", collision != wanted && p[0] == 81, failures);
                if (collision != MAP_FAILED && collision != wanted) munmap(collision, 65536);
                void* replaced = mmap(fixed, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                check("owned_range_decommit", replaced == fixed, failures);
                if (replaced == fixed && mprotect(fixed, 65536, PROT_READ | PROT_WRITE) == 0)
                    check("recommit_zeroes_private_page", p[0] == 0, failures);
                else ++failures;
            }
        }
        munmap(fixed, 65536);
    }
#ifdef NXRT_TEST_PS5
    // The second ordinary 4 GiB anonymous reservation returned ENOMEM on the
    // target. Check native sparse reservations separately.
    constexpr uintptr_t bases[] = {0x7c00000000ULL, 0x7e00000000ULL, 0x7800000000ULL, 0x6000000000ULL};
    constexpr size_t spans[] = {0x100000000ULL, 0x100000000ULL, 0x300000000ULL, 65536};
    void* reservations[4]{};
    bool all_reserved = true;
    for (unsigned i = 0; i < 4; ++i) {
        void* at = reinterpret_cast<void*>(bases[i]);
        const int result = sceKernelReserveVirtualRange(&at, spans[i], 0, static_cast<size_t>(page));
        std::printf("native_reservation_%u rc=%08x requested=%p actual=%p bytes=%zu\n",
                    i, static_cast<unsigned>(result), reinterpret_cast<void*>(bases[i]), at, spans[i]);
        if (result == 0) reservations[i] = at;
        if (result != 0 || reinterpret_cast<uintptr_t>(at) != bases[i]) all_reserved = false;
    }
    check("native_full_address_space", all_reserved, failures);
    if (all_reserved) {
        int64_t physical = -1;
        const int allocated = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                                          65536, static_cast<size_t>(page), 12, &physical);
        std::printf("native_allocate_64k rc=%08x\n", static_cast<unsigned>(allocated));
        check("native_small_backing", allocated == 0, failures);
        if (allocated == 0) {
            const int enabled = sceKernelEnableDmemAliasing();
            std::printf("native_enable_aliasing rc=%08x\n", static_cast<unsigned>(enabled));
            void* primary = reservations[2];
            void* alias = reservations[3];
            const int mapped = sceKernelMapDirectMemory(&primary, 65536, PROT_READ | PROT_WRITE,
                                                       MAP_FIXED, physical, static_cast<size_t>(page));
            const int aliased = sceKernelMapDirectMemory(&alias, 65536, PROT_READ | PROT_WRITE,
                                                        MAP_FIXED, physical, static_cast<size_t>(page));
            std::printf("native_map rc=%08x alias_rc=%08x\n", static_cast<unsigned>(mapped), static_cast<unsigned>(aliased));
            const bool views_ok = mapped == 0 && aliased == 0 && primary == reservations[2] && alias == reservations[3];
            check("native_shared_views", views_ok, failures);
            if (views_ok) {
                auto* a = static_cast<volatile unsigned char*>(primary);
                auto* b = static_cast<volatile unsigned char*>(alias);
                a[0] = 19; b[page] = 37;
                check("native_alias_writes", b[0] == 19 && a[page] == 37, failures);
            }
            // Unmap both views before releasing the physical allocation.
            for (unsigned i : {2u, 3u}) {
                if (reservations[i] && munmap(reservations[i], spans[i]) == 0) reservations[i] = nullptr;
                else ++failures;
            }
            if (!reservations[2] && !reservations[3])
                check("native_release_backing", sceKernelReleaseDirectMemory(physical, 65536) == 0, failures);
        }
    }
    for (unsigned i = 0; i < 4; ++i)
        if (reservations[i]) check("native_release_reservation", munmap(reservations[i], spans[i]) == 0, failures);
#endif
    sigaction(SIGBUS, &old_bus, nullptr);
    sigaction(SIGSEGV, &old_segv, nullptr);
    std::printf("memory_capability_failures=%d\n", failures);
    return failures ? 1 : 0;
}
