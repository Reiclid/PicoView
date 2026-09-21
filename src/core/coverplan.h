// Generated artwork for a track that has none - the part that is just maths.
//
// This header knows nothing about Windows, Direct2D or Wayland. Everything in
// it is arithmetic over a seed, which is what makes the same song come back
// the same picture on every machine and in every build of the program. The
// platform layers do the two things that are not arithmetic: work out a seed
// from a file name, and put the pixels on a screen.
#pragma once

#include <cstdint>
#include <vector>

namespace pv {

struct CoverLobe {
    float x = .5f, y = .5f;        // centre, as a fraction of the square
    float r = .4f;                 // radius, likewise
    float dx = 0, dy = 0;          // how far it drifts
    float speed = 1.f, phase = 0;
    float weight = 1.f;            // how much of the beat it takes
    float aspect = 1.f;            // >1 stretches the lobe into a petal
    float lean = 0;                // which way the petal points, radians
    float alpha = 1.f;
    float z = 0;                   // -1..1, only the three-dimensional version
    float cr = 1, cg = 1, cb = 1;
    // What this part of the object is, for the three-dimensional version.
    // A pile of spheres always looks like a pile of spheres.
    int   kind = 0;                // 0 sphere, 1 capsule, 2 ring, 3 rounded box
    float bx = 0, by = 0, bz = 0;  // capsule reach, ring axis, or box extents
    float param = 0.35f;           // secondary radius, as a share of the first
};

struct CoverPlan {
    std::vector<CoverLobe> lobes;
    float rot = 0;                 // how fast the whole cluster turns
    float spread = .18f;           // how far the lobes sit from the centre
    float hue = 0;                 // where the family of hues starts
    float twist = 0;               // how much the whole body is wrung out
    float mirror = 0;              // >0 folds it symmetrical about that plane
};

// FNV-1a over the bytes of a name, lower cased. Both platforms hash the file
// name the same way so a track looks the same on either.
uint64_t coverSeed(const char* utf8Name);
uint64_t coverSeedBytes(const void* bytes, size_t n);

void  coverPlanFromSeed(uint64_t seed, CoverPlan& out);
void  coverHsl(float h, float sat, float l, float& r, float& g, float& b);
float coverFalloff(float t);                  // 1 at the middle, 0 at the rim

// The resting pose, rasterised into premultiplied BGRA. `stride` is bytes per
// row; the caller owns the memory and has already made it big enough.
void  coverRasterBGRA(const CoverPlan& plan, int size, uint8_t* out, int stride);

}  // namespace pv
