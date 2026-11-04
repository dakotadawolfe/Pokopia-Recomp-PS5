// SPDX-License-Identifier: GPL-3.0-or-later
// Maps the supplied game images without linking, dispatching or executing them.
#include "test_framework.h"
#include "private_game_images.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/dispatch.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

NXTEST_GROUP(game_images)
namespace {
namespace mem = nxrt::mem;
constexpr auto crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
        table[i] = value;
    }
    return table;
}
constexpr auto table = crc_table();
uint32_t crc_update(uint32_t value, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) value = table[(value ^ data[i]) & 255] ^ (value >> 8);
    return value;
}
uint32_t crc(const uint8_t* data, size_t size) { return crc_update(~0u, data, size) ^ ~0u; }
bool check_file(const std::string& path, const GameImageInput& expected) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != static_cast<std::streamoff>(expected.bytes)) return false;
    file.seekg(0);
    std::array<uint8_t, 65536> buffer{};
    file.read(reinterpret_cast<char*>(buffer.data()), sizeof expected.header);
    if (!file || std::memcmp(buffer.data(), expected.header, sizeof expected.header)) return false;
    file.seekg(0);
    uint32_t value = ~0u;
    uint64_t left = expected.bytes;
    while (left) {
        const auto size = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        if (!file) return false;
        value = crc_update(value, buffer.data(), size);
        left -= size;
    }
    return (value ^ ~0u) == expected.crc32;
}
struct Cleanup {
    ~Cleanup() { nxrt::modules_unload_all(); nxrt::hle_clear(); mem::shutdown(); }
};
}

NXTEST(decrypted_game_images_map_with_original_contents_and_permissions) {
    Cleanup cleanup;
    nxrt::modules_unload_all(); mem::shutdown();
    REQUIRE(mem::init({.heap_reserve = 12ull << 30}));
    REQUIRE(mem::heap_create(12ull << 30));
    // The oracle is produced with Python lz4 + hashlib + zlib, independently of
    // the runtime's NSO decompressor. SHA-256 also verifies staging/readback.
    CHECK_EQ(crc(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xcbf43926u);
    for (const auto& expected : game_image_inputs) {
        std::printf("GAME_IMAGE loading %s at %llx\n", expected.name,
                    static_cast<unsigned long long>(expected.base));
        const auto path = std::string("/data/pokopia-recomp/game/exefs/") + expected.name;
        REQUIRE(check_file(path, expected));
        nxrt::LoadOptions options;
        options.name = expected.name; options.base = expected.base;
        std::string error;
        auto* module = nxrt::module_load_nso_file(path.c_str(), options, &error);
        if (!module) std::printf("GAME_IMAGE load failed: %s\n", error.c_str());
        REQUIRE(module);
        CHECK_EQ(module->base(), expected.base);
        const auto& data = expected.segments[2];
        CHECK_EQ(module->size(), (data.offset + data.bytes + expected.bss + mem::kPage - 1) & ~(mem::kPage - 1));
        CHECK(!module->symbols().empty());
        for (const auto& segment : expected.segments) {
            const auto* address = reinterpret_cast<const uint8_t*>(expected.base + segment.offset);
            CHECK_EQ(crc(address, segment.bytes), segment.crc32);
        }
        const auto* bss = reinterpret_cast<const uint8_t*>(expected.base + data.offset + data.bytes);
        CHECK(std::all_of(bss, bss + expected.bss, [](uint8_t byte) { return byte == 0; }));
        std::printf("GAME_IMAGE mapped %s bytes=%llu symbols=%zu; contents checked\n", expected.name,
                    static_cast<unsigned long long>(module->size()), module->symbols().size());
    }
    CHECK_EQ(nxrt::modules().size(), size_t{4});
    // Pokopia's own rtld will relocate its images later. Keep the original
    // contents here; module_link_all and guest entry points are not called.
    REQUIRE(nxrt::module_protect_all());
    for (const auto& expected : game_image_inputs) {
        constexpr uint32_t permissions[] = {5, 1, 3};
        for (unsigned i = 0; i < 3; ++i) {
            const auto& segment = expected.segments[i];
            const uint64_t address = expected.base + segment.offset;
            CHECK_EQ(mem::vm_query(address).perm, permissions[i]);
            CHECK(mem::is_mapped(address, segment.bytes));
            CHECK_EQ(crc(reinterpret_cast<const uint8_t*>(address), segment.bytes), segment.crc32);
        }
        CHECK(!nxrt::module_by_name(expected.name)->linked());
    }
    nxrt::modules_unload_all();
    for (const auto& expected : game_image_inputs) {
        CHECK(!mem::is_mapped(expected.base, mem::kPage));
        REQUIRE(mem::reserve_fixed(expected.base, mem::kAllocGranularity, "game image released"));
        CHECK(mem::release_fixed(expected.base));
    }
    std::puts("GAME_IMAGE four images checked and unloaded; no guest code executed");
}
