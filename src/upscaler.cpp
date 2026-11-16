// DLSS in place of Resident Evil 7's TAA, DirectX 11 build (docs/FINDINGS.md). The design is
// RE7DLSS's (the DirectX 12 build has the same post chain):
//
// (RE2/RE3 notes below; RE7's ids, registers and SceneInfo offsets are in the constants further down.)
// The TAA pass (PreTonemap2_PS, psddfad309) is a full-screen draw reading the effect mask (t0),
// depth (t1, reverse-Z), the scene colour (t2), last frame's TAA output (t3) and the velocity (t4:
// NDC offset to the previous position, y up), with the camera constants (SceneInfo) in b0. DLSS
// takes the same inputs: motion vectors scaled by (0.5 w, -0.5 h), jitter SceneInfo.projElement[0].zw
// (clip space) times half the render size.
//
// DLAA (render size = output size, Image Quality 100% or more): the TAA draw is skipped and DLSS's
// output is drawn into the TAA target. Everything after it runs as the game does.
//
// Upscaling (render size below the output size): everything after TAA runs at render size, then a
// full-screen pass (psaff99b25) stretches the image (and the depth) to the output size. So:
//   1. TAA: DLSS writes an output-size image X; the game's TAA still runs (bloom input, history).
//   2. Bloom composite (ps629a5b35, additive): the game's draw goes into X.
//   3. Tonemap (ps81003188, reads t0 by pixel position and SceneInfo's screen size): the game's
//      draw goes into Y at output size, with t0 = X and a SceneInfo copy with the output size.
//   4. The game's sharpening (psff2662f2): the same, from Y into Z (optional).
//   5. The stretch: the game's draw runs (it writes the output-size depth), then Y or Z is drawn
//      over its colour.
// Steps 2-4 change the game's bindings and let the game's own draw run (so RenoDX's per-draw work
// still applies, as in RE7DLSS 0.1.1); the game's bindings are put back before its next GPU call.
//
// D3D11 keeps one pipeline state per device context, so DLSS (and the add-on's own copies) run in
// a separate, clean state (ID3DDeviceContextState) that is swapped in and out.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_1.h>

#include "common.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "blit_ps.h"  // kBlitPs, kBlitVs: blit.hlsl compiled by build.ps1
#include "blit_vs.h"
#include "rcas_cs.h"  // kRcasCs: rcas.hlsl
#include "exposure_cs.h"  // kExposureCs: exposure.hlsl
#include "down_ps.h"      // kDownPs, kDownVs: downsample.hlsl
#include "down_vs.h"
#include "mvfix_cs.h"     // kMvFixCs: mvfix.hlsl
#include "bridge.hpp"
#include "dlss.hpp"
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

// Resident Evil 7 DirectX 11 shaders (docs/FINDINGS.md, captures 2026-11-16): an older RE Engine
// than RE2/RE3's DX11 builds, with the same post chain but other shaders. The TAA reads no depth
// and no effect mask: the depth comes from the last depth target the scene drew with.
constexpr uint32_t kTaaPs = 0xbeedaaab;      // TAA (HDRImage t0, PrevHDRImage t1, Velocity t2)
constexpr uint32_t kTaaColor = 0, kTaaMotion = 2;
constexpr uint32_t kBloomPs = 0x1828aeac;    // bloom upsample; the composite is its draw into the TAA output
constexpr uint32_t kTonemapPs = 0x180ea9ae;  // tonemap, lens effects (t0 by pixel position)
constexpr uint32_t kSharpenPs = 0x0aad5957;  // film grain (t0); RE7 has no sharpening pass
constexpr uint32_t kStretchPs = 0x09af4c8f;  // render size -> output size (t0 depth, t1 colour)
constexpr uint32_t kStretchColor = 1;
constexpr int kToggleHotkey = VK_F9;

// SceneInfo (cb0 of the TAA, tonemap and grain; 608 bytes), as float indices.
constexpr int kViewProj = 0;            // row_major float4x4, byte 0
constexpr int kProjMat = 48;            // byte 192: projMat; [0] x scale, [5] y scale
constexpr int kJitter = 56;             // byte 224: projMat row 2 x, y = jitter (as RE2's projElement[0].zw)
constexpr int kProjInv = 64;            // byte 256: projInvMat (row 3 xy: jitter / scale)
constexpr int kViewProjInv = 80;        // byte 320
constexpr int kPrevViewProj = 96;       // byte 384
constexpr int kNearPlane = 115;         // byte 460: cameraNearPlane
constexpr int kScreenSize = 148;        // byte 592: width, height, 1 / width, 1 / height
constexpr int kSceneInfoFloats = 152;   // 608 bytes
constexpr int kSceneInfoBufferFloats = 192;  // the add-on's copy: 768 bytes (a multiple of 256)

// ---------------------------------------------------------------- settings

struct Settings {
    bool enabled = true;  // F9
    int preset = dlss::kPresetAuto;
    bool upscale = true;       // below 100%: DLSS to the output size (else DLAA at render size)
    bool sharpen = true;       // the game's sharpening at output size when upscaling
    // DLSS gets the game's exposure (exposure.hlsl) instead of its own auto-exposure. Off by
    // default: with it, DLSS turned Leon see-through (user, 2026-11-13; the value was right, ~0.09).
    bool gameExposure = false;
    bool effectMask = true;    // DLSS gets the TAA's effect mask (particles, effects)
    bool textureDetail = true; // material samplers get a mip bias of log2(render scale) when upscaling
    bool longJitter = true;    // when upscaling, the game's 16-position jitter replaced by 8 / scale^2 positions
    float sharpness = 0;       // extra sharpening after DLSS (RCAS), or FSR's own; 0 = off
    int upscaler = 0;          // Tech: 0 Auto (DLSS where NGX runs, else FSR), 1 DLSS, 2 FSR
    bool flipJitterX = false, flipJitterY = false;
    bool dlssDepthCopy = false;  // testing: DLSS gets an r32f copy of the depth (as FSR does), not saved
    bool mvJittered = false;     // DLSS's MVJittered flag (superseded by mvFix; kept off)
    bool mvFix = true;           // the jitter taken out of the game's motion vectors (mvfix.hlsl)
    int mode = 0;  // render resolution through RE7DLSS-DX11.lua (0: the game's Image Quality slider)
} g_set;

struct Mode {
    const char* name;
    float scale;
};
// No Ultra Performance (33%): in RE2's DX12 build its colours come out wrong.
const Mode kModes[] = {{"Game's Image Quality slider", 0},
                       {"Native (100%): DLAA", 1.0f},
                       {"Quality (67%)", 0.667f},
                       {"Balanced (58%)", 0.58f},
                       {"Performance (50%)", 0.5f}};
constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);

struct Preset {
    const char* name;
    int value;
};
const Preset kPresets[] = {{"Automatic (K, M at 50%, L at 33%)", dlss::kPresetAuto},
                           {"K (DLSS 4 transformer)", 11},
                           {"J", 10},
                           {"L (DLSS 4.5)", 12},
                           {"M (DLSS 4.5)", 13},
                           {"NVIDIA's default", 0}};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Upscale", g_set.upscale);
    reshade::get_config_value(nullptr, RD_PRODUCT, "GameSharpening", g_set.sharpen);
    reshade::get_config_value(nullptr, RD_PRODUCT, "GameExposure", g_set.gameExposure);
    reshade::get_config_value(nullptr, RD_PRODUCT, "EffectMask", g_set.effectMask);
    reshade::get_config_value(nullptr, RD_PRODUCT, "TextureDetail", g_set.textureDetail);
    reshade::get_config_value(nullptr, RD_PRODUCT, "LongJitter", g_set.longJitter);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Sharpness", g_set.sharpness);
    if (!(g_set.sharpness >= 0 && g_set.sharpness <= 1)) g_set.sharpness = 0;
    reshade::get_config_value(nullptr, RD_PRODUCT, "Upscaler", g_set.upscaler);
    if (g_set.upscaler < 0 || g_set.upscaler > 2) g_set.upscaler = 0;
    reshade::get_config_value(nullptr, RD_PRODUCT, "Mode", g_set.mode);
    if (g_set.mode < 0 || g_set.mode >= kModeCount) g_set.mode = 0;
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Upscale", g_set.upscale);
    reshade::set_config_value(nullptr, RD_PRODUCT, "GameSharpening", g_set.sharpen);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Mode", g_set.mode);
    reshade::set_config_value(nullptr, RD_PRODUCT, "GameExposure", g_set.gameExposure);
    reshade::set_config_value(nullptr, RD_PRODUCT, "EffectMask", g_set.effectMask);
    reshade::set_config_value(nullptr, RD_PRODUCT, "TextureDetail", g_set.textureDetail);
    reshade::set_config_value(nullptr, RD_PRODUCT, "LongJitter", g_set.longJitter);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Sharpness", g_set.sharpness);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Upscaler", g_set.upscaler);
}

