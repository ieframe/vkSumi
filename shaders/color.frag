#version 450

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSrc;

struct Censor { vec4 rect; vec4 param; };

layout(set = 0, binding = 1) uniform Params {
    float brightness;
    float contrast;
    float exposure;
    float saturation;

    float vibrance;
    float hue_deg;
    float gamma;
    float temperature;

    float tint;
    float red_gain;
    float green_gain;
    float blue_gain;

    float shadows;
    float midtones;
    float highlights;
    float vignette;

    mat3  cvd;
    float cvd_strength;

    Censor region[MAX_CENSOR];
} K;

vec3 rgb2hsv(vec3 c)
{
    vec4 k = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, k.wz), vec4(c.gb, k.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c)
{
    vec4 k = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + k.xyz) * 6.0 - k.www);
    return c.z * mix(k.xxx, clamp(p - k.xxx, 0.0, 1.0), c.y);
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 daltonize(vec3 c, mat3 simulation, float strength)
{
    vec3 seen  = simulation * c;
    vec3 lost  = c - seen;
    vec3 shift = vec3(0.0, 0.7 * lost.r + lost.g, 0.7 * lost.r + lost.b);
    return c + shift * strength;
}

float censorMask(vec2 uv, vec4 rect, float feather, float shape)
{
    vec2  d = abs(uv - rect.xy);
    vec2  r = max(rect.zw, vec2(1e-5));
    float f = max(feather, 0.001);
    if (shape > 0.5)
        return 1.0 - smoothstep(1.0, 1.0 + f, length(d / r));
    vec2 t = vec2(1.0) - smoothstep(r, r * (1.0 + f), d);
    return min(t.x, t.y);
}

vec3 censorColor(vec2 uv, float mode, float strength)
{
    if (mode > 1.5) return vec3(0.0);
    vec2 res = vec2(textureSize(uSrc, 0));
    if (mode > 0.5)
    {
        vec2 stp = (max(strength, 1.0) * 0.25) / res;
        vec3 sum = vec3(0.0);
        for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            sum += texture(uSrc, uv + vec2(x, y) * stp).rgb;
        return sum / 25.0;
    }
    float g = max(strength, 1.0);
    return texture(uSrc, (floor(uv * res / g) + 0.5) * g / res).rgb;
}

void main()
{
    vec3 c = texture(uSrc, vUV).rgb;

    c *= exp2(K.exposure);

    c *= vec3(1.0) + vec3(K.red_gain, K.green_gain, K.blue_gain);

    c.r += K.temperature * 0.10;
    c.b -= K.temperature * 0.10;
    c.g -= K.tint        * 0.10;
    c.r += K.tint        * 0.05;
    c.b += K.tint        * 0.05;

    c = (c - 0.5) * (1.0 + K.contrast) + 0.5;

    c += K.brightness;

    // weights split the image into dark, mid and bright bands
    float l = clamp(luma(c), 0.0, 1.0);
    float shadow_w    = pow(1.0 - l, 2.0);
    float highlight_w = pow(l, 2.0);
    float mid_w       = 1.0 - shadow_w - highlight_w;
    c += K.shadows    * shadow_w;
    c += K.highlights * highlight_w;
    c *= 1.0 + K.midtones * mid_w;

    float gy = luma(c);
    c = mix(vec3(gy), c, 1.0 + K.saturation);

    // already-saturated pixels get less boost
    float maxC = max(c.r, max(c.g, c.b));
    float minC = min(c.r, min(c.g, c.b));
    float sat  = maxC - minC;
    c = mix(vec3(luma(c)), c, 1.0 + K.vibrance * (1.0 - sat));

    if (abs(K.hue_deg) > 0.001)
    {
        vec3 hsv = rgb2hsv(max(c, vec3(0.0)));
        hsv.x = fract(hsv.x + K.hue_deg / 360.0);
        c = hsv2rgb(hsv);
    }

    if (abs(K.gamma) > 0.001)
        c = pow(max(c, vec3(0.0)), vec3(1.0 / exp2(K.gamma)));

    if (K.cvd_strength > 0.0)
        c = daltonize(c, K.cvd, K.cvd_strength);

    float corner = length(vUV - 0.5) * 1.41421356;
    c *= 1.0 - K.vignette * smoothstep(0.3, 1.0, corner);

    for (int i = 0; i < MAX_CENSOR; ++i)
    {
        vec4 rect = K.region[i].rect;
        if (rect.z <= 0.0) continue;
        vec4 prm = K.region[i].param;
        float m = censorMask(vUV, rect, prm.y, prm.z);
        if (m > 0.0) c = mix(c, censorColor(vUV, prm.w, prm.x), m);
    }

    oColor = vec4(c, 1.0);
}
