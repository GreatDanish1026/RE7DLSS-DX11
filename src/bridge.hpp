// FSR 3.1 / FSR 4 for a Direct3D 11 game: AMD's FidelityFX runtime is Direct3D 12 only, so it runs
// on a Direct3D 12 device of the add-on's own (same GPU), with the inputs and output in textures
// shared between the two devices and shared fences ordering the work.
#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace bridge {

struct Inputs {
    ID3D11Resource* color = nullptr;     // render size
    // Depth (reversed Z) is drawn by the caller into DepthTarget()'s render target first.
    ID3D11Resource* motion = nullptr;    // render size
    ID3D11Resource* reactive = nullptr;  // optional, r8
    ID3D11Resource* output = nullptr;    // output size, rgba16f
    unsigned renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    float jitterX = 0, jitterY = 0, motionScaleX = 1, motionScaleY = 1;
    float cameraNear = 0.01f, cameraFovY = 1.0f;
    float sharpness = 0;
    bool reset = false;
};

// Creates the Direct3D 12 device and loads AMD's runtime (once). False with a reason if FSR can't run.
bool Init(ID3D11Device* device, std::string& error);
// The shared r32f depth texture's render target view (w x h, made or remade as needed): the caller
// draws the game's depth into it before Evaluate (a depth-stencil texture can't be copied into it).
ID3D11RenderTargetView* DepthTarget(unsigned w, unsigned h, std::string& error);
// Copies the inputs over, runs FSR on the Direct3D 12 queue, and copies its result into in.output;
// the Direct3D 11 context waits for it on the GPU.
bool Evaluate(ID3D11DeviceContext* context, const Inputs& in, std::string& error);
void EndFrame();
void Shutdown();
std::string Status();       // FSR's status line
std::string VersionName();  // "FSR 3.1.6" / "FSR 4.1.2" once running

}  // namespace bridge
