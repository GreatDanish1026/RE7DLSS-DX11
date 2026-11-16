// What main.cpp (logging, shader ids, binding tracking, frame capture) shares with the upscaler.
#pragma once

#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <string>

namespace rd {

void Log(const char* fmt, ...);
std::wstring ExeDir();  // with a trailing backslash

constexpr uint32_t kSrvSlots = 128, kCbSlots = 16, kUavSlots = 64;

// The game's pipeline state on a device context as it last set it (the add-on's own calls don't
// change this): pixel shader and its first bindings, render targets, viewport and scissor.
struct GameState {
    uint32_t ps = 0;  // pixel shader id (FNV-1a of the bytecode)
    reshade::api::resource_view psSrv[8] = {};
    reshade::api::buffer_range psCb[4] = {};
    reshade::api::resource_view vsSrv0 = {0};  // vertex shader t0
    reshade::api::resource_view rtv[8] = {};
    uint32_t rtvCount = 0;
    reshade::api::resource_view dsv = {0};
    reshade::api::viewport vp = {};
    reshade::api::rect scissor = {};
};
bool GetGameState(reshade::api::command_list* cl, GameState* out);
// Binds the game's render targets, viewport, scissor and pixel shader t0-t7 / b0-b3 again (after the
// add-on changed them for one draw).
void RestoreGameState(reshade::api::command_list* cl);

// The resource a view was created for (from the add-on's own map).
reshade::api::resource ViewResource(reshade::api::resource_view view);
// A 512- or 256-byte constant buffer's contents as the CPU last wrote them (SceneInfo, Tonemap).
bool ReadCb(reshade::api::resource buffer, uint64_t offset, void* out, size_t size);

// A 512-byte SceneInfo the CPU is writing (before the GPU reads it): the main view's jitter is
// replaced by the add-on's longer sequence (upscaler.cpp). Called with main.cpp's g_resMutex held.
void PatchSceneInfo(float* scene);

// Overlay helpers (upscaler.cpp).
float ControlWidth();
void Help(const char* text);
extern const ImVec4 kWarn;

// upscaler.cpp
void UpscalerInit();
void UpscalerShutdown();
void UpscalerDestroyDevice(reshade::api::device* dev);
bool UpscalerOnDraw(reshade::api::command_list* cl, uint32_t vertices, uint32_t instances);  // true: draw skipped
void UpscalerBeforeWork(reshade::api::command_list* cl);  // any other GPU work: puts redirected state back
// After the game binds samplers (D3D11 PSSetSamplers): material samplers swapped for biased copies.
void UpscalerOnSamplers(reshade::api::command_list* cl, reshade::api::shader_stage stages,
                        const reshade::api::descriptor_table_update& u);
void UpscalerPresent(reshade::api::command_queue* q, reshade::api::swapchain* sc);
void UpscalerOverlay();

// main.cpp: the frame capture tool (Advanced section).
void DrawCaptureTool();

}  // namespace rd
