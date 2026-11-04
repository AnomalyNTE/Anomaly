# Status Bar

Anomaly plugin with a liquid glass overlay for frame rate, 1% low, local time,
memory load, CPU/GPU load, frequency, power and GPU temperature. Each item, the
six screen positions and spacing can be configured independently.

Version 0.3.2 fixes the settings window close button. Closing the settings window
keeps the overlay visible. Reopen the window through the plugin list; disable the
plugin to stop the overlay and sampler. Closing a window does not reset or change
the saved metric settings.

## Requirements and build

Windows x64, CMake 3.22+, Visual Studio C++ tools and the Anomaly runtime's public
UI overlay API are required. The plugin uses only public SDK services. Build from
the repository root with its supported presets:

```powershell
cmake --preset windows-vs2022 -DANOMALY_BUILD_TEST_FIXTURES=ON
cmake --build --preset windows-relwithdebinfo --target anomaly_builtin_status_bar anomaly_status_bar_fixture anomaly_ui_overlay_fixture
ctest --test-dir .build/windows-vs2022 -C RelWithDebInfo -R "^status_bar_" --output-on-failure
```

The plugin package is in
`.build/windows-vs2022/bin/RelWithDebInfo/builtin-plugins/StatusBar` and is installed
by the `GameRuntime` component. Keep `manifest.json`, `plugin.dll`, `assets` and
`locales` together.
Settings remain in the plugin's existing `settings.txt` state document; item
identifiers and file format are unchanged.

## Localization and review

The settings window, metric labels, units and dynamic readings use one
`anomaly::sdk::Localizer` cached at Load. English fallbacks are in C++; Chinese
messages are in `locales/zh-CN.json`. Window and widget identities use stable IDs
independent of the translation.

```powershell
pwsh -NoProfile -File tools/check_plugin_i18n.ps1 -Plugin StatusBar
```

Regression tests exercise the plugin's actual draw callback: the host receives a
live close flag, closing/collapsing keeps the overlay running, reopening restores
the settings, and frame drawing balances window scopes. Localization checks
include reordered numeric arguments, missing services, long translations,
English fallback and stable IDs. Hardware sampling and real glass rendering
still need a runtime check; unit tests do not start the hardware sampler.

The host integration fixture loads the real plugin through PluginManager and
checks close, reopen, collapse, disable, balanced scopes and an incomplete
optional overlay tail. This fixture starts and stops the normal sampler.

NVIDIA/PDH first-use initialization has shown about 23 MiB of retained process
memory in the offline host, above its default 10 MiB growth budget. A successful
short reload run is evidence of resource cleanup, not of meeting that budget.

The plugin is AGPL-3.0-only. The Material Symbols subset is distributed under the
separate Apache-2.0 license in `assets/MaterialSymbols-LICENSE.txt`.