// ---------------------------------------------------------------- status

std::mutex g_statusMutex;
std::string g_lastError, g_mode;
uint64_t g_frame = 0, g_lastRun = 0, g_runs = 0;

void Fail(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (why != g_lastError) Log("not upscaling: %s", why.c_str());
    g_lastError = why;
}

// ---------------------------------------------------------------- our resources

device* g_dev = nullptr;
uint32_t g_outW = 0, g_outH = 0;  // the swap chain's size

struct Texture {
    resource res = {0};
    resource_view srv = {0}, rtv = {0}, uav = {0};
    uint32_t w = 0, h = 0;
    format fmt = format::unknown;
};
Texture g_x;  // DLSS output (rgba16f, UAV; render target for the bloom, shader resource for the tonemap)
Texture g_y;  // tonemapped output-size image
Texture g_z;  // sharpened output-size image
Texture g_depth32;  // testing: render-size r32f copy of the depth for DLSS
Texture g_mv;       // render-size rg16f motion in render pixels, without the jitter (mvfix.hlsl)

void Free(Texture& t) {
    if (!g_dev) return;
    if (t.srv.handle) g_dev->destroy_resource_view(t.srv);
    if (t.rtv.handle) g_dev->destroy_resource_view(t.rtv);
    if (t.uav.handle) g_dev->destroy_resource_view(t.uav);
    if (t.res.handle) g_dev->destroy_resource(t.res);
    t = Texture();
}

bool Ensure(device* dev, Texture& t, uint32_t w, uint32_t h, format fmt, bool uav, const char* name) {
    if (t.res.handle && t.w == w && t.h == h && t.fmt == fmt) return true;
    Free(t);
    resource_usage usage = resource_usage::shader_resource | resource_usage::render_target;
    if (uav) usage |= resource_usage::unordered_access;
    const resource_desc d(w, h, 1, 1, fmt, 1, memory_heap::default_, usage);
    if (!dev->create_resource(d, nullptr, resource_usage::shader_resource, &t.res)) {
        Log("couldn't create the %ux%u %s texture (format %u)", w, h, name, static_cast<unsigned>(fmt));
        t = Texture();
        return false;
    }
    dev->create_resource_view(t.res, resource_usage::shader_resource, resource_view_desc(fmt), &t.srv);
    dev->create_resource_view(t.res, resource_usage::render_target, resource_view_desc(fmt), &t.rtv);
    if (uav) dev->create_resource_view(t.res, resource_usage::unordered_access, resource_view_desc(fmt), &t.uav);
    if (!t.srv.handle || !t.rtv.handle || (uav && !t.uav.handle)) {
        Log("couldn't create views of the %s texture", name);
        Free(t);
        return false;
    }
    t.w = w, t.h = h, t.fmt = fmt;
    Log("created the %ux%u %s texture (format %u)", w, h, name, static_cast<unsigned>(fmt));
    return true;
}

resource g_exposureOne = {0};  // 1x1 r32f = 1.0
resource g_sceneCb = {0};      // SceneInfo copy with the output size

ID3D11Resource* ExposureOne(device* dev) {
    if (!g_set.gameExposure) return nullptr;
    if (!g_exposureOne.handle) {
        float one = 1.0f;
        subresource_data data = {&one, 4, 4};
        if (!dev->create_resource(resource_desc(1, 1, 1, 1, format::r32_float, 1, memory_heap::default_,
                                                resource_usage::shader_resource),
                                  &data, resource_usage::shader_resource, &g_exposureOne)) {
            g_exposureOne = {0};
            return nullptr;
        }
    }
    return reinterpret_cast<ID3D11Resource*>(g_exposureOne.handle);
}

// ---------------------------------------------------------------- the add-on's own draws
//
// Copies (with format conversion) run in a clean pipeline state of their own, so none of the
// game's bindings change.

ID3DDeviceContextState* g_cleanState = nullptr;
ID3D11VertexShader* g_blitVs = nullptr;
ID3D11PixelShader* g_blitPs = nullptr;
bool g_blitFailed = false;

bool BlitReady(ID3D11Device* d3d) {
    if (g_blitVs && g_blitPs && g_cleanState) return true;
    if (g_blitFailed) return false;
    ID3D11Device1* dev1 = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
        const D3D_FEATURE_LEVEL level = d3d->GetFeatureLevel();
        dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_cleanState);
        dev1->Release();
    }
    d3d->CreateVertexShader(kBlitVs, sizeof(kBlitVs), nullptr, &g_blitVs);
    d3d->CreatePixelShader(kBlitPs, sizeof(kBlitPs), nullptr, &g_blitPs);
    if (!g_cleanState || !g_blitVs || !g_blitPs) {
        g_blitFailed = true;
        Log("couldn't create the copy shaders (Direct3D 11.1 needed)");
        return false;
    }
    return true;
}

// src (shader resource view) -> dst (render target view), both w x h.
bool Blit(command_list* cl, resource_view src, resource_view dst, uint32_t w, uint32_t h) {
    auto* d3d = reinterpret_cast<ID3D11Device*>(cl->get_device()->get_native());
    if (!BlitReady(d3d)) return false;
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) return false;
    ID3DDeviceContextState* game = nullptr;
    ctx1->SwapDeviceContextState(g_cleanState, &game);
    ctx1->ClearState();
    auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(dst.handle);
    auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(src.handle);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(g_blitVs, nullptr, 0);
    ctx1->PSSetShader(g_blitPs, nullptr, 0);
    ctx1->PSSetShaderResources(0, 1, &srv);
    ctx1->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx1->RSSetViewports(1, &vp);
    ctx1->Draw(3, 0);
    ctx1->ClearState();  // nothing of ours stays bound (src may be written next)
    ctx1->SwapDeviceContextState(game, nullptr);
    if (game) game->Release();
    ctx1->Release();
    return true;
}

// The output-size image scaled down into a render-size target (downsample.hlsl).
ID3D11VertexShader* g_downVs = nullptr;
ID3D11PixelShader* g_downPs = nullptr;
ID3D11SamplerState* g_linear = nullptr;
ID3D11Buffer* g_downCb = nullptr;

bool Downsample(command_list* cl, resource_view src, resource_view dst, uint32_t w, uint32_t h) {
    auto* d3d = reinterpret_cast<ID3D11Device*>(cl->get_device()->get_native());
    if (!BlitReady(d3d)) return false;
    if (!g_downPs) {
        d3d->CreateVertexShader(kDownVs, sizeof(kDownVs), nullptr, &g_downVs);
        d3d->CreatePixelShader(kDownPs, sizeof(kDownPs), nullptr, &g_downPs);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        d3d->CreateSamplerState(&sd, &g_linear);
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        d3d->CreateBuffer(&bd, nullptr, &g_downCb);
    }
    if (!g_downVs || !g_downPs || !g_linear || !g_downCb) return false;
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) return false;
    ID3DDeviceContextState* game = nullptr;
    ctx1->SwapDeviceContextState(g_cleanState, &game);
    ctx1->ClearState();
    const float params[4] = {1.0f / w, 1.0f / h, 0, 0};
    ctx1->UpdateSubresource(g_downCb, 0, nullptr, params, 0, 0);
    auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(dst.handle);
    auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(src.handle);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(g_downVs, nullptr, 0);
    ctx1->PSSetShader(g_downPs, nullptr, 0);
    ctx1->PSSetShaderResources(0, 1, &srv);
    ctx1->PSSetSamplers(0, 1, &g_linear);
    ctx1->PSSetConstantBuffers(0, 1, &g_downCb);
    ctx1->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx1->RSSetViewports(1, &vp);
    ctx1->Draw(3, 0);
    ctx1->ClearState();
    ctx1->SwapDeviceContextState(game, nullptr);
    if (game) game->Release();
    ctx1->Release();
    return true;
}

// ---------------------------------------------------------------- sharpening (RCAS)
//
// With the Sharpening slider above 0, DLSS writes into Xs and AMD's RCAS (rcas.hlsl) writes the
// sharpened image into X, where DLSS's output normally goes. FSR uses its own RCAS.

