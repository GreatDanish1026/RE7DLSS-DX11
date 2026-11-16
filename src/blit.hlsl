// Copies an image into a render target with a full-screen triangle (the render target write converts
// formats: DLSS writes rgba16f, the game's targets are r11g11b10f or rgba16f). Source and target are
// the same size, so each pixel is loaded, not filtered.

Texture2D<float4> Source : register(t0);

void VsMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PsMain(float4 pos : SV_Position) : SV_Target {
    return float4(Source.Load(int3(pos.xy, 0)).rgb, 1);
}
