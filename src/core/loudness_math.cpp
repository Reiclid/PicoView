#include "loudness_math.h"

#include <algorithm>
#include <cmath>

namespace pv {

Biquad kWeightShelf(double fs) {
    const double f0 = 1681.974450955533;
    const double G = 3.999843853973347;      // dB
    const double Q = 0.7071752369554196;
    double K = tan(3.14159265358979 * f0 / fs);
    double Vh = pow(10.0, G / 20.0);
    double Vb = pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    Biquad q;
    q.b0 = (Vh + Vb * K / Q + K * K) / a0;
    q.b1 = 2.0 * (K * K - Vh) / a0;
    q.b2 = (Vh - Vb * K / Q + K * K) / a0;
    q.a1 = 2.0 * (K * K - 1.0) / a0;
    q.a2 = (1.0 - K / Q + K * K) / a0;
    return q;
}

Biquad kWeightHighpass(double fs) {
    const double f0 = 38.13547087602444;
    const double Q = 0.5003270373238773;
    double K = tan(3.14159265358979 * f0 / fs);
    double a0 = 1.0 + K / Q + K * K;
    Biquad q;
    q.b0 = 1.0;
    q.b1 = -2.0;
    q.b2 = 1.0;
    q.a1 = 2.0 * (K * K - 1.0) / a0;
    q.a2 = (1.0 - K / Q + K * K) / a0;
    return q;
}

float gatedLoudnessLufs(const std::vector<double>& z) {
    if (z.empty()) return -70.f;
    auto loud = [](double e) { return -0.691 + 10.0 * log10(std::max(1e-12, e)); };

    double sum = 0;
    int    n = 0;
    for (double e : z)
        if (loud(e) > -70.0) { sum += e; ++n; }
    if (!n) return -70.f;

    double rel = loud(sum / n) - 10.0;
    double sum2 = 0;
    int    n2 = 0;
    for (double e : z)
        if (loud(e) > -70.0 && loud(e) > rel) { sum2 += e; ++n2; }
    if (!n2) return (float)loud(sum / n);
    return (float)loud(sum2 / n2);
}

}  // namespace pv