Texture g_xs;  // DLSS's output before sharpening
ID3D11ComputeShader* g_rcasCs = nullptr;
ID3D11Buffer* g_rcasCb = nullptr;
bool g_rcasFailed = false;

bool Sharpen(command_list* cl, Texture& src, Texture& dst, float sharpness) {
    auto* d3d = reinterpret_cast<ID3D11Device*>(cl->get_device()->get_native());
    if (!BlitReady(d3d)) return false;
    if (!g_rcasCs && !g_rcasFailed) {
        d3d->CreateComputeShader(kRcasCs, sizeof(kRcasCs), nullptr, &g_rcasCs);
        D3D11_BUFFER_DESC bd = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        d3d->CreateBuffer(&bd, nullptr, &g_rcasCb);
        g_rcasFailed = !g_rcasCs || !g_rcasCb;
        Log(g_rcasFailed ? "couldn't create the sharpening shader" : "sharpening shader (RCAS) created");
    }
    if (g_rcasFailed || !src.srv.handle || !dst.uav.handle) return false;
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) return false;
    ID3DDeviceContextState* game = nullptr;
    ctx1->SwapDeviceContextState(g_cleanState, &game);
    ctx1->ClearState();
    uint32_t params[8] = {};
    memcpy(&params[0], &sharpness, 4);
    params[4] = dst.w, params[5] = dst.h;
    ctx1->UpdateSubresource(g_rcasCb, 0, nullptr, params, 0, 0);
    auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(src.srv.handle);
    auto* uav = reinterpret_cast<ID3D11UnorderedAccessView*>(dst.uav.handle);
    ctx1->CSSetShader(g_rcasCs, nullptr, 0);
    ctx1->CSSetConstantBuffers(0, 1, &g_rcasCb);
    ctx1->CSSetShaderResources(0, 1, &srv);
    ctx1->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx1->Dispatch((dst.w + 7) / 8, (dst.h + 7) / 8, 1);
    ctx1->ClearState();
    ctx1->SwapDeviceContextState(game, nullptr);
    if (game) game->Release();
    ctx1->Release();
    static bool logged = false;
    if (!logged) logged = true, Log("upscaled image sharpened (RCAS, strength %.2f)", sharpness);
    return true;
}

// ---------------------------------------------------------------- motion vectors without the jitter
//
// The game's velocity includes this frame's jitter (its velocity pass rebuilds positions with the
// jittered viewProjInv and projects them with the unjittered prevViewProj). With it, DLSS shifted
// its history by the jitter every frame and left soft striped patches on still faces at Performance
// (2026-11-15; DLSS's MVJittered flag only partly helped). mvfix.hlsl writes motion in render pixels
// with the jitter taken out: velocity * (0.5 w, -0.5 h) + jitter in pixels.

ID3D11ComputeShader* g_mvCs = nullptr;
ID3D11Buffer* g_mvCb = nullptr;

bool FixMotion(command_list* cl, resource_view velocity, uint32_t w, uint32_t h, float jitterX, float jitterY) {
    device* dev = cl->get_device();
    auto* d3d = reinterpret_cast<ID3D11Device*>(dev->get_native());
    // rg16f (as games usually give DLSS): rg32f can't be shared with Direct3D 12 for FSR (0.1.2).
    if (!BlitReady(d3d) || !Ensure(dev, g_mv, w, h, format::r16g16_float, true, "motion vector")) return false;
    if (!g_mvCs) {
        d3d->CreateComputeShader(kMvFixCs, sizeof(kMvFixCs), nullptr, &g_mvCs);
        D3D11_BUFFER_DESC bd = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        d3d->CreateBuffer(&bd, nullptr, &g_mvCb);
        if (!g_mvCs || !g_mvCb) return false;
        Log("motion vectors: the jitter is taken out (mvfix.hlsl)");
    }
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) return false;
    ID3DDeviceContextState* game = nullptr;
    ctx1->SwapDeviceContextState(g_cleanState, &game);
    ctx1->ClearState();
    struct {
        float scale[2], offset[2];
        uint32_t size[2], pad[2];
    } p = {{0.5f * w, -0.5f * h}, {jitterX, jitterY}, {w, h}, {0, 0}};
    ctx1->UpdateSubresource(g_mvCb, 0, nullptr, &p, 0, 0);
    auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(velocity.handle);
    auto* uav = reinterpret_cast<ID3D11UnorderedAccessView*>(g_mv.uav.handle);
    ctx1->CSSetShader(g_mvCs, nullptr, 0);
    ctx1->CSSetConstantBuffers(0, 1, &g_mvCb);
    ctx1->CSSetShaderResources(0, 1, &srv);
    ctx1->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx1->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ctx1->ClearState();
    ctx1->SwapDeviceContextState(game, nullptr);
    if (game) game->Release();
    ctx1->Release();
    return true;
}

// ---------------------------------------------------------------- the game's exposure
//
// The TAA multiplies the scene colour by Exposure = WhitePtSrv[0] (the game's auto-exposure,
// when on) x exposureAdjustment before tonemapping (0.05 or so in the captures). DLSS needs that
// value: told 1.0, it treats the colour as ~20x brighter and dims small bright lights, the more so
// the lower the render resolution (0.2.0). exposure.hlsl computes it on the GPU into a 1x1 texture.

ID3D11Texture2D* g_exposureTex = nullptr;
ID3D11UnorderedAccessView* g_exposureUav = nullptr;
ID3D11ComputeShader* g_exposureCs = nullptr;
bool g_exposureFailed = false;

ID3D11Resource* GameExposure(command_list* cl, const GameState& g) {
    if (!g_set.gameExposure || g_exposureFailed) return nullptr;
    auto* d3d = reinterpret_cast<ID3D11Device*>(cl->get_device()->get_native());
    if (!BlitReady(d3d)) return nullptr;
    if (!g_exposureCs) {
        D3D11_TEXTURE2D_DESC d = {1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
        d3d->CreateTexture2D(&d, nullptr, &g_exposureTex);
        if (g_exposureTex) d3d->CreateUnorderedAccessView(g_exposureTex, nullptr, &g_exposureUav);
        d3d->CreateComputeShader(kExposureCs, sizeof(kExposureCs), nullptr, &g_exposureCs);
        if (!g_exposureTex || !g_exposureUav || !g_exposureCs) {
            g_exposureFailed = true;
            Log("couldn't create the exposure shader: DLSS uses its own auto-exposure");
            return nullptr;
        }
    }
    const buffer_range& cb = g.psCb[1];  // the TAA's Tonemap constants
    if (!cb.buffer.handle || !g.vsSrv0.handle) {
        static bool logged = false;
        if (!logged) logged = true, Log("the game's exposure inputs weren't found: DLSS uses its own auto-exposure");
        return nullptr;
    }
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) return nullptr;
    ID3DDeviceContextState* game = nullptr;
    ctx1->SwapDeviceContextState(g_cleanState, &game);
    ctx1->ClearState();
    auto* buf = reinterpret_cast<ID3D11Buffer*>(cb.buffer.handle);
    const UINT first = static_cast<UINT>(cb.offset / 16), num = 16;
    if (cb.offset) ctx1->CSSetConstantBuffers1(0, 1, &buf, &first, &num);
    else ctx1->CSSetConstantBuffers(0, 1, &buf);
    auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(g.vsSrv0.handle);
    ctx1->CSSetShaderResources(0, 1, &srv);
    ctx1->CSSetUnorderedAccessViews(0, 1, &g_exposureUav, nullptr);
    ctx1->CSSetShader(g_exposureCs, nullptr, 0);
    ctx1->Dispatch(1, 1, 1);
    ctx1->ClearState();
    ctx1->SwapDeviceContextState(game, nullptr);
    if (game) game->Release();
    ctx1->Release();
    static bool logged = false;
    if (!logged) logged = true, Log("DLSS exposure: the game's (auto-exposure x exposureAdjustment, computed on the GPU)");

    // Every ~10 s: the value, read back a few frames later without stalling (for the log).
    static ID3D11Texture2D* staging = nullptr;
    static uint64_t copiedAt = 0;
    static int reads = 0;
    if (!staging) {
        D3D11_TEXTURE2D_DESC d = {1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        d3d->CreateTexture2D(&d, nullptr, &staging);
    }
    if (staging && reads < 30) {
        if (!copiedAt && g_frame % 600 == 0) {
            ctx->CopyResource(staging, g_exposureTex);
            copiedAt = g_frame;
        } else if (copiedAt && g_frame > copiedAt + 3) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
                const float e = *static_cast<const float*>(m.pData);
                ctx->Unmap(staging, 0);
                float tm[8] = {};
                const bool tmOk = ReadCb(cb.buffer, cb.offset, tm, sizeof(tm));
                Log("game exposure now %.5f (exposureAdjustment %.5f, useAutoExposure %d)", e, tmOk ? tm[0] : -1.0f,
                    tmOk ? *reinterpret_cast<int*>(&tm[4]) : -1);
                copiedAt = 0;
                ++reads;
            }
        }
    }
    return g_exposureTex;
}

