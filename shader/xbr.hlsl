// xBR-lv2 pixel-art upscaler for DisplayManager. Build: fxc /T ps_2_b /E main /O3 /Vn g_xbrPS2b /Fh src/xbr.h.
// ps_2_b (32 temps, 512 slots; any ps_3_0-class GPU and DXVK accept it) because it needs no vertex shader with
// the pre-transformed quad, unlike ps_3_0; ps_2_0 is too small and ps_2_a runs out of temp registers.
// Algorithm and constants: xBR level 2 by Hyllian (MIT license, see below), ported to D3D9 HLSL, with its corner
// types A-D, the 30/60-degree (level 2) edges, the edge width and a final blend with the plain texel as live knobs.
// It finds edges in a 5x5 neighbourhood of each source texel (on a luma metric), decides which of the four corners
// of the texel an edge crosses at 30/45/60 degrees, and blends the output pixel toward the neighbour across that edge;
// flat areas stay point-sampled. Works at any output scale; the anti-aliasing width follows the scale.
//
// c0 = (texW, texH, 1/texW, 1/texH)
// c1 = (width/scale, strength, slopes, corner)
//      width    edge anti-aliasing band, 1 = the original (scale = output size / source size)
//      strength 0..1: blend between the plain texel (0) and the full xBR result (1)
//      slopes   1 = also smooth 30/60-degree edges (level 2), 0 = 45-degree diagonals only
//      corner   0..3 = corner type A (roundest) .. D (keeps the most corners / small details)
// s0 = the captured 640x480 frame, POINT filtered, CLAMP addressed.
//
// Copyright (C) 2011-2016 Hyllian - sergiogdb@gmail.com
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright
// notice and this permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.

sampler2D s0 : register(s0);
float4    c0 : register(c0);
float4    c1 : register(c1);

#define Y_WEIGHT 48.0
#define EQ_THRESHOLD 15.0
#define LV2_COEFFICIENT 2.0

static const float4 Ao = float4( 1.0, -1.0, -1.0,  1.0);
static const float4 Bo = float4( 1.0,  1.0, -1.0, -1.0);
static const float4 Co = float4( 1.5,  0.5, -0.5,  0.5);
static const float4 Ax = float4( 1.0, -1.0, -1.0,  1.0);
static const float4 Bx = float4( 0.5,  2.0, -0.5, -2.0);
static const float4 Cx = float4( 1.0,  1.0, -0.5,  0.0);
static const float4 Ay = float4( 1.0, -1.0, -1.0,  1.0);
static const float4 By = float4( 2.0,  0.5, -2.0, -0.5);
static const float4 Cy = float4( 2.0,  0.0, -1.0,  0.5);
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722) * Y_WEIGHT;

