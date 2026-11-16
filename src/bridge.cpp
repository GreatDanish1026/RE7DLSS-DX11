// FSR through a Direct3D 12 device next to the game's Direct3D 11 one (bridge.hpp).
//
// Per frame:
//   D3D11: copy colour, depth, motion vectors and the reactive mask into shared textures,
//          signal the shared fence (value N), flush.
//   D3D12: wait for N, transition the shared textures, FSR dispatch (fsr.cpp, unchanged from the
//          Direct3D 12 add-ons), transition back to COMMON, signal N+1.
//   D3D11: wait for N+1 (on the GPU), copy the shared output into the add-on's output texture.
// Shared textures are created by Direct3D 12 (D3D12_HEAP_FLAG_SHARED) and opened by Direct3D 11.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "bridge.hpp"
#include "common.hpp"
#include "fsr.hpp"

#include <mutex>

namespace bridge {
namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::mutex g_mutex;
bool g_initTried = false, g_ready = false;
std::string g_initError, g_status = "not started";

ID3D11Device5* g_d11 = nullptr;
ID3D12Device* g_d12 = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
constexpr int kLists = 3;
ID3D12CommandAllocator* g_alloc[kLists] = {};
uint64_t g_allocValue[kLists] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence12 = nullptr;
ID3D11Fence* g_fence11 = nullptr;
uint64_t g_value = 0;
int g_next = 0;

struct Shared {
    ID3D12Resource* r12 = nullptr;
    ID3D11Resource* r11 = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
Shared g_color, g_depth, g_motion, g_reactive, g_output;

void Free(Shared& s) {
    SafeRelease(s.r11);
    SafeRelease(s.r12);
    s = Shared();
}

// A texture both devices can use, recreated when the size or format changes. The GPU may still
// use the old one: the queue is waited on first.
void WaitIdle() {
    if (!g_fence12 || !g_value) return;
    if (g_fence12->GetCompletedValue() < g_value) {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fence12->SetEventOnCompletion(g_value, e);
        WaitForSingleObject(e, 2000);
        CloseHandle(e);
    }
}

bool Ensure(Shared& s, UINT w, UINT h, DXGI_FORMAT fmt, bool uav, const char* name, std::string& error,
            bool renderTarget = false) {
    if (s.r12 && s.w == w && s.h == h && s.fmt == fmt) return true;
    WaitIdle();
    Free(s);
    // Made by Direct3D 11 as a shared (NT handle) texture and opened by Direct3D 12: the usual way
    // round (D3D12-made shared textures opened by D3D11 failed with E_INVALIDARG in 0.2.0).
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0) |
                  (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ID3D11Texture2D* t = nullptr;
    const char* step = "CreateTexture2D";
    HRESULT hr = g_d11->CreateTexture2D(&d, nullptr, &t);
    IDXGIResource1* dxgi = nullptr;
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr)) step = "IDXGIResource1", hr = t->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr))
        step = "CreateSharedHandle",
        hr = dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
    if (SUCCEEDED(hr)) step = "OpenSharedHandle", hr = g_d12->OpenSharedHandle(handle, IID_PPV_ARGS(&s.r12));
    SafeRelease(dxgi);
    if (handle) CloseHandle(handle);
    s.r11 = t;
    if (FAILED(hr)) {
        Free(s);
        char buf[200];
        snprintf(buf, sizeof(buf), "couldn't share the %ux%u %s texture (format %u) with Direct3D 12: %s failed (0x%08lx)", w,
                 h, name, static_cast<unsigned>(fmt), step, static_cast<unsigned long>(hr));
        error = buf;
        rd::Log("FSR bridge: %s", buf);
        g_ready = false;  // don't try again every frame
        g_initError = g_status = buf;
        return false;
    }
    s.w = w, s.h = h, s.fmt = fmt;
    rd::Log("FSR bridge: shared %ux%u %s texture (format %u)", w, h, name, static_cast<unsigned>(fmt));
    return true;
}

ID3D11RenderTargetView* g_depthRtv = nullptr;  // of g_depth

