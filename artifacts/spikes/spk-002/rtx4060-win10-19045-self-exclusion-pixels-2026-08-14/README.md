# SPK-002 monitor WGC self-exclusion pixel evidence / 2026-08-14

- Spike: `SPK-002 / WDA self-exclusion dynamic pixel subset`
- Capture and verifier commit: `c1c4536`
- Machine/OS: Windows 10 Pro for Workstations `10.0.19045.6466`, x64
- GPU/driver: NVIDIA GeForce RTX 4060 Laptop GPU, NVIDIA `610.74` / Windows `32.0.16.1074`
- Capture target: primary `3840x2160` monitor; controlled `640x320` crop with one `256x256` production overlay
- Result: **Passed for the controlled monitor-WGC dynamic WDA pixel subset**

## Steps

```powershell
cmake --build --preset alpha-release --target ba_fx_wgc_self_exclusion_spike -- /m:1
$revision = git rev-parse --short HEAD
$output = "artifacts\spikes\spk-002\rtx4060-win10-19045-self-exclusion-pixels-2026-08-14"
build\alpha-x64\src\capture\Release\ba-click-fx-wgc-self-exclusion-spike.exe `
  "--output=$output" `
  "--revision=$revision" `
  --timeout-ms=15000
python -B tools\verify-wgc-self-exclusion-spike.py `
  "$output\self-exclusion.json" `
  "--report=$output\verification.json"
```

The collector uses a 15-second cooperative deadline, an 18-second in-process watchdog and an
independent 30-second command timeout. The opt-in CTest path is also `RUN_SERIAL` with a 30-second
process timeout. The run completed in about 1.2 seconds.

## Raw Evidence

- Affinity sequence: requested and observed `WDA_NONE -> WDA_EXCLUDEFROMCAPTURE -> WDA_NONE`.
- Both included captures differ from the excluded capture at all `36864/36864` overlay ROI pixels;
  maximum RGB delta is `0.7938690185546875`.
- The excluded overlay ROI and the same-size remote background ROI are pixel-identical. Their
  maximum RGB delta and changed-pixel count are both zero, so a uniform black protection surface
  cannot satisfy this evidence.
- The repeated included overlay ROI and the remote control ROI have zero cross-stage RGB delta.
- Three non-overlapping `64x64` stage markers are present in raw FP16. Every pair changes all
  `4096` marker pixels; the minimum pairwise RGB delta is `1.201629638671875`.
- Every accepted frame advances its generation and QPC marker. The stable pairs are temporally
  ordered, and each stage restores `WS_EX_LAYERED | WS_EX_TRANSPARENT` (`0x082800A8` observed).
- Fourteen acquired frames were closed. One FramePool, one Session and both event registrations
  were paired with close or unregistration; all live-resource and failure counters ended at zero.
- PNG files are informational previews. The verifier reads the three `.rgba16f` files and
  independently recomputes every acceptance metric instead of trusting `self-exclusion.json`.

SHA-256:

- `self-exclusion.json`: `BA7349668D176DBEA12D39F4F08C00649084361B2329425EFF360E4ADDEAA801`
- `verification.json`: `5FA376A284CEFBC7F4D847DC24BBF595EAA16262473CC3EA92411D0797F08988`
- `included-before.rgba16f`: `7728BD09DDB5DE3564A0FF0018AC78A5176D7153103D91FF0AA99B6CDFBCE89B`
- `excluded.rgba16f`: `25E01A5E2EDFF92EEF68EC464B8668E4B68273FCB06F006C7DF2E878841814EB`
- `included-after.rgba16f`: `F97E29D4FB8A5646492588BAA75599491648BF131E29CFD06C4E517FF1DD0469`
- `included-before.png`: `D1423BECF771DD6FA57CF01D8D277789F0CA05A520A85C515F10DDB3D6615282`
- `excluded.png`: `855F7B4628245447CC842FB6EAE7C9E241A4C3B5095CC9E404EDCFA70043ECC9`
- `included-after.png`: `0F63E88018AF5179F8FE5CB41A97D0985FDB682083B8B235F7BE12C1EB1E13BB`

## Limitations

- This is one primary-monitor result on one Windows 10/GPU/driver combination.
- The matrix deliberately keeps one monitor-WGC Session alive while changing affinity. It does
  not validate the product transaction order `Stop sensor -> change WDA -> start sensor`.
- The probe allows the Windows system capture border. It does not prove borderless permission,
  package identity, denial fallback or final privacy-border visibility.
- It does not cover HDR/Advanced Color, another monitor, mixed DPI, multi-adapter, display close,
  device loss, external recorders or `BackgroundAware`/`RecordingCompatible` product switching.
- This subset result does not accept the full SPK-002 matrix or promote ADR-003/ADR-004 from
  `Proposed`.
