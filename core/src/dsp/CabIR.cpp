// Clipper — default cab IR generator (M5). See CabIR.h.
//
// ---------------------------------------------------------------------------
// Recipe (a plausible closed-back 2x12, NOT a measured cabinet — a documented
// placeholder until real IR upload lands). Everything is deterministic: a seeded
// linear-congruential generator supplies the diffuse tail, so the IR is identical
// on every call and tests can assert on it.
//
//   Length: 1024 samples (~21 ms @ 48 kHz) — long enough for the low-end body,
//           short enough to stay one partition-friendly and cheap.
//
//   Time structure (before filtering):
//     * direct hit at n = 6 (a small pre-delay),
//     * three early reflections at +21 / +47 / +89 samples with alternating sign
//       and decaying gain (baffle / back-panel bounces of a closed-back box),
//     * a diffuse exponential-decay noise tail (seeded) for body, tau ~ len/4.5.
//
//   Spectral shaping (cascade of RBJ biquads applied to the time structure):
//     * low cut:  2nd-order high-pass @ 80 Hz   (no sub thump from a guitar cab),
//     * speaker rolloff: FOUR cascaded 2nd-order low-passes @ 5 kHz (~ -24 dB/oct
//       above the corner — the steep top-end rolloff that makes a mic'd guitar
//       speaker sound like a guitar speaker and not a tweeter),
//     * presence bump: peaking +4 dB @ 2.5 kHz, Q 1.1 (the upper-mid bite).
//
//   Normalized so the peak SPECTRAL magnitude is 1.0 (M6.6): the cab never
//   boosts any band above its input level; 1 kHz sits ~-3 dB below unity.
//
// Measured magnitude response of the generated IR (raw DTFT of the IR, dB
// relative to the 1 kHz bin; measured @ 48 kHz, see test_amp_model.cpp which
// asserts this shape and runs at 44.1 k and 96 k too):
//     60 Hz   ~ -9 dB      (low cut)
//     100 Hz  ~ -3 dB      (low cut easing in)
//     1 kHz   =  0 dB       (reference / passband)
//     2.5 kHz ~ +3 dB      (presence bump)
//     5 kHz   ~ -10 dB     (into the speaker rolloff)
//     8 kHz   ~ -36 dB     (well down the steep ~48 dB/oct rolloff)
// The load-bearing shape assertions: 1 kHz >> 8 kHz, 5 kHz below 1 kHz, presence
// region not collapsed, low end rolled off relative to 1 kHz.
//
// NB: the frequency response is measured as the RAW DTFT of the IR (sum h[n]
// e^{-jwn}) — that IS |H(f)| exactly. Do NOT use a Hann-windowed / amplitude-
// normalized Goertzel here: that estimator is for sinusoid amplitude and gives
// meaningless low-frequency values for an impulse response.
// ---------------------------------------------------------------------------

#include "clipper/dsp/CabIR.h"

#include "clipper/dsp/Biquad.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace clipper::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Deterministic LCG (Numerical Recipes constants) -> [-1, 1).
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    float next() {
        s = s * 1664525u + 1013904223u;
        // top 24 bits -> [0,1)
        const float u = (s >> 8) * (1.0f / 16777216.0f);
        return 2.0f * u - 1.0f;
    }
};
}  // namespace

std::vector<float> generateDefaultCab2x12IR(double sampleRate) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;

    // Length scales with sample rate so the physical time structure is constant.
    const int len = static_cast<int>(std::lround(1024.0 * fs / 48000.0));
    std::vector<float> h(static_cast<size_t>(len), 0.0f);

    // Scale the integer sample offsets by the rate ratio so early reflections and
    // the decay land at the same physical times at any sample rate.
    const double rr = fs / 48000.0;
    auto at = [&](int n48) { return static_cast<int>(std::lround(n48 * rr)); };

    // Direct hit + two SMALL early reflections. Kept deliberately low in
    // amplitude: large reflections comb-filter the magnitude response into peaks
    // and notches that (a) differ between sample rates and (b) fight the smooth
    // speaker rolloff we want. At these amplitudes they add a subtle box
    // character (< ~1 dB ripple) without disturbing the overall shape.
    h[static_cast<size_t>(at(5))] += 1.0f;
    h[static_cast<size_t>(at(5 + 33))] += -0.10f;
    h[static_cast<size_t>(at(5 + 71))] += 0.06f;

    // Diffuse decaying tail (seeded noise * exponential envelope) — small, just
    // enough to give the response some organic texture; the filter cascade below
    // dominates the shape, keeping the magnitude response smooth and consistent
    // across sample rates so tests can assert on it.
    Lcg rng(0x0C1A5511u);
    const double tau = len / 5.0;
    const int tailStart = at(12);
    for (int n = tailStart; n < len; ++n) {
        const double env = std::exp(-(n - tailStart) / tau);
        h[static_cast<size_t>(n)] += static_cast<float>(0.02 * env) * rng.next();
    }

    // --- Spectral shaping. Apply the biquad cascade forward over the buffer. ---
    Biquad hp;
    hp.setCoeffs(rbj::highPass(95.0, 0.707, fs));
    Biquad lp[4];
    for (auto& b : lp) b.setCoeffs(rbj::lowPass(5000.0, 0.707, fs));
    Biquad presence;
    presence.setCoeffs(rbj::peaking(2500.0, 3.5, 1.0, fs));

    for (int n = 0; n < len; ++n) {
        float x = h[static_cast<size_t>(n)];
        x = hp.process(x);
        for (auto& b : lp) x = b.process(x);
        x = presence.process(x);
        h[static_cast<size_t>(n)] = x;
    }

    // Normalize to UNITY SPECTRAL PEAK (max |H(f)| = 1 over the audio band):
    // a cab may COLOR the tone but must never BOOST any frequency past the
    // level entering it. M5/M6.5 normalized |H(1 kHz)| = 1, which left the
    // +3 dB presence bump ABOVE unity — engaging the cab then pushed presence-
    // band peaks into the output limiter and fizzed the clean path (M6.6 field
    // report: "fizzy only with the cab on"). With peak normalization the 1 kHz
    // reference sits ~-3 dB and the documented relative shape is unchanged.
    auto dtftMag = [&](double f) {
        const double w = 2.0 * kPi * f / fs;
        double re = 0.0, im = 0.0;
        for (int n = 0; n < len; ++n) {
            re += h[static_cast<size_t>(n)] * std::cos(w * n);
            im -= h[static_cast<size_t>(n)] * std::sin(w * n);
        }
        return std::sqrt(re * re + im * im);
    };
    // Log-spaced search grid over the audible band (the response is smooth by
    // construction — biquad cascade — so 160 points bound the true peak well).
    double peakMag = 0.0;
    const double fLo = 40.0, fHi = std::min(16000.0, fs * 0.45);
    for (int k = 0; k < 160; ++k) {
        const double f = fLo * std::pow(fHi / fLo, k / 159.0);
        peakMag = std::max(peakMag, dtftMag(f));
    }
    if (peakMag > 1e-9) {
        const float g = static_cast<float>(1.0 / peakMag);
        for (float& v : h) v *= g;
    }

    return h;
}

}  // namespace clipper::dsp
