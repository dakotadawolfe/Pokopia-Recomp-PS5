# Pokopia Recomp for PS5

Native PS5 port in development, based on drakolordx7's Pokopia recompilation.
The private development build connects game startup, read-only RomFS and the
native NVN renderer. Console testing has reached a black presentation while
loading assets and shaders; a game menu and gameplay remain unverified.
There is no playable PS5 release yet.

See [the PS5 project scope and current status](ps5/README.md) and
[release requirements](ps5/RELEASE_REQUIREMENTS.md). A public PS5 package must
contain only the runtime and redistributable preparation tools. It must derive
game code and shaders entirely on the console from the user's own decrypted
files, without a PC for setup or play. That on-console preparation is not
implemented yet. Current PC-generated game executables and caches are private
development artifacts and must never be uploaded or distributed.

The PS5 application will use a fixed graphics profile and expose controller,
audio and save options.

## Upstream Windows project

The instructions below describe the separate upstream Windows project. Its
downloads and PC preparation workflow are not the PS5 release workflow.

Pokémon Pokopia (Switch 2) recompiled to run natively on Windows.

[Download the latest build](https://github.com/drakolordx7/pokopia-recomp/releases/latest)

You need your own extracted copy of the game, version 2.0.1a. Not affiliated with Nintendo, The Pokémon Company or Koei Tecmo.

## Requirements

- Windows 10 or 11, 64-bit
- GPU with Vulkan 1.3
- 16 GB of RAM

## Usage

1. Unzip the build and run `PokopiaLauncher.exe`.
2. Pick your dump folder and press Set up. It compiles the game on your PC (about 7 minutes), then builds the shader cache (1 to 2 hours). You can pause it and continue later.
3. Press Play.

F1 opens the settings in game. Middle mouse toggles Joy-Con 2 mouse mode for placing items.

## Reporting problems

[Open an issue](../../issues/new/choose) with `pkp.log` from the session where it happened, your GPU and your Windows version.

## Contributing

- Open pull requests against `main` and say what you tested on which GPU.
- Code for this game goes in `recomp/games/pkp/` and `runtime/games/pkp/`; the rest is shared with other games.
- Building from source needs Visual Studio 2022 Build Tools or newer, LLVM, CMake, Ninja, Python 3.13, .NET 8 SDK, vcpkg with `libvpx:x64-windows-static`, Rust and Node.js. Copy the game's `exefs` files to `game\dump\exefs\`, run `recomp\scripts\build_game.cmd pkp` in a Developer Command Prompt, then `pwsh -File game-build\scripts\build.ps1`. Shaders: [tools/shaders/README.md](tools/shaders/README.md). Launcher: `npm install`, `npm run tauri build` in `launcher\`.
- Runtime tests: `pwsh -File runtime\scripts\build.ps1 -Test`.

## Credits

Built on [nxrecomp](https://github.com/drakolordx7/nxrecomp). Uses code from [Ryujinx](https://github.com/Ryubing/Ryujinx), [Mesa](https://gitlab.freedesktop.org/mesa/mesa), smo-recomp, ScarletNX, ssbu-recomp, SuperMarioWonderRecomp, yuzu, Eden, libnx, [astc-encoder](https://github.com/ARM-software/astc-encoder), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [libvpx](https://chromium.googlesource.com/webm/libvpx), AMD FidelityFX, NVIDIA Image Scaling, [SDL3](https://github.com/libsdl-org/SDL), [Tauri](https://tauri.app), [Zig](https://ziglang.org) and Python, plus [Kenney input prompts](https://kenney.nl/assets/input-prompts) and DejaVu fonts. Licences are in `runtime/THIRD_PARTY_NOTICES.md`, `recomp/THIRD_PARTY.md` and `tools/shaders/licenses/`.

## License

GPL-3.0. Third-party code keeps its own licence.
