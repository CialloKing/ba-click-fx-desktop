# ADR-001: Final Composition 与层顺序

- Status: Proposed
- Decision owner: Rendering

## Context

单个预乘透明 surface 可以表达 coverage 与 `RGB > Alpha` 的加法能量，但不能天然表达任意层顺序。
`N_premul + E` 只等价于 emission 在 coverage 之后；若 emission 应被 coverage 遮挡，则必须使用
`N_premul + (1 - A)E`。Bloom 又是全场景后处理，不能仅凭材质名推断顺序。

## Proposed decision

第一版使用以下规范顺序：

1. 求值 Coverage；
2. 在其上叠加 DirectEmission；
3. 在最终材质结果上叠加 Bloom；
4. 输出 `rgb = N_premul + E_direct + E_bloom, a = A`。

最终 pass 直接写 `R16G16B16A16_FLOAT`，不得再执行 unpremultiply、alpha canonicalization 或
`rgb <= alpha` clamp。`ExtendedPremultiplied` 是项目术语，底层仍使用 DXGI premultiplied alpha。

## Consequences

- 可以用单个 DirectComposition visual 输出 coverage 与加法光。
- 若 Unity Golden 证明存在被 coverage 遮挡的 emission，必须拆分通道或引入独立的
  `OccludedEmission`，不能偷偷改公式。
- HighVisibility 的压暗/对比度层必须是显式策略，不能混入 PreserveDesktop surface。

## Acceptance

- Spike A 在 SDR/HDR 下验证 `A=0, RGB>0` 和 `RGB>A` 的桌面合成数值。
- Unity 关键时间点 Golden 确认 proposed layer order；Coverage、DirectEmission、BloomResult 和
  FinalOverlay 均保留中间证据。
- 对三种解析公式建立 CPU/GPU 单元测试，禁止后处理把 RGB 截到 alpha。

