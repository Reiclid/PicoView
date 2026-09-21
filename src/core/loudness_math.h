// How loud something is, per ITU-R BS.1770 - the part that is just maths.
//
// Nothing here knows how the samples were obtained. Media Foundation hands
// them over on Windows and ffmpeg will on Linux, and both feed the same
// filters and the same gate, so the two builds cannot disagree about how loud
// a file is. That matters: the whole point of the measurement is that it means
// the same thing everywhere.
#pragma once

#include <vector>

namespace pv {

// One section of the K-weighting chain. Direct form I, doubles throughout:
// single precision drifts audibly over a few million samples of a high-pass
// this narrow.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void reset() { x1 = x2 = y1 = y2 = 0; }
    double run(double x) {
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// The standard gives these as analogue prototypes and tabulates the result for
// 48 kHz only. These are the bilinear transforms, so they are right at any
// sample rate - and at 48 kHz they reproduce the published coefficients to
// eleven decimal places.
Biquad kWeightShelf(double sampleRate);      // the head's own resonance
Biquad kWeightHighpass(double sampleRate);   // what the ear barely registers

// Mean of the block energies with the standard's two gates applied: silence
// goes first, then anything more than 10 dB below what is left. A film's
// dialogue sets the level; its quiet passages do not drag it down.
float gatedLoudnessLufs(const std::vector<double>& blockEnergies);

}  // namespace pv
