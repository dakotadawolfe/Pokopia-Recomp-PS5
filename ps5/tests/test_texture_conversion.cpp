// SPDX-License-Identifier: GPL-3.0-or-later
#include "astc2bc7.h"
#include "bc7decomp.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <exception>
#include <vector>
#if defined(NXRT_TARGET_PS5)
#include <cerrno>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#endif

namespace {
int checks = 0;
struct Failure { int line; };
void require(bool ok, int line) {
    ++checks;
    if (!ok) throw Failure{line};
}
#define CHECK(expr) require((expr), __LINE__)
using Color = std::array<uint8_t, 4>;
// ASTC LDR void-extent block, with full-range UNORM16 constant components.
std::array<uint8_t, 16> solid(Color color) {
    std::array<uint8_t, 16> block{0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    for (unsigned c = 0; c < 4; ++c) block[8 + c * 2] = block[9 + c * 2] = color[c];
    return block;
}
std::vector<uint8_t> decode_bc7(uint32_t w, uint32_t h, const std::vector<uint8_t>& encoded) {
    std::vector<uint8_t> result(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y += 4) for (uint32_t x = 0; x < w; x += 4) {
        bc7decomp::color_rgba pixels[16];
        CHECK(bc7decomp_ref::unpack_bc7(encoded.data() + (size_t(y / 4) * ((w + 3) / 4) + x / 4) * 16, pixels));
        for (uint32_t dy = 0; dy < 4 && y + dy < h; ++dy)
            for (uint32_t dx = 0; dx < 4 && x + dx < w; ++dx)
                std::memcpy(result.data() + (size_t(y + dy) * w + x + dx) * 4, &pixels[dy * 4 + dx], 4);
    }
    return result;
}
void astc_formats() {
    constexpr std::array<std::array<uint32_t, 2>, 14> footprints{{
        {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}}};
    for (auto fp : footprints) for (auto format : {A2B_ASTC_UNORM, A2B_ASTC_SRGB}) {
        a2b_input in{format, fp[0] * 2 - 1, fp[1] * 2 - 1, fp[0], fp[1]};
        const Color colors[4]{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
        std::vector<uint8_t> blocks(64);
        for (unsigned i = 0; i < 4; ++i) { auto b = solid(colors[i]); std::copy(b.begin(), b.end(), blocks.begin() + i * 16); }
        std::vector<uint8_t> rgba(size_t(in.width) * in.height * 4, 0);
        CHECK(a2b_decode_astc_rgba8(&in, blocks.data(), blocks.size(), 3, rgba.data(), nullptr) == A2B_OK);
        std::printf("TEXTURE_ASTC_DECODE footprint=%ux%u format=%d status=PASS\n", fp[0], fp[1], int(format));
        for (uint32_t y = 0; y < in.height; ++y) for (uint32_t x = 0; x < in.width; ++x)
            CHECK(std::memcmp(rgba.data() + (size_t(y) * in.width + x) * 4, colors[(y / fp[1]) * 2 + x / fp[0]].data(), 4) == 0);
        std::vector<uint8_t> bc7(a2b_output_size(&in));
        a2b_options opt{2, 3, 0, 0}; a2b_format output{};
        CHECK(a2b_transcode(&in, blocks.data(), blocks.size(), &opt, &output, bc7.data(), bc7.size(), nullptr) == A2B_OK);
        CHECK(output == (format == A2B_ASTC_SRGB ? A2B_BC7_SRGB : A2B_BC7_UNORM));
        auto decoded = decode_bc7(in.width, in.height, bc7);
        for (size_t n = 3; n < decoded.size(); n += 4) CHECK(decoded[n] == 255);
        uint64_t error = 0;
        for (size_t n = 0; n < rgba.size(); ++n) { int d = int(rgba[n]) - decoded[n]; error += d * d; }
        CHECK(double(error) / rgba.size() < 180.0);
        std::printf("TEXTURE_ASTC_BC7 footprint=%ux%u format=%d status=PASS\n", fp[0], fp[1], int(format));
    }
}
void bc7_presets_and_threads() {
    constexpr uint32_t w = 67, h = 71;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (bool opaque : {false, true}) {
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            auto p = rgba.data() + (size_t(y) * w + x) * 4;
            p[0] = x * 3; p[1] = y * 3; p[2] = (x + y) * 2; p[3] = opaque ? 255 : (x * 2 + y);
        }
        for (int q = 0; q <= 6; ++q) {
            a2b_options opt{q, 1, 0, 0};
            std::vector<uint8_t> one(size_t((w + 3) / 4) * ((h + 3) / 4) * 16), many(one.size());
            CHECK(a2b_encode_bc7(w, h, rgba.data(), &opt, one.data(), one.size(), nullptr) == A2B_OK);
            opt.threads = 4;
            CHECK(a2b_encode_bc7(w, h, rgba.data(), &opt, many.data(), many.size(), nullptr) == A2B_OK);
            CHECK(one == many);
            auto decoded = decode_bc7(w, h, one);
            uint64_t error = 0;
            for (size_t n = 0; n < rgba.size(); ++n) { int d = int(rgba[n]) - decoded[n]; error += d * d; }
            CHECK(double(error) / rgba.size() < 30.0);
            if (opaque) for (size_t n = 3; n < decoded.size(); n += 4) CHECK(decoded[n] == 255);
        }
    }
}
void sizes_and_passthrough() {
    for (int f = A2B_BC1_UNORM; f <= A2B_BC7_SRGB; ++f) {
        a2b_input in{static_cast<a2b_format>(f), 7, 9, 0, 0};
        std::vector<uint8_t> input(a2b_output_size(&in)), out(input.size() + 16, 0xa7);
        for (size_t n = 0; n < input.size(); ++n) input[n] = uint8_t(n * 17);
        a2b_format format{};
        CHECK(a2b_transcode(&in, input.data(), input.size(), nullptr, &format, out.data(), input.size(), nullptr) == A2B_OK);
        CHECK(format == in.format && std::equal(input.begin(), input.end(), out.begin()));
        CHECK(std::all_of(out.begin() + input.size(), out.end(), [](auto v) { return v == 0xa7; }));
        CHECK(a2b_transcode(&in, input.data(), input.size() - 1, nullptr, &format, out.data(), input.size(), nullptr) == A2B_ERR_SIZE);
        CHECK(a2b_transcode(&in, input.data(), input.size(), nullptr, &format, out.data(), input.size() - 1, nullptr) == A2B_ERR_SIZE);
    }
    auto block = solid({32,64,128,255}); std::array<uint8_t, 64> rgba{};
    a2b_input in{A2B_ASTC_UNORM, 4, 4, 4, 4};
    CHECK(a2b_decode_astc_rgba8(&in, block.data(), 15, 1, rgba.data(), nullptr) == A2B_ERR_SIZE);
    in.block_w = 7;
    CHECK(a2b_decode_astc_rgba8(&in, block.data(), 16, 1, rgba.data(), nullptr) == A2B_ERR_ARGS);
    CHECK(a2b_init("unwanted-dynamic-library") == A2B_ERR_ARGS);
}
}
static int texture_conversion_checks() {
    checks = 0;
    try {
        CHECK(a2b_init(nullptr) == A2B_OK);
        std::puts("Static decoder initialized");
        astc_formats(); std::puts("ASTC formats passed");
        bc7_presets_and_threads(); std::puts("BC7 presets passed");
        sizes_and_passthrough();
        std::printf("Texture conversion: %d checks passed\n", checks);
        return 0;
    } catch (const Failure& e) { std::fprintf(stderr, "Texture conversion FAIL line=%d checks=%d\n", e.line, checks); }
      catch (const std::exception& e) { std::fprintf(stderr, "Texture conversion FAIL exception=%s\n", e.what()); }
    return 1;
}

int pkp_texture_conversion_probe() {
#if defined(NXRT_TARGET_PS5)
    // ISPC's opaque-block encoder alone uses a 157 KiB frame. Payload loader
    // and application main stacks vary; declare the conversion worker's size.
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return 4;
    const int configured = pthread_attr_setstacksize(&attr, 8 << 20);
    pthread_t worker; int result = 1;
    std::puts("TEXTURE_CONVERSION_WORKER requested_stack_bytes=8388608");
    const int created = configured ? configured : pthread_create(&worker, &attr, [](void* p) -> void* {
        *static_cast<int*>(p) = texture_conversion_checks(); return nullptr;
    }, &result);
    pthread_attr_destroy(&attr);
    if (created || pthread_join(worker, nullptr)) return 4;
    return result;
#else
    return texture_conversion_checks();
#endif
}

#if !defined(PKP_TEXTURE_NO_MAIN)
int main() {
#if defined(NXRT_TARGET_PS5)
    for (const char* p : {"/data/pokopia-recomp", "/data/pokopia-recomp/tests"})
        if (mkdir(p, 0777) != 0 && errno != EEXIST) return 4;
    char path[160];
    std::snprintf(path, sizeof(path), "/data/pokopia-recomp/tests/textures-runtime-%lld-%d.log",
        static_cast<long long>(std::time(nullptr)), getpid());
    if (!std::freopen(path, "a", stdout) || !std::freopen(path, "a", stderr)) return 4;
    std::setvbuf(stdout, nullptr, _IONBF, 0); std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::puts("Texture conversion started");
#endif
    return pkp_texture_conversion_probe();
}
#endif
