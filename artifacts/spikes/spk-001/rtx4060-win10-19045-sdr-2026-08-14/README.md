# SPK-001 SDR evidence / 2026-08-14

- Spike: `SPK-001 / DirectComposition FP16 extended premultiplied composition`
- Capture commit: `9f5b777`
- Verifier commit: `97af85b`
- Machine/OS: Windows `10.0.19045`, primary display `3840x2160`
- GPU/driver: NVIDIA GeForce RTX 4060 Laptop GPU, `32.0.16.1074`, adapter LUID `00000000:0001153F`
- Display/color mode: `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`, 10 bits per color, SDR
- Result: **Passed for the SDR matrix cell**

## Steps

```powershell
cmake --build --preset alpha-release --target ba_fx_composition_spike
$output = "artifacts\local\spikes\spk-001\$env:COMPUTERNAME-2026-08-14-sdr-9f5b777"
build\alpha-x64\src\capture\Release\ba-click-fx-composition-spike.exe `
  "--output=$output" `
  --revision=9f5b777 `
  --timeout-ms=25000
python -B tools\verify-composition-spike.py `
  "$output\capture.json" `
  "--report=$output\verification.json"
```

The collector used the production `R16G16B16A16_FLOAT` swap chain, scRGB color space,
premultiplied DirectComposition target and `Present(1, 0)`. A separate hardware D3D11 device on
the same adapter observed the monitor through WGC with the overlay included and the cursor excluded.
The collector's total run was bounded by an internal 25-second watchdog. The evidence invocation
was additionally bounded by its execution harness at 40 seconds.

## Raw Evidence

- `capture.json`: 4 backgrounds, 20 presentations, two stable WGC samples per presentation
- `verification.json`: 48 source-over channel checks, maximum absolute error `0.001953125`
- `capture.json` SHA-256: `DE95B80713D33BB4682F441F14FA86DD4A1B179EF373B0EE12EBBE16B75F3284`
- `verification.json` SHA-256: `0AEB335D39415054D48A1226A8B03A274F203E220177F655FD75CD652E6D1040`

Black-background observations retained `(rgb,a)=(0.25,0)` as `0.25` RGB and retained
`(1,0.25)` as `1.0` RGB after DComp/DWM. The `(4,0.5)` case remained `4.0` on black. All black,
18% gray, color and white cases matched `C = S.rgb + (1-S.a)B` within the locked FP16 tolerance;
the WGC verifier recorded no transport-formula degradation.

## Limitations

- HDR active was not available in this run and remains `Not Run`.
- The constant source is injected by `ClearRenderTargetView` into the production swap chain. Current
  FP16 layer Golden separately proves the final shader output and layer formula; neither evidence is
  claimed to replace the other.
- `desktopGdiDiagnosticSrgb8` is an unsynchronized diagnostic. It demonstrates that legacy GDI
  capture can canonicalize `A>0` output and is not used for the WGC source-over verdict.
- This is not a physical scanout measurement. The white-background GDI diagnostic saturated at
  `[255,255,255]`; final visible SDR headroom remains an explicit product-degradation boundary.
- This result does not accept ADR-001 or prove SPK-002/SPK-003 hardware matrices.
