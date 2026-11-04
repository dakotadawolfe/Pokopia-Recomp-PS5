// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only fsp-srv bring-up. The guest parses its RomFS from IStorage; asset
// bytes come from the configured loose tree, never from synthetic success data.
// Wire layouts: libnx nx/source/services/fs.c and Switchbrew Filesystem services.
#include "fs_romfs.h"
#include "fs_volume.h"
#include "services_internal.h"
#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/subpage.h"
#include <atomic>
#include <limits>
#include <mutex>

namespace nxrt::ipc::detail {
uint64_t note_unhandled(Service& s, uint32_t cmd);
}

namespace nxrt::services {
namespace {
using ipc::Request;
using ipc::Response;
using ipc::Service;

// Unknown filesystem operations must fail even when an exploratory IPC config
// changes the generic stub result. Retain the ordinary IPC unhandled counters.
class ReadOnlyService final : public Service {
public:
    explicit ReadOnlyService(const char* name) : Service(name) {}
    uint32_t handle(Request& req, Response& rsp) override {
        if (cmds_.contains(req.cmd)) return Service::handle(req, rsp);
        if (ipc::detail::note_unhandled(*this, req.cmd) == 1)
            NXLOG_W("fs", "%s cmd %u is not ported", name().c_str(), req.cmd);
        return fs::rc::kNotImplemented;
    }
};

struct State {
    std::mutex mutex;
    const std::shared_ptr<kernel::KProcess> process = kernel::current_process();
    const std::string root = romfs_dir();
    std::shared_ptr<fs::RomfsImage> image;
    bool failed = false;
};
std::mutex state_mutex;
std::shared_ptr<State> state;
std::shared_ptr<State> current() {
    std::lock_guard lock(state_mutex);
    if (!state) state = std::make_shared<State>();
    return state;
}

std::shared_ptr<fs::RomfsImage> image_for(const std::shared_ptr<State>& state) {
    std::lock_guard lock(state->mutex);
    if (state->image) return state->image;
    if (state->failed) return nullptr;
    auto image = std::make_shared<fs::RomfsImage>();
    std::string error;
    if (!image->build(state->root, &error) || image->size() > static_cast<uint64_t>(INT64_MAX)) {
        NXLOG_E("fs", "Cannot serve RomFS '%s': %s", state->root.c_str(), error.c_str());
        state->failed = true;
        return nullptr;
    }
    const auto stats = image->stats();
    NXLOG_I("fs", "RomFS '%s': %u files, %u directories, %llu bytes of data, %llu image bytes", state->root.c_str(),
            stats.files, stats.dirs, static_cast<unsigned long long>(stats.data_bytes),
            static_cast<unsigned long long>(image->size()));
    state->image = image;
    return image;
}

uint32_t scalar_request(const Request& req, size_t bytes = 0) {
    if (!req.in_copy.empty() || !req.in_move.empty()) return kernel::kInvalidHandle;
    if (!req.a.empty() || !req.b.empty() || !req.w.empty() || !req.x.empty() ||
        !req.c.empty() || !req.in_services.empty()) return fs::rc::kInvalidArgument;
    // Plain CMIF includes trailing alignment bytes; domain input sizes are exact.
    return req.in_size < bytes || (bytes && !req.in) ? fs::rc::kInvalidArgument : 0;
}

std::shared_ptr<Service> storage(const std::shared_ptr<fs::RomfsImage>& image) {
    auto service = std::make_shared<ReadOnlyService>("nn::fssrv::sf::IStorage");
    service->add_cmd(0, "Read", [image](Request& req, Response&) -> uint32_t {
        if (!req.in || req.in_size < 2 * sizeof(int64_t)) return fs::rc::kInvalidArgument;
        const auto offset = req.arg<int64_t>(0), size = req.arg<int64_t>(8);
        if (offset < 0 || size < 0) return fs::rc::kOutOfRange;
        const auto at = static_cast<uint64_t>(offset), count = static_cast<uint64_t>(size);
        if (at > image->size() || count > image->size() - at) return fs::rc::kOutOfRange;
        if (!count) return 0;
        const auto out = req.out_buf(0);
        if (!out.addr || count > out.size) return fs::rc::kInvalidArgument;
        if (!mem::SubpageMap::allows(out.addr, count, 2)) return kernel::kInvalidMemoryState;
        return image->read(at, reinterpret_cast<void*>(out.addr), count) ? 0 : fs::rc::kGenericFailure;
    });
    service->add_cmd(1, "Write", [](Request&, Response&) -> uint32_t { return fs::rc::kReadOnlyFileSystem; });
    service->add_cmd(2, "Flush", [](Request& req, Response&) -> uint32_t {
        // This storage has no pending writes or dirty data buffers.
        return scalar_request(req);
    });
    service->add_cmd(3, "SetSize", [](Request&, Response&) -> uint32_t { return fs::rc::kReadOnlyFileSystem; });
    service->add_cmd(4, "GetSize", [image](Request&, Response& rsp) -> uint32_t {
        rsp.push<int64_t>(static_cast<int64_t>(image->size())); return 0;
    });
    service->add_cmd(5, "OperateRange", [image](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = scalar_request(req, 24)) return rc;
        const auto operation = req.arg<uint32_t>(0);
        const auto offset = req.arg<int64_t>(8), size = req.arg<int64_t>(16);
        if (operation <= 1) return fs::rc::kReadOnlyFileSystem;
        if (operation != 2 && operation != 3) return fs::rc::kNotImplemented;
        if (offset < 0 || size < 0 || size > INT64_MAX - offset) return fs::rc::kOutOfRange;
        if (operation == 2) {
            // SDK callers can invalidate the entire cache with size INT64_MAX.
            // There are no cached data bytes; release the cached read handles.
            image->invalidate_cache();
        } else {
            const auto at = static_cast<uint64_t>(offset), count = static_cast<uint64_t>(size);
            if (at > image->size() || count > image->size() - at) return fs::rc::kOutOfRange;
        }
        // QueryRangeInfo: no AES key flags or speed emulation on decrypted
        // host files; the remaining 0x38 bytes are reserved. Invalidate also
        // has this output on the IPC interface, without a queried range.
        rsp.out.resize(0x40, 0); return 0;
    });
    return service;
}

std::shared_ptr<Service> proxy() {
    const auto state = current();
    const auto bound = std::make_shared<std::atomic<bool>>(false);
    auto service = std::make_shared<ReadOnlyService>("fsp-srv");
    service->add_cmd(1, "SetCurrentProcess", [state, bound](Request& req, Response&) -> uint32_t {
        if (!req.in || req.in_size < sizeof(uint64_t) || !req.in_copy.empty() || !req.in_move.empty())
            return fs::rc::kInvalidArgument;
        if (!req.pid || req.pid != state->process->id || state->process != kernel::current_process())
            return kernel::kInvalidHandle;
        bound->store(true, std::memory_order_release);
        return 0;
    });
    const auto open = [state, bound](Request&, Response& rsp) -> uint32_t {
        if (!bound->load(std::memory_order_acquire)) return kernel::kInvalidState;
        const auto image = image_for(state);
        if (!image) return fs::rc::kTargetNotFound;
        rsp.push_object(storage(image));
        return 0;
    };
    service->add_cmd(200, "OpenDataStorageByCurrentProcess", open);
    service->add_cmd(205, "OpenDataStorageWithProgramIndex", [open](Request& req, Response& rsp) -> uint32_t {
        if (!req.in || req.in_size < 1) return fs::rc::kInvalidArgument;
        // This runtime hosts only the current program (index zero).
        if (req.arg<uint8_t>(0) != 0) return fs::rc::kTargetNotFound;
        return open(req, rsp);
    });
    const auto scalar = [bound](const Request& req, size_t bytes = 0) -> uint32_t {
        if (!bound->load(std::memory_order_acquire)) return kernel::kInvalidState;
        return scalar_request(req, bytes);
    };
    service->add_cmd(1003, "DisableAutoSaveDataCreation", [scalar](Request& req, Response&) -> uint32_t {
        // This read-only service never creates saves automatically.
        return scalar(req);
    });
    service->add_cmd(1004, "SetGlobalAccessLogMode", [scalar](Request& req, Response&) -> uint32_t {
        if (const auto rc = scalar(req, 4)) return rc;
        const auto mode = req.arg<uint32_t>(0);
        if (mode > 3) return fs::rc::kInvalidArgument;
        // No guest SD-card/debug log sink is installed. Disabling it is valid;
        // do not acknowledge enabling a destination we cannot write.
        return mode ? fs::rc::kNotImplemented : 0;
    });
    service->add_cmd(1005, "GetGlobalAccessLogMode", [scalar](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = scalar(req)) return rc;
        rsp.push<uint32_t>(0); return 0;
    });
    service->add_cmd(1011, "GetProgramIndexForAccessLog", [scalar](Request& req, Response& rsp) -> uint32_t {
        if (const auto rc = scalar(req)) return rc;
        // Match OpenDataStorageWithProgramIndex: only program zero is mounted.
        rsp.push<uint32_t>(0); rsp.push<uint32_t>(1); return 0;
    });
    return service;
}
} // namespace

std::string romfs_dir() { return config_get_str("fs", "romfs", "/data/pokopia-recomp/game/romfs"); }

void register_fs() { add("fsp-srv", proxy); }
void fs_reset() {
    std::lock_guard lock(state_mutex);
    state.reset();
}
} // namespace nxrt::services