float4 df(float4 a, float4 b) { return abs(a - b); }
float  cdf(float3 a, float3 b) { float3 d = abs(a - b); return d.r + d.g + d.b; }
float4 eq(float4 a, float4 b) { return (df(a, b) < EQ_THRESHOLD) ? 1.0 : 0.0; }
float4 ne(float4 a, float4 b) { return (a != b) ? 1.0 : 0.0; }
float4 wd(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h) {
	return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0 * df(g, h);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
	float2 texel  = uv * c0.xy;
	float2 base   = floor(texel);
	float2 fp     = texel - base;
	float2 ctr    = (base + 0.5) * c0.zw;      // center of texel E
	float  dx = c0.z, dy = c0.w;

	// Only the center and its 4 neighbours are needed as colors; the rest go straight to luma (keeps ps_2_a's
	// temp registers in budget).
	#define LUM(ox, oy) dot(tex2D(s0, ctr + float2((ox) * dx, (oy) * dy)).rgb, LUMA)
	float3 B = tex2D(s0, ctr + float2(0, -dy)).rgb, D = tex2D(s0, ctr + float2(-dx, 0)).rgb, E = tex2D(s0, ctr).rgb;
	float3 F = tex2D(s0, ctr + float2(dx, 0)).rgb,  H = tex2D(s0, ctr + float2(0, dy)).rgb;

	// The four components are the four 90-degree rotations of the same neighbourhood (x = the bottom-right corner).
	float4 b = float4(dot(B, LUMA), dot(D, LUMA), dot(H, LUMA), dot(F, LUMA));
	float4 c = float4(LUM(1, -1), LUM(-1, -1), LUM(-1, 1), LUM(1, 1));                 // C A G I
	float4 e = dot(E, LUMA).xxxx;
	float4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
	float4 i4 = float4(LUM(2, 1), LUM(1, -2), LUM(-2, -1), LUM(-1, 2));                 // I4 C1 A0 G5
	float4 i5 = float4(LUM(1, 2), LUM(2, -1), LUM(-1, -2), LUM(-2, 1));                 // I5 C4 A1 G0
	float4 h5 = float4(LUM(0, 2), LUM(2, 0), LUM(0, -2), LUM(-2, 0));                   // H5 F4 B1 D0
	float4 f4 = h5.yzwx;

	// Lines below which a corner gets interpolated (45, 30 and 60 degrees).
	float4 fx     = Ao * fp.y + Bo * fp.x;
	float4 fxLeft = Ax * fp.y + Bx * fp.x;
	float4 fxUp   = Ay * fp.y + By * fp.x;

	float4 lv0 = ne(e, f) * ne(e, h);
	// Corner types (extra conditions before a corner is rounded): A none, B/C the usual detail checks, D = B plus
	// keeping 2-texel-wide features.
	float4 c1v = i4.yzwx, g0v = i5.wxyz;       // C1 and G0 in each rotation
	float4 condB = saturate((1 - eq(f, b)) * (1 - eq(h, d)) + eq(e, i) * (1 - eq(f, i4)) * (1 - eq(h, i5)) +
	                        eq(e, g) + eq(e, c));
	float4 condC = saturate((1 - eq(f, b)) * (1 - eq(f, c)) + (1 - eq(h, d)) * (1 - eq(h, g)) +
	                        eq(e, i) * saturate((1 - eq(f, f4)) * (1 - eq(f, i4)) + (1 - eq(h, h5)) * (1 - eq(h, i5))) +
	                        eq(e, g) + eq(e, c));
	float4 condD = condB * saturate(ne(f, f4) * ne(f, i) + ne(h, h5) * ne(h, i) + ne(h, g) + ne(f, c) +
	                                eq(b, c1v) * eq(d, g0v));
	float  corner = c1.w;
	float4 cond = corner < 0.5 ? 1.0 : corner < 1.5 ? condB : corner < 2.5 ? condC : condD;
	float4 lv1 = lv0 * cond;
	float4 lv2Left = ne(e, g) * ne(d, g) * c1.z;
	float4 lv2Up   = ne(e, c) * ne(b, c) * c1.z;

	float  dl = c1.x;
	float4 delta  = dl.xxxx;
	float4 deltaL = float4(0.5 * dl, dl, 0.5 * dl, dl);
	float4 deltaU = deltaL.yxwz;
	float4 fx45i = saturate((fx     + delta  - Co - 0.25) / (2 * delta));
	float4 fx45  = saturate((fx     + delta  - Co)        / (2 * delta));
	float4 fx30  = saturate((fxLeft + deltaL - Cx)        / (2 * deltaL));
	float4 fx60  = saturate((fxUp   + deltaU - Cy)        / (2 * deltaU));

	float4 wd1 = wd(e, c, g, i, h5, f4, h, f);
	float4 wd2 = wd(h, d, i5, f, i4, b, e, i);

	float4 edri    = ((wd1 <= wd2) ? 1.0 : 0.0) * lv0;
	float4 edr     = ((wd1 <  wd2) ? 1.0 : 0.0) * lv1;
	float4 edrLeft = ((LV2_COEFFICIENT * df(f, g) <= df(h, c)) ? 1.0 : 0.0) * lv2Left * edr;
	float4 edrUp   = ((df(f, g) >= LV2_COEFFICIENT * df(h, c)) ? 1.0 : 0.0) * lv2Up * edr;

	fx45 *= edr; fx30 *= edrLeft; fx60 *= edrUp; fx45i *= edri;
	float4 px = (df(e, f) <= df(e, h)) ? 1.0 : 0.0;
	float4 m  = max(max(fx30, fx60), max(fx45, fx45i));

	float3 res1 = lerp(E,    lerp(H, F, px.x), m.x);
	res1        = lerp(res1, lerp(B, D, px.z), m.z);
	float3 res2 = lerp(E,    lerp(F, B, px.y), m.y);
	res2        = lerp(res2, lerp(D, H, px.w), m.w);
	float3 res  = lerp(res1, res2, step(cdf(E, res1), cdf(E, res2)));
	return float4(lerp(E, res, c1.y), 1.0);
}