// ---------------------------------------------------------------- texture detail (mip bias)
//
// At a lower render size the GPU picks smaller mip levels, so textures look blurrier than at
// native. DLSS's guidance: a mip bias of log2(render width / output width). D3D11 sampler objects
// can't be changed, so each material sampler (anisotropic or trilinear, wrapping, with mips) the
// game binds is followed by a copy of it with the bias, made once per bias. The bias follows the
// render scale as it changes.

std::mutex g_samplerMutex;
std::map<uint64_t, sampler_desc> g_materialSamplers;  // the game's, by handle
std::map<uint64_t, sampler> g_biased;                 // the game's -> ours at g_biasMade
float g_biasMade = 0;
std::atomic<float> g_mipBias{0};

bool IsMaterialSampler(const sampler_desc& d) {
    return (d.filter == filter_mode::anisotropic || d.filter == filter_mode::min_mag_mip_linear) &&
           d.address_u == texture_address_mode::wrap && d.max_lod > d.min_lod;
}

// A new sampler can reuse the address of one the game released: whatever was recorded for that
// handle (and our biased copy of it) belongs to the old one and goes. (0.2.0 kept the old copy:
// the game then sampled with the wrong sampler, and Leon's dithered fade turned see-through until
// the bias changed.)
void OnInitSampler(device* dev, const sampler_desc& d, sampler s) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    auto b = g_biased.find(s.handle);
    if (b != g_biased.end()) {
        dev->destroy_sampler(b->second);
        g_biased.erase(b);
    }
    if (IsMaterialSampler(d)) g_materialSamplers[s.handle] = d;
    else g_materialSamplers.erase(s.handle);
}

void OnDestroySampler(device* dev, sampler s) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    g_materialSamplers.erase(s.handle);
    auto it = g_biased.find(s.handle);
    if (it != g_biased.end()) {
        dev->destroy_sampler(it->second);
        g_biased.erase(it);
    }
}

// ---------------------------------------------------------------- upscaling state

struct Up {
    bool active = false;  // upscaling this frame: the passes after TAA go to output size
    bool bloomDone = false, tonemapDone = false, sharpenDone = false;
    resource taaOut = {0}, tonemapOut = {0}, sharpenOut = {0};  // the game's render-size targets
    Texture* final = nullptr;
} g_up;
bool g_restorePending = false;  // the game's bindings were changed for its draw: put back next

enum class Tech { Auto = 0, Dlss = 1, Fsr = 2 };
Tech g_active = Tech::Auto;  // the upscaler that ran last
bool g_lastUpscale = false;  // the last run upscaled (below 100%)

std::atomic<float> g_renderScale{1};

bool CameraCut(const float* scene) {
    float diff = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            diff = std::fmax(diff, std::fabs(scene[kViewProj + r * 4 + c] - scene[kPrevViewProj + r * 4 + c]));
    return diff > 1.0f || !std::isfinite(diff);
}

float JitterX(const float* scene, uint32_t w) { return -scene[kJitter] * w * 0.5f * (g_set.flipJitterX ? -1 : 1); }
float JitterY(const float* scene, uint32_t h) { return scene[kJitter + 1] * h * 0.5f * (g_set.flipJitterY ? -1 : 1); }

// The scene depth for the upscalers: the last depth target bound before the TAA, at the render
// size. FSR's copy (and DLSS's optional r32f copy) needs a shader view of it: made once per texture.
resource g_depthRes = {0};
resource_view g_depthSrv = {0};

resource_view DepthSrv(device* dev, resource depth) {
    if (depth == g_depthRes && g_depthSrv.handle) return g_depthSrv;
    if (g_depthSrv.handle) dev->destroy_resource_view(g_depthSrv);
    g_depthSrv = {0};
    g_depthRes = depth;
    const resource_desc d = dev->get_resource_desc(depth);
    const format f = d.texture.format == format::r24_g8_typeless ? format::r24_unorm_x8_uint
                     : d.texture.format == format::r32_g8_typeless ? format::r32_float_x8_uint
                                                                     : format::r32_float;
    if (!dev->create_resource_view(depth, resource_usage::shader_resource, resource_view_desc(f), &g_depthSrv)) {
        g_depthSrv = {0};
        Log("couldn't make a shader view of the depth (format %u)", static_cast<uint32_t>(d.texture.format));
    }
    return g_depthSrv;
}

