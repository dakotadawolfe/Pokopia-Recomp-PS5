// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/shader_words.h"
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
void check(bool ok) {
    ++checks;
    if (!ok) throw std::runtime_error("shader word check " + std::to_string(checks));
}
void write_words(const fs::path& path, uint32_t tag) {
    // Synthetic reader fixture, never extracted from a game. Only the reader's
    // SPIR-V container header is exercised here, not instruction validation.
    std::vector<uint32_t> data(1024, tag);
    data[0] = 0x07230203;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(uint32_t));
    if (!file) throw std::runtime_error("could not write test fixture");
}
void run(const fs::path& directory) {
    using Store = nxrt::ShaderWordStore;
    for (unsigned i = 0; i < 6; ++i) write_words(directory / (std::to_string(i) + ".spv"), i);
    auto path = [&](unsigned i) { return (directory / (std::to_string(i) + ".spv")).string(); };
    Store cache(8192);
    auto pinned = cache.acquire(path(0));
    check(pinned && pinned->size() == 1024 && pinned->back() == 0);
    check(cache.acquire(path(0)).get() == pinned.get());
    check(cache.stats().hits == 1);
    check(bool(cache.acquire(path(1))));
    check(bool(cache.acquire(path(0)))); // 1 is now the oldest.
    check(bool(cache.acquire(path(2))));
    check(cache.stats().resident_bytes == 8192 && cache.stats().entries == 2);
    const auto before = cache.stats().reads;
    check(bool(cache.acquire(path(0))));
    check(cache.stats().reads == before);
    check(bool(cache.acquire(path(1))));
    check(cache.stats().reads == before + 1);
    cache.release(path(0));
    check(pinned->back() == 0); // Eviction cannot invalidate a caller's handle.
    for (unsigned i = 0; i < 4096; ++i) {
        auto words = cache.acquire(path(i % 6));
        check(words && words->back() == i % 6 && cache.stats().resident_bytes <= 8192);
    }
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 8; ++t) workers.emplace_back([&, t] {
        for (unsigned i = 0; i < 128; ++i) {
            const unsigned tag = (i + t) % 6;
            auto words = cache.acquire(path(tag));
            std::this_thread::yield();
            if (!words || words->back() != tag || cache.stats().resident_bytes > 8192) ++failures;
        }
    });
    for (auto& worker : workers) worker.join();
    check(failures == 0 && pinned->back() == 0);
    Store oversized(64);
    check(bool(oversized.acquire(path(0))));
    check(oversized.stats().entries == 0 && oversized.stats().resident_bytes == 0);
    check(!cache.acquire((directory / "missing.spv").string()));
    const auto bad = directory / "bad.spv";
    { std::ofstream f(bad, std::ios::binary); f << "bad"; }
    check(!Store::read(bad.string()));
    { std::ofstream f(bad, std::ios::binary); f << std::string(24, '\0'); }
    check(!Store::read(bad.string()));
    { std::ofstream f(bad, std::ios::binary); f.seekp((64ll << 20)); f.put('\0'); }
    check(!Store::read(bad.string()));
    check(cache.stats().resident_bytes <= 8192);
}
}

int main() {
#if defined(NXRT_TARGET_PS5)
    const fs::path parent = "/data/pokopia-recomp/tests";
    fs::create_directories(parent);
    const auto report = parent / ("shader-words-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(getpid()) + ".log");
    if (!std::freopen(report.c_str(), "a", stdout) || !std::freopen(report.c_str(), "a", stderr)) return 4;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#else
    const fs::path parent = fs::temp_directory_path();
#endif
    const auto directory = parent / ("pkp-shader-words-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(getpid()));
    if (!fs::create_directory(directory)) return 4;
    int status = 0;
    std::puts("Shader word checks started");
    try { run(directory); }
    catch (const std::exception& error) { std::printf("FAIL %s\n", error.what()); status = 1; }
    // This directory was newly created by this process and contains only its
    // synthetic fixtures. Never remove inputs belonging to the game/user.
    std::error_code error;
    fs::remove_all(directory, error);
    if (error) { std::printf("Cleanup failed: %s\n", error.message().c_str()); status = 1; }
    std::printf("Shader word checks: checks=%u failures=%d\n", checks, status);
    return status;
}
