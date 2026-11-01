# Building from source

## Requirements

- **Windows 10 / 11 x64**
- **Visual Studio 2022** C++ build tools (MSVC)
- **Windows SDK**
- **CMake 3.22+**
- **PowerShell 7+ (`pwsh`)** — required by `tools\*.ps1`, the test tools and the release flow

## The only supported build path

> [!IMPORTANT]
> The main repository has exactly **one** supported build path: the CMake Preset `windows-vs2022` configure plus the `windows-relwithdebinfo` build. CI, releases and local builds all go through it. Do **not** create additional `cmake -S/-B`, NMake, Ninja or stage-named build trees. [`CMakePresets.json`](../../../CMakePresets.json) is authoritative for all build configuration.

### One command

`build.cmd` is a thin wrapper around the command sequence below. By default it builds only the targets required for Runtime, Tools and SDK, and produces a GameRuntime install tree once the build completes:

```powershell
.\build.cmd
```

| Argument | Effect |
| --- | --- |
| (no argument) | Build the Runtime, Tools and SDK targets and install a clean runtime package that is ready to deploy |
| `fixtures` | Additionally build the development fixtures (`anomaly-platform-preview`, BetterPose and the auto-combat test program) |
| `symbols` | Additionally generate linker PDBs and stage the Symbols component; local debugging only |
| `testplugins` | Additionally build and install the developer test plugin packages (`MapSpawnExporter`, `NteMovementHold`, `NteNavmeshDemo`, `NtePickupDemo`, `TeleportLandmarksProbe`); they belong to the separate `TestPlugins` component and never ship in a release package |
| `package` | Additionally install Tools and SDK (including Symbols when `symbols` is enabled) into `.build\windows-vs2022\package`, matching release packaging |

Arguments can be combined, for example `.\build.cmd fixtures package`. Neither development fixtures nor PDBs belong to the three released components, so they are not built by default and appear in the build tree only when explicitly enabled.

### Manual execution (equivalent)

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --parallel
cmake --install .build\windows-vs2022 --config RelWithDebInfo `
  --prefix .build\windows-vs2022\game-package --component GameRuntime
```

Enable the optional switches explicitly at configure time only when you need them: `-DANOMALY_BUILD_TEST_FIXTURES=ON`, `-DANOMALY_BUILD_SYMBOLS=ON`, `-DANOMALY_BUILD_TEST_PLUGINS=ON` (equivalent to `build.cmd testplugins`, and it requires the additional `--component TestPlugins` install step).

### Output locations

| Content | Path |
| --- | --- |
| Built tools (CLI, validation tools) | `.build\windows-vs2022\bin\RelWithDebInfo` |
| Clean runtime package that is ready to deploy | `.build\windows-vs2022\game-package` |
| Release component staging directory (`build.cmd package`) | `.build\windows-vs2022\package\{runtime,tools,sdk}` |

For the runtime package layout, see [Installation and updates](../user-guide/installation.md). The CLI and validation tools stay in `bin` and never enter the game runtime package.

## Preview UI (no game)

`anomaly-platform-preview.exe` is a development fixture and requires enabling `ANOMALY_BUILD_TEST_FIXTURES` first (`.\build.cmd fixtures`). Once enabled, run it directly to preview and debug the interface and plugins in a standalone process through the same Platform and PluginManager path used inside the game.

## Release packaging

The official component packages are produced from the same build tree:

```powershell
pwsh -NoProfile -File .\tools\package_release.ps1 `
  -BuildDirectory .build\windows-vs2022 `
  -Version 1.0.0 `
  -OutputDirectory .build\release\1.0.0
```

It outputs three deterministically assembled ZIPs plus checksum files:

| Component ZIP | Contents |
| --- | --- |
| **Runtime** | The proxy, Core, configuration, bundled Profiles, the default built-in plugin packages and 1 example plugin package (NteCombatDemo); developer test plugin packages are not part of the release (see `build.cmd testplugins`) |
| **SDK** | Headers, CMake package, five examples (source + standalone CMake project) |
| **Tools** | Six official command-line tools |

Plus `SHA256SUMS.txt`, `release-manifest.json` and three SPDX 2.3 `sbom/*.spdx.json` files. The tag workflow publishes the Runtime, Tools and SDK ZIPs and generates a GitHub build provenance attestation for all three archives. PDBs are not part of the release: the default build produces no PDBs, and `build.cmd symbols` provides them for local debugging.

## Command-line tools

The `Tools` component contains six official tools:

| Tool | Purpose |
| --- | --- |
| `anomaly-cli.exe` | Diagnostic client (named pipe) |
| `anomaly-inspect.exe` | Offline inspection client |
| `anomaly-plugin.exe` | Plugin package validation and assembly (`validate` / `pack`) |
| `anomaly-profile.exe` | Build / Profile validation and offline `fingerprint` |
| `anomaly-abi-snapshot.exe` | ABI baseline generation and validation |
| `anomaly-test-host.exe` | Plugin fixture host without the game |

`AnomalyLauncher.exe` ships with the Runtime; `anomaly-platform-preview.exe` is a development fixture that appears in the build directory only after `build.cmd fixtures`.

## Related

- [Architecture overview](architecture.md)
- [Contributing guide](contributing.md)
- [Plugin development](plugin-development.md)
