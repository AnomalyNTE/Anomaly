// One liquid glass surface: a rounded-rectangle lens over the captured
// backdrop. The quad is the surface inflated by its shadow margin; the pixel
// shader resolves the shape from a signed distance field.
//
// What separates liquid glass from frosted glass is the edge, so the edge
// carries several distinct cues:
//   * the face shows a blurred, saturated copy of what lies beneath;
//   * a shoulder along the edge works like a thick convex lens and samples a
//     sharp copy from further inside, so the background bends toward the rim;
//   * the shoulder is shaded on the side facing away from the light, which
//     gives the pane thickness even over a flat background;
//   * a crisp specular line runs along the rim, brightest on the corners that
//     face the light and on the opposite corners where the light leaves;
//   * a soft glow sits inside the lit corners and a gloss on the upper face.

cbuffer GlassConstants : register(b0) {
    float4 g_screen;  // width, height, 1/width, 1/height
    float4 g_rect;    // min x, min y, max x, max y in pixels
    float4 g_shape;   // radius, scale, margin, alpha
    float4 g_tint;    // rgb, strength
    float4 g_optics;  // refraction, chroma, face blur (0 sharp, 1 light, 2 heavy), saturation
    float4 g_light;   // rim, lift, dark appearance, minimum alpha
    float4 g_shadow;  // alpha, offset y, blur, gloss
    float4 g_extra;   // bevel width, inner glow, magnification, contact line
    float4 g_edge;    // shoulder shade, shoulder blur
};

Texture2D g_sharp : register(t0);
Texture2D g_heavy : register(t1);
Texture2D g_light_blur : register(t2);
SamplerState g_sampler : register(s0);

float4 vs_quad(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2(id & 1, id >> 1);
    float margin = g_shape.z;
    float2 p = lerp(g_rect.xy - margin, g_rect.zw + margin, corner);
    return float4(p.x * g_screen.z * 2.0 - 1.0, 1.0 - p.y * g_screen.w * 2.0, 0.0, 1.0);
}

