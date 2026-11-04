# Native Pokopia for PS5

The main goal is a native PS5 application using the existing static recompilation
and the user's decrypted Pokopia files. Development and builds run locally.
The initial upstream revision is `2c29515b23a6d78d967775fd7e9f24f50402cca7`.

## Product scope

- Load decrypted ExeFS and RomFS from `C:/Games/pokopia` during local preparation.
  An explicit input path can point to an existing dump elsewhere. Do not request
  keys or firmware; this application consumes extracted game files.
- Produce a native PS5 application and its executable, using the existing
  homebrew SDK, RADV and native presentation work where appropriate.
- Target steady 60 real frames per second, with frame generation off. Establish
  one fixed graphics profile against the base PS5 hardware, then validate it on
  other PS5 models. Do not expose resolution, upscaling, GPU accuracy, performance
  presets or other graphics tuning in the product.
- Provide basic controller mapping, stick sensitivity/deadzones, invert look
  and vibration controls, with persisted preferences. DualSense is the initial
  controller target; other controller support requires device verification.
- Provide audio volume controls, reliable save/load, safe save import/export,
  clean exit, clear missing-file errors and useful crash logs. Retain applicable
  game language and accessibility controls.
- Keep original game files and existing saves intact. Keep all game-derived
  output and private input manifests outside Git.

60 FPS is the performance target, not a measured result. The port is not complete
until boot, rendering, controller input, audio, saves and sustained gameplay have
been verified on console. Cross-model support also requires representative
hardware testing; a base-console run does not establish PS5 Pro behavior.

## Input validation

From the repository root, with Python 3.10 or newer:

```powershell
python ps5/validate_input.py
```

The default source is `C:/Games/pokopia`. It supports either a directory with
`exefs` and `romfs`, or an extracted tree with `base` and `updates/<version>`.
Use `--input PATH` for another location. From WSL, pass the `/mnt/c/...` path.

The validator matches every executable module and `main.npdm` against upstream's
supported hashes, checks the NSO build ID, and records the available RomFS layers.
It writes only `work/ps5/input-manifest.json`; it does not copy or alter the dump.
That ignored manifest records paths for later preparation. RomFS file presence
and size are checked, but asset completeness is not claimed from those checks.
Any future asset staging must apply recorded layers in order, with later layers
overriding earlier ones.

Validated locally on 2026-11-03:

- `main`, `sdk`, `subsdk0`, `rtld` and `main.npdm` match upstream's supported hashes.
- Main build ID: `76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E`.
- The existing dump has the supported executable under update `655360`, with
  a RomFS directory containing about 7.25 GB of files.
- The requested default directory is not currently present; validation used the
  existing dump through `--input`. Machine-specific paths stay in the ignored
  manifest.

## Bring-up milestones

1. Validate the decrypted inputs and establish reproducible local preparation.
   Module hashes are verified; game lifting and asset preparation remain.
2. Port memory reservations, aliases, protection and fault handling. Coordinate
   guest addresses with the PS5 GPU allocator before combining the runtimes.
3. Port thread, synchronization, filesystem and exception services, and exercise
   their behavior with runtime tests.
4. Link the lifted game and reach its startup using the PS5 runtime.
5. Integrate RADV/VideoOut, shader preparation, audio and controllers.
6. Validate saves and sustained gameplay, then select the fixed graphics profile
   from console frame-time measurements.

The earlier compiler proof translated an original 34-instruction ARM fixture,
matched its results on a PC and linked it into a PS5 ELF. It did not execute
Pokopia or run on console. The detailed
[portability audit](https://github.com/dakotadawolfe/EglantinePS5/blob/44e3f3e/docs/POKOPIA_RECOMP_PS5.md)
and reproducible test are retained in the existing emulator project.

## Repository ownership

`origin` is `dakotadawolfe/Pokopia-Recomp-PS5`; `upstream` is the original
`drakolordx7/pokopia-recomp`. Commit only source, documentation and build tools.
Use `work/`, `game/` or `build/` for generated and game-derived files, all ignored
by the inherited `.gitignore`. No GitHub build workflow is required.
