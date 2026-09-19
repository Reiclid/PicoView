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

// ------------------------------------------------------------------ shapes
float smin(float a, float b, float k) {
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}

float sdCapsule(float3 p, float3 a, float3 b, float r) {
    float3 pa = p - a, ba = b - a;
    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-4));
    return length(pa - ba * h) - r;
}

float sdRing(float3 p, float3 c, float3 axis, float major, float minor) {
    float3 q = p - c;
    float y = dot(q, axis);
    float x = length(q - axis * y);
    return length(float2(x - major, y)) - minor;
}

float sdRoundBox(float3 p, float3 c, float3 b, float r) {
    float3 q = abs(p - c) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;
}

// Wringing and folding the space is what makes two objects built from the
// same parts read as different things.
float3 deform(float3 p) {
    if (uForm.y > 0.001) p.x = abs(p.x) - uForm.y;
    float a = p.y * uForm.x;
    float c = cos(a), s = sin(a);
    return float3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
}

float map(float3 pw) {
    float3 p = deform(pw);
    float d = 1e9;
    int n = (int)uParams.x;
    [loop] for (int i = 0; i < n; ++i) {
        float3 A = uShapeA[i].xyz;
        float  r = uShapeA[i].w;
        float3 B = uShapeB[i].xyz;
        float  kp = uShapeB[i].w;
        int    kind = (int)floor(kp);
        float  minor = max(0.04, frac(kp)) * r;

        float di;
        if (kind == 1)      di = sdCapsule(p, A, A + B, r * 0.62);
        else if (kind == 2) di = sdRing(p, A, normalize(B + float3(0, 0, 1e-3)), r, minor);
        else if (kind == 3) di = sdRoundBox(p, A, abs(B) * r * 0.85, minor);
        else                di = length(p - A) - r;

        d = smin(d, di, uParams.y);
    }
    // Frost: a shallow ripple on the surface. Small enough not to break the
    // distance field, which is why the march below still converges.
    d += uMat.y * (noise3(p * 7.5 + uTime.x * 0.2) - 0.5);
    return d;
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
// A plain white studio: a bright side, a dark floor, three hard lamps.
// Nothing here is coloured, deliberately - every colour in the picture is the
// object splitting this light.
float3 env(float3 d) {
    float up = saturate(d.y * 0.5 + 0.5);
    float3 c = lerp(float3(0.16, 0.17, 0.20), float3(0.98, 0.99, 1.04), up * up);
    c += pow(saturate(dot(d, normalize(float3(-0.52, 0.70, -0.45)))), 60.0) * 9.0;
    c += pow(saturate(dot(d, normalize(float3(0.76, 0.22, -0.52)))), 80.0) * 7.0;
    c += pow(saturate(dot(d, normalize(float3(0.10, -0.72, -0.60)))), 50.0) * 4.0;
    return c;
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
        // Frosted, not polished. The grain on the normal is most of what
        // separates a rain sheet from a glass marble.
        n = normalize(n + 0.17 * (float3(noise3(p * 15.0 + 3.1),
                                         noise3(p * 15.0 + 7.7),
                                         noise3(p * 15.0 + 11.3)) - 0.5));

        // How much material is behind this point, sampled a few steps in.
        float thick = 0.0;
        [unroll] for (int s = 1; s <= 5; ++s)
            thick += saturate(-map(p + rd * (0.13 * s)) * 2.2);
        thick *= 0.2;

        float fres = pow(1.0 - saturate(dot(n, -rd)), 3.0);

        // The three channels bend by different amounts. That difference is
        // the whole palette: white light in, colour out.
        float disp = uMat.x;
        float3 tr;
        tr.r = env(refract(rd, n, 1.0 / (1.45 - disp))).r;
        tr.g = env(refract(rd, n, 1.0 / 1.45)).g;
        tr.b = env(refract(rd, n, 1.0 / (1.45 + disp))).b;

        // A second, rougher sample: frosted plastic scatters what passes it.
        float3 nb = normalize(n + 0.45 * (float3(noise3(p * 6.0 + 21.0),
                                                 noise3(p * 6.0 + 37.0),
                                                 noise3(p * 6.0 + 53.0)) - 0.5));
        tr = lerp(tr, env(refract(rd, nb, 1.0 / 1.45)), 0.30);

        // Thick parts swallow more of the short wavelengths, the way a real
        // block of plastic goes warm through the middle.
        tr *= exp(-thick * uMat.z * float3(0.55, 0.78, 1.0));

        // A grazing edge reflects, but only so far: let it reach pure white
        // and the object grows an outline it should not have.
        float3 refl = env(reflect(rd, n));
        col = lerp(tr, refl, saturate(0.05 + 0.42 * fres));
        col *= 1.18;

        // Thin-film interference: the same white light, split again by the
        // skin of the material. This is where most of the colour comes from
        // on a surface this soft - pure refraction only paints the edges.
        float film = thick * 3.2 + fres * 1.6 + noise3(p * 4.0) * 0.35;
        float3 irid = 0.5 + 0.5 * cos(6.2831 * (film + float3(0.00, 0.33, 0.67)));
        col *= 0.86 + 0.34 * irid;
        col += irid * (0.10 + 0.55 * fres) * (0.4 + 0.6 * uTime.z);
        // Frost is uneven, and that unevenness is what reads as plastic.
        col *= (0.84 + 0.32 * noise3(p * 34.0 + uTime.x * 0.1)) * 1.30;

        // How much of the window behind it this pixel hides.
        alpha = saturate((0.34 + 0.52 * thick + 0.26 * fres) * uMat.w);
    }

    // The halo is light: it adds without hiding anything behind it.
    col = col * alpha + glow * float3(0.86, 0.89, 1.0) * 0.85;
    alpha = saturate(alpha + glow * 0.30);

    col += (hash13(float3(uv * 1400.0, uParams.z + 2.0)) - 0.5) * 0.020 * alpha;
    return float4(max(col, 0.0), alpha);
}
