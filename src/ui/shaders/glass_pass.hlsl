// Backdrop preparation for liquid glass: a 2x2 box downsample and a
// separable 9-tap Gaussian, both drawn as one full-screen triangle.

cbuffer PassConstants : register(b0) {
    float4 g_texel;  // source 1/width, 1/height, direction x, direction y
    float4 g_pass;   // x: tap spread in texels
};

Texture2D g_source : register(t0);
SamplerState g_sampler : register(s0);

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Varyings vs_full(uint id : SV_VertexID) {
    Varyings output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 ps_down(Varyings input) : SV_Target {
    float2 t = g_texel.xy;
    float4 color = g_source.SampleLevel(g_sampler, input.uv + float2(-t.x, -t.y), 0.0);
    color += g_source.SampleLevel(g_sampler, input.uv + float2(t.x, -t.y), 0.0);
    color += g_source.SampleLevel(g_sampler, input.uv + float2(-t.x, t.y), 0.0);
    color += g_source.SampleLevel(g_sampler, input.uv + float2(t.x, t.y), 0.0);
    return color * 0.25;
}

float4 ps_blur(Varyings input) : SV_Target {
    float2 step = g_texel.xy * g_texel.zw * g_pass.x;
    float4 color = g_source.SampleLevel(g_sampler, input.uv, 0.0) * 0.2270270270;
    color += (g_source.SampleLevel(g_sampler, input.uv + step * 1.3846153846, 0.0) +
              g_source.SampleLevel(g_sampler, input.uv - step * 1.3846153846, 0.0)) * 0.3162162162;
    color += (g_source.SampleLevel(g_sampler, input.uv + step * 3.2307692308, 0.0) +
              g_source.SampleLevel(g_sampler, input.uv - step * 3.2307692308, 0.0)) * 0.0702702703;
    return color;
}
