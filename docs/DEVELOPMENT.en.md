# Development guide

[Back to README](../README.en.md) · [中文指南](DEVELOPMENT.md)

Run commands from the repository root. See [SUPPORT.md](../SUPPORT.md) for diagnostics and evidence boundaries,
[ARCHITECTURE.md](../ARCHITECTURE.md) for rendering, and [ADR-0009](adr/0009-identity-installer-scheme-c.md) for installation transactions.

## Building from source

### Prerequisites

Source builds target Windows x64. Install:

- Git.
- Visual Studio 2022 (17.x) with CMake 3.25+, or Visual Studio 2026 (18.x) with CMake 4.2+;
  [the VS 2026 generator was added in CMake 4.2](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html).
  Install the **Desktop development with C++** workload, MSVC x64/x86 build tools, and a Windows 10/11 SDK.
  Current release builds use Visual Studio 2026 and Windows SDK `10.0.28000.0` (servicing version `28000.2526`).
  Windows SDK 10.0.19041 or newer remains the compatibility baseline; CI checks 19041, 22621, and 26100 separately.
  Release builds and older-SDK compatibility checks do not establish recording-mode acceptance on target hardware.
- Windows PowerShell 5.1 or PowerShell 7 for the packaging and contract scripts. Python 3
  is recommended for the full set of Python contract tests; without it, those optional tests
  are not registered. Node.js is used to maintain Unity texture snapshots and Star history, not to build the product.

Start from a fresh checkout:

```powershell
git clone https://github.com/CialloKing/ba-click-fx-desktop.git
cd ba-click-fx-desktop
```

Run from **Developer PowerShell for VS**, or make sure `cmake.exe`, MSVC, and the Windows SDK
are available in the current terminal:

```powershell
cmake --version
git --version
```

### Full build (Spout2 enabled)

The standard `x64` preset enables Spout2. vcpkg itself is not committed to this repository;
install and bootstrap a separate checkout once:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& 'C:\dev\vcpkg\bootstrap-vcpkg.bat'
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
```

If vcpkg is already installed, set `VCPKG_ROOT` to its absolute checkout path. Manifest mode
then installs `spout2[dx]` version `2.007.010#0` from `vcpkg.json` (builtin baseline
`a0400024711b283056538ac19ced80b91a83c24c`) into `build\x64\vcpkg_installed` using the
`x64-windows-static` triplet. The first configure
requires network access to download and build the dependency; later builds can reuse the
local vcpkg cache. Do not rely on an untracked Spout2 archive or edit the installed tree by
hand.

Run the complete Release configure, build, and CTest workflow:

```powershell
cmake --workflow --preset release-verify
```

### Slim build (Spout2 disabled)

Slim is useful when OBS/Spout2 output is not needed. It does not load the vcpkg toolchain and
does not require `VCPKG_ROOT`:

```powershell
cmake --workflow --preset slim-release-verify
```

Slim still builds the complete effects path, Host, and Control Center; only the Spout2 output
control is removed. Full and Slim use separate build trees (`build\x64` and
`build\x64-slim`). Do not switch presets inside the same build directory.

For a faster Full Host-only build, configure the Full tree first and run:

```powershell
cmake --build --preset host-release --parallel 4
```

There is currently no `slim-host-release` preset. For a faster Slim Host build, use:

```powershell
cmake --preset x64-slim
cmake --build build\x64-slim --config Release --target ba_click_fx_desktop --parallel 4
```

The workflows already run tests. They can also be run explicitly after configuration:

```powershell
ctest --preset release
ctest --preset slim-release
```

### Troubleshooting source builds

- If CMake cannot find `vcpkg.cmake`, set `VCPKG_ROOT` to a checkout containing
  `scripts\buildsystems\vcpkg.cmake`, remove the affected `build\x64` tree, and configure
  again. CMake caches the toolchain path.
- If CMake reports a missing Spout2 header or `SpoutDX_static.lib`, let the manifest install
  finish and verify that the Full build is using `x64-windows-static`.
