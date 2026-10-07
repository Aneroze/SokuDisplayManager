// Sharp-bilinear for the game's scaled sprites (SpriteSharpness / BackgroundSharpness, ps_2_0). Build:
//   fxc /T ps_2_0 /E main /O3 /Vn g_sharpSpritePS /Fh src/sharpsprite.h shader/sharpsprite.hlsl
// Like DisplayManager's sharpbilinear.hlsl (texel interiors flat, a narrow blend band at texel edges), but per sprite
// draw and alpha-correct:
// c0 = (texW, texH, scaleU, scaleV): screen pixels per texel along u / v for this draw (from its vertices).
// c1.x = k: sharpness relative to that scale. 1 = the band is one screen pixel wide; higher = crisper.
// c1.zw = strength along u / v: 1 = the sharp-bilinear weights, 0 = the nearest texel (exactly the game's POINT),
// in between = a mix of the two results (the blend weights are mixed per axis; bilinear is linear in them).
// The sampler stays POINT; the shader fetches the 4 neighbouring texel centres itself and blends them premultiplied
// by alpha, so the transparent texels around a sprite (whatever their colour) don't darken its edges. Output is
// multiplied by the vertex colour, like the game's fixed-function stage 0 (MODULATE texture x diffuse).
sampler2D s0 : register(s0);
float4    c0 : register(c0);
float4    c1 : register(c1);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0 {
    float2 texSize = c0.xy;
    float2 sharp   = max(c0.zw * c1.x, 1.0);
    float2 texel   = uv * texSize;
    float2 s       = frac(texel);
    float2 region  = 0.5 - 0.5 / sharp;                    // half-width of the flat zone inside a texel
    float2 cd      = s - 0.5;
    float2 f       = (cd - clamp(cd, -region, region)) * sharp + 0.5;
    float2 p       = floor(texel) + f - 0.5;               // bilinear position between texel centres
    float2 i0      = floor(p);
    float2 w       = p - i0;
    w = lerp(step(0.5, w), w, c1.zw);                       // step(0.5, w): the texel the POINT sampler would pick
    float2 inv     = 1.0 / texSize;
    float4 a = tex2D(s0, (i0 + float2(0.5, 0.5)) * inv);
    float4 b = tex2D(s0, (i0 + float2(1.5, 0.5)) * inv);
    float4 c = tex2D(s0, (i0 + float2(0.5, 1.5)) * inv);
    float4 d = tex2D(s0, (i0 + float2(1.5, 1.5)) * inv);
    a.rgb *= a.a; b.rgb *= b.a; c.rgb *= c.a; d.rgb *= d.a;
    float4 m = lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
    m.rgb /= max(m.a, 1.0 / 512.0);
    return m * col;
}
