# RE7DLSS-DX11 1.0.0

ReShade add-on for the **DirectX 11 version of Resident Evil 7** (Resident Evil 7 biohazard, Steam beta
branch `dx11_non-rt`, without ray tracing) that replaces the game's TAA with **NVIDIA DLSS** or
**AMD FSR 3.1 / FSR 4**. For the current DirectX 12 version of the game, use RE7DLSS instead.

- **Native anti-aliasing**: at 100% render resolution, DLSS (DLAA) or FSR anti-alias at native
  resolution.
- **Upscaling**: below 100%, DLSS or FSR upscale to your screen resolution, and the game's bloom,
  tonemapping, lens effects and film grain run at full resolution.
- **Render resolution dropdown**: Native, Quality (67%), Balanced (58%), Performance (50%), or the
  game's own Image Quality slider. The bundled REFramework sets it.
- **Upscaler**: Auto (DLSS on NVIDIA RTX cards, FSR on others), DLSS, or FSR. AMD's FSR runtime is
  Direct3D 12 only, so FSR runs on a Direct3D 12 device next to the game's, with the images shared
  between the two; on Radeon cards that support it, AMD's runtime picks FSR 4.
- **DLSS model**: Automatic (K at Native, Quality and Balanced, M at Performance), or K, J, L or M.
  Changing the render resolution in the overlay sets it back to Automatic.
- **Clean upscaling at low resolutions**: the RE Engine's motion vectors include each frame's camera
  jitter; the add-on takes it out before DLSS and FSR get them (in Resident Evil 3's DirectX 11
  version, the jitter left soft striped patches on still faces at Performance). It also gives the
  game a longer jitter sequence while upscaling (32 positions at Performance instead of the game's
  16), as DLSS asks for.
- **Sharpening** slider (off by default): AMD's contrast-adaptive sharpening (RCAS) after DLSS, or
  FSR's own for FSR.
- **Sharper textures** (on by default): when upscaling, textures are sampled as at your screen
  resolution (mip bias, as NVIDIA recommends), following the render resolution as it changes.
- **F9** switches upscaling on and off, to compare with the game's TAA.

## Requirements

- Resident Evil 7 on the Steam beta branch **dx11_non-rt** (Properties > Betas), with DirectX 11
  selected and **TAA** anti-aliasing (the plain TAA option).
- DLSS: an NVIDIA RTX graphics card. FSR: any graphics card with Direct3D 12 support, Windows 10 or
  later.
- ReShade 6.8 or later **with add-on support**.

## Install

**With Fluffy Mod Manager**: put `RE7DLSS-DX11-1.0.0.zip` in Fluffy Mod Manager's `Games\RE7\Mods`
folder (or drag it onto the manager) and enable it. **By hand**: copy the contents of the zip's
`RE7DLSS-DX11` folder into the game folder (where `re7.exe` is).

It installs:

- `RE7DLSS-DX11.addon64` (the add-on), `nvngx_dlss.dll` (NVIDIA's DLSS 310.9.16) and
  `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` (AMD's FSR SDK 2.4.0 runtime).
- `dinput8.dll`: **REFramework**, the build for this version of the game (TDB49, from REFramework
  nightly 01366, which starts under ReShade on DirectX 11). It replaces any REFramework already in
  the folder; newer REFramework builds don't support this game version.
- `reframework\autorun\RE7DLSS-DX11.lua`: sets the render resolution chosen in the overlay.

## Settings

ReShade overlay (Home key), Add-ons tab, **RE7DLSS-DX11**; saved in `ReShade.ini` under
`[RE7DLSS-DX11]`.

- **Settings**: upscaling on/off (F9), upscaler, render resolution, DLSS model, sharpening.
- **Status**: what's running (upscaler, render size, native or upscaling), or why it isn't.
- **Advanced**: upscaling below 100% (off: the upscaler anti-aliases at the render resolution and
  the game stretches the result), the game's film grain at full resolution, the longer jitter
  sequence, sharper textures, the game's exposure for DLSS (off by default: in Resident Evil 2 it
  made characters' dithered fade look see-through), and testing switches and a frame capture tool
  for development.

## Notes

- Not supported: frame generation, and Ray Reconstruction (this version of the game has no ray
  tracing).
- If the NVIDIA app's DLSS override (model preset) is set, the driver uses that model whatever the
  DLSS model setting here says.
- Not tested yet: RenoDX HDR, and FSR 4 on Radeon cards.
- `RE7DLSS-DX11.log` next to `re7.exe` records what the add-on found and did.

## Licence

MIT (LICENSE). Third-party licences in `RE7DLSS-DX11-licenses` and THIRD_PARTY_NOTICES.txt.
