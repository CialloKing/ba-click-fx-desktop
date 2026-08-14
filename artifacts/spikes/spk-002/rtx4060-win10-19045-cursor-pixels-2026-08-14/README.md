# SPK-002 controlled HWND cursor pixel evidence / 2026-08-14

- Spike: `SPK-002 / WGC cursor inclusion-exclusion subset`
- Capture and verifier commit: `d41b9a0`
- Machine/OS: Windows 10 Pro for Workstations `10.0.19045`, x64
- GPU/driver: NVIDIA GeForce RTX 4060 Laptop GPU, `32.0.16.1074`
- Capture target: one controlled `320x240` top-level `HWND` fixture
- Result: **Passed for the controlled-window cursor pixel subset**

## Steps

```powershell
cmake --build --preset alpha-release --target ba_fx_wgc_cursor_spike
$output = "artifacts\spikes\spk-002\rtx4060-win10-19045-cursor-pixels-2026-08-14"
build\alpha-x64\src\capture\Release\ba-click-fx-wgc-cursor-spike.exe `
  "--output=$output" `
  --revision=d41b9a0 `
  --timeout-ms=12000
python -B tools\verify-wgc-cursor-spike.py `
  "$output\cursor.json" `
  "--report=$output\verification.json"
```

The collector used a 12-second cooperative deadline, a 15-second in-process watchdog and an
independent 30-second harness timeout. The opt-in CTest path additionally uses `RUN_SERIAL` and a
30-second process timeout. It saves and restores the cursor position without using `ClipCursor`,
`BlockInput`, `SetSystemCursor` or `ShowCursor`.

## Raw Evidence

- `cursor.json`: three explicitly controlled WGC sessions in
  `included-before -> excluded -> included-after` order
- `verification.json`: status `accepted`, 176 changed cursor pixels before and after exclusion,
  maximum RGB delta `1.9711151123046875`, 0 repeat-instability pixels and 0 control-ROI delta
- `included-before.rgba16f` and `included-after.rgba16f`: identical authoritative FP16 captures
- `excluded.rgba16f`: authoritative FP16 cursor-excluded capture
- PNG files: informational previews only; the verifier reads and recomputes from FP16 payloads

SHA-256:

- `cursor.json`: `90F6AA35C5ECA5CAB716BAD6C591759D54C5213201DDE0DCD17F889105F3BC27`
- `verification.json`: `EF1C7AA984017510A1501805A975C57B52B90E390DFC33457B8578C963303FB2`
- `included-before.rgba16f`: `D3E4DD1C5327EAD52397D166419899495BB6B1C89800E7A533E2C4391C1CD0CB`
- `included-after.rgba16f`: `D3E4DD1C5327EAD52397D166419899495BB6B1C89800E7A533E2C4391C1CD0CB`
- `excluded.rgba16f`: `B5ACB7E3B56D190B94FAEA5802DE3F81FC90CAD13D407FE957158E2D005ACC71`
- `included-before.png`: `763293CC6BE6BC19BC16E3D0E33D90402480C40340E20AFC81006CB3611CE581`
- `included-after.png`: `763293CC6BE6BC19BC16E3D0E33D90402480C40340E20AFC81006CB3611CE581`
- `excluded.png`: `F90E0E8E6D3150DE1D8C910D55078238D210C3E9BC7F78E8A06D9FBAD410134F`

Each session explicitly wrote and read back `IGraphicsCaptureSession2::IsCursorCaptureEnabled`.
The included sessions confirmed `true`; the excluded session confirmed `false`. Every accepted
sample advanced beyond its transition generation and QPC marker. The custom 32x32 monochrome
cursor has 176 opaque pixels; both included captures changed exactly those 176 pixels within
`(148,108)-(171,131)`. The excluded ROI was uniform. The remote 32x32 control ROI and both
included captures were pixel-identical.

Six acquired frames were closed. Three FramePools, three Sessions, three `FrameArrived`
registrations and three `item.Closed` registrations were paired with close or unregistration.
All live-resource and failure counters ended at zero.

## Limitations

- This is one controlled `HWND` result on one Windows 10/GPU/driver combination, not monitor
  capture, multi-display, DPI-scaling or cursor-theme coverage.
- The probe allowed the Windows system capture border. It does not prove borderless permission,
  denial fallback, package identity or final DWM border visibility.
- The fixture was the topmost window under the cursor. User movement, hidden/suppressed cursor or
  another window taking that point causes the run to fail rather than weakening the thresholds.
- Self-exclusion, `BackgroundAware`/`RecordingCompatible` transitions and external recorder output
  were not part of this collector.
- This subset result does not accept the full SPK-002 matrix or promote ADR-003 from `Proposed`.
