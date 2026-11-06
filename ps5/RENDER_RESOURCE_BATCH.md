# Renderer resource corrections

The last captured game run ended in a GPU unmapped-page read. This batch
addresses concrete resource and shader defects before another full startup.
It is not evidence of a working game frame or a confirmed single crash cause.

## Changes and sources

- Backport [PS5_Mesa f5610fe](https://github.com/mihawk-99/PS5_Mesa/commit/f5610fe27090acd2509e5ce807140676f0b04473): retain owned buffer backing until submitted GPU work completes. Imported backing remains caller-owned. The original driver, SDK and source checkout stay unchanged; the build script verifies the pinned source and replaces two archive objects in a separate output directory.
- Populate NVN2 compute and graphics driver constant banks using [c33d6d4](https://github.com/featureJosh/pokopia-recomp/commit/c33d6d4a4d32f26a76f7efd54f905e20d6820f4b) and [07f7ba7](https://github.com/featureJosh/pokopia-recomp/commit/07f7ba7c3de507507d617c7156a07a195bf1e497). Reflection and replay share the table layout. Each distinct table gets an immutable upload snapshot retained by its submission; cache lookup compares the complete bank contents.
- Rewrite vertex reads of the driver base-instance word to SPIR-V `BaseInstance` where that input exists, including GPU indirect draws. Direct draws also update the constant-bank value.
- Correct `.H0_NH1` packed-half lane rotation. [nxnative's shader notes](https://github.com/seedds/nxnative/blob/d928699b426fce398393c165755c89dc25f02ef0/docs/reference/shaders.md#hfma2-h0_nh1) explain the older upper-lane-negation error and identify the corrected Eden-NS2 behavior. This is source-supported semantics, not a new hardware arithmetic oracle result from this project.
- Compute exact buffer/image copy spans including array/depth slices, compressed edge blocks and row/slice padding. Reject overflow, invalid subresources, alignment and insufficient backing before submission. Format-changing copies still need explicit conversion; rejecting one is an unsupported operation, not successful rendering.
- Refuse descriptor sets with unwritten arrays, missing required backing, or failed fallback-view creation. Clamp unaligned uniform uploads to available backing. This prevents invalid submission; it does not add descriptor-array support.

## Reproduction

Use the pinned public driver recipe in `GRAPHICS_PORT.md`. Pass
`--retire-buffers` to `build_graphics_radv_partition.py`, then isolate the
result using `prepare_graphics_radv.py`. Supply the resulting partition and
isolation manifests plus their actual hashes to `build_graphics_app.sh`.
The retirement patch is included in this repository; no private machine data
is needed to obtain it. The separate private game build still requires the
user's decrypted inputs and generated modules.

Host checks:

```sh
python3 -B ps5/test_radv_retirement.py /path/to/partition/radv_ps5_bo.c
cmake --build /path/to/audio-host --target ps5_shader_control_tests ps5_driver_bank_tests ps5_base_instance_tests ps5_copy_span_tests
ctest --test-dir /path/to/audio-host -R 'shader_control|driver_bank|base_instance|copy_span' --output-on-failure
```

The retirement harness compiles the actual patched functions under ASan/UBSan
with a controlled queue, covering pending/completed work, failed waits, sparse
storage and imported-memory ownership. Driver-bank tests cover compute layout,
graphics stage mapping, point size and instance data. Synthetic SPIR-V tests
validate rewritten modules with `spirv-val`, including repeat application and
nonmatching modules. Copy tests cover padding, arrays, compression and overflow.
The native PS5 compile/link and FSELF checks are separate from these host tests.

Private shaders are rebuilt locally and validated before installation; none of
the cache, generated game code, binaries or runtime captures belong in Git.
For the next live run, preserve the early-frame traces, game report and kernel
fault record before relaunching. Compare completed work and actual screen output,
not just draw counters. The remaining first-draw replay and startup profiling
work depends on that capture. Release-time on-console preparation and title-owned
asset/save storage remain separate unfinished requirements.
