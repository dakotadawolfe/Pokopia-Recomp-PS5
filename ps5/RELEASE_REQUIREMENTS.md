# On-console preparation and distribution

The PS5 release must work entirely on the console from user-supplied decrypted
game files. This requirement covers initial setup, updates and play. PC-based
preparation is only a private development workflow.

## Public package

Distribute the independently maintained runtime, folder picker, controller and
audio settings, and preparation tools whose licenses permit redistribution,
together with their required notices and source. Do not distribute original
game code, assets, firmware, keys, saves, lifted game sources, compiled game
modules, game-containing executables, or shader/texture/pipeline caches derived
from the game. Conversion into C++, x86-64 or SPIR-V does not make an output a
public project asset.

Keep these private artifacts out of Git history, releases, Actions artifacts,
package registries and issue attachments. The same rule applies to archives
and encoded or renamed copies. No game-data uploads or game-derived downloads
belong in the release workflow. Diagnostic reports must exclude raw game code,
memory dumps and assets; any future report export needs explicit user action.

Third-party runtime code and synthetic test assets still require provenance
and their applicable licenses. A filename check does not establish ownership.

## First run on PS5

1. Let the user choose an existing decrypted game folder on USB or internal
   storage. Validate the supported version and required inputs locally without
   altering the original files or saves. Do not request keys or firmware.
2. Copy the validated game content into the installed title's own directory
   on internal storage. The user chooses the source once; after successful
   installation, launch and play must work with the USB device disconnected.
   Keep the source intact, verify the copy and support interrupted installation.
3. Derive the native modules and required shaders on PS5 using the local copy.
   A developer-generated executable or cache must not be a prerequisite.
   Keep generated code, shaders, textures, pipeline caches and preparation
   state inside the installed title's own directory. Do not use a separate
   `/data/` tree for release content. Identify outputs by input content, tool
   version and relevant target/driver version so stale results are never
   silently reused.
4. Show preparation progress and errors. Support cancellation and restart;
   activate a completed output atomically so interrupted work does not replace
   the last valid output. Check available space before starting.
5. Load the private native modules and assets from the completed internal
   installation. A runtime update must retain user saves/preferences and
   regenerate only incompatible outputs. Never upload or share the cache.
6. Show useful preparation/loading progress instead of an unexplained black
   screen. Measure file reads, decompression, shader preparation and runtime
   startup separately; a local copy alone is not proof of a faster launch.

## Installed-title lifecycle and saves

Normal game deletion must remove the installed assets, generated modules,
caches, temporary installation files and other app-owned content. Verify the
actual writable title location and ShadowMountPlus/native uninstall behavior
before choosing a path. Placing files in a directory named after the title is
not sufficient evidence that the system will remove them with the game.
Release acceptance includes checking for leftover app-owned files after
deletion; development paths under `/data/pokopia-recomp` do not satisfy this.

Use native PS5 save-data integration associated with the active local user,
so saves appear in the console's normal Saved Data management. A plain save
directory does not satisfy this requirement. Save deletion/retention must be
explicitly specified separately from installed-content cleanup; never infer
permission to erase an existing save during installation, update or testing.

## Current development limitation

The current native startup executable statically includes PC-generated game
code. Its shader cache is also prepared on the PC and copied to the developer's
console. These are private development artifacts, not release candidates.

The PS5-hosted code generator, compiler/linker, shader translator and resumable
preparation flow still need to be ported and integrated. The private-module
loading/package boundary also needs implementation and console validation.
So do title-contained installation/uninstallation and native PS5 save-data
management. The current test reads the selected assets in place and uses
separate development cache/report paths under `/data/`.
Existing runtime and shader tests do not establish that these tools can run
within the PS5's memory, storage and process constraints. Menu, gameplay and
the 60 FPS target remain unverified.

Before release, install only the generic package on a console without any
developer-generated modules/cache and complete preparation and play using
only its existing user-provided files. Test interruption/recovery, unsupported
inputs, insufficient space and updates, preserving saves throughout. Audit
the actual release package and every dependency's redistribution requirements.

## Local repository checks

Enable the checked-in hooks in each development checkout:

```sh
git config core.hooksPath .githooks
python ps5/check_public_source.py --all
```

The commit hook checks staged Git objects, not unstaged working files. The push
hook checks each outgoing commit, including an artifact added and removed
before the push. Checks reject known private paths/extensions, executable and
game-container signatures, and nxrecomp-generated source headers. The three
synthetic ELF loader fixtures are allowed only at their reviewed paths and
exact Git object IDs; their source and build script live in
`runtime/tests/fixture/`.

These local hooks are bypassable and cannot identify every possible game
asset, including renamed images or copied source snippets. They do not inspect
manual GitHub uploads. Provenance review and a separate final package audit
remain required; do not describe the checks as legal clearance.