- If `No CMAKE_CXX_COMPILER could be found` or the Windows SDK is missing, add the C++
  workload, MSVC x64 tools, and SDK in Visual Studio Installer, then open a new Developer
  PowerShell.
- A first Full configure needs network access. A failure while downloading the manifest
  dependency is not fixed by changing application source files.

## Launching development builds

Source builds place the two executables in different directories. Start both explicitly so the Control Center connects to the running Host:

```powershell
Start-Process .\build\x64\src\desktop\Release\ba-click-fx-desktop.exe
Start-Process .\build\x64\src\control-center\Release\BAFX.ControlCenter.exe
```

For Slim, replace `x64` with `x64-slim`. To exercise Start Host, use the Portable package below, which places both executables together.
Packaging examples use PowerShell 7's `pwsh`; on Windows PowerShell 5.1, use `powershell` instead.

## Smoke tests and visual demo

Both release workflows enable `BAFX_ENABLE_DESKTOP_SMOKE_TESTS=ON`. Their CTest runs therefore
register four desktop integration tests (smoke, timed-exit, device-recovery, and frame-pacing-stall)
that require a logged-in, unlocked interactive Windows desktop. The Full Debug smoke target is:

```powershell
cmake --build --preset debug --target smoke_desktop
```

That target runs the bounded `--smoke-test` check and exits with code 0 on success. Slim has no
Debug build preset, but its smoke target can be built explicitly:

```powershell
cmake --preset x64-slim
cmake --build build\x64-slim --config Debug --target smoke_desktop
```

The visual demo is a different, continuously running entry point and must not be confused with
the smoke test. The following is the Full Debug path; use the `build\x64-slim` path for Slim:

```powershell
build\x64\src\desktop\Debug\ba-click-fx-desktop.exe --demo-click
build\x64-slim\src\desktop\Debug\ba-click-fx-desktop.exe --demo-click
```

Headless compatibility CI uses an ordinary CMake configure with `BUILD_TESTING=OFF` and does
not enable Spout2. It proves compilation against the selected Windows SDK only; it does not
prove Full/Spout2, interactive smoke, WGC, HDR, or cross-adapter behavior.

## Packaging

The packaging scripts use CMake plus the selected Full/Slim prerequisites. When a script
configures/builds Full, it requires `VCPKG_ROOT` unless a maintainer checkout is available at
`..\SDK\vcpkg`; Slim packaging does not require vcpkg. With `-SkipBuild`, an existing matching
build tree is used and the script does not need to configure vcpkg again.

Portable Full package:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-test-bundle.ps1
```

Portable Slim package:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-test-bundle.ps1 -Slim
```

The script builds the Release Host and Win32 Control Center, writes the executables, support
documents, and per-file SHA-256 manifest to a ZIP, and verifies the archive. It also runs the
bounded smoke check during verification. `-SkipBuild` is only for an already built matching
tree; `-SkipVerification` is an explicit optional bypass and should not be used as release
evidence. The smoke verification requires a logged-in, unlocked interactive Windows desktop;
it should not be used as headless CI evidence.

The default output is `artifacts\local\ba-click-fx-desktop-<version>-Portable-windows-x64.zip`
with a matching `.sha256` file. The Host-only review package is written under
`artifacts\local\host-visual-review\<commit>\`. Host-review and installer scripts also require
an interactive desktop when they build or verify the workflow.

Keep the directory structure when extracting a Portable ZIP. Open `BAFX.ControlCenter.exe`
and click Start Host. The Control Center's Start Host button requires both
executables to remain in the same directory; closing the Control Center does not stop the Host,
which can be exited from the notification area. The Portable package can be copied to another
directory, but it has no Package Identity.

Host-only visual review package:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-host-review-bundle.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-host-review-bundle.ps1 -Slim
```

