# RE7DLSS-DX11 findings

Resident Evil 7 biohazard, Steam beta branch `dx11_non-rt` (target build 11046100), DirectX 11.

Goal: DLSS (and FSR) in place of the game's TAA, as RE2DLSS-DX11 and RE3DLSS-DX11 do. Started
2026-11-16 as a fork of RE3DLSS-DX11 1.0.0 (which has the motion vector and jitter fixes). RE7's
DX11 build is an older RE Engine (TDB 49) than RE2/RE3's (TDB 66/67), so its shader ids, registers
and SceneInfo layout have to be found again before the upscaler is pointed at it.

REFramework for this build: nightly 01366, asset `RE7_TDB49.zip` (downloaded 2026-11-16 into third_party\reframework, installed in the game folder with the 0.0.1 probe).

## 0.0.1 probe

RE3DLSS-DX11's code with RE2/RE3's shader ids. If RE7 uses other ids the upscaler never starts and
the build is the frame capture tool only. The SceneInfo jitter patch now waits until the known TAA
shader has run, so it can't touch a constant buffer with another layout.

## Captures 1-2 (2026-11-16: 3840x2160, TAA, HDR off; 1 at Image Quality 100%, 2 at 50%)

`docs/captures`. REFramework TDB49 starts on DX11 under ReShade (its log is still named
re2_framework_log.txt) and runs RE7DLSS-DX11.lua, but `get_ImageQualityRate` returns 0 through
`sdk.call_native_func` in this build: the script now never takes 0 as the game's value (whether the
setter works is still to be seen). The back buffer is rgba16f even with HDR off. Everything runs on
the immediate context. None of RE2/RE3's ids appear: the post chain has the same shape with new
shaders and different inputs.

| Pass | Shader | Reads | Writes |
|---|---|---|---|
| G-buffer (MRT) | (draws) | | #0f476 r11g11b10f colour, rgba8srgb, rgb10a2, **#0f1e2 rgba16snorm velocity**, depth **#0f4ce r32_typeless (d32f)** |
| **TAA** | **psbeedaaab** (vs45c9c59d) | **t0 colour, t1 history, t2 velocity**; b0 SceneInfo **608 bytes**, b1 256 bytes | render size r11g11b10f |
| history copy | copy | TAA output | history |
| bloom | ps41d730b1, ps7db720c9, psfe287427, ps1828aeac (up) | | |
| bloom composite | ps1828aeac at render size | t0 bloom | into the TAA output |
| **tonemap** | **ps180ea9ae** (vs45c9c59d; LUTs t1-t3, t4/t5 1209x680) | t0 TAA output; b0 SceneInfo, b1 as TAA | render size |
| **sharpen?** | **ps0aad5957** | t0 tonemapped; b0 SceneInfo | back into the TAA output |
| **stretch** (50% only) | **ps09af4c8f** (vs612ddc6c) | t0 depth #0f4ce, t1 colour; b0 SceneInfo | output size colour + d32f |
| output | ps5d280918 | t0 | back buffer rgba16f |

At 100% there is no stretch and the output pass reads the sharpened image directly. Unlike RE2/RE3,
the TAA reads no depth and no effect mask: DLSS's depth has to come from the scene depth #0f4ce (the
depth target of the draws just before the TAA, and the stretch's t0). No HUD pass in these frames.
Next: shader bytecode and SceneInfo contents (ReShade.ini DumpShaders) to find the jitter and the
velocity encoding.