// Binds a render target (no depth), viewport and scissor of w x h.
void BindTarget(command_list* cl, resource_view rtv, uint32_t w, uint32_t h) {
    cl->bind_render_targets_and_depth_stencil(1, &rtv, resource_view{0});
    const viewport vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    cl->bind_viewports(0, 1, &vp);
    const rect sc = {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
    cl->bind_scissor_rects(0, 1, &sc);
}

void BindPsSrv(command_list* cl, uint32_t slot, resource_view v) {
    descriptor_table_update u = {};
    u.binding = slot;
    u.count = 1;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = &v;
    cl->push_descriptors(shader_stage::pixel, pipeline_layout{0}, 0, u);
}

// SceneInfo with the output size, bound as the pixel shader's b0.
bool BindSceneInfoAtOutputSize(command_list* cl, const GameState& g, uint32_t w, uint32_t h) {
    // RE7's SceneInfo is 608 bytes. D3D11 binds constant buffer ranges only in multiples of 256
    // bytes (PSSetConstantBuffers1), so a 608-byte range didn't bind and the tonemap kept the game's
    // render-size SceneInfo (0.1.0: the image in the top-left quarter). The copy is 768 bytes and
    // bound whole.
    float scene[kSceneInfoBufferFloats] = {};
    if (!ReadCb(g.psCb[0].buffer, g.psCb[0].offset, scene, kSceneInfoFloats * sizeof(float))) return false;
    device* dev = cl->get_device();
    if (!g_sceneCb.handle &&
        !dev->create_resource(resource_desc(sizeof(scene), memory_heap::default_, resource_usage::constant_buffer), nullptr,
                              resource_usage::constant_buffer, &g_sceneCb)) {
        g_sceneCb = {0};
        return false;
    }
    scene[kScreenSize] = static_cast<float>(w);
    scene[kScreenSize + 1] = static_cast<float>(h);
    scene[kScreenSize + 2] = 1.0f / w;
    scene[kScreenSize + 3] = 1.0f / h;
    cl->update_buffer_region(scene, g_sceneCb, 0, sizeof(scene));
    const buffer_range cb = {g_sceneCb, 0, UINT64_MAX};
    descriptor_table_update u = {};
    u.count = 1;
    u.type = descriptor_type::constant_buffer;
    u.descriptors = &cb;
    cl->push_descriptors(shader_stage::pixel, pipeline_layout{0}, 0, u);
    return true;
}

// ---------------------------------------------------------------- passes

std::atomic<bool> g_taaSeen{false};  // the game's TAA shader has run (gates the SceneInfo patch)

bool OnTaa(command_list* cl, const GameState& g) {
    g_taaSeen = true;
    device* dev = cl->get_device();
    const resource depth = ViewResource(g.lastDsv);
    const resource color = ViewResource(g.psSrv[kTaaColor]);
    const resource motion = ViewResource(g.psSrv[kTaaMotion]);
    const resource mask = {0};  // RE7's TAA has no effect mask
    const resource target = ViewResource(g.rtv[0]);
    if (!depth.handle || !color.handle || !motion.handle || !target.handle || !g.rtv[0].handle) {
        Fail("TAA inputs not found (depth, colour, velocity or render target)");
        return false;
    }
    float scene[kSceneInfoFloats];
    if (!g.psCb[0].buffer.handle || !ReadCb(g.psCb[0].buffer, g.psCb[0].offset, scene, sizeof(scene))) {
        Fail("camera constants (SceneInfo) not found");
        return false;
    }
    const resource_desc cd = dev->get_resource_desc(color);
    const resource_desc td = dev->get_resource_desc(target);
    // On the first frame (and the first after a menu) the game binds an 8x8 placeholder as the
    // velocity: the game's TAA handles that frame.
    const resource_desc md = dev->get_resource_desc(motion);
    if (md.texture.width != cd.texture.width || md.texture.height != cd.texture.height) {
        static int logged = 0;
        if (logged++ < 3)
            Log("a frame without motion vectors (velocity %ux%u, colour %ux%u): the game's TAA used", md.texture.width,
                md.texture.height, cd.texture.width, cd.texture.height);
        return false;
    }
    const resource_desc dd = dev->get_resource_desc(depth);
    if (dd.texture.width != cd.texture.width || dd.texture.height != cd.texture.height) {
        static int logged = 0;
        if (logged++ < 3)
            Log("the last depth target (%ux%u) isn't the scene's (%ux%u): the game's TAA used", dd.texture.width,
                dd.texture.height, cd.texture.width, cd.texture.height);
        return false;
    }
    const resource_view depthSrv = DepthSrv(dev, depth);
    {
        static bool logged = false;
        if (!logged) logged = true, Log("scene depth: %ux%u format %u", dd.texture.width, dd.texture.height,
                                        static_cast<uint32_t>(dd.texture.format));
    }
    uint32_t renderW = cd.texture.width, renderH = cd.texture.height;
    const float sw = scene[kScreenSize], sh = scene[kScreenSize + 1];
    if (sw >= 64 && sh >= 64 && sw <= renderW && sh <= renderH) renderW = static_cast<uint32_t>(sw), renderH = static_cast<uint32_t>(sh);
    g_renderScale = g_outW ? static_cast<float>(renderW) / g_outW : 1.0f;

    // Which upscaler: Auto takes DLSS when NGX starts (RTX cards), FSR otherwise.
    auto* d3d = reinterpret_cast<ID3D11Device*>(dev->get_native());
    std::string err;
    Tech tech = static_cast<Tech>(g_set.upscaler);
    if (tech == Tech::Auto) {
        std::string dlssErr;
        tech = dlss::Init(d3d, dlssErr) ? Tech::Dlss : Tech::Fsr;
    }
    if (tech == Tech::Dlss ? !dlss::Init(d3d, err) : !bridge::Init(d3d, err)) {
        Fail(err);
        return false;
    }
    if (tech != g_active) {
        if (g_active == Tech::Dlss) dlss::ReleaseFeatures();
        Log("Upscaler: %s", tech == Tech::Dlss ? "DLSS" : "FSR");
        g_active = tech;
    }

    const bool upscale = g_set.upscale && g_outW && g_outH && renderW < g_outW && renderH < g_outH &&
                         renderW * 3 + 16 >= g_outW && renderH * 3 + 16 >= g_outH;
    const uint32_t outW = upscale ? g_outW : td.texture.width, outH = upscale ? g_outH : td.texture.height;
    if (!Ensure(dev, g_x, outW, outH, format::r16g16b16a16_float, true, "upscaler output")) {
        Fail("no upscaler output texture");
        return false;
    }
    const bool sharpenDlss = tech == Tech::Dlss && g_set.sharpness >= 0.005f &&
                             Ensure(dev, g_xs, outW, outH, format::r16g16b16a16_float, true, "DLSS output before sharpening");
    const float jitterX = JitterX(scene, renderW), jitterY = JitterY(scene, renderH);
    {
        // How many different jitter positions the game cycles through (DLSS wants about 8 / scale^2:
        // 32 at 50%), logged once per render size from 256 frames.
        static std::map<uint64_t, int> seen;
        static uint32_t forW = 0, frames = 0;
        static bool done = false;
        if (forW != renderW) seen.clear(), forW = renderW, frames = 0, done = false;
        if (!done) {
            const int qx = static_cast<int>(std::lround(jitterX * 1000)), qy = static_cast<int>(std::lround(jitterY * 1000));
            ++seen[(static_cast<uint64_t>(static_cast<uint32_t>(qx)) << 32) | static_cast<uint32_t>(qy)];
            if (++frames == 256) {
                // One position (no jitter, e.g. the frames after a load: 0.1.1's first log) is measured
                // again; logged once.
                static bool loggedStill = false;
                const bool still = seen.size() <= 1;
                if (!still || !loggedStill)
                    Log("jitter at %ux%u: %zu different positions in 256 frames (DLSS suggests %.0f at this scale)%s",
                        renderW, renderH, seen.size(), 8.0f / (g_renderScale.load() * g_renderScale.load()),
                        still ? "; measuring again" : "");
                if (still) loggedStill = true, seen.clear(), frames = 0;
                else done = true;
            }
        }
    }
    const bool reset = CameraCut(scene);
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());

    // Motion for the upscaler: the game's velocity with this frame's jitter taken out (mvfix.hlsl),
    // in render pixels; else the velocity as it is, scaled by the upscaler.
    ID3D11Resource* motionIn = reinterpret_cast<ID3D11Resource*>(motion.handle);
    float mvScaleX = 0.5f * renderW, mvScaleY = -0.5f * renderH;  // NDC offsets, y up -> render pixels, y down
    if (g_set.mvFix) {
        const float rawJx = -scene[kJitter] * renderW * 0.5f, rawJy = scene[kJitter + 1] * renderH * 0.5f;
        if (FixMotion(cl, g.psSrv[kTaaMotion], renderW, renderH, rawJx, rawJy)) {
            motionIn = reinterpret_cast<ID3D11Resource*>(g_mv.res.handle);
            mvScaleX = mvScaleY = 1.0f;
        }
    }

    if (tech == Tech::Dlss) {
        dlss::Inputs in;
        in.color = reinterpret_cast<ID3D11Resource*>(color.handle);
        in.depth = reinterpret_cast<ID3D11Resource*>(depth.handle);
        if (g_set.dlssDepthCopy && depthSrv.handle && Ensure(dev, g_depth32, renderW, renderH, format::r32_float, false, "depth copy") &&
            Blit(cl, depthSrv, g_depth32.rtv, renderW, renderH))
            in.depth = reinterpret_cast<ID3D11Resource*>(g_depth32.res.handle);
        in.motion = motionIn;
        in.output = reinterpret_cast<ID3D11Resource*>((sharpenDlss ? g_xs : g_x).res.handle);
        in.renderWidth = renderW, in.renderHeight = renderH;
        in.outputWidth = outW, in.outputHeight = outH;
        in.jitterX = jitterX, in.jitterY = jitterY;
        in.motionScaleX = mvScaleX;
        in.motionScaleY = mvScaleY;
        in.exposure = GameExposure(cl, g);
        in.biasMask = g_set.effectMask && mask.handle ? reinterpret_cast<ID3D11Resource*>(mask.handle) : nullptr;
        in.reset = reset;
        in.preset = g_set.preset;
        in.mvJittered = g_set.mvJittered;
        if (!dlss::Evaluate(ctx, in, err)) {
            Fail(err);
            return false;
        }
        if (sharpenDlss) Sharpen(cl, g_xs, g_x, g_set.sharpness);
    } else {
        // FSR runs on Direct3D 12: the depth is drawn into the shared r32f texture first.
        ID3D11RenderTargetView* depthRtv = bridge::DepthTarget(renderW, renderH, err);
        if (!depthRtv || !depthSrv.handle ||
            !Blit(cl, depthSrv, resource_view{reinterpret_cast<uint64_t>(depthRtv)}, renderW, renderH)) {
            Fail(err.empty() ? "couldn't copy the depth for FSR" : err);
            return false;
        }
        bridge::Inputs in;
        in.color = reinterpret_cast<ID3D11Resource*>(color.handle);
        in.motion = motionIn;
        in.reactive = g_set.effectMask && mask.handle ? reinterpret_cast<ID3D11Resource*>(mask.handle) : nullptr;
        in.output = reinterpret_cast<ID3D11Resource*>(g_x.res.handle);
        in.renderWidth = renderW, in.renderHeight = renderH;
        in.outputWidth = outW, in.outputHeight = outH;
        in.jitterX = jitterX, in.jitterY = jitterY;
        in.motionScaleX = mvScaleX;
        in.motionScaleY = mvScaleY;
        const float nearZ = scene[kNearPlane], yScale = scene[kProjMat + 5];
        in.cameraNear = nearZ > 0 && nearZ < 10 ? nearZ : 0.01f;
        in.cameraFovY = yScale > 0.05f ? 2 * std::atan(1 / yScale) : 1.0f;
        in.sharpness = g_set.sharpness;
        in.reset = reset;
        if (!bridge::Evaluate(ctx, in, err)) {
            Fail(err);
            return false;
        }
    }
    g_lastUpscale = upscale;

    bool skip;
    if (upscale) {
        // The render-size passes after the TAA (the bloom's bright pass and blur) read the TAA
        // target: it gets the upscaled image scaled down, and the game's TAA is skipped. (Leaving
        // the game's TAA running for them, as RE7DLSS does, made the bloom fade toward Performance:
        // the game's TAA keeps less of small bright lights than DLSS.) The passes after use X.
        if (!Downsample(cl, g_x.srv, g.rtv[0], renderW, renderH)) {
            Fail("couldn't scale the upscaled image down for the bloom");
            return false;
        }
        g_up = Up();
        g_up.active = true;
        g_up.taaOut = target;
        skip = true;
    } else {
        // DLAA: DLSS's output goes into the TAA target and the game's TAA is skipped.
        if (!Blit(cl, g_x.srv, g.rtv[0], outW, outH)) {
            Fail("couldn't copy DLSS's output into the TAA target");
            return false;
        }
        g_up.active = false;
        skip = true;
    }

    const std::string mode = std::string(tech == Tech::Dlss ? "DLSS " : "FSR ") +
                             (upscale ? std::to_string(renderW) + "x" + std::to_string(renderH) + " -> " +
                                            std::to_string(outW) + "x" + std::to_string(outH)
                                      : (tech == Tech::Dlss ? "DLAA " : "native AA ") + std::to_string(renderW) + "x" +
                                            std::to_string(renderH));
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (!g_runs++ || !g_lastError.empty() || mode != g_mode)
        Log("Upscaler running: %s, jitter %.3f %.3f px%s", mode.c_str(), jitterX, jitterY, reset ? ", reset" : "");
    g_mode = mode;
    g_lastError.clear();
    g_lastRun = g_frame + 1;
    return skip;
}

