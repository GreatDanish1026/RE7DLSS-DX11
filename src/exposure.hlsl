// The game's exposure for DLSS, computed as its TAA vertex shader (vs3f48df32) does:
// Exposure = (useAutoExposure ? WhitePtSrv[0] : 1) * exposureAdjustment. The TAA multiplies the scene
// colour by it before tonemapping, so DLSS's exposure texture is this value.

cbuffer Tonemap : register(b0) {  // the game's own constant buffer (b1 of the TAA)
    float exposureAdjustment;
    float tonemapRange;
    float sharpness;
    float preTonemapRange;
    int useAutoExposure;
};

ByteAddressBuffer WhitePt : register(t0);  // the game's WhitePtSrv (t0 of the TAA's vertex shader)
RWTexture2D<float> Exposure : register(u0);

[numthreads(1, 1, 1)]
void main() {
    const float white = useAutoExposure ? asfloat(WhitePt.Load(0)) : 1.0f;
    const float e = white * exposureAdjustment;
    Exposure[uint2(0, 0)] = (e > 0 && e < 1e6) ? e : 1.0f;
}
