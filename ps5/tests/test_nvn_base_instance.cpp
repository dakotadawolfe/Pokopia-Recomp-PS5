// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic module cases adapted from featureJosh/pokopia-recomp 07f7ba7.
#include "test_framework.h"
#include "nvn_shaders.h"
#include "nvn_spirv_patch.h"
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>
NXTEST_GROUP(base_instance)
using namespace nxrt::nvn::be;
namespace gpu = nxrt::nvn::be::gpu;
namespace fs = std::filesystem;
static int run_spirv_val(const std::vector<uint32_t>& words, const std::string&, std::string& log) {
    const char* binary = "base-instance-test.spv";
    const char* report = "base-instance-validation.txt";
    { std::ofstream stream(binary, std::ios::binary);
      stream.write(reinterpret_cast<const char*>(words.data()), words.size()*4); }
    const int rc = std::system("spirv-val --target-env vulkan1.3 base-instance-test.spv > base-instance-validation.txt 2>&1");
    { std::ifstream stream(report); log.assign(std::istreambuf_iterator<char>(stream), {}); }
    fs::remove(binary); fs::remove(report);
    return rc;
}
namespace {

// A vertex shader shaped like the translator's DOF bokeh / lens flare sprites: gl_Position = float(gl_InstanceIndex - gl_BaseInstance
// + floatBitsToInt(vp_c0[100].y)). ids 1..31; with_base = false leaves out the BaseInstance decoration.
std::vector<uint32_t> base_instance_module(bool with_base, uint32_t model = 0) {
    auto op = [](uint32_t code, uint32_t len) { return (len << 16) | code; };
    std::vector<uint32_t> w = {0x07230203u, 0x00010300u, 0u, 32u, 0u};
    auto add = [&](std::initializer_list<uint32_t> v) { w.insert(w.end(), v); };
    add({op(17, 2), 1});                        // OpCapability Shader
    add({op(17, 2), 4427});                     // OpCapability DrawParameters
    add({op(14, 3), 0, 1});                     // OpMemoryModel Logical GLSL450
    uint32_t main_name;
    std::memcpy(&main_name, "main", 4);
    add({op(15, 8), model, 1, main_name, 0, 19, 20, 21});   // OpEntryPoint
    if (model == 4) add({op(16, 3), 1, 7});     // fragment: OpExecutionMode OriginUpperLeft
    uint32_t nm[2] = {};
    std::memcpy(nm, "vp_c0", 5);
    add({op(5, 4), 18, nm[0], nm[1]});          // OpName %18 "vp_c0"
    add({op(71, 4), 12, 6, 16});                // ArrayStride 16
    add({op(71, 3), 13, 2});                    // Block
    add({op(72, 5), 13, 0, 35, 0});             // member 0 Offset 0
    add({op(71, 4), 18, 34, 0});                // DescriptorSet 0
    add({op(71, 4), 18, 33, 1});                // Binding 1
    add({op(71, 4), 19, 11, 43});               // InstanceIndex (the fragment variant is only fed to the rewrite, never validated)
    if (with_base) add({op(71, 4), 20, 11, 4425});      // BaseInstance
    add({op(71, 4), 21, 11, 0});                // Position
    add({op(19, 2), 2});                        // void
    add({op(33, 3), 3, 2});                     // fn void()
    add({op(22, 3), 4, 32});                    // float
    add({op(23, 4), 5, 4, 4});                  // vec4
    add({op(21, 4), 6, 32, 1});                 // int
    add({op(21, 4), 7, 32, 0});                 // uint
    add({op(43, 4), 6, 8, 0});
    add({op(43, 4), 6, 9, 1});
    add({op(43, 4), 6, 10, 100});
    add({op(43, 4), 7, 11, 4096});
    add({op(28, 4), 12, 5, 11});                // vec4[4096]
    add({op(30, 3), 13, 12});                   // struct
    add({op(32, 4), 14, 2, 13});                // Uniform struct*
    add({op(32, 4), 15, 2, 4});                 // Uniform float*
    add({op(32, 4), 16, 1, 6});                 // Input int*
    add({op(32, 4), 17, 3, 5});                 // Output vec4*
    add({op(59, 4), 14, 18, 2});                // vp_c0
    add({op(59, 4), 16, 19, 1});                // gl_InstanceIndex
    add({op(59, 4), 16, 20, 1});                // gl_BaseInstance
    add({op(59, 4), 17, 21, 3});                // gl_Position
    add({op(54, 5), 2, 1, 0, 3});               // OpFunction
    add({op(248, 2), 22});
    add({op(61, 4), 6, 23, 19});
    add({op(61, 4), 6, 24, 20});
    add({op(130, 5), 6, 25, 23, 24});           // OpISub
    add({op(65, 7), 15, 26, 18, 8, 10, 9});     // &vp_c0[0][100].y
    add({op(61, 4), 4, 27, 26});                // the load the rewrite replaces
    add({op(124, 4), 6, 28, 27});               // OpBitcast int
    add({op(128, 5), 6, 29, 25, 28});           // OpIAdd
    add({op(111, 4), 4, 30, 29});               // OpConvertSToF
    add({op(80, 7), 5, 31, 30, 30, 30, 30});    // OpCompositeConstruct
    add({op(62, 3), 21, 31});                   // OpStore
    add({op(253, 1)});                          // OpReturn
    add({op(56, 1)});                           // OpFunctionEnd
    return w;
}

// (pointer operand -> count) of the module's OpLoads
std::map<uint32_t, int> loads_by_pointer(const std::vector<uint32_t>& w) {
    std::map<uint32_t, int> m;
    for (size_t i = 5; i < w.size();) {
        const uint32_t len = w[i] >> 16;
        if (!len) break;
        if ((w[i] & 0xffff) == 61 && len >= 4) ++m[w[i + 3]];
        i += len;
    }
    return m;
}

}  // namespace