// Bloom composite (additive): the game's draw into its TAA output goes into X.
void OnBloom(command_list* cl, const GameState& g) {
    if (!g_up.active || g_up.bloomDone || !g_x.rtv.handle || ViewResource(g.rtv[0]) != g_up.taaOut) return;
    BindTarget(cl, g_x.rtv, g_x.w, g_x.h);
    g_restorePending = true;
    g_up.bloomDone = true;
}

// The tonemap (and the game's sharpening) read t0 by pixel position and SceneInfo's screen size:
// their draws go to output size with t0 = src and a SceneInfo copy with the output size.
bool Redirect(command_list* cl, const GameState& g, Texture& src, Texture& dst, const char* name) {
    device* dev = cl->get_device();
    const resource target = ViewResource(g.rtv[0]);
    if (!target.handle || !src.srv.handle) return false;
    const format fmt = dev->get_resource_desc(target).texture.format;
    if (!Ensure(dev, dst, src.w, src.h, fmt, false, name)) return false;
    if (!BindSceneInfoAtOutputSize(cl, g, src.w, src.h)) {
        Fail(std::string(name) + ": SceneInfo not found");
        return false;
    }
    BindPsSrv(cl, 0, src.srv);
    BindTarget(cl, dst.rtv, dst.w, dst.h);
    g_restorePending = true;
    return true;
}

void OnTonemap(command_list* cl, const GameState& g) {
    if (!g_up.active || g_up.tonemapDone) return;
    if (ViewResource(g.psSrv[0]) != g_up.taaOut) return;
    const resource out = ViewResource(g.rtv[0]);
    if (!Redirect(cl, g, g_x, g_y, "tonemapped")) {
        g_up.active = false;  // the game's own chain for the rest of this frame
        RestoreGameState(cl);
        g_restorePending = false;
        return;
    }
    g_up.tonemapDone = true;
    g_up.tonemapOut = out;
    g_up.final = &g_y;
}

void OnSharpen(command_list* cl, const GameState& g) {
    if (!g_up.active || !g_up.tonemapDone || g_up.sharpenDone || !g_set.sharpen) return;
    if (ViewResource(g.psSrv[0]) != g_up.tonemapOut) return;
    const resource out = ViewResource(g.rtv[0]);
    if (!Redirect(cl, g, g_y, g_z, "sharpened")) return;  // the tonemapped image is used as it is
    g_up.sharpenDone = true;
    g_up.sharpenOut = out;
    g_up.final = &g_z;
}

// The stretch: the game's draw (its output-size depth), then our output-size image over its colour.
bool OnStretch(command_list* cl, const GameState& g) {
    if (!g_up.active || !g_up.final || !g_up.final->srv.handle) return false;
    const resource src = ViewResource(g.psSrv[kStretchColor]);
    if (src != (g_up.sharpenDone ? g_up.sharpenOut : g_up.tonemapOut)) return false;
    Texture& img = *g_up.final;
    const resource target = ViewResource(g.rtv[0]);
    const resource_desc td = target.handle ? cl->get_device()->get_resource_desc(target) : resource_desc{};
    g_up.active = false;
    if (td.texture.width != img.w || td.texture.height != img.h) {
        Fail("output target " + std::to_string(td.texture.width) + "x" + std::to_string(td.texture.height) +
             " isn't the output size: game's upscaling used");
        return false;
    }
    cl->draw(3, 1, 0, 0);  // the game's pass
    if (!Blit(cl, img.srv, g.rtv[0], img.w, img.h)) Fail("couldn't copy the upscaled image into the output target");
    return true;
}

// ---------------------------------------------------------------- render resolution
//
// The game's render scale is via.render.RenderConfig.ImageQualityRate (the Image Quality slider).
// The REFramework script RE7DLSS-DX11.lua sets it: this side writes the wanted scale (0: leave the
// slider alone) to reframework/data/RE7DLSS-DX11_mode.txt and reads back RE7DLSS-DX11_status.txt
// ("<current scale> <frame>", or "error ...").

std::wstring DataDir() { return ExeDir() + L"reframework\\data\\"; }

float g_writtenScale = -1;

void WriteWantedScale() {
    const float want = g_set.enabled && g_set.mode > 0 ? kModes[g_set.mode].scale : 0;
    if (want == g_writtenScale) return;
    if (GetFileAttributesW((ExeDir() + L"reframework").c_str()) == INVALID_FILE_ATTRIBUTES) return;  // no REFramework
    CreateDirectoryW(DataDir().c_str(), nullptr);
    if (FILE* f = _wfopen((DataDir() + L"RE7DLSS-DX11_mode.txt").c_str(), L"w")) {
        fprintf(f, "%.4f\n", want);
        fclose(f);
        g_writtenScale = want;
        Log("render scale for RE7DLSS-DX11.lua: %s", want > 0 ? std::to_string(want).c_str() : "the game's slider");
    }
}

// The script's last report: 1 = scale read (recent), 0 = no recent report, -1 = the script's error.
int ScriptStatus(float* scale, std::string* error) {
    const std::wstring path = DataDir() + L"RE7DLSS-DX11_status.txt";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t t = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    const uint64_t n = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    if (n > t && n - t > 50000000ull) return 0;  // older than 5 s
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return 0;
    char buf[256] = {};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    if (strncmp(buf, "error", 5) == 0) {
        *error = buf;
        while (!error->empty() && (error->back() == '\n' || error->back() == '\r')) error->pop_back();
        return -1;
    }
    return sscanf(buf, "%f", scale) == 1 ? 1 : 0;
}

// ---------------------------------------------------------------- longer jitter sequence
//
// The game cycles 16 jitter positions (logged 2026-11-15). DLSS wants about 8 / scale^2 (32 at 50%):
// with 16 at Performance it leaves soft striped patches on still surfaces (Jill's face). While
// upscaling, the main view's SceneInfo gets a Halton(2,3) sequence of that length instead, written
// as the CPU fills the buffer, before the GPU reads it, so the game's draws, its TAA and the
// upscaler all use the same jitter. In RE7's SceneInfo (floats; checked against a capture
// 2026-11-16):
//   projMat row 2 xy (56, 57): jitter in clip units (jitter x px = -z * w / 2, y px = w * h / 2);
//   viewProjMat (0..15, row-major, row vectors): columns x and y carry -jitter x the w column;
//   viewProjInvMat (80..95): its row 3 carries the inverse (+= dx * row 0 + dy * row 1);
//   projInvMat row 3 xy (76, 77): jitter / projMat x, y scale (48, 53).
// prevViewProjMat has no jitter, so motion vectors don't change.

float Halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    while (i) {
        f /= static_cast<float>(b);
        r += f * static_cast<float>(i % b);
        i /= b;
    }
    return r;
}

std::atomic<uint32_t> g_jitterPhases{0};  // the sequence length in use (0: the game's)

