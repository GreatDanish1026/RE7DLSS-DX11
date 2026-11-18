// Debug view (Advanced, "Show the effect mask"): the add-on's effect mask (render size) drawn in red
// over the upscaler's output (output size), in place of the picture.

cbuffer Params : register(b0) {
    float2 OutSize;  // output size in pixels
    float2 Scale;    // render size / output size
};

Texture2D<float> Mask : register(t0);        // render resolution
RWTexture2D<float4> Output : register(u0);  // output resolution

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(float2(id) >= OutSize)) return;
    Output[id] = float4(Mask[uint2((float2(id) + 0.5) * Scale)], 0, 0, 1);
}
