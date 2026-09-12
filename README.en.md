# ba-click-fx-desktop

[中文（默认文档）](README.md) · [Download the latest release](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest)

[![GitHub Stars](https://img.shields.io/github/stars/CialloKing/ba-click-fx-desktop.svg)](https://github.com/CialloKing/ba-click-fx-desktop/stargazers)

Native Windows desktop click effects and cursor trails, using Blue Archive's Unity/game resources as the visual reference.
Includes a transparent overlay, a native Control Center, and transparent effects output for OBS. Current product version: **0.2.12**.

Targets Windows 10/11 x64; the installer requires OS build `19041` or later. Current visual review covers a single primary SDR display.
Release users do not need the Visual C++ runtime, Windows App SDK, or development tools. The Host renders effects; the Control Center manages settings and lifecycle.
The Control Center currently uses a Chinese interface; key controls below include their on-screen Chinese labels.

## Contents

- [Download and quick start](#download-and-quick-start)
- [Settings and rendering modes](#settings-and-rendering-modes)
- [Support boundaries and certificates](#support-boundaries-and-certificates)
- [OBS and Spout2](#obs-and-spout2)
- [FAQ and feedback](#faq-and-feedback)
- [Building from source](#building-from-source)
- [Documentation](#documentation)
- [Star History](#star-history)
- [Development notes](#development-notes)
- [License](#license)

## Download and quick start

Use the [official Release page](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest).
Official releases contain four Full assets: the installer, Portable ZIP, and a `.sha256` checksum for each.
Both packages include complete effects, the Control Center, and the Spout2 sender; OBS requires a separate receiver plugin.

| Need | Choose | Details |
|---|---|---|
| Everyday use, Start Menu and desktop shortcuts | Installer: `*-setup-windows-x64.exe` | Requires administrator UAC; registers the current user's app identity; borderless WGC still needs system permission |
| No administrator access, extract and run | Portable: `*-Portable-windows-x64.zip` | Extract into a writable directory; no app identity or promise of borderless WGC |

1. Download the installer or ZIP from the table and its matching `.sha256`; expand the verification instructions below to check it.
2. **Installer**: run it and approve UAC. It opens the Control Center after installation and adds Start Menu/desktop shortcuts.
   **Portable**: extract the complete archive into a writable directory and open `BAFX.ControlCenter.exe`.
   Keep its directory structure and accompanying files; `ba-click-fx-desktop.exe` must be beside the Control Center to start the Host.
3. Click Start Host (启动 Host), then click or drag the mouse to see effects. Adjust effects, trails, and background mode on the Basic settings (基础设置) page.

<details>
<summary>Verify the download (SHA-256)</summary>

Run these commands in PowerShell, replacing the placeholder with the full downloaded filename:

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath '.\downloaded-filename'
Get-Content -LiteralPath '.\downloaded-filename.sha256'
```

Compare the computed SHA-256 with the checksum file's first column. If they differ, download again before installing or extracting.

</details>

**Updates and configuration**: click Check for updates (检查更新) on the System (系统) page, then download and run the new installer or replace the program files
from a complete Portable package. Exit the Host and back up configuration first. Do not mix Host and Control Center versions.
The version check never downloads or installs updates automatically.

Portable stores `BAFX.config.json`, `fx-profiles`, and logs beside the executables; installed builds use the installation directory's `data` folder.
Uninstall through the Start Menu or Windows Installed apps. Uninstall preserves `data` by default; for a complete reset, back it up,
exit the applications, uninstall, then delete that folder.

## Settings and rendering modes

The five Control Center pages are Basic settings (基础设置), Advanced parameters (高级参数), Display and Performance (显示与性能), Hotkeys (快捷键), and System (系统). Common settings include effect size,
trail length/width, Bloom intensity/quality, Windows startup, and built-in/custom effects profiles.
Profiles store effects only; they do not overwrite background, display, input, performance, or system settings.

| Background mode | Use and behavior |
|---|---|
| Background-aware (背景感知, `background-aware`, default) | Composites with a WGC background sample; capture or self-exclusion failure falls back to FX-only |
| Recording-compatible (录屏兼容, test mode, `recording-compatible`) | Selectable only on OS build `28000` or later; tries WGC session-local self-exclusion, then falls back to other capture paths or FX-only if unavailable; recording compatibility still awaits acceptance |
| Light-background optimization (浅色背景优化, `light-background`) | Disables WGC and applies a stricter alpha limit; useful for comparing effects on light desktops |

FX-only renders effects without a captured background. It is an internal fallback, not a fourth selectable background mode.
The recording-compatible option is labeled “录屏兼容（测试，仅 Windows 11 26H1 及以后）”; the selection is rejected if the OS build is too old or cannot be determined.
None of the modes guarantees pixel-for-pixel reproduction of game visuals on arbitrary desktops.

Core performance mode (核心性能模式（关闭 Bloom 与背景）) retains disks, rings, shards, and trails while skipping Bloom and WGC. It uses conservative SDR, 60 FPS, and FX-only.
It is independent of the Full/Slim build variants. HDR requests and the experimental adaptive Active-FX ROI option default to off.

Global hotkeys are all unbound by default. The Hotkeys page configures pause/resume, always-on trail, next profile, and Host shutdown.
Bindings accept a single main key or Ctrl/Alt/Shift/Win plus a main key; F12 is prohibited. System or application conflicts may prevent registration.
Reset defaults (重置默认) preserves saved hotkeys and the current paused/running state.

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

**Host startup is not gated by certificate validation**. Certificate problems affect only borderless WGC: failed requests fall back to FX-only, while the Host and other capture paths can still run.
Run the current installer again, including for same-version repair, to sign again. Successful installation alone does not prove borderless authorization or hardware acceptance.

The distributed installer has no public code signature, so SmartScreen may show "Unknown Publisher". Users do not need separate certificates, MSIX files, or SDK tools.

<details>
<summary>Certificates versus an abnormal installation state</summary>

The installer creates a target-machine certificate to sign the Sparse Package that supplies Package Identity. Each run of the current installer creates a new certificate and signs again.
Valid certificates have no near-expiry threshold; expiry, missing/damaged certificates, or signature mismatch makes borderless requests fall back to FX-only.

Installation-state validation is separate: the Control Center requires a complete matching pair of `INSTALL-STATE.json` and `.bak` to activate an installed Host.
An abnormal installation state (安装状态异常) still blocks startup. Run the installer again as the same Windows user who runs the Host; do not manually delete state or transaction files.
See [ADR-0009](docs/adr/0009-identity-installer-scheme-c.md) for signing, certificate storage, and recovery details.

</details>

## OBS and Spout2

1. Install a compatible [Spout2 receiver plugin](https://github.com/Off-World-Live/obs-spout2-plugin/releases/) for OBS.
   Check Enable OBS transparent effects output (启用 OBS 透明特效输出) on the Control Center's System page and check sender/plugin status.
2. Put game/desktop capture below the `Spout2 Capture` source in OBS. Select sender `ba-click-fx-desktop` for the top source.
3. Set Composite Mode to **`Premultiplied Alpha`**; keep the source blending method at **`Default`** and blending mode at **`Normal`**.
4. Apply `Transform -> Fit to Screen` to the Spout2 source. Idle frames show only the underlying capture; clicks and drags overlay effects.

Spout2 sends only transparent effects, without desktop background pixels, and can work when WGC is unavailable.
See the [OBS guide](docs/OBS_SPOUT2.md) for plugin detection, older-scene migration, and acceptance procedures.

## FAQ and feedback

**No trail when moving the mouse?** Trails appear only while a mouse button is held by default. Start the Host and make sure Mouse trail (鼠标拖尾) is enabled on the Basic settings page.
Enable Always-on trail (拖尾常驻) to show trails during ordinary movement.

**A yellow border appears on screen?** This is the Windows capture indicator, allowed by default. Disable Allow yellow capture border (允许黄色捕获边框) on the Basic settings page
to start capture only when borderless permission and capability checks succeed; otherwise the Host falls back to FX-only. Installed builds also need system permission.

**Effects continue after closing the Control Center?** The Control Center and Host are separate processes. Use Pause effects (暂停特效)/Resume effects (恢复特效) or the notification-area menu for a temporary pause.
To stop effects completely, click Close Host (关闭 Host) or use the Host's notification-area Exit command. Closing the Control Center window does not automatically stop the Host.

**No effects in OBS?** Make sure the Host is running and not paused. On the System page, check Enable OBS transparent effects output and inspect sender/plugin status.
In OBS, select the correct sender, keep the Spout2 source visible and on top, and check alpha compositing and canvas size using the [steps above](#obs-and-spout2). Idle frames are fully transparent; click or drag to test.

If the issue persists, report it in [GitHub Issues](https://github.com/CialloKing/ba-click-fx-desktop/issues) with:

- Host and Control Center versions, and whether you use the Installer or Portable package.
- Windows version and OS build (run `winver`), display count, and HDR status; for OBS issues, include OBS and plugin versions.
- Reproduction steps, the time of the issue, expected and actual behavior, and your background mode.
- `ba-click-fx-desktop-support.log`; include relevant `.log.1` through `.log.3` files if the affected period has rotated out. Portable logs are beside the executables; installed builds use the installation directory's `data` folder.

For installation or uninstallation failures, attach the installer log at the full path shown in the error dialog. Check logs for usernames and local paths before posting publicly.
See [SUPPORT.md](SUPPORT.md) for further diagnostics.

## Building from source

Full/Slim are build variants, separate from the Installer/Portable installation choices. Full includes Spout2; Slim removes Spout2 while retaining the Host, Control Center, and complete effects.
Slim is available through source builds and local packaging only; there are no official prebuilt downloads.

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

<details>
<summary>How Star history is recorded</summary>

Pre-bootstrap rows are reconstructed from current stargazers (`reconstructed`) and cannot recover removed Stars.
Daily API totals recorded afterward are marked `observed` and may decrease. Missed dates remain absent; no interpolated or fabricated snapshots are added.

</details>

## Development notes

The desktop application is built from scratch using C++20, Win32, D3D11, HLSL, and DirectComposition.
The separate [web ba-click-fx project](https://github.com/CialloKing/ba-click-fx) has a [browser demo](https://ba-click-fx.cialloking.top).
Both use Unity/game resources as the visual reference; rendering implementations, configuration, and IPC are independent.

The project is primarily generated and iterated with AI (**no handwritten code**), with runtime testing, parameter tuning, and visual calibration.
The architecture contract remains **v0.3 / Proposed**; implemented paths do not automatically become hardware support claims.

## License

[GNU GPL v2](LICENSE). See [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) for accompanying component licenses.
