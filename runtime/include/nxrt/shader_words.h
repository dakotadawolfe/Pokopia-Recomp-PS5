// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxrt {
// Acquired handles pin immutable words across eviction. The budget bounds
// idle residency; active pipeline builders can temporarily exceed it.
class ShaderWordStore {
public:
    using Words = std::vector<uint32_t>;
    using Handle = std::shared_ptr<const Words>;
    struct Stats { size_t resident_bytes, entries; uint64_t reads, hits; };
    explicit ShaderWordStore(size_t budget) : budget_(budget) {}

    static std::shared_ptr<Words> read(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return {};
        const auto length = file.tellg();
        // Bound malformed files before allocating; the shader loader still
        // performs reflection. Read directly into the final word allocation.
        constexpr std::streamoff maximum = 64ll << 20;
        if (length < 20 || length > maximum || static_cast<std::streamoff>(length) % 4) return {};
        auto words = std::make_shared<Words>(static_cast<size_t>(length) / 4);
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(words->data()), static_cast<std::streamsize>(length)) ||
            (*words)[0] != 0x07230203) return {};
        return words;
    }

    Handle acquire(const std::string& path) {
        {
            std::lock_guard lock(mutex_);
            if (auto it = entries_.find(path); it != entries_.end()) {
                order_.splice(order_.begin(), order_, it->second.position);
                ++hits_;
                return it->second.words;
            }
        }
        // No file I/O under the mutex. Racing first readers may each read,
        // but only one copy is published in the resident cache.
        Handle words = read(path);
        std::lock_guard lock(mutex_);
        ++reads_;
        if (!words) return {};
        if (auto it = entries_.find(path); it != entries_.end()) {
            order_.splice(order_.begin(), order_, it->second.position);
            return it->second.words;
        }
        const size_t bytes = words->size() * sizeof(uint32_t);
        if (bytes > budget_) return words;
        while (resident_bytes_ > budget_ - bytes) {
            auto it = entries_.find(order_.back());
            resident_bytes_ -= it->second.words->size() * sizeof(uint32_t);
            entries_.erase(it);
            order_.pop_back();
        }
        order_.push_front(path);
        try { entries_.emplace(path, Entry{words, order_.begin()}); }
        catch (...) { order_.pop_front(); throw; }
        resident_bytes_ += bytes;
        return words;
    }

    Stats stats() const {
        std::lock_guard lock(mutex_);
        return {resident_bytes_, entries_.size(), reads_, hits_};
    }

    void release(const std::string& path) {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(path);
        if (it == entries_.end()) return;
        resident_bytes_ -= it->second.words->size() * sizeof(uint32_t);
        order_.erase(it->second.position);
        entries_.erase(it);
    }

private:
    struct Entry { Handle words; std::list<std::string>::iterator position; };
    const size_t budget_;
    mutable std::mutex mutex_;
    std::list<std::string> order_;
    std::unordered_map<std::string, Entry> entries_;
    size_t resident_bytes_ = 0;
    uint64_t reads_ = 0, hits_ = 0;
};
} // namespace nxrt
