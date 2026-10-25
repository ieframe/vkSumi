#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSrc;

struct Censor { vec4 rect; vec4 param; };

layout(set = 0, binding = 1) uniform Params {
    #define KNOB(name, value) float name;
    #include "knobs.def"
    #undef KNOB

    mat3  cvd;
    float cvd_strength;

    Censor region[MAX_CENSOR];

    float srgb_output;
    float compare;
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

// weights split the image into dark, mid and bright bands
vec3 bandWeights(vec3 c)
{
    float l         = clamp(luma(c), 0.0, 1.0);
    float shadow    = pow(1.0 - l, 2.0);
    float highlight = pow(l, 2.0);
    return vec3(shadow, 1.0 - shadow - highlight, highlight);
}

// pixels blend the two nearest hue bands
float hueSaturation(vec3 c)
{
    float bands[6] = float[](K.red_saturation,  K.yellow_saturation, K.green_saturation,
                             K.cyan_saturation, K.blue_saturation,   K.magenta_saturation);
    if (bands == float[6](0.0, 0.0, 0.0, 0.0, 0.0, 0.0)) return 0.0;

    float pos = rgb2hsv(max(c, vec3(0.0))).x * 6.0;
    int   lo  = int(pos) % 6;
    return mix(bands[lo], bands[(lo + 1) % 6], fract(pos));
}

vec3 hueChroma(float hue_deg)
{
    vec3 tint = hsv2rgb(vec3(fract(hue_deg / 360.0), 1.0, 1.0));
    return tint - luma(tint);
}

vec3 daltonize(vec3 c, mat3 simulation, float strength)
{
    vec3 seen  = simulation * c;
    vec3 lost  = c - seen;
    vec3 shift = vec3(0.0, 0.7 * lost.r + lost.g, 0.7 * lost.r + lost.b);
    return c + shift * strength;
}

// amd cas, sharpens less where local contrast is already high
vec3 sharpen(vec3 c, vec2 uv, float sharpness)
{
    vec2 px = 1.0 / vec2(textureSize(uSrc, 0));
    vec3 nw = texture(uSrc, uv + vec2(-1, -1) * px).rgb;
    vec3 n  = texture(uSrc, uv + vec2( 0, -1) * px).rgb;
    vec3 ne = texture(uSrc, uv + vec2( 1, -1) * px).rgb;
    vec3 w  = texture(uSrc, uv + vec2(-1,  0) * px).rgb;
    vec3 e  = texture(uSrc, uv + vec2( 1,  0) * px).rgb;
    vec3 sw = texture(uSrc, uv + vec2(-1,  1) * px).rgb;
    vec3 s  = texture(uSrc, uv + vec2( 0,  1) * px).rgb;
    vec3 se = texture(uSrc, uv + vec2( 1,  1) * px).rgb;

    vec3 mn = min(min(min(n, w), min(c, e)), s);
    vec3 mx = max(max(max(n, w), max(c, e)), s);
    mn += min(mn, min(min(nw, ne), min(sw, se)));
    mx += max(mx, max(max(nw, ne), max(sw, se)));

    vec3 amp  = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));
    vec3 lobe = -amp * clamp(sharpness, 0.0, 1.0) * 0.2;
    return clamp((c + (n + w + e + s) * lobe) / (1.0 + 4.0 * lobe), 0.0, 1.0);
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

vec3 linearToSrgb(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 srgbToLinear(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

float gradientNoise(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// triangular noise of one 8bit step hides the banding of the display quantizing after us
vec3 dither(vec3 c)
{
    float n = gradientNoise(gl_FragCoord.xy) + gradientNoise(gl_FragCoord.xy + vec2(47.0, 17.0)) - 1.0;
    return c + n / 255.0;
}

vec3 grade(vec3 c, vec2 uv)
{
    c *= exp2(K.exposure);

    c *= vec3(1.0) + vec3(K.red_gain, K.green_gain, K.blue_gain);

    c.r += K.temperature * 0.10;
    c.b -= K.temperature * 0.10;
    c.g -= K.tint        * 0.10;
    c.r += K.tint        * 0.05;
    c.b += K.tint        * 0.05;

    c = (c - 0.5) * (1.0 + K.contrast) + 0.5;

    c += K.brightness;

    vec3 band = bandWeights(c);
    c += K.shadows    * band.x;
    c += K.highlights * band.z;
    c *= 1.0 + K.midtones * band.y;

    float gy = luma(c);
    c = mix(vec3(gy), c, 1.0 + K.saturation);

    // already-saturated pixels get less boost
    float maxC = max(c.r, max(c.g, c.b));
    float minC = min(c.r, min(c.g, c.b));
    float sat  = maxC - minC;
    c = mix(vec3(luma(c)), c, 1.0 + K.vibrance * (1.0 - sat));

    c = mix(vec3(luma(c)), c, 1.0 + hueSaturation(c));

    if (abs(K.hue_deg) > 0.001)
    {
        vec3 hsv = rgb2hsv(max(c, vec3(0.0)));
        hsv.x = fract(hsv.x + K.hue_deg / 360.0);
        c = hsv2rgb(hsv);
    }

    if (abs(K.gamma) > 0.001)
        c = pow(max(c, vec3(0.0)), vec3(1.0 / exp2(K.gamma)));

    vec3 tone = bandWeights(c);
    c += hueChroma(K.shadow_tint_hue)    * tone.x * K.shadow_tint;
    c += hueChroma(K.highlight_tint_hue) * tone.z * K.highlight_tint;

    if (K.cvd_strength > 0.0)
        c = daltonize(c, K.cvd, K.cvd_strength);

    float corner = length(uv - 0.5) * 1.41421356;
    c *= 1.0 - K.vignette * smoothstep(0.3, 1.0, corner);

    return c;
}

vec3 look(vec3 c, vec2 uv, bool original)
{
    return original ? c : grade(c, uv);
}

void main()
{
    bool original = K.compare > 0.5 && vUV.x < 0.5;
    vec3 c = texture(uSrc, vUV).rgb;
    if (K.sharpness > 0.0 && !original) c = sharpen(c, vUV, K.sharpness);
    c = look(c, vUV, original);

    for (int i = 0; i < MAX_CENSOR; ++i)
    {
        vec4 rect = K.region[i].rect;
        if (rect.z <= 0.0) continue;
        vec4 prm = K.region[i].param;
        float m = censorMask(vUV, rect, prm.y, prm.z);
        if (m > 0.0) c = mix(c, look(censorColor(vUV, prm.w, prm.x), vUV, original), m);
    }

    float split = floor(float(textureSize(uSrc, 0).x) * 0.5);
    if (K.compare > 0.5 && floor(gl_FragCoord.x) == split) c = 1.0 - clamp(c, 0.0, 1.0);

    c = K.srgb_output > 0.5 ? srgbToLinear(dither(linearToSrgb(c))) : dither(c);

    oColor = vec4(c, 1.0);
}
