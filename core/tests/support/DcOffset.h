// Clipper — test support: DC offset measured ON SIGNAL, not on silence.
//
// WHY THIS EXISTS (2026-07-24 audit finding 16):
//
//   "The DC blind spot is systemic: test_muff_model.cpp:389, test_rat_model.cpp:366,
//    test_ts_model.cpp:297 and test_gold_model.cpp:344 all check DC on SILENT input only."
//
// All four dirt pedals asserted `|mean(out)| < 1e-3` after feeding 0.3 s of ZEROS. That is
// trivially true and it is not the property anyone cares about: these are ASYMMETRIC
// clippers, and an asymmetric clipper produces DC *from signal*. Silence in, silence out
// says only that the model does not self-oscillate.
//
// The property a player observes: a pedal that sits on a DC offset eats the headroom of
// everything downstream (the amp's input stage, the cab convolver, the output limiter),
// clips lopsidedly, and thumps when the note stops as the offset decays. Every sibling
// carries `dcBlockHz = 12.0` for exactly this reason — `SdModel.cpp:66` says so in a
// comment. The Muff has no output high-pass anywhere, which is finding 16.
//
// So: drive a real tone at a real level and measure the mean of the SETTLED tail, relative
// to the output peak. Relative, because absolute volts are meaningless across pedals whose
// output levels differ by 20 dB.
//
// TWO STIMULI, and the second is what gives the assertion teeth.
//
//   1. A CLEAN tone. Catches a model whose asymmetric clipping RECTIFIES — generates DC
//      out of a DC-free input. This is the Muff's defect (finding 16): +0.44 V on a 2.38 V
//      peak, 18.4 %.
//   2. The same tone with a DC OFFSET ON THE INPUT. Verified by measurement: with a clean
//      input, deleting the RAT's / TS's / GOLD's output coupling cap outright changes their
//      measured DC by NOTHING, because a symmetric (or already-blocked) clipper does not
//      rectify — so a clean-input test alone cannot fail if the cap goes missing, which is
//      precisely the regression worth catching. Feed +0.1 V of offset and the cap becomes
//      load-bearing: present, the output DC stays under 0.1 % of peak; bypassed, the offset
//      rides straight through to the amp.
//      It is also the realistic case. A DC offset arriving at a pedal's input is ordinary
//      (a cheap interface, a mis-biased buffer, an upstream pedal with the same defect), and
//      what a player hears when it reaches the amp is lost headroom and lopsided clipping.
//
// Portable C++17, header-only.

#ifndef CLIPPER_TESTS_SUPPORT_DC_OFFSET_H
#define CLIPPER_TESTS_SUPPORT_DC_OFFSET_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace clipper::test {

struct DcOnSignal {
    double mean = 0.0;      // mean of the settled tail (V)
    double peak = 0.0;      // peak |sample| of the settled tail (V)
    double fraction = 0.0;  // |mean| / peak — the headroom actually lost
};

// `out` must be a render of a steady tone. `skipFraction` drops the model's settling
// transient (its DC blocker's own step response) before measuring.
inline DcOnSignal measureDcOnSignal(const std::vector<float>& out, double skipFraction = 0.3) {
    DcOnSignal d;
    if (out.empty()) return d;
    const std::size_t start =
        std::min(out.size() - 1, static_cast<std::size_t>(skipFraction * out.size()));
    double sum = 0.0, pk = 0.0;
    for (std::size_t i = start; i < out.size(); ++i) {
        sum += out[i];
        pk = std::max(pk, std::fabs(static_cast<double>(out[i])));
    }
    d.mean = sum / static_cast<double>(out.size() - start);
    d.peak = pk;
    d.fraction = pk > 0.0 ? std::fabs(d.mean) / pk : 0.0;
    return d;
}

// The bar. A 12 Hz first-order blocker leaves essentially nothing of a 220 Hz tone's
// rectified mean, so a pedal that HAS one measures far under this; 1 % of peak is a
// generous ceiling that still fails hard for a pedal with no blocker at all. Measured with
// the shipped code: RAT 0.000 % clean / 0.091 % offset, TS 0.000 / 0.000,
// GOLD 0.000 / 0.000, Muff 18.4 / 18.4 (finding 16).
constexpr double kDcFractionBar = 0.01;

// The input DC offset used for stimulus 2. 0.1 V against a 0.2 V tone is a realistic bad
// interface, and large enough that a missing coupling cap is unmissable.
constexpr float kInputDcOffset = 0.1f;

}  // namespace clipper::test

#endif  // CLIPPER_TESTS_SUPPORT_DC_OFFSET_H
