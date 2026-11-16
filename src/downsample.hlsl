// The upscaled image scaled down to the render size, into the game's TAA target: the passes that
// still run at render size (the bloom's bright pass and blur) then see the upscaler's image, as
// they do at native resolution. Each target pixel averages a 2x2 grid of bilinear samples over its
// footprint in the source (up to 3x larger), which keeps small bright lights' energy.

Texture2D<float4> Source : register(t0);
SamplerState Linear : register(s0);

cbuffer Params : register(b0) {
    float2 InvTarget;  // 1 / target size
    float2 Pad;
};

void VsMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PsMain(float4 pos : SV_Position) : SV_Target {
    const float2 uv = pos.xy * InvTarget;
    const float2 q = 0.25 * InvTarget;  // a quarter of the target pixel, in UV
    float3 c = Source.SampleLevel(Linear, uv + float2(-q.x, -q.y), 0).rgb;
    c += Source.SampleLevel(Linear, uv + float2(q.x, -q.y), 0).rgb;
    c += Source.SampleLevel(Linear, uv + float2(-q.x, q.y), 0).rgb;
    c += Source.SampleLevel(Linear, uv + float2(q.x, q.y), 0).rgb;
    return float4(c * 0.25, 1);
}