bool EnsureLike(Shared& s, ID3D11Resource* src, const char* name, std::string& error) {
    ID3D11Texture2D* t = nullptr;
    if (FAILED(src->QueryInterface(IID_PPV_ARGS(&t)))) {
        error = std::string(name) + " isn't a 2D texture";
        return false;
    }
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    t->Release();
    return Ensure(s, d.Width, d.Height, d.Format, false, name, error);
}

void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, D3D12_RESOURCE_BARRIER* out) {
    *out = {};
    out->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out->Transition.pResource = r;
    out->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    out->Transition.StateBefore = from;
    out->Transition.StateAfter = to;
}

}  // namespace

bool Init(ID3D11Device* device, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initTried) {
        error = g_initError;
        return g_ready;
    }
    g_initTried = true;
    auto fail = [&](const std::string& why, HRESULT hr) {
        char buf[32];
        snprintf(buf, sizeof(buf), " (0x%08lx)", static_cast<unsigned long>(hr));
        g_initError = why + (hr ? buf : "");
        g_status = g_initError;
        rd::Log("FSR bridge: %s", g_initError.c_str());
        error = g_initError;
        return false;
    };
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&g_d11));
    if (FAILED(hr)) return fail("Direct3D 11.4 (Windows 10 1703 or later) needed for FSR", hr);

    // Direct3D 12 on the same GPU as the game.
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    SafeRelease(dxgi);
    if (FAILED(hr)) return fail("the game's graphics card wasn't found", hr);
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d12));
    DXGI_ADAPTER_DESC ad = {};
    adapter->GetDesc(&ad);
    SafeRelease(adapter);
    if (FAILED(hr)) return fail("this graphics card has no Direct3D 12 support, which FSR needs", hr);
    rd::Log("FSR bridge: Direct3D 12 device on %ls", ad.Description);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
    for (int i = 0; i < kLists && SUCCEEDED(hr); ++i)
        hr = g_d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));
    if (SUCCEEDED(hr))
        hr = g_d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list));
    if (SUCCEEDED(hr)) hr = g_list->Close();
    if (FAILED(hr)) return fail("couldn't create the Direct3D 12 queue", hr);

    hr = g_d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_fence12));
    HANDLE fh = nullptr;
    if (SUCCEEDED(hr)) hr = g_d12->CreateSharedHandle(g_fence12, nullptr, GENERIC_ALL, nullptr, &fh);
    if (SUCCEEDED(hr)) hr = g_d11->OpenSharedFence(fh, IID_PPV_ARGS(&g_fence11));
    if (fh) CloseHandle(fh);
    if (FAILED(hr)) return fail("couldn't share a fence between Direct3D 11 and 12", hr);

    std::string err;
    if (!fsr::Init(g_d12, err)) return fail(err, 0);
    g_ready = true;
    g_status = "ready";
    return true;
}

ID3D11RenderTargetView* DepthTarget(unsigned w, unsigned h, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready) {
        error = g_initError.empty() ? "FSR not started" : g_initError;
        return nullptr;
    }
    if (g_depth.r12 && g_depth.w == w && g_depth.h == h && g_depthRtv) return g_depthRtv;
    SafeRelease(g_depthRtv);
    if (!Ensure(g_depth, w, h, DXGI_FORMAT_R32_FLOAT, false, "depth", error, true)) {
        error = g_initError;
        return nullptr;
    }
    if (FAILED(g_d11->CreateRenderTargetView(g_depth.r11, nullptr, &g_depthRtv))) {
        error = "couldn't make a render target of the shared depth texture";
        return nullptr;
    }
    return g_depthRtv;
}