void PatchJitter(float* s) {
    const float jx = s[kJitter], jy = s[kJitter + 1], sw = s[kScreenSize], sh = s[kScreenSize + 1];
    const float ax = s[kProjMat], ay = s[kProjMat + 5];
    if (jx == 0 && jy == 0) return;  // views without jitter (shadows and the like)
    if (!(sw >= 64 && sh >= 64 && sw <= 16384 && sh <= 16384)) return;
    if (!(ax > 0.05f && ax < 50 && ay > 0.05f && ay < 50)) return;
    if (s[kProjMat + 11] != -1.0f) return;  // not a perspective projection like the main view's
    if (std::fabs(jx) * sw > 2 || std::fabs(jy) * sh > 2) return;  // more than a pixel: not a jitter
    if (!g_outW) return;
    const float scale = sw / static_cast<float>(g_outW);
    // Native (and the game's other jittered SceneInfo at the output size, used after the stretch):
    // the game's own sequence.
    if (scale >= 0.99f) return;
    uint32_t n = static_cast<uint32_t>(std::ceil(8.0f / (scale * scale)));
    n = n < 8 ? 8 : n > 64 ? 64 : n;
    const uint32_t i = static_cast<uint32_t>(g_frame % n) + 1;  // Halton skips index 0
    const float px = Halton(i, 2) - 0.5f, py = Halton(i, 3) - 0.5f;
    const float nx = -2 * px / sw, ny = 2 * py / sh;
    const float dx = nx - jx, dy = ny - jy;
    for (int r = 0; r < 4; ++r) {
        s[r * 4 + 0] -= dx * s[r * 4 + 3];
        s[r * 4 + 1] -= dy * s[r * 4 + 3];
    }
    for (int c = 0; c < 4; ++c) s[kViewProjInv + 12 + c] += dx * s[kViewProjInv + c] + dy * s[kViewProjInv + 4 + c];
    s[kJitter] = nx, s[kJitter + 1] = ny;
    s[kProjInv + 12] = nx / ax, s[kProjInv + 13] = ny / ay;
    g_jitterPhases = n;
    static uint32_t logged = 0;  // (0.1.1 logged this on almost every write)
    if (logged != n) logged = n, Log("jitter: %u positions at %.0fx%.0f (the game's 16 replaced)", n, sw, sh);
}

bool g_keyWasDown = false;
const char* kVersion = RD_VERSION;

}  // namespace

const ImVec4 kWarn(1, 0.6f, 0.3f, 1);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void UpscalerInit() {
    LoadSettings();
    Log("Upscaling %s at start (F9 toggles), upscaler setting %d (0 auto, 1 DLSS, 2 FSR)", g_set.enabled ? "on" : "off",
        g_set.upscaler);
    reshade::register_event<reshade::addon_event::init_sampler>(&OnInitSampler);
    reshade::register_event<reshade::addon_event::destroy_sampler>(&OnDestroySampler);
}

void PatchSceneInfo(float* scene) {
    // Only once the known TAA shader has run: the SceneInfo layout is checked for that build.
    if (g_set.enabled && g_set.longJitter && g_taaSeen) PatchJitter(scene);
    else g_jitterPhases = 0;
}

void UpscalerShutdown() {
    reshade::unregister_event<reshade::addon_event::init_sampler>(&OnInitSampler);
    reshade::unregister_event<reshade::addon_event::destroy_sampler>(&OnDestroySampler);
    dlss::Shutdown();
    bridge::Shutdown();
}

// After the game binds samplers: its material ones are replaced by copies with the mip bias.
void UpscalerOnSamplers(command_list* cl, shader_stage stages, const descriptor_table_update& u) {
    const float bias = g_mipBias.load();
    if (bias == 0 || (stages & shader_stage::pixel) == 0 || !u.count) return;
    sampler repl[16];
    const uint32_t n = u.count < 16 ? u.count : 16;
    bool any = false;
    device* dev = cl->get_device();
    {
        std::lock_guard<std::mutex> lock(g_samplerMutex);
        if (g_biasMade != bias) {
            for (auto& [game, ours] : g_biased) dev->destroy_sampler(ours);
            g_biased.clear();
            g_biasMade = bias;
        }
        for (uint32_t i = 0; i < n; ++i) {
            repl[i] = static_cast<const sampler*>(u.descriptors)[i];
            auto m = g_materialSamplers.find(repl[i].handle);
            if (m == g_materialSamplers.end()) continue;
            auto b = g_biased.find(repl[i].handle);
            if (b == g_biased.end()) {
                sampler_desc d = m->second;
                d.mip_lod_bias += bias;
                sampler s = {0};
                if (!dev->create_sampler(d, &s)) continue;
                b = g_biased.emplace(repl[i].handle, s).first;
            }
            repl[i] = b->second;
            any = true;
        }
    }
    if (!any) return;
    descriptor_table_update r = u;
    r.count = n;
    r.descriptors = repl;
    cl->push_descriptors(shader_stage::pixel, pipeline_layout{0}, 0, r);
}

void UpscalerDestroyDevice(device* dev) {
    if (dev != g_dev && g_dev) return;
    g_dev = dev;
    bridge::Shutdown();
    {
        std::lock_guard<std::mutex> lock(g_samplerMutex);
        for (auto& [game, ours] : g_biased) dev->destroy_sampler(ours);
        g_biased.clear();
        g_materialSamplers.clear();
    }
    for (IUnknown* o : std::initializer_list<IUnknown*>{g_downVs, g_downPs, g_linear, g_downCb})
        if (o) o->Release();
    g_downVs = nullptr, g_downPs = nullptr, g_linear = nullptr, g_downCb = nullptr;
    if (g_exposureCs) g_exposureCs->Release();
    if (g_exposureUav) g_exposureUav->Release();
    if (g_exposureTex) g_exposureTex->Release();
    g_exposureCs = nullptr, g_exposureUav = nullptr, g_exposureTex = nullptr;
    if (g_depthSrv.handle) dev->destroy_resource_view(g_depthSrv);
    g_depthSrv = {0}, g_depthRes = {0};
    if (g_mvCs) g_mvCs->Release();
    if (g_mvCb) g_mvCb->Release();
    g_mvCs = nullptr, g_mvCb = nullptr;
    if (g_rcasCs) g_rcasCs->Release();
    if (g_rcasCb) g_rcasCb->Release();
    g_rcasCs = nullptr, g_rcasCb = nullptr;
    for (Texture* t : {&g_x, &g_xs, &g_y, &g_z, &g_depth32, &g_mv}) Free(*t);
    if (g_exposureOne.handle) dev->destroy_resource(g_exposureOne);
    if (g_sceneCb.handle) dev->destroy_resource(g_sceneCb);
    g_exposureOne = g_sceneCb = {0};
    if (g_blitVs) g_blitVs->Release();
    if (g_blitPs) g_blitPs->Release();
    if (g_cleanState) g_cleanState->Release();
    g_blitVs = nullptr, g_blitPs = nullptr, g_cleanState = nullptr;
    dlss::Shutdown();
    g_dev = nullptr;
}

void UpscalerBeforeWork(command_list* cl) {
    if (!g_restorePending) return;
    g_restorePending = false;
    RestoreGameState(cl);
}

bool UpscalerOnDraw(command_list* cl, uint32_t vertices, uint32_t instances) {
    UpscalerBeforeWork(cl);
    if (!g_set.enabled || vertices != 3 || instances != 1) return false;
    GameState g;
    if (!GetGameState(cl, &g)) return false;
    if (!g_dev) g_dev = cl->get_device();
    switch (g.ps) {
        case kTaaPs: return OnTaa(cl, g);
        case kBloomPs: OnBloom(cl, g); return false;
        case kTonemapPs: OnTonemap(cl, g); return false;
        case kSharpenPs: OnSharpen(cl, g); return false;
        case kStretchPs: return OnStretch(cl, g);
        default: return false;
    }
}

