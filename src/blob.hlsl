// The generated cover, as a real object.
//
// A handful of spheres melted together with a smooth minimum, ray marched in
// one pass over a full-screen triangle. The material is the point: frosted
// translucent plastic - the light goes through it, the thin parts glow, the
// edges catch a rim, and a little noise on the surface keeps it from looking
// like polished glass.
//
// Compiled by build.bat with fxc into a header, so the exe stays one file.

cbuffer Scene : register(b0) {
    float4 uTime;        // x time, y pulse (the low end), z level, w unused
    float4 uParams;      // x ball count, y blend radius, z seed, w rotation
    float4 uLight;       // w intensity
    float4 uLampCol[3];  // the three lamps, rgb
    float4 uBg0;         // background, top
    float4 uBg1;         // background, bottom
    float4 uBall[8];     // xyz centre, w radius
    float4 uTint[8];     // rgb colour
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);       // one triangle covering the square
    o.uv = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// ------------------------------------------------------------------ noise
float hash13(float3 p) {
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise3(float3 x) {
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(hash13(i + float3(0, 0, 0)), hash13(i + float3(1, 0, 0)), f.x),
                     lerp(hash13(i + float3(0, 1, 0)), hash13(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(hash13(i + float3(0, 0, 1)), hash13(i + float3(1, 0, 1)), f.x),
                     lerp(hash13(i + float3(0, 1, 1)), hash13(i + float3(1, 1, 1)), f.x), f.y), f.z);
}

// ------------------------------------------------------------------ shape
float smin(float a, float b, float k) {
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}

float map(float3 p) {
    float d = 1e9;
    int n = (int)uParams.x;
    [loop] for (int i = 0; i < n; ++i)
        d = smin(d, length(p - uBall[i].xyz) - uBall[i].w, uParams.y);
    // Frost: a shallow ripple on the surface. Small enough not to break the
    // distance field, which is why the march below still converges.
    d += 0.014 * (noise3(p * 7.5 + uTime.x * 0.2) - 0.5);
    return d;
}

// Which lobe a point belongs to, softened the same way the shape is, so the
// colour flows across the joins instead of switching at them.
float3 tintAt(float3 p) {
    float3 acc = 0;
    float wsum = 1e-5;
    int n = (int)uParams.x;
    [loop] for (int i = 0; i < n; ++i) {
        float d = length(p - uBall[i].xyz) - uBall[i].w;
        float w = exp(-max(d, 0.0) * 4.5);
        acc += uTint[i].rgb * w;
        wsum += w;
    }
    return acc / wsum;
}

float3 normalAt(float3 p) {
    float2 e = float2(0.0022, 0);
    return normalize(float3(map(p + e.xyy) - map(p - e.xyy),
                            map(p + e.yxy) - map(p - e.yxy),
                            map(p + e.yyx) - map(p - e.yyx)));
}

float3x3 rotY(float a) {
    float c = cos(a), s = sin(a);
    return float3x3(c, 0, s,  0, 1, 0,  -s, 0, c);
}
float3x3 rotX(float a) {
    float c = cos(a), s = sin(a);
    return float3x3(1, 0, 0,  0, c, -s,  0, s, c);
}

// ------------------------------------------------------------------ shading
// The backdrop is a function rather than a value because the material looks
// through it: a transparent sheet bends what is behind it, and sampling the
// same gradient at a bent coordinate is all that takes.
float3 bgAt(float2 uv) {
    float2 q = uv - 0.5;
    float3 bg = lerp(uBg0.rgb, uBg1.rgb, saturate(uv.y * 0.9 + 0.05));
    bg *= 1.0 - 0.85 * saturate(length(q) * 1.9 - 0.42);
    return bg;
}

float4 PSMain(VSOut input) : SV_Target {
    float2 uv = input.uv;
    float2 q = uv - 0.5;

    float3 bg = bgAt(uv) + (hash13(float3(uv * 900.0, uParams.z)) - 0.5) * 0.016;

    // ---- camera
    float3 ro = float3(0, 0, -2.75);
    float3 rd = normalize(float3(q * 1.55, 1.4));
    float spin = uTime.x * uParams.w;
    float3x3 rot = mul(rotY(spin), rotX(sin(spin * 0.42) * 0.28));
    ro = mul(rot, ro);
    rd = mul(rot, rd);

    // ---- march
    float t = 0.0;
    float glow = 0.0;
    bool hit = false;
    [loop] for (int i = 0; i < 72; ++i) {
        float3 p = ro + rd * t;
        float d = map(p);
        // What passes close to the surface without touching it becomes the
        // halo: it costs nothing here and is most of the sense of light.
        glow += exp(-d * 9.0) * 0.012;
        if (d < 0.0016) { hit = true; break; }
        t += max(d * 0.85, 0.004);
        if (t > 6.0) break;
    }

    float3 col = bg;
    if (hit) {
        float3 p = ro + rd * t;
        float3 n = normalAt(p);
        // Frosted, not polished. The grain on the normal is most of what
        // separates a rain sheet from a glass marble.
        n = normalize(n + 0.16 * (float3(noise3(p * 15.0 + 3.1),
                                         noise3(p * 15.0 + 7.7),
                                         noise3(p * 15.0 + 11.3)) - 0.5));

        float3 tint = tintAt(p);
        // The material is nearly colourless, like a rain sheet. What gives it
        // colour is the light, which is where the track's palette went.
        float3 albedo = lerp(float3(0.90, 0.92, 0.95), tint, 0.20);

        // How much material is behind this point, sampled a few steps in.
        float thick = 0.0;
        [unroll] for (int s = 1; s <= 5; ++s)
            thick += saturate(-map(p + rd * (0.13 * s)) * 2.2);
        thick *= 0.2;

        float fres = pow(1.0 - saturate(dot(n, -rd)), 2.6);

        // Three coloured lamps around it, turning with the object.
        float sp2 = uTime.x * uParams.w * 0.6;
        float3 dirs[3] = {
            normalize(float3(-0.62, 0.70, -0.40 + 0.25 * sin(sp2))),
            normalize(float3(0.78 + 0.2 * cos(sp2), 0.22, -0.55)),
            normalize(float3(0.05, -0.80, -0.55 - 0.2 * sin(sp2 * 0.7)))
        };
        float3 cols[3] = { uLampCol[0].rgb, uLampCol[1].rgb, uLampCol[2].rgb };

        float3 lit = 0;
        float3 gloss = 0;
        [unroll] for (int k = 0; k < 3; ++k) {
            float3 L = dirs[k];
            float wrap = saturate(dot(n, L) * 0.5 + 0.5);              // soft plastic
            float3 h = normalize(L - rd);
            float sp = pow(saturate(dot(n, h)), 16.0) * (0.25 + 0.75 * noise3(p * 20.0));
            // Light that came through from behind: the thin parts glow, which
            // is the whole look of a sheet of translucent plastic.
            float back = pow(saturate(dot(-n, L) * 0.5 + 0.5), 2.0) * exp(-thick * 2.4);
            lit += cols[k] * (wrap * 0.42 + back * 0.55);
            gloss += cols[k] * sp;
        }
        lit *= uLight.w;

        // Look through it: the sheet bends the backdrop, and the further the
        // surface leans away the more it bends.
        float2 duv = uv + n.xy * (0.05 + 0.07 * thick) + (n.xy * 0.02) * fres;
        float3 behind = bgAt(saturate(duv));

        // Milky where there is material to cross, clear where there is not.
        float milk = saturate(0.14 + 0.62 * thick);
        float3 body = behind * (1.0 - milk * 0.82) + albedo * lit * milk;
        body += albedo * lit * fres * 0.85;                    // the lit edge
        body += gloss * 0.7;                                   // a dull highlight
        // Frost is uneven, and that unevenness is what the eye reads as plastic.
        body *= 0.82 + 0.36 * noise3(p * 34.0 + uTime.x * 0.1);

        col = body;
    }

    // The halo takes the colour of the lamps, so the light looks like it is
    // coming off the object rather than being painted behind it.
    float3 lampAvg = (uLampCol[0].rgb + uLampCol[1].rgb + uLampCol[2].rgb) / 3.0;
    col += glow * lampAvg * 1.35;
    // A last touch of grain over everything, which is what sells the frost.
    col += (hash13(float3(uv * 1400.0, uParams.z + 2.0)) - 0.5) * 0.030;
    return float4(saturate(col), 1.0);
}
