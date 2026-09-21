// The Windows side of the generated artwork: turn a path into a seed, and a
// plan into a PixelBuf. Everything that decides what the picture looks like is
// in core/coverplan.cpp, shared with the Wayland build.
#include "pg.h"

// The seed is taken from the UTF-8 of the lower-cased file name, because that
// is the one spelling of a name both builds can agree on.
static uint64_t seedOf(const wstring& path) {
    wstring name = lowerOf(fileNameOf(path));
    if (name.empty()) return 1;
    int n = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return 1;
    std::string u8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), &u8[0], n, nullptr, nullptr);
    return pv::coverSeedBytes(u8.data(), u8.size());
}

void coverPlan(const wstring& path, CoverPlan& out) {
    pv::coverPlanFromSeed(seedOf(path), out);
}

void coverRaster(const wstring& path, int size, PixelBuf& out) {
    size = clampi(size, 32, 1024);
    CoverPlan plan;
    coverPlan(path, plan);
    out.w = out.h = size;
    out.px.assign((size_t)size * size * 4, 0);
    pv::coverRasterBGRA(plan, size, out.px.data(), size * 4);
}