void UpscalerPresent(command_queue* q, swapchain* sc) {
    if (g_restorePending) {
        g_restorePending = false;
        // (nothing of the game's runs between a redirected draw and the present in practice)
    }
    ++g_frame;
    if (g_up.active) {  // a frame where the chain didn't reach the stretch (menus): nothing redirected now
        g_up.active = false;
    }
    const resource_desc bd = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
    if (bd.texture.width != g_outW || bd.texture.height != g_outH) {
        g_outW = bd.texture.width, g_outH = bd.texture.height;
        Log("output size %ux%u", g_outW, g_outH);
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_set.enabled = !g_set.enabled;
        SaveSettings();
        Log("Upscaling %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    if (g_frame % 30 == 0) WriteWantedScale();  // also picks up REFramework installed after the start
    bridge::EndFrame();

    // Texture mip bias: log2 of the render scale while upscaling ran in the last second or so.
    const bool upscaling = g_set.enabled && g_lastUpscale && g_lastRun + 60 >= g_frame && g_lastRun;
    const float scale = g_renderScale.load();
    const float want = upscaling && g_set.textureDetail && scale < 0.99f && scale > 0.2f ? std::log2(scale) : 0.0f;
    if (std::fabs(want - g_mipBias.load()) >= 0.05f || (want == 0 && g_mipBias.load() != 0)) {
        g_mipBias = want;
        size_t n;
        {
            std::lock_guard<std::mutex> lock(g_samplerMutex);
            n = g_materialSamplers.size();
        }
        Log("texture mip bias now %.2f (render scale %.2f, %zu material samplers seen)", want, scale, n);
    }
    (void)q;
}

void UpscalerOverlay() {
    ImGui::Text(RD_PRODUCT " %s", kVersion);
    bool changed = false;
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Settings")) {
            changed |= ImGui::Checkbox("Upscaling (F9)", &g_set.enabled);
            Help("DLSS or FSR in place of the game's TAA. 100% render resolution gives native anti-aliasing (DLAA), "
                 "lower values are upscaled to the screen.");
            {
                const char* techs[] = {"Auto", "DLSS", "FSR 3.1 / FSR 4"};
                ImGui::SetNextItemWidth(ControlWidth());
                if (ImGui::Combo("Upscaler", &g_set.upscaler, techs, 3)) {
                    changed = true;
                    Log("Upscaler setting: %s", techs[g_set.upscaler]);
                }
                Help("Auto: DLSS on NVIDIA RTX cards, FSR on others.\nFSR runs through Direct3D 12 next to the game "
                     "(AMD's runtime is Direct3D 12 only); it picks FSR 4 on Radeon cards that support it.");
            }
            const char* modeNames[kModeCount];
            for (int i = 0; i < kModeCount; ++i) modeNames[i] = kModes[i].name;
            ImGui::SetNextItemWidth(ControlWidth());
            if (ImGui::Combo("Render resolution", &g_set.mode, modeNames, kModeCount)) {
                changed = true;
                WriteWantedScale();
                // The DLSS model follows the render resolution: back to Automatic, which picks the model
                // NVIDIA recommends for it (K down to Balanced, M at Performance).
                if (g_set.preset != dlss::kPresetAuto) {
                    g_set.preset = dlss::kPresetAuto;
                    Log("render resolution changed: DLSS model back to Automatic");
                }
            }
            Help("Sets the game's render scale (its Image Quality setting) while upscaling is on, and keeps it there. "
                 "Needs REFramework's build for this game version (TDB49, for the dx11_non-rt branch): the bundled "
                 "RE7DLSS-DX11.lua sets it. \"Game's Image Quality slider\" leaves it to the game's menu.");
            {
                float now = 0;
                std::string scriptError;
                const int st = ScriptStatus(&now, &scriptError);
                if (st > 0)
                    ImGui::TextDisabled("Render scale now: %.0f%%%s", now * 100,
                                        g_set.mode > 0 && g_set.enabled ? "" : " (the game's slider)");
                else if (st < 0) ImGui::TextColored(kWarn, "RE7DLSS-DX11.lua: %s", scriptError.c_str());
                else if (g_set.mode > 0)
                    ImGui::TextColored(kWarn, "RE7DLSS-DX11.lua isn't running: is REFramework (TDB49 build) installed?");
            }
            int idx = 0;
            for (int i = 0; i < kPresetCount; ++i)
                if (kPresets[i].value == g_set.preset) idx = i;
            const char* names[kPresetCount];
            for (int i = 0; i < kPresetCount; ++i) names[i] = kPresets[i].name;
            ImGui::BeginDisabled(g_set.upscaler == 2 || (g_set.upscaler == 0 && g_active == Tech::Fsr));
            ImGui::SetNextItemWidth(ControlWidth());
            if (ImGui::Combo("DLSS model", &idx, names, kPresetCount)) {
                g_set.preset = kPresets[idx].value;
                changed = true;
            }
            ImGui::EndDisabled();
            Help("Automatic: the model NVIDIA recommends for the render resolution: K at Native, Quality and Balanced, "
                 "M at Performance. Changing the render resolution here sets it back to Automatic; pick a model after "
                 "that to keep it.");
            if (g_set.preset == dlss::kPresetAuto && g_active == Tech::Dlss && g_lastRun) {
                const float s = g_renderScale.load();
                ImGui::TextDisabled("Automatic is using %s", s >= 0.54f ? "K" : s >= 0.42f ? "M" : "L");
            }
            ImGui::SetNextItemWidth(ControlWidth());
            changed |= ImGui::SliderFloat("Sharpening", &g_set.sharpness, 0.0f, 1.0f, g_set.sharpness < 0.005f ? "Off" : "%.2f");
            Help("Extra sharpening of the upscaled image (AMD's RCAS, contrast-adaptive: crisper edges without halos). "
                 "0 = off. For DLSS it runs after it; for FSR it's FSR's own.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Status")) {
            const bool running = g_lastRun + 2 >= g_frame && g_lastRun;
            std::string err, mode;
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                err = g_lastError, mode = g_mode;
            }
            if (!g_set.enabled) ImGui::TextUnformatted("Upscaling off (F9)");
            else if (running) ImGui::Text("%s", mode.c_str());
            else ImGui::TextColored(kWarn, "Not upscaling%s%s", err.empty() ? "" : ": ", err.c_str());
            if (g_active != Tech::Fsr) ImGui::TextDisabled("DLSS: %s", dlss::Status().c_str());
            if (g_active == Tech::Fsr || g_set.upscaler == 2) ImGui::TextDisabled("FSR: %s", bridge::Status().c_str());
            ImGui::TextDisabled("Output %ux%u, render scale %.0f%%", g_outW, g_outH, g_renderScale.load() * 100);
            ImGui::TextDisabled("Texture mip bias %.2f", g_mipBias.load());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced")) {
            ImGui::SeparatorText("Image quality");
            changed |= ImGui::Checkbox("Upscale below 100%", &g_set.upscale);
            Help("Off: below 100% DLSS anti-aliases at the render resolution and the game stretches the result.");
            changed |= ImGui::Checkbox("Game's film grain at output resolution", &g_set.sharpen);
            Help("The game's film grain pass, run on the upscaled image. Off: no grain while upscaling.");
            changed |= ImGui::Checkbox("Use the game's exposure", &g_set.gameExposure);
            Help("Off (default): DLSS estimates the exposure itself. On: DLSS is told the game's exposure; in testing "
                 "this made characters' dithered fade (Leon close to the camera) look see-through. FSR always "
                 "estimates its own.");
            changed |= ImGui::Checkbox("Longer jitter sequence", &g_set.longJitter);
            Help("When upscaling, the game's 16 camera jitter positions are replaced by as many as DLSS asks for at "
                 "the render resolution (32 at Performance). Fewer leave soft striped patches on still surfaces such "
                 "as faces at Performance.");
            changed |= ImGui::Checkbox("Sharper textures", &g_set.textureDetail);
            Help("When upscaling, textures are sampled as at the output resolution (mip bias log2 of the render scale, "
                 "as NVIDIA recommends). Follows the render resolution as it changes.");
            ImGui::SeparatorText("Development");
            changed |= ImGui::Checkbox("Flip jitter X", &g_set.flipJitterX);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Flip jitter Y", &g_set.flipJitterY);
            Help("Testing only: the sign of the camera jitter given to DLSS. Leave both off.");
            if (ImGui::Checkbox("Take the jitter out of the motion vectors", &g_set.mvFix))
                Log("test: jitter taken out of the motion vectors: %s", g_set.mvFix ? "on" : "off");
            Help("On (default): the game's motion vectors include each frame's camera jitter; it's taken out "
                 "before DLSS and FSR get them. Off: as the game made them (soft striped patches on still faces "
                 "with DLSS at Performance). Not saved.");
            ImGui::Checkbox("DLSS: depth as a 32-bit float copy", &g_set.dlssDepthCopy);
            Help("Testing only (not saved): DLSS gets a copy of the depth in a plain float texture, as FSR does, "
                 "instead of the game's depth-stencil texture.");
            DrawCaptureTool();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (changed) SaveSettings();
}

}  // namespace rd
