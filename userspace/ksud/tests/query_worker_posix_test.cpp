#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "mount/query_worker.hpp"

namespace ksud {
bool switch_mnt_ns(int pid) {
    assert(pid == 1);
    return true;
}
}  // namespace ksud

int main() {
    using sumhp::mount::fsutil::run_query_in_init_mount_ns;
    if (!run_query_in_init_mount_ns([] { return true; })) {
        if (errno == ENOSYS || errno == EPERM || errno == EACCES) {
            std::cout << "POSIX smoke unavailable: pidfd denied/unsupported, errno=" << errno
                      << '\n';
            return 0;
        }
        assert(false && "unexpected query worker setup error");
    }
    assert(!run_query_in_init_mount_ns([] {
        errno = ESTALE;
        return false;
    }) && errno == ESTALE);
    assert(!run_query_in_init_mount_ns([]() -> bool { throw std::runtime_error("sample"); }) &&
           errno == EIO);
    auto* shared = static_cast<int*>(
        mmap(nullptr, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    assert(shared != MAP_FAILED);
    *shared = 0;
    assert(run_query_in_init_mount_ns([&] {
               *shared = 42;
               return true;
           }) &&
           *shared == 42);
    const auto start = std::chrono::steady_clock::now();
    assert(!run_query_in_init_mount_ns([] {
        for (;;)
            pause();
        return true;
    }) && errno == ETIMEDOUT);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed >= std::chrono::seconds(4) && elapsed < std::chrono::seconds(10));
    bool recovered = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (run_query_in_init_mount_ns([] { return true; })) {
            recovered = true;
            break;
        }
        assert(errno == EBUSY);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(recovered);
    munmap(shared, sizeof(int));
    std::cout << "POSIX fork/pidfd/timeout/reaping smoke passed; namespace entry is stubbed\n";
}