float rounded_box(float2 p, float2 half_size, float radius) {
    float2 q = abs(p) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float4 sample_blurred(float2 uv, float amount) {
    float4 sharp = g_sharp.SampleLevel(g_sampler, uv, 0.0);
    if (amount <= 0.0) return sharp;
    float4 light = g_light_blur.SampleLevel(g_sampler, uv, 0.0);
    if (amount <= 1.0) return lerp(sharp, light, amount);
    return lerp(light, g_heavy.SampleLevel(g_sampler, uv, 0.0), saturate(amount - 1.0));
}

float4 ps_glass(float4 position : SV_Position) : SV_Target {
    float2 p = position.xy;
    float2 center = (g_rect.xy + g_rect.zw) * 0.5;
    float2 half_size = max((g_rect.zw - g_rect.xy) * 0.5, 0.5);
    float radius = min(g_shape.x, min(half_size.x, half_size.y));
    float s = g_shape.y;
    bool dark = g_light.z > 0.5;
    float d = rounded_box(p - center, half_size, radius);
    float coverage = saturate(0.5 - d);

    // Outside the edge: a soft shadow and a hairline of contact.
    float shadow_distance =
        max(rounded_box(p - center - float2(0.0, g_shadow.y), half_size, radius), 0.0);
    float shadow = g_shadow.x *
        pow(saturate(1.0 - shadow_distance / max(g_shadow.z, 1.0)), 2.4);
    float contact = g_extra.w * (1.0 - smoothstep(0.0, 1.25 * s, max(d, 0.0)));
    float outside = 1.0 - (1.0 - shadow) * (1.0 - contact);
    if (coverage <= 0.0) return float4(0.0, 0.0, 0.0, outside * g_shape.w);

    float2 e = float2(1.0, 0.0);
    float2 n = float2(
        rounded_box(p - center + e.xy, half_size, radius) -
            rounded_box(p - center - e.xy, half_size, radius),
        rounded_box(p - center + e.yx, half_size, radius) -
            rounded_box(p - center - e.yx, half_size, radius));
    n = n / max(length(n), 1e-4);
    float inside = max(-d, 0.0);
    float bevel_width = min(max(g_extra.x * s, 1.0), min(half_size.x, half_size.y));
    // x runs from 0 on the rim to 1 where the shoulder meets the flat face.
    // The shoulder is a quarter circle, so its slope -- and the bending --
    // climbs steeply toward the rim.
    float x = saturate(inside / bevel_width);
    float bend = 1.0 - sqrt(saturate(1.0 - (1.0 - x) * (1.0 - x)));
    float shoulder = 1.0 - smoothstep(0.0, 1.0, x);

    float2 lensed = p + (center - p) * g_extra.z;
    float2 uv = lensed * g_screen.zw;
    float2 offset = -n * g_optics.x * s * bend * g_screen.zw;
    float split = g_optics.y * bend;

    // The face is blurred as much as the variant asks, and the shoulder of the
    // lens samples a sharper copy, which is what lets the bending be seen. The
    // two have to meet somewhere: a shoulder far sharper than a deeply frosted
    // face reads as a second edge inside the pane -- a glass ring around a
    // separate, blurred picture -- so the sharpest level the shoulder may use is
    // g_edge.y, which the host sets per surface.
    float face_blur = g_optics.z;
    float blur = lerp(face_blur, min(face_blur, g_edge.y), shoulder);
    float4 color = sample_blurred(uv + offset, blur);
    color.r = sample_blurred(uv + offset * (1.0 + split), blur).r;
    color.b = sample_blurred(uv + offset * (1.0 - split), blur).b;

    // The backdrop is premultiplied, so the body stays premultiplied until the
    // lighting, which is applied to the straight colour.
    float luminance = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    color.rgb = max(lerp(luminance.xxx, color.rgb, g_optics.w), 0.0);
    float t = g_tint.a;
    color.rgb = color.rgb * (1.0 - t) + g_tint.rgb * t;
    color.a = color.a * (1.0 - t) + t;
    color.rgb = lerp(color.rgb, (dark ? 0.0 : 1.0) * color.a, g_light.y);
    if (color.a < g_light.w) {
        color.rgb += g_tint.rgb * (g_light.w - color.a);
        color.a = g_light.w;
    }
    float3 straight = color.rgb / max(color.a, 1e-4);

    // Light arrives from the upper left.
    float2 light_direction = normalize(float2(-0.62, -0.78));
    float facing = dot(n, light_direction);
    float toward = saturate(facing);
    float away = saturate(-facing);

    // Thickness: the shoulder darkens, mostly where it turns away from the
    // light, the way a real slab of glass shades its far edge.
    float shade = g_edge.x * shoulder * shoulder * (0.35 + 0.65 * away);
    straight *= 1.0 - shade;

    // A crisp rim line, faint along straight edges and bright on the corners
    // that face toward or directly away from the light.
    float rim_line = 1.0 - smoothstep(0.15 * s, 1.15 * s, inside);
    float corner_light = pow(abs(facing), 3.0);
    float specular = rim_line * (0.22 + 0.78 * corner_light) * g_light.x;
    // A softer glow just inside the lit corner, and a gloss on the upper face.
    float glow = pow(saturate(shoulder), 2.5) * toward * toward * g_extra.y;
    float upper = saturate(1.0 - (p.y - g_rect.y) / max(half_size.y * 0.9, 1.0));
    float gloss = g_shadow.w * upper * upper * upper;
    float highlight = saturate(specular + glow + gloss) * (dark ? 0.75 : 1.0);
    straight = lerp(straight, 1.0, highlight);

    float alpha = color.a * coverage * g_shape.w;
    float rest = outside * (1.0 - coverage) * g_shape.w;
    float out_alpha = alpha + rest * (1.0 - alpha);
    return float4(straight * alpha / max(out_alpha, 1e-4), out_alpha);
}
