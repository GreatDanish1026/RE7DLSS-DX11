# RE7DLSS-DX11 findings

Resident Evil 7 biohazard, Steam beta branch `dx11_non-rt` (target build 11046100), DirectX 11.

Goal: DLSS (and FSR) in place of the game's TAA, as RE2DLSS-DX11 and RE3DLSS-DX11 do. Started
2026-11-16 as a fork of RE3DLSS-DX11 1.0.0 (which has the motion vector and jitter fixes). RE7's
DX11 build is an older RE Engine (TDB 49) than RE2/RE3's (TDB 66/67), so its shader ids, registers
and SceneInfo layout have to be found again before the upscaler is pointed at it.

REFramework for this build: nightly 01366, asset `RE7_TDB49.zip` (not downloaded yet).

## 0.0.1 probe

RE3DLSS-DX11's code with RE2/RE3's shader ids. If RE7 uses other ids the upscaler never starts and
the build is the frame capture tool only. The SceneInfo jitter patch now waits until the known TAA
shader has run, so it can't touch a constant buffer with another layout.
