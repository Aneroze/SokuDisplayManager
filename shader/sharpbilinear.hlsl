// Sharp-bilinear upscale for DisplayManager (ps_2_0).
// c0 = (texW, texH, sharpness, 0). sharpness=1 -> plain bilinear; larger -> progressively toward point.
// Canonical "sharp bilinear" (anti-aliased nearest): keeps texel interiors flat and only interpolates
// across a narrowing band at texel edges, so it sits between point and full bilinear.
sampler2D s0 : register(s0);
float4    c0 : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float2 texSize = c0.xy;
    float  sharp   = max(c0.z, 1.0);
    float2 texel   = uv * texSize;
    float2 tfloor  = floor(texel);
    float2 s       = frac(texel);
    float2 region  = 0.5 - 0.5 / sharp;          // width of the flat (nearest) zone
    float2 cd      = s - 0.5;
    float2 f       = (cd - clamp(cd, -region, region)) * sharp + 0.5;
    float2 uv2     = (tfloor + f) / texSize;
    return tex2D(s0, uv2);
}