Installer package:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-user-installer.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-user-installer.ps1 -Slim
```

The installer script requires Inno Setup 6.3 or newer (`ISCC.exe`). It searches `PATH` and the
standard installation directories; use `-ISCC <path>` to select another copy. The installer
uses a target-machine certificate for the Sparse Package template and is not a public code
signature. `-SkipBuild` only applies when the matching Full or Slim outputs already exist.

## Control Center and runtime behavior

The Host and Control Center communicate through a versioned local Named Pipe. Portable builds
keep `BAFX.config.json`, `ba-click-fx-desktop-support.log`, and custom `fx-profiles` beside the
Host. Identity installs keep the same files under the writable `data` directory. A local
`BAFX.Host.v1` mutex enforces one Host instance.

The current configuration is explicit schema 20. Schemas 14 through 19 migrate in a fixed
order; unknown versions, missing sections, unknown fields, and enum aliases are rejected and
the Host uses in-memory defaults after logging the error. New defaults request
`background-aware`, allow the Windows capture border, keep HDR disabled, follow the display
refresh rate, and emit a pressed-only trail without an additional input sampling limit.

The Control Center exposes five top-level pages: Basic, Advanced, Display and Performance,
Hotkeys, and System. It can pause/resume the Host, edit effects and rendering settings, select
the built-in Unity Original, Lightweight, Click-only, and Trail-only profiles, and manage
effects-only custom profiles. Profile files never overwrite background, display, input,
performance, or system settings.

The Control Center uses `UiLanguage`, stable `TextId` values and compiled Chinese/English tables.
**System → System behavior → Language** saves `BAFX.ControlCenter.language` separately in the configuration directory.
Portable stores it beside the executables; installed builds use the installation directory's `data` folder. It contains `auto`, `zh-CN`, or `en-US` and is excluded from release packages.
Retranslation preserves drafts and reuses cached status; it does not write Host settings or start an update check.

The Display and Performance page reports the actual per-display bounds, DPI, physical/capture
refresh rates, DRR, color query results, SDR white level, output fallback, WGC state, and
failure state returned by `GetDisplayState`. It also exposes the opt-in experimental Active-FX
ROI switch. ROI telemetry describes measured execution paths and does not infer GPU savings
from rectangle area.

Host and Control Center must have the same normalized `MAJOR.MINOR.PATCH` product version before
the Control Center enables settings writes. A mismatch is shown explicitly and still permits
starting or stopping the Host. The overlay remains mouse-transparent and does not take focus;
the notification-area menu can pause/resume the Host, and the Host can be exited from that menu.
The default trail is pressed-only; enabling always-on trail makes ordinary pointer movement emit
trail only, without creating a click disk or ring. All global hotkeys are unbound by default.

The three display modes use these wire values:

```text
background-aware
recording-compatible
light-background
```

Both `background-aware` and `recording-compatible` request WGC; `light-background` disables it.
If capture or self-exclusion cannot be established safely, the current batch falls back to FX-only.
Portable builds do not claim borderless capture capability because they have no Package Identity.

`recording-compatible` is a test mode for Windows 11 26H2 and later (minimum OS build `26300`).
It attempts session-local window exclusion, then falls back to global window exclusion and finally
FX-only if needed. Global exclusion may hide effects from external recordings. Passing the version
check does not establish runtime API support or recording compatibility; target-hardware validation remains incomplete.
OBS can receive a separate transparent effects layer through Spout2 without enabling this test mode.

The `effects.bloomIntensity` value is the Unity Bloom scalar (default `1.7`, range `0..10`),
not a multiplier relative to `1.0`. Bloom quality presets map to diffusion values `4/6/7/10`.
Active-FX ROI remains disabled by default and is experimental; WARP and dirty-present counters
are renderer/path contracts, not proof of visible DWM results or whole-system speedups.

Diagnostic logs rotate at 8 MiB per file and keep up to three backups (about 32 MiB total).
`ClearLogs` removes the current log and retained backups after confirmation. When reporting a
problem, provide the current log and any remaining rotated logs; no extra diagnostic package is
required.

## IPC examples

The underlying protocol can be checked with PowerShell or another Named Pipe client:

```text
GetState
GetDisplayState
GetConfig
GetFxConfig
GetHotkeyState
BeginHotkeyCapture
EndHotkeyCapture 42
RetryHotkeys
SetConfig {"generation":1,"path":"effects.globalScale","value":1.25}
SetConfig {"generation":1,"path":"input.trailOnlyWhilePressed","value":false}
SetConfig {"generation":1,"path":"background.mode","value":"recording-compatible"}
SetConfig {"generation":1,"path":"background.allowSystemBorder","value":false}
SetFxParam {"generation":1,"path":"effects.diskRadius","value":40}
SetHotkeys 1 {"togglePause":{"modifiers":["ctrl"],"key":80}}
SaveFxProfile 1 Night Soft
ApplyFxProfile 2 Night Soft
DeleteFxProfile 3 Night Soft
ResetFxConfig
ClearLogs
Pause
Resume
Shutdown
```

`GetDisplayState` accepts only the current strict schema 4 and does not modify configuration.
`GetFxConfig`, `SetFxParam`, `SetFxParams`, and `ResetFxConfig` operate on the native flat
`effects.*` namespace. Generation mismatches return `generation_conflict`; rendering changes
are applied on the next frame. `ResetFxConfig` resets effects only, while the Control Center's
full reset restores other persisted settings and preserves the saved hotkey group.

## Unity reference validation

After configuring a build tree, this read-only command verifies the checked-in Unity reference
file hashes without copying or modifying game assets:

```powershell
cmake --build build\x64 --config Release --target verify_unity_reference
```

## Global hotkeys

The Hotkeys page records, clears, saves, and retries four actions: pause/resume, toggle
always-on trail, next effects profile, and Host shutdown. Registration uses
`RegisterHotKey`/`WM_HOTKEY` with `MOD_NOREPEAT`. A binding may be one non-modifier key or one
key combined with Ctrl, Alt, Shift, or Win. F12, modifier-only bindings, multi-key macros, and
guaranteed Win combinations are not supported. Registration can affect another application;
input is not transparently forwarded.

Recording keeps old registrations active but does not execute their actions. Focus loss,
cancellation, a 30-second timeout, or a five-second disconnect ends capture. Saving is atomic:
new combinations are reserved before the complete configuration is written, and the old set
is retained if registration, generation checks, or disk writes fail. A restart is requested
when activation or old-registration cleanup cannot be confirmed.

## README and Star history maintenance

See the [preview asset notes](images/README.md) for the README image's origin and reproduction commands. After rendering changes, regenerate it and check both language captions.

Use Node.js 24 with no npm installation:

```powershell
node tools/verify-readme.mjs
node tests/star-history.mjs
```

`.github/workflows/star-history.yml` checks documentation and the updater with read-only repository permissions on relevant PRs/main pushes.
Only the daily 03:17 Asia/Shanghai schedule or a manual dispatch writes to `star-history`.
Data commits contain only `README.md`, `stars.csv`, and `star-history.svg`; they do not modify product source or version.

To reproduce an update locally, check out the data branch into a separate directory, then run from this repository root:

```powershell
$env:GITHUB_REPOSITORY = 'CialloKing/ba-click-fx-desktop'
node tools/update-star-history.mjs --data-dir ..\ba-click-fx-desktop-star-history
```

The script reads `GITHUB_TOKEN` or `GH_TOKEN`; without a token it uses public API limits. Never save tokens in files or commits.
Use `--bootstrap` only for an empty data directory: it reconstructs past dates from current stargazers, then records today's actual total.
Reconstruction cannot recover removed Stars. CSV fields `source` and `observed_at` distinguish reconstruction from observations.
Missed dates stay absent; repeating the same day's count leaves files unchanged. API/data validation failures do not write results.
This maintenance task is independent of product builds and Windows graphics acceptance.

## Unity resource maintenance

The packed texture generator is a maintainer tool only. Unity PNGs, Node.js, and the Unity
project are not build or runtime dependencies:

```powershell
node tools\generate-packed-fx-textures.mjs `
  --project "D:\path\to\UnityProject"
```