bool Evaluate(ID3D11DeviceContext* context, const Inputs& in, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready) {
        error = g_initError.empty() ? "FSR not started" : g_initError;
        return false;
    }
    ID3D11DeviceContext4* ctx = nullptr;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&ctx)))) {
        error = "Direct3D 11.4 device context not available";
        return false;
    }
    bool ok = g_depthRtv && EnsureLike(g_color, in.color, "colour", error) &&
              EnsureLike(g_motion, in.motion, "motion vector", error) &&
              (!in.reactive || EnsureLike(g_reactive, in.reactive, "reactive mask", error)) &&
              Ensure(g_output, in.outputWidth, in.outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "output", error);
    if (!ok) {
        ctx->Release();
        return false;
    }

    // Direct3D 11: inputs over, then the fence.
    ctx->CopyResource(g_color.r11, in.color);  // (depth: drawn into g_depth by the caller)
    ctx->CopyResource(g_motion.r11, in.motion);
    if (in.reactive) ctx->CopyResource(g_reactive.r11, in.reactive);
    const uint64_t inputsReady = ++g_value;
    ctx->Signal(g_fence11, inputsReady);
    ctx->Flush();

    // Direct3D 12: FSR.
    ID3D12CommandAllocator* alloc = g_alloc[g_next];
    if (g_fence12->GetCompletedValue() < g_allocValue[g_next]) {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fence12->SetEventOnCompletion(g_allocValue[g_next], e);
        WaitForSingleObject(e, 2000);
        CloseHandle(e);
    }
    alloc->Reset();
    g_list->Reset(alloc, nullptr);
    ID3D12Resource* inputs[] = {g_color.r12, g_depth.r12, g_motion.r12, in.reactive ? g_reactive.r12 : nullptr};
    D3D12_RESOURCE_BARRIER b[5];
    UINT n = 0;
    for (ID3D12Resource* r : inputs)
        if (r) Barrier(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &b[n++]);
    Barrier(g_output.r12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &b[n++]);
    g_list->ResourceBarrier(n, b);

    fsr::Inputs f;
    f.color = g_color.r12;
    f.depth = g_depth.r12;
    f.motion = g_motion.r12;
    f.reactive = in.reactive ? g_reactive.r12 : nullptr;
    f.output = g_output.r12;
    f.renderWidth = in.renderWidth, f.renderHeight = in.renderHeight;
    f.outputWidth = in.outputWidth, f.outputHeight = in.outputHeight;
    f.jitterX = in.jitterX, f.jitterY = in.jitterY;
    f.motionScaleX = in.motionScaleX, f.motionScaleY = in.motionScaleY;
    f.cameraNear = in.cameraNear, f.cameraFar = 0, f.cameraFovY = in.cameraFovY;
    f.metersPerUnit = 1.0f;
    f.sharpness = in.sharpness;
    f.reset = in.reset;
    ok = fsr::Evaluate(g_list, f, error);

    for (UINT i = 0; i < n; ++i) std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter);
    g_list->ResourceBarrier(n, b);
    g_list->Close();
    g_queue->Wait(g_fence12, inputsReady);
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    const uint64_t done = ++g_value;
    g_queue->Signal(g_fence12, done);
    g_allocValue[g_next] = done;
    g_next = (g_next + 1) % kLists;

    // Direct3D 11: wait for FSR on the GPU, then the result over.
    ctx->Wait(g_fence11, done);
    if (ok) ctx->CopyResource(in.output, g_output.r11);
    ctx->Release();
    g_status = fsr::Status();
    return ok;
}

void EndFrame() {
    if (g_ready) fsr::EndFrame();
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    WaitIdle();
    if (g_ready) fsr::Shutdown();
    SafeRelease(g_depthRtv);
    for (Shared* s : {&g_color, &g_depth, &g_motion, &g_reactive, &g_output}) Free(*s);
    SafeRelease(g_fence11);
    SafeRelease(g_fence12);
    SafeRelease(g_list);
    for (auto& a : g_alloc) SafeRelease(a);
    SafeRelease(g_queue);
    SafeRelease(g_d12);
    SafeRelease(g_d11);
    g_ready = false;
    g_initTried = false;
    g_value = 0;
}

std::string Status() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_ready ? fsr::Status() : g_status;
}

std::string VersionName() { return fsr::VersionName(); }

}  // namespace bridge
