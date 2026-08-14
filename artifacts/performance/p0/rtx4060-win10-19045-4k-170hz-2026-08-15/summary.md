# P0 paired performance baseline

- Status: `passed`
- Scenario: `p0-static-click-message-pressure-v3`
- Revision: `c87c83ae566b874e976c7c95230f58c0487ad31d`
- Captured at: `2026-08-14T17:40:42.646Z`
- Workload: fixed 130 ms click after 50 ms WGC warm-up, 10500 ms
- Message pressure: 705 harmless thread messages, 5 per batch every 25 ms
- Adapter: `NVIDIA GeForce RTX 4060 Laptop GPU`
- Driver: `32.0.16.1074`
- Output: `3840x2160`

| Metric | FX-only | background-aware | delta | ratio |
|---|---:|---:|---:|---:|
| Frames | 1667 | 1659 | -8 | 0.995 |
| Presented FPS | 166.643 | 165.832 | -0.811 | 0.995 |
| Raw Input messages | 0 | 0 | 0 | unavailable |
| Input queue age p95 (ms) | unavailable | unavailable | unavailable | unavailable |
| Input queue age max (ms) | unavailable | unavailable | unavailable | unavailable |
| Message-to-Present p95 (ms) | unavailable | unavailable | unavailable | unavailable |
| Message-to-Present max (ms) | unavailable | unavailable | unavailable | unavailable |
| Other messages dispatched | 706 | 706 | 0 | 1.000 |
| Frame-ready wakes | 1667 | 1659 | -8 | 0.995 |
| Message wakes | 213 | 161 | -52 | 0.756 |
| Frame pacing timeouts | 0 | 0 | 0 | unavailable |
| Frame pacing failures | 0 | 0 | 0 | unavailable |
| WGC producer FPS | 0.000 | 167.532 | 167.532 | unavailable |
| WGC accepted FPS | 0.000 | 164.433 | 164.433 | unavailable |
| WGC accepted samples | 0 | 1645 | 1645 | unavailable |
| WGC sample age p95 (us) | unavailable | 186 | unavailable | unavailable |
| CPU WGC drain p95 (us) | unavailable | 213 | unavailable | unavailable |
| CPU Present p50 (us) | 211 | 60 | -151 | 0.284 |
| CPU Present p95 (us) | 346 | 93 | -253 | 0.269 |
| CPU Present p99 (us) | 450 | 172 | -278 | 0.382 |
| CPU Present max (us) | 5661 | 12913 | 7252 | 2.281 |
| GPU pending frames max | 1 | 1 | 0 | 1.000 |
| GPU WGC drain/copy p95 (us) | unavailable | 251 | unavailable | unavailable |
| GPU background snapshot p95 (us) | unavailable | 256 | unavailable | unavailable |
| GPU FX materials p95 (us) | 64 | 45 | -19 | 0.703 |
| GPU Bloom/final p50 (us) | 2003 | 2659 | 656 | 1.328 |
| GPU Bloom/final p95 (us) | 2392 | 2883 | 491 | 1.205 |
| GPU Bloom/final p99 (us) | 2684 | 7650 | 4966 | 2.850 |
| GPU Bloom/final max (us) | 23282 | 18991 | -4291 | 0.816 |
| GPU FX total p95 (us) | 2453 | 2927 | 474 | 1.193 |
| GPU command span p95 (us) | 2453 | 3428 | 975 | 1.397 |
| GPU command span max (us) | 23346 | 19476 | -3870 | 0.834 |

## Interpretation

- Primary incremental cost: `gpu-command-path`; largest listed incremental GPU stage is `bloom-and-final-composite` at 491 us p95.
- GPU command span p95 changed by +975 us (+39.747%).
- WGC drain/copy p95: `introduced`, 251 us; percent change unavailable.
- Background snapshot p95: `introduced`, 256 us; percent change unavailable.
- Bloom/final p95 changed by +491 us (+20.527%); status `increased`.
- Present p95 changed by -253 us (-73.121%); max changed by +7252 us. Tail regression observed: `true`.
- Presented FPS changed by -0.811 (-0.487%).
- Input backlog: `not-measured`; Raw Input registration was disabled for this paired render baseline.
- Scope: Largest positive p95 change among WGC drain/copy, background snapshot, and Bloom/final. Percentiles are not additive and do not prove per-frame causality.
- Both modes kept `GPU.PendingFrames.Max <= 1` while message wakes were observed.

## Limitations

- The deterministic pressure uses harmless thread messages, not synthetic Raw Input.
- Input-to-Present-return is not DWM, scanout, panel, or photon latency.
- This paired run is a local SDR primary-monitor baseline, not hardware-matrix acceptance.
