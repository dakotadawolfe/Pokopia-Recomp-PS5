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
    service->add_cmd(3, "SetSize", [](Request&, Response&) -> uint32_t { return fs::rc::kReadOnlyFileSystem; });
    service->add_cmd(4, "GetSize", [image](Request&, Response& rsp) -> uint32_t {
        rsp.push<int64_t>(static_cast<int64_t>(image->size())); return 0;
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
