# ba-click-fx-desktop

[中文（默认文档）](README.md) · [Download the latest release](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest)

[![GitHub Stars](https://img.shields.io/github/stars/CialloKing/ba-click-fx-desktop.svg)](https://github.com/CialloKing/ba-click-fx-desktop/stargazers)

Native Windows desktop click effects and cursor trails, using Blue Archive's Unity/game resources as the visual reference.
Includes a transparent overlay, a native Control Center, and transparent effects output for OBS. Current product version: **0.2.12**.

Targets Windows 10/11 x64; current visual review covers a single primary SDR display. Release users do not need to install
the Visual C++ runtime, Windows App SDK, or development tools. The Host renders effects; the Control Center manages settings and lifecycle.

## Contents

- [Download and quick start](#download-and-quick-start)
- [Settings and rendering modes](#settings-and-rendering-modes)
- [Support boundaries and certificates](#support-boundaries-and-certificates)
- [OBS and Spout2](#obs-and-spout2)
- [Building from source](#building-from-source)
- [Documentation](#documentation)
- [Star History](#star-history)
- [Development notes](#development-notes)
- [License](#license)

## Download and quick start

Use the [official Release page](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest).
Installer/Portable describes installation; Full/Slim describes build variants. Official releases contain four Full assets:
the installer, Portable ZIP, and a `.sha256` checksum for each. Slim is available through source builds and local packaging only.

| Need | Choose | Details |
|---|---|---|
| Everyday use, Start Menu shortcuts, or trying borderless WGC | Full Installer: `*-setup-windows-x64.exe` | Requires administrator UAC; registers Package Identity for the current user |
| No administrator access, extract and run | Full Portable: `*-Portable-windows-x64.zip` | No Package Identity; does not promise borderless WGC |
| Transparent OBS effects | Either Full release package above | Includes the Spout2 sender; install the OBS receiver plugin separately |
| Build from source without Spout2 | Slim | Still includes the Host, Control Center, and complete effects |

1. Download the installer or ZIP and its matching `.sha256`. In PowerShell, compare the SHA-256 hash with the checksum file's first column:

   ```powershell
   Get-FileHash -Algorithm SHA256 -LiteralPath '.\downloaded-filename'
   Get-Content -LiteralPath '.\downloaded-filename.sha256'
   ```

2. **Installer**: run it and approve UAC. It opens the Control Center after installation and adds Start Menu/desktop shortcuts.
   **Portable**: extract the complete archive into a writable directory and open `BAFX.ControlCenter.exe`.
   Keep its directory structure and accompanying files; `ba-click-fx-desktop.exe` must be beside the Control Center to start the Host.
3. Click Start Host and adjust effects, trails, and background mode on the Basic page. Trails appear while the mouse is pressed by default;
   enabling always-on trail also emits trails during ordinary movement.
4. Pause/resume from the Control Center or notification-area menu. Stop the process with Close Host or the Host's notification-area Exit command.
   Closing the Control Center window does not automatically stop the Host.

**Updates and configuration**: click Check for updates on the System page, then download and run the new installer or replace the program files
from a complete Portable package. Exit the Host and back up configuration first. Do not mix Host and Control Center versions.
The version check never downloads or installs updates automatically.

Portable stores `BAFX.config.json`, `fx-profiles`, and logs beside the executables; installed builds use the installation directory's `data` folder.
Uninstall through the Start Menu or Windows Installed apps. Uninstall preserves `data` by default; for a complete reset, back it up,
exit the applications, uninstall, then delete that folder.

## Settings and rendering modes

The five Control Center pages are Basic, Advanced, Display and Performance, Hotkeys, and System. Common settings include effect size,
trail length/width, Bloom intensity/quality, Windows startup, and built-in/custom effects profiles.
Profiles store effects only; they do not overwrite background, display, input, performance, or system settings.

| Background mode | Use and behavior |
|---|---|
| Background-aware (`background-aware`, default) | Composites with a WGC background sample; capture or self-exclusion failure falls back to FX-only |
| Recording-compatible fit (`recording-compatible`) | Disables WGC and uses a transparent-overlay fit; compatibility with every recorder is not guaranteed |
| Light-background optimization (`light-background`) | Disables WGC and applies a stricter alpha limit; useful for comparing effects on light desktops |

FX-only renders effects without a captured background. It is an internal fallback, not a fourth selectable background mode.
None of the modes guarantees pixel-for-pixel reproduction of game visuals on arbitrary desktops.

Core performance mode retains disks, rings, shards, and trails while skipping Bloom and WGC. It uses conservative SDR, 60 FPS, and FX-only.
It is independent of the Full/Slim build variants. HDR requests and the experimental adaptive Active-FX ROI option default to off.

Global hotkeys are all unbound by default. The Hotkeys page configures pause/resume, always-on trail, next profile, and Host shutdown.
Bindings accept a single main key or Ctrl/Alt/Shift/Win plus a main key; F12 is prohibited. System or application conflicts may prevent registration.
Reset defaults preserves saved hotkeys and the current paused/running state.

## Support boundaries and certificates

| Capability | Current scope |
|---|---|
| Click effects, trails, Control Center, FX-only | Testable on Windows 10/11 x64 with a single primary SDR display; see the support document for evidence |
| Full/Slim builds, Portable/installer | Build and packaging validation exists; Slim is source-only |
| WGC background capture | Depends on the OS, runtime APIs, and self-exclusion; failure falls back to FX-only |
| Borderless WGC on Windows 11 target hardware | **Not Run**: real user authorization, borderless visuals, and final DWM pixels still await acceptance |
| HDR/Advanced Color | Experimental paths exist; no completed support claim |
| Multiple displays, mixed DPI/refresh rates, cross-adapter operation | Full hardware acceptance remains incomplete |
| OBS/Spout2 | Configure Full as described below; plugin and recording acceptance depends on the specific environment |

Successful builds, offscreen tests, and WARP software rendering do not prove physical hardware support.
See [SUPPORT.md](SUPPORT.md) and [validation documentation](docs/VALIDATION.md) for evidence, exclusions, and diagnostic fields.

**User self-signing**: the installer creates a target-machine certificate to sign the Sparse Package that supplies Package Identity.
The distributed installer has no public code signature, so SmartScreen may show "Unknown Publisher". Users do not need separate certificates, MSIX files, or SDK tools.

**Host startup is not gated by certificate validation**. Certificates affect only borderless WGC. Expiry, missing/damaged certificates,
or signature mismatch makes borderless requests fall back to FX-only; the Host and other capture paths can still run.
Valid certificates have no near-expiry threshold. Running the current installer again, including same-version repair, creates a new certificate and signs again.

**Installation-state validation is separate**: the Control Center requires a complete matching pair of `INSTALL-STATE.json` and `.bak`
to activate an installed Host. For an abnormal installation state, run the installer again as the same Windows user who runs the Host;
do not manually delete state or transaction files. See [ADR-0009](docs/adr/0009-identity-installer-scheme-c.md) for signing, certificate storage, and recovery details.

The Windows yellow capture border is allowed by default. If you disable Allow yellow capture border, capture starts only after borderless permission
and capability checks succeed; otherwise it falls back to FX-only. Successful installation alone does not prove borderless authorization or target-hardware acceptance.

## OBS and Spout2

1. Install a compatible [Spout2 receiver plugin](https://github.com/Off-World-Live/obs-spout2-plugin/releases/) for OBS.
   Enable OBS transparent effects output on the Control Center's System page and check sender/plugin status.
2. Put game/desktop capture below the `Spout2 Capture` source in OBS. Select sender `ba-click-fx-desktop` for the top source.
3. Set Composite Mode to **`Premultiplied Alpha`**; keep the source blending method at **`Default`** and blending mode at **`Normal`**.
4. Apply `Transform -> Fit to Screen` to the Spout2 source. Idle frames show only the underlying capture; clicks and drags overlay effects.

Spout2 sends only transparent effects, without desktop background pixels, and can work when WGC is unavailable.
See the [OBS guide](docs/OBS_SPOUT2.md) for plugin detection, older-scene migration, and acceptance procedures.

## Building from source

Install Git, CMake 3.25+, Visual Studio 2022+ with **Desktop development with C++**, MSVC x64 tools, and Windows SDK 10.0.19041+.
Scripts support Windows PowerShell 5.1/PowerShell 7. Python 3 enables the full Python contract tests.
Node.js is for maintenance tools such as Unity resource generation and Star history; ordinary product builds do not require it.

Run in Developer PowerShell. Tests require a logged-in, unlocked interactive Windows desktop:

```powershell
git clone https://github.com/CialloKing/ba-click-fx-desktop.git
cd ba-click-fx-desktop
```

**Full (with Spout2)**: bootstrap a separate vcpkg checkout once. If already installed, just set `VCPKG_ROOT`.

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& 'C:\dev\vcpkg\bootstrap-vcpkg.bat'
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
cmake --workflow --preset release-verify
```

**Slim (without Spout2 or vcpkg)**:

```powershell
cmake --workflow --preset slim-release-verify
```

Development builds place the Host and Control Center in separate output directories.
See the [development guide](docs/DEVELOPMENT.en.md) for launch layout, complete builds, packaging, troubleshooting, tests, and IPC examples.

## Documentation

- [Development guide](docs/DEVELOPMENT.en.md): complete build, packaging, IPC, and resource maintenance instructions.
- [Support and validation scope](SUPPORT.md): testable behavior, logs, and explicit exclusions.
- [Architecture](ARCHITECTURE.md) and [ADRs](docs/adr/README.md): rendering, control-plane, and installation decisions.
- [Roadmap](docs/ROADMAP.md), [Spikes](docs/SPIKES.md), and [validation](docs/VALIDATION.md): priorities and evidence gates.
- [Unity reference](docs/UNITY_REFERENCE.md): visual references and external-resource evidence boundaries.
- [Changelog](CHANGELOG.md): version history. Detailed reference documents currently use Chinese.

## Star History

The dedicated `star-history` branch stores Star-count history. GitHub Actions schedules updates daily at 03:17 Asia/Shanghai; actual execution may be delayed.

<p align="center">
  <a href="https://github.com/CialloKing/ba-click-fx-desktop/blob/star-history/stars.csv">
    <img src="https://raw.githubusercontent.com/CialloKing/ba-click-fx-desktop/refs/heads/star-history/star-history.svg" alt="ba-click-fx-desktop Star history" width="960">
  </a>
</p>

[View the raw CSV data](https://github.com/CialloKing/ba-click-fx-desktop/blob/star-history/stars.csv).
Pre-bootstrap rows are reconstructed from current stargazers (`reconstructed`) and cannot recover removed Stars.
Daily API totals recorded afterward are marked `observed` and may decrease. Missed dates remain absent; no interpolated or fabricated snapshots are added.

## Development notes

The desktop application is built from scratch using C++20, Win32, D3D11, HLSL, and DirectComposition.
The separate [web ba-click-fx project](https://github.com/CialloKing/ba-click-fx) has a [browser demo](https://ba-click-fx.cialloking.top).
Both use Unity/game resources as the visual reference; rendering implementations, configuration, and IPC are independent.

The project is primarily generated and iterated with AI (**no handwritten code**), with runtime testing, parameter tuning, and visual calibration.
The architecture contract remains **v0.3 / Proposed**; implemented paths do not automatically become hardware support claims.

## License

[GNU GPL v2](LICENSE). See [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) for accompanying component licenses.
