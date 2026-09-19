// The generated cover, as a real object.
//
// Shapes melted together with a smooth minimum and ray marched in one pass
// over a full-screen triangle. Two things carry the look:
//
//   The light is white. Every colour you see is the object splitting it -
//   the three channels refract at slightly different angles, which is what a
//   prism does and what a thick edge of frosted plastic does.
//
//   Nothing is painted behind it. The shader writes premultiplied alpha, so
//   the object is genuinely translucent against whatever the window shows.
//
// Compiled by build.bat with fxc into a header, so the exe stays one file.

cbuffer Scene : register(b0) {
    float4 uTime;        // x time, y pulse (the low end), z level, w unused
    float4 uParams;      // x shape count, y blend radius, z seed, w rotation
    float4 uForm;        // x twist, y mirror fold, z step scale, w unused
    float4 uMat;         // x dispersion, y frost, z absorption, w opacity
    float4 uGlass;       // rgb what the material lets through, w unused
    float4 uShapeA[8];   // xyz centre, w radius
    float4 uShapeB[8];   // xyz reach / axis / extents, w kind + secondary radius
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
// One body, not a pile of parts. A sphere is the whole object; the control
// points only pull its surface out or push it in, each from its own place and
// at its own speed. When they gather it is round, when they spread it grows
// lobes - but it is always one closed surface, which is what a pile of
// primitives melted together never quite looks like.

// Wringing and folding the space is what makes two objects built the same way
// read as different things.
float3 deform(float3 p) {
    if (uForm.y > 0.001) p.x = abs(p.x) - uForm.y;
    float a = p.y * uForm.x;
    float c = cos(a), s = sin(a);
    return float3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
}

float map(float3 pw) {
    float3 p = deform(pw);
    float d = length(p) - uParams.y;                 // the body itself
    int n = (int)uParams.x;
    [loop] for (int i = 0; i < n; ++i) {
        float3 c = uShapeA[i].xyz;
        float  amp = uShapeA[i].w;                   // negative points dent it
        float  wid = max(0.12, uShapeB[i].w);
        float  q = length(p - c) / wid;
        d -= amp * exp(-q * q);
    }
    // Everything above is a field, not a distance, so the march is told to
    // trust it only part way.
    return d * 0.62;
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

// ------------------------------------------------------------------ light
// A white studio with small, dim lamps. Small, because something has to have
// an edge before blurring it means anything; dim, because the glare was the
// first thing wrong with this.
float3 env(float3 d) {
    // The lamps drift on their own slow paths, so the light crossing the
    // object keeps moving even when the object itself is still.
    float t = uTime.x * 0.23;
    float3 l0 = normalize(float3(-0.45 + 0.42 * sin(t),        0.72, -0.50 + 0.30 * cos(t * 0.8)));
    float3 l1 = normalize(float3(0.76,  0.18 + 0.45 * sin(t * 0.7 + 2.1), -0.38 + 0.35 * cos(t * 1.1)));
    float3 l2 = normalize(float3(0.10 + 0.40 * cos(t * 0.9), -0.55 + 0.25 * sin(t * 0.6),  0.80));

    float up = d.y * 0.5 + 0.5;
    float3 c = lerp(float3(0.05, 0.055, 0.07), float3(0.40, 0.42, 0.48), up * up);
    c += pow(saturate(dot(d, l0)), 30.0) * 2.60;
    c += pow(saturate(dot(d, l1)), 44.0) * 1.70;
    c += pow(saturate(dot(d, l2)), 36.0) * 1.10;
    return c;
}

// Frosted glass does not bend light one way, it bends it every way at once.
// Taking one sample and jittering the normal only makes the surface look
// dirty; taking several and averaging them is the blur itself, and the three
// channels take slightly different paths through it.
float3 blurThrough(float3 rd, float3 n, float disp, float rough, float3 seed) {
    float3 acc = 0;
    [unroll] for (int k = 0; k < 6; ++k) {
        float3 j = float3(noise3(seed + k * 13.1),
                          noise3(seed + k * 7.7 + 31.0),
                          noise3(seed + k * 5.3 + 67.0)) - 0.5;
        float3 nn = normalize(n + rough * j);
        acc.r += env(refract(rd, nn, 1.0 / (1.45 - disp))).r;
        acc.g += env(refract(rd, nn, 1.0 / 1.45)).g;
        acc.b += env(refract(rd, nn, 1.0 / (1.45 + disp))).b;
    }
    return acc / 6.0;
}

float4 PSMain(VSOut input) : SV_Target {
    float2 uv = input.uv;
    float2 q = uv - 0.5;

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
    [loop] for (int i = 0; i < 80; ++i) {
        float3 p = ro + rd * t;
        float d = map(p);
        // What passes close without touching becomes the halo.
        glow += exp(-d * 11.0) * 0.009;
        if (d < 0.0016) { hit = true; break; }
        t += max(d * uForm.z, 0.004);
        if (t > 6.0) break;
    }

    float3 col = 0;
    float  alpha = 0;

    if (hit) {
        float3 p = ro + rd * t;
        float3 n = normalAt(p);
        // Barely roughened. Grain was making it look like smoke; what these
        // objects actually are is smooth, with everything inside them soft
        // because the material scatters, not because the surface is dirty.
        n = normalize(n + 0.035 * (float3(noise3(p * 9.0 + 3.1),
                                          noise3(p * 9.0 + 7.7),
                                          noise3(p * 9.0 + 11.3)) - 0.5));

        // How much material is behind this point, sampled a few steps in.
        float thick = 0.0;
        [unroll] for (int s = 1; s <= 6; ++s)
            thick += saturate(-map(p + rd * (0.22 * s)) * 1.6);
        thick *= 0.1667;

        float fres = pow(1.0 - saturate(dot(n, -rd)), 3.2);

        // What you see through it, genuinely blurred: six paths through the
        // surface, averaged. Thin parts scatter less than thick ones, the way
        // a sheet of frosted plastic goes from hazy to opaque as it thickens.
        float rough = 0.09 + 0.34 * thick;
        float3 tr = blurThrough(rd, n, uMat.x, rough, p * 3.0 + uTime.x * 0.05);

        // The glass has a colour of its own, which is what survives the trip:
        // a thin edge passes nearly everything and stays pale, a thick middle
        // keeps only what the material lets through and arrives saturated.
        float3 glass = lerp(uGlass.rgb, float3(1, 1, 1), 0.16);
        tr *= glass;
        tr *= exp(-thick * uMat.z * 2.6 * (1.0 - uGlass.rgb));
        tr += uGlass.rgb * 0.16 * (1.0 - thick);               // milk, tinted

        // A wide, weak sheen. A frosted surface has no hard highlight on it -
        // that was the glare.
        float tl = uTime.x * 0.23;
        float3 key = normalize(float3(-0.45 + 0.42 * sin(tl), 0.72, -0.50 + 0.30 * cos(tl * 0.8)));
        float3 hv = normalize(key - rd);
        float gloss = pow(saturate(dot(n, hv)), 4.0) * 0.22;

        // The reflection is blurred too, and split, which is what colours the
        // rim without putting a white line around the object.
        float3 rr = reflect(rd, n);
        float3 refl = 0;
        [unroll] for (int k = 0; k < 3; ++k) {
            float3 j = float3(noise3(p * 5.0 + k * 17.0),
                              noise3(p * 5.0 + k * 23.0 + 5.0),
                              noise3(p * 5.0 + k * 29.0 + 9.0)) - 0.5;
            float3 rn = normalize(rr + 0.22 * j);
            refl.r += env(normalize(rn + n * uMat.x * 0.25)).r;
            refl.g += env(rn).g;
            refl.b += env(normalize(rn - n * uMat.x * 0.25)).b;
        }
        refl /= 3.0;

        col = lerp(tr, refl, saturate(0.04 + 0.26 * fres));
        col += gloss;
        col *= 1.22;

        // How much of the window behind it this pixel hides.
        alpha = saturate((0.62 + 0.34 * thick + 0.18 * fres) * uMat.w);
    }

    // The halo is light: it adds without hiding anything behind it.
    col = col * alpha + glow * float3(0.86, 0.89, 1.0) * 0.85;
    alpha = saturate(alpha + glow * 0.30);

    // Just enough grain to stop the gradients banding on an 8-bit target.
    col += (hash13(float3(uv * 1400.0, uParams.z + 2.0)) - 0.5) * 0.006 * alpha;
    return float4(max(col, 0.0), alpha);
}