// c0 + 0x644 (the draw's base instance) becomes a load of BaseInstance; the module stays valid and the rewrite does not repeat.
NXTEST(base_instance_from_c0_on_a_sprite_shaped_module) {
    const std::vector<uint32_t> in = base_instance_module(true);
    std::string log;
    std::error_code ec;
    const bool have_val = true;
    if (have_val) CHECK_MSG(run_spirv_val(in, "bi_in", log) == 0, "input module: " + log);
    std::vector<uint32_t> out;
    REQUIRE(gpu::spirv_base_instance_from_c0(in, out));
    CHECK_EQ(out[3], in[3] + 1u);                     // one new id (the int load before the bitcast)
    std::map<uint32_t, int> loads = loads_by_pointer(out);
    CHECK_EQ(loads[26], 0);                           // no load through &vp_c0[0][100].y
    CHECK_EQ(loads[20], 2);                           // gl_BaseInstance: the shader's own load and the new one
    if (have_val) CHECK_MSG(run_spirv_val(out, "bi_out", log) == 0, "patched module: " + log);
    ShaderReflection r;
    std::string err;
    CHECK_MSG(reflect_spirv(out.data(), out.size(), r, &err), err);
    std::vector<uint32_t> again;
    CHECK(!gpu::spirv_base_instance_from_c0(out, again));
}

NXTEST(base_instance_from_c0_needs_a_vertex_shader_with_base_instance) {
    std::vector<uint32_t> out = {1u, 2u, 3u};
    const std::vector<uint32_t> keep = out;
    CHECK(!gpu::spirv_base_instance_from_c0(base_instance_module(false), out));
    CHECK(!gpu::spirv_base_instance_from_c0(base_instance_module(true, 4), out));   // not a vertex entry point
    CHECK(!gpu::spirv_base_instance_from_c0(std::vector<uint32_t>{0x07230203u, 0x00010300u}, out));
    CHECK(out == keep);
}
