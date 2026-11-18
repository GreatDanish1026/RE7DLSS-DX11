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

## Shaders and SceneInfo (2026-11-16, `docs/shaders`, capture 3)

RDEF names, TAA psbeedaaab: t0 HDRImage, t1 PrevHDRImage, t2 Velocity; cb0 SceneInfo, cb1 Tonemap
(preTonemapRange, AABlend). Same maths as RE2's PreTonemap2_PS: colour x Exposure (VS
vs45c9c59d: WhitePtSrv t0 x Tonemap.exposureAdjustment), unjittered by projMat[2].xy x
screenInverseSize x 0.5, history at uv + velocity x (0.5, -0.5). ps180ea9ae = tonemap + lens
distortion, vignette (Kerare), film grain parameters, colour correction LUTs, image plane;
ps0aad5957 = noise / film grain (EnvironmentInfo.timeMillisecond), not sharpening; ps09af4c8f =
stretch (ReadonlyDepth t0, SrcImage t1, screenInverseSize); ps1828aeac = bloom upsample, also used
for the composite into the TAA output.

SceneInfo, 608 bytes (RE2/RE3: 512):

| Byte | Field |
|---|---|
| 0 | viewProjMat (row-major) |
| 64 / 128 | viewMat / viewInvMat |
| 192 | projMat: [0] x scale, [5] y scale, **row 2 xy = jitter** (RE2's projElement[0].zw), [11] = -1 |
| 256 | projInvMat (row 3 xy = jitter / scale) |
| 320 | viewProjInvMat |
| 384 | prevViewProjMat |
| 448 / 460 | prevCameraPos / cameraNearPlane (0.05) |
| 464 / 476 | prevCameraDir / cameraFarPlane |
| 480 | viewFrustum[6] |
| 576 | ZToLinear, subdivisionLevel |
| 592 / 600 | screenSize / screenInverseSize |

Capture 3 values: jitter 0, -0.000154 (-0.17 px at 2160), projInv row 3 y -7.28e-5 = -0.000154 / 2.119.

The overlay's Performance setting wrote 0.5 for the script and the script reported setting it, but
capture 4 was still at 3840x2160: `set_ImageQualityRate` through call_native_func does nothing in
this build either.

## 0.1.0 (2026-11-16, untested)

RE3DLSS-DX11's upscaler pointed at RE7: TAA beedaaab (colour t0, velocity t2, no mask), bloom
composite 1828aeac (only its draw into the TAA output), tonemap 180ea9ae, film grain 0aad5957 (the
"sharpening" step, overlay label changed), stretch 09af4c8f; SceneInfo offsets above (608-byte
buffers tracked and patched). Depth: the last depth target bound before the TAA (main.cpp tracks it
per context), checked against the colour size, with a shader view made by the add-on for FSR's copy.
The longer jitter patch uses RE7's matrices (viewProj, projMat row 2, projInv row 3,
viewProjInv row 3). The Lua script writes a probe of RenderConfig to RE7DLSS-DX11_probe.txt and
also tries the setter through the method object.

## 0.1.0 test (2026-11-16)

DLAA ran fine (user). DLSS started (NGX D3D11, preset K at DLAA, M at 50%); scene depth found
(3840x2160 format 39, r32_typeless); motion fix and the 32-position jitter ran. The overlay's
Performance did nothing; with the game's slider at 0.5, **the image showed in the top-left quarter
with edge streaks over the rest** (screenshot). Cause: the tonemap samples t0 at SV_Position x
SceneInfo.screenInverseSize, and the add-on's output-size SceneInfo copy (608 bytes) was bound as a
range: D3D11 (PSSetConstantBuffers1) only binds ranges in multiples of 256 bytes, so the bind
failed and the tonemap used the game's render-size SceneInfo. RE2/RE3's 512 bytes were fine. 0.1.1:
the copy is 768 bytes, bound whole.

Lua probe: via.render.RenderConfig exists in the TDB (get/set_ImageQualityRate, System.Single) but
`sdk.get_native_singleton("via.render.RenderConfig")` is **null** in RE7's build, so every call
went to nothing. 0.1.1's probe looks for another way to the object (managed singleton, Renderer,
app option types).

## 0.1.1 test (2026-11-16): upscaling works

Game slider at 0.5: DLSS 1920x1080 -> 3840x2160 (Performance, preset M), tonemap and grain at 4K,
RCAS ran; "everything looked good" (user). The first 256 frames after the load counted 1 jitter
position (0.1.0 counted 32 at the same size); 0.1.2 measures again when it counts 1.

Probe: via.render.Renderer is a native singleton and has get/set_ImageQualityRate (and
get_RenderConfig); no sdk.get_tdb in this REFramework build. 0.1.2's script uses the Renderer.

## 0.1.2 test (2026-11-16): render resolution dropdown and FSR work

"Everything seemed to go well" (user). The script set the scale through via.render.Renderer at
every step (applied within a frame or two): DLAA 3840x2160 (K), Quality 2561x1440 (K, 18 jitter
positions), Balanced 2227x1252 (K, 24), Performance 1920x1080 (M, 32), each jitter count matching
what DLSS suggests. FSR 3.1.6 through the D3D12 bridge at 50%, 58%, 67% and native, then back to
DLSS DLAA, no errors.

## 1.0.0 (2026-11-16)

0.1.2 as tested. README rewritten for RE7 (film grain in place of the game's sharpening, no effect
mask, NVIDIA app override note, RenoDX and FSR 4 on Radeon untested); package
RE7DLSS-DX11-1.0.0.zip (78,493,611 bytes, 14 files, REFramework TDB49 dinput8.dll).

## Effect mask (1.1.0, 2026-11-18)

RE7's TAA has no effect mask; RE7DLSS (DX12) 1.3.3 makes one (effmask.hlsl). In this DX11 build the
order differs from DX12 (captures 1-3): lighting ps56cef943 -> compute csaf18c6e2 -> volumetric fog
(ps05177c3c one-vertex draw at half size, psda2d37dc, blended into the scene colour by psffe10a4a) ->
psc512f974 small copy -> effect draws into the scene colour -> TAA psbeedaaab. So the fog is already
in before the effects: the add-on copies the scene colour at the first GPU work after psffe10a4a (same
context) and makes the mask at the TAA (before = the copy, after = TAA t0). No end pass needed.
Advanced: "Effect mask" (ini EffectMask, on) and "Show the effect mask (testing)" (effview.hlsl).
Tested (user, 2026-11-18): the mask view shows the effects red and the fog black; log: copy after
psffe10a4a and the mask made at 80% and 67% (no copy only on the menu, before any fog).
