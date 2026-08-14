# SPK-002 controlled HWND lifecycle evidence / 2026-08-14

- Spike: `SPK-002 / WGC lifecycle subset`
- Capture and verifier commit: `5d56716`
- Machine/OS: Windows 10 Pro for Workstations `10.0.19045`, x64
- GPU/driver: NVIDIA GeForce RTX 4060 Laptop GPU, `32.0.16.1074`
- Capture target: two controlled top-level `HWND` fixtures
- Result: **Passed for the controlled-window lifecycle subset**

## Steps

```powershell
cmake --build --preset alpha-release --target ba_fx_wgc_lifecycle_spike
$output = "artifacts\spikes\spk-002\rtx4060-win10-19045-window-lifecycle-2026-08-14"
build\alpha-x64\src\capture\Release\ba-click-fx-wgc-lifecycle-spike.exe `
  "--output=$output" `
  --revision=5d56716 `
  --timeout-ms=12000
python -B tools\verify-wgc-lifecycle-spike.py `
  "$output\lifecycle.json" `
  "--report=$output\verification.json"
```

The collector used a 12-second cooperative deadline, a 15-second in-process watchdog and an
independent 22-second harness timeout. The opt-in CTest path additionally uses `RUN_SERIAL` and a
30-second process timeout.

## Raw Evidence

- `lifecycle.json`: 10 resize/close events and 8 restart/stop events
- `verification.json`: status `accepted`, 1 explicit FramePool reconfiguration, 4 acquired frames
- `lifecycle.json` SHA-256: `F7F9403A378704CF68CC4B14B5A6AA20771F1537084E56302B4ED8BA24681D35`
- `verification.json` SHA-256: `795B12F02F1B72A0448C0FA92A0D1E4A226E6D8A3455FB6CB79698403337DB02`

The first session acquired a `320x240` frame, resized the target to `480x300`, observed
`ReconfigureRequired`, recreated the pool, advanced epoch `1 -> 2`, and acquired generation 2 at
the new size. Destroying the first target produced `Stopped`. A second target then started a new
session, acquired a frame, completed explicit stop twice, and released its target afterward.

Across both scenarios every acquired frame was closed. The two FramePools, two Sessions,
`FrameArrived` registrations and `item.Closed` registrations were paired with their respective
close or unregistration operation. All five live-resource counters and both failure counters ended
at zero.

## Limitations

- This is a controlled `HWND` lifecycle result, not monitor `ContentSize` or display-close evidence.
- The probe allowed the Windows system capture border. It does not prove borderless permission,
  denial fallback or self-exclusion.
- Cursor exclusion was confirmed through the session capability; pixel inclusion/exclusion was not
  part of this collector. The separate controlled pixel matrix is recorded in
  `../rtx4060-win10-19045-cursor-pixels-2026-08-14/README.md`.
- `BackgroundAware`/`RecordingCompatible` mode switching and external recorder observations were
  not part of this collector.
- HOT/WARM/COLD power behavior, pressure/soak, device loss and multi-display cases remain `Not Run`.
- This subset result does not accept the full SPK-002 matrix or promote ADR-003 from `Proposed`.
