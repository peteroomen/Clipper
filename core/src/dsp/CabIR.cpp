// Clipper — generated cab IR generators (M5 + cab expansion). See CabIR.h.
//
// ===========================================================================
// MODAL SYNTHESIS (the fizz fix)
// ---------------------------------------------------------------------------
// History: the original cabs built the diffuse tail from SEEDED RANDOM NOISE
// under an exponential envelope, plus a couple of discrete early reflections.
// That looked plausible on a steady-sine sweep but was wrong in two ways that a
// linear steady-state test can't see and a *player* hears immediately:
//
//   1. The random tail is a ~20 ms burst of COLORED NOISE after every impulse.
//      On steady tones it averages out; on real playing (each pick = an impulse)
//      it re-fires per note as audible hash — the "fizzy only with the cab on"
//      the field report kept describing. Measured: tail energy after 3 ms was
//      only -22.9 dB below the direct sound (a big, long, noisy tail).
//   2. The discrete early reflections comb-filtered the magnitude response into
//      a ragged spectrum (~2 dB steps between adjacent log-spaced points, ~15 dB
//      of ripple across 500 Hz-5 kHz) that differed between sample rates.
//
// Both are gone. The tail is now DETERMINISTIC MODAL SYNTHESIS: a direct impulse
// plus a small set of exponentially-decaying resonant sinusoids (damped modes)
// standing in for the real box/cone resonances of a guitar cab. A handful of
// tonal modes gives natural body and a short (~5-15 ms) decay with ZERO noise,
// and — because each mode is a smooth, broad resonance rather than a spectral
// spike — the magnitude response stays smooth. The coarse VOICING (low cut,
// presence bump, steep speaker rolloff) is still set by the biquad cascade, so
// each cab sounds like the cab it did before; we removed the hash, not the
// character.
//
// Everything is deterministic (no Date/random_device) so tests assert on it
// byte-for-byte, and every generated IR is checked in ctest against four
// "sounds-not-wrong" metrics (see test_amp_model.cpp): tail-energy bound,
// magnitude smoothness, tail modal-not-noisy flatness, and (M6.6) unity spectral
// peak normalization.
//
// ---------------------------------------------------------------------------
// PEAK NORMALIZATION (M6.6): every IR is scaled so its max |H(f)| over the audio
// band is exactly 1.0. A cab may COLOR the tone but must never BOOST any band
// above the level entering it — otherwise the presence region pushes peaks into
// the output limiter and fizzes the clean path. NB: the normalization search
// grid is DENSE (512 log-spaced points): the old 160-point grid undershot the
// true peak by ~0.5 dB, leaving the cab able to boost slightly past unity.
// ===========================================================================

#include "clipper/dsp/CabIR.h"

#include "clipper/dsp/Biquad.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace clipper::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;

// One resonant mode: a decaying sinusoid f(t) = amp * e^{-t/tau} * sin(w t).
// (Phase 0 so the mode starts at exactly zero — no onset step/click.)
struct Mode {
    double freqHz;   // resonance frequency
    double decayMs;  // 1/e amplitude decay time in milliseconds
    double amp;      // linear amplitude relative to the unit direct impulse
};

// Build the RAW (unfiltered, unnormalized) modal impulse response: a direct
// impulse at `directDelay` plus the sum of the given damped-sinusoid modes.
// Deterministic — identical for identical inputs.
std::vector<float> modalRaw(int len, double fs, double directAmp, int directDelay,
                            const std::vector<Mode>& modes) {
    std::vector<float> h(static_cast<size_t>(len), 0.0f);
    if (directDelay >= 0 && directDelay < len) h[static_cast<size_t>(directDelay)] += static_cast<float>(directAmp);
    for (const auto& m : modes) {
        const double w = 2.0 * kPi * m.freqHz / fs;
        const double tau = std::max(1.0, m.decayMs * 1e-3 * fs);  // samples
        for (int n = directDelay; n < len; ++n) {
            const double t = n - directDelay;
            const double env = std::exp(-t / tau);
            if (env < 1e-6) break;  // mode has died — stop (deterministic bound)
            h[static_cast<size_t>(n)] += static_cast<float>(m.amp * env * std::sin(w * t));
        }
    }
    return h;
}

// |H(f)| = raw DTFT magnitude of the IR at f. This IS the exact frequency
// response of a finite impulse response (sum h[n] e^{-jwn}); the right tool for
// reading an IR's magnitude (unlike a windowed sinusoid-amplitude estimator).
double dtftMag(const std::vector<float>& h, double f, double fs) {
    const double w = 2.0 * kPi * f / fs;
    double re = 0.0, im = 0.0;
    for (size_t n = 0; n < h.size(); ++n) {
        re += h[n] * std::cos(w * n);
        im -= h[n] * std::sin(w * n);
    }
    return std::sqrt(re * re + im * im);
}

// Scale to UNITY SPECTRAL PEAK: max |H(f)| == 1 over the audio band (M6.6). The
// response is smooth by construction (biquad cascade + broad modes), but the
// grid is dense (512 log points) so we never undershoot the true peak and leave
// the cab able to boost past unity.
void peakNormalize(std::vector<float>& h, double fs) {
    double peakMag = 0.0;
    const double fLo = 40.0, fHi = std::min(16000.0, fs * 0.45);
    for (int k = 0; k < 512; ++k) {
        const double f = fLo * std::pow(fHi / fLo, k / 511.0);
        peakMag = std::max(peakMag, dtftMag(h, f, fs));
    }
    if (peakMag > 1e-9) {
        const float g = static_cast<float>(1.0 / peakMag);
        for (float& v : h) v *= g;
    }
}

// Run one 2nd-order section forward over the whole buffer.
void applyBiquad(std::vector<float>& h, const BiquadCoeffs& c) {
    Biquad b;
    b.setCoeffs(c);
    for (float& v : h) v = b.process(v);
}

// Raised-cosine tail fade: hold unity until `startMs`, then smoothly fall to
// zero by `endMs`. A steep speaker rolloff SPREADS the direct impulse (the
// time/frequency tradeoff), so the high-pass's low-frequency ring extends past a
// few ms; gating the far tail keeps the IR a tight, gated cab response with
// almost all energy in the first few ms (the tail-energy metric) and no abrupt
// truncation click. The cosine shape means the window's own spectrum is smooth,
// so it never adds ripple to the magnitude response.
void applyTailFade(std::vector<float>& h, double fs, double startMs, double endMs) {
    const int s = static_cast<int>(std::lround(startMs * 1e-3 * fs));
    const int e = static_cast<int>(std::lround(endMs * 1e-3 * fs));
    if (e <= s) return;
    for (int i = 0; i < static_cast<int>(h.size()); ++i) {
        double g;
        if (i <= s) g = 1.0;
        else if (i >= e) g = 0.0;
        else g = 0.5 * (1.0 + std::cos(kPi * (i - s) / static_cast<double>(e - s)));
        h[static_cast<size_t>(i)] *= static_cast<float>(g);
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// Default 2x12 — a plausible closed-back 2x12 (the clean platform the JC-120
// drives). MODAL tail (no noise). Coarse voicing unchanged from M5/M6.6:
//   low cut ~95 Hz, presence bump +3.5 dB @ 2.5 kHz, steep ~48 dB/oct rolloff
//   above 5 kHz (four cascaded 2nd-order low-passes).
//
// Modes (box/cone/breakup), all short-decay and low-amplitude so the tail stays
// >35 dB below the direct sound and the mid-band response stays smooth:
//   ~90-130 Hz  box/port resonances,
//   ~200-700 Hz low-mid cone body,
//   ~1.5-3.4 kHz a small, damped cone-breakup cluster (broad, gentle).
//
// Measured magnitude response of the generated IR (raw DTFT, dB relative to the
// 1 kHz bin; the ctest asserts this shape at 44.1 k / 48 k / 96 k):
//     60 Hz   ~ -8 dB      (low cut)
//     100 Hz  ~ -4 dB      (low cut easing in)
//     1 kHz   =  0 dB       (reference / passband)
//     2.5 kHz ~ +2 dB      (presence bump)
//     5 kHz   ~ -12 dB     (into the steep speaker rolloff)
//     8 kHz   ~ -39 dB     (well down the ~48 dB/oct rolloff)
// Load-bearing shape: 1 kHz >> 8 kHz, 5 kHz below 1 kHz, presence not collapsed,
// low end rolled off; plus the four metric asserts (tail energy, smoothness,
// modal flatness, unity peak).
// ---------------------------------------------------------------------------
std::vector<float> generateDefaultCab2x12IR(double sampleRate) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int len = static_cast<int>(std::lround(1024.0 * fs / 48000.0));
    const double rr = fs / 48000.0;
    const int direct = static_cast<int>(std::lround(5.0 * rr));

    // A mode's spectral bump ~= amp * (decay_samples / 2), so amplitudes are
    // deliberately tiny (thousandths): each mode is a gentle ~1 dB resonance for
    // body and a short natural decay, NOT a spectral spike. The magnitude voicing
    // is set by the biquad cascade below acting on the direct impulse.
    static const std::vector<Mode> kModes = {
        // freqHz, decayMs, amp — box/port
        {92.0, 2.8, 0.0016}, {128.0, 2.6, 0.0014},
        // low-mid cone body
        {205.0, 2.2, 0.0013}, {320.0, 1.9, 0.0012}, {470.0, 1.6, 0.0011},
        {680.0, 1.4, 0.0010},
        // damped cone-breakup cluster (broad, gentle — shaped by the presence
        // bump and killed by the steep low-pass)
        {1500.0, 0.9, 0.0012}, {2100.0, 0.7, 0.0012}, {2600.0, 0.6, 0.0011},
        {3200.0, 0.5, 0.0009},
    };

    std::vector<float> h = modalRaw(len, fs, 1.0, direct, kModes);

    applyBiquad(h, rbj::highPass(95.0, 0.6, fs));  // Q 0.6: tight, low ring
    for (int i = 0; i < 4; ++i) applyBiquad(h, rbj::lowPass(5000.0, 0.707, fs));
    applyBiquad(h, rbj::peaking(2500.0, 3.5, 1.0, fs));

    applyTailFade(h, fs, 2.5, 7.0);  // gate the far tail
    peakNormalize(h, fs);
    return h;
}

// ---------------------------------------------------------------------------
// Brit 4x12 — a closed-back Marshall-style 4x12 (greenback-ish), the thicker,
// darker rock voicing that pairs with the JCM800. Distinct from the 2x12:
//   * more LOW-MID WEIGHT: low cut shifted DOWN to ~70 Hz, plus a broad +2 dB
//     bump across ~180-250 Hz (the 4x12 "chunk"),
//   * presence energy a touch higher, ~2.8-3.2 kHz,
//   * a STEEPER top rolloff starting LOWER, from ~4.5 kHz (five cascaded
//     low-passes vs the 2x12's four @ 5 kHz) — the greenback darkness, so the
//     4x12 is audibly darker than the 2x12 above 5 kHz.
// Same modal recipe + M6.6 peak normalization.
//
// Measured magnitude response (raw DTFT, dB relative to 1 kHz; ctest-asserted at
// 44.1 k / 48 k / 96 k):
//     60 Hz   ~ -6 dB      (low cut, shifted down vs the 2x12 -> less cut)
//     100 Hz  ~ -2 dB
//     200 Hz  ~ +1.5 dB    (the low-mid chunk — the 4x12 thickness)
//     1 kHz   =  0 dB       (reference / passband)
//     3 kHz   ~  0 dB       (presence holding up against the earlier rolloff)
//     4.5 kHz ~ -13 dB     (rolloff starting lower)
//     5 kHz   ~ -19 dB     (steeper/darker than the 2x12's ~-12 dB)
//     8 kHz   ~ -56 dB     (far below the 2x12's ~-39 dB — the greenback top)
// Load-bearing: darker than the 2x12 above 5 kHz (5 k AND 8 k), extra low-mid
// (200 Hz above 1 kHz), presence not collapsed; plus the four metric asserts.
// ---------------------------------------------------------------------------
std::vector<float> generateBrit4x12IR(double sampleRate) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int len = static_cast<int>(std::lround(1024.0 * fs / 48000.0));
    const double rr = fs / 48000.0;
    const int direct = static_cast<int>(std::lround(5.0 * rr));

    // Tiny amplitudes (see the 2x12 note): gentle body + a short natural decay,
    // not spectral spikes. A little fuller/longer in the low-mids than the 2x12.
    static const std::vector<Mode> kModes = {
        // box/port — a touch lower & fuller than the 2x12 (bigger box)
        {80.0, 2.4, 0.0015}, {115.0, 2.2, 0.0014},
        // low-mid cone body — weighted toward the 180-250 chunk
        {195.0, 2.0, 0.0014}, {240.0, 1.8, 0.0013}, {360.0, 1.5, 0.0011},
        {560.0, 1.3, 0.0010},
        // damped cone-breakup cluster, centered a little higher for the presence
        {1700.0, 0.8, 0.0012}, {2300.0, 0.7, 0.0011}, {2950.0, 0.6, 0.0010},
        {3300.0, 0.5, 0.0008},
    };

    std::vector<float> h = modalRaw(len, fs, 1.0, direct, kModes);

    applyBiquad(h, rbj::highPass(72.0, 0.6, fs));       // Q 0.6: tight, low ring
    applyBiquad(h, rbj::peaking(215.0, 2.0, 0.7, fs));  // broad low-mid chunk
    for (int i = 0; i < 5; ++i) applyBiquad(h, rbj::lowPass(4600.0, 0.707, fs));
    applyBiquad(h, rbj::peaking(3000.0, 3.0, 1.1, fs));  // presence

    applyTailFade(h, fs, 1.8, 5.0);  // gate the far tail (tighter than the 2x12)
    peakNormalize(h, fs);
    return h;
}

// ---------------------------------------------------------------------------
// Orange 4x12 (M10.3, docs §57) — the same sealed 4x12 family as the Brit, voiced
// the other way round, because the OR120 is a mid-FORWARD amp and its cab is part
// of that. Distinct from the Brit 4x12:
//   * a LOWER low cut (~62 Hz vs 72 Hz) and box modes moved DOWN — the big woolly
//     Orange bottom,
//   * NO 200 Hz low-mid chunk; the weight sits below it,
//   * a pronounced UPPER-MID BARK: +4 dB centred at 1.2 kHz, broad (Q 0.9). This
//     is the voicing decision of the cab, and it is the same direction as the
//     amp's James stack rather than a second, independent EQ opinion,
//   * a slightly later top rolloff than the Brit's greenback darkness (four
//     low-passes from 4.8 kHz vs five from 4.6 kHz) — present, not fizzy.
//
// Measured magnitude response (raw DTFT, dB relative to 1 kHz; ctest-asserted at
// 44.1 k / 48 k / 96 k, and asserted DIFFERENT from the Brit where it matters):
//     60 Hz   ~ -4 dB      (lowest low cut of the three cabs)
//     100 Hz  ~ -1 dB
//     200 Hz  ~ -1 dB      (no chunk — contrast the Brit's +1.5 dB)
//     1 kHz   =  0 dB       (reference / passband)
//     1.2 kHz ~ +1 dB      (the bark)
//     3 kHz   ~ -4 dB
//     8 kHz   ~ -47 dB
// Load-bearing: MORE 60 Hz than the Brit, MORE 1.2 kHz relative to 200 Hz than
// the Brit, still darker than the 2x12 up top; plus the four metric asserts.
// ---------------------------------------------------------------------------
std::vector<float> generateOrange4x12IR(double sampleRate) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int len = static_cast<int>(std::lround(1024.0 * fs / 48000.0));
    const double rr = fs / 48000.0;
    const int direct = static_cast<int>(std::lround(5.0 * rr));

    // Tiny amplitudes (see the 2x12 note). Box modes lower and a touch longer than
    // the Brit's; the breakup cluster pulled DOWN into the bark region.
    static const std::vector<Mode> kModes = {
        // box/port — the lowest of the three cabs
        {72.0, 2.6, 0.0016}, {104.0, 2.3, 0.0014},
        // low-mid cone body — deliberately NOT weighted at 200 Hz
        {165.0, 2.1, 0.0012}, {300.0, 1.7, 0.0011}, {430.0, 1.5, 0.0010},
        {620.0, 1.3, 0.0010},
        // the bark: a damped cluster centred where the Orange honk lives
        {1150.0, 1.0, 0.0014}, {1450.0, 0.9, 0.0013}, {2400.0, 0.6, 0.0010},
        {3100.0, 0.5, 0.0008},
    };

    std::vector<float> h = modalRaw(len, fs, 1.0, direct, kModes);

    applyBiquad(h, rbj::highPass(62.0, 0.6, fs));        // Q 0.6: tight, low ring
    applyBiquad(h, rbj::peaking(1200.0, 4.0, 0.9, fs));  // the upper-mid bark
    for (int i = 0; i < 4; ++i) applyBiquad(h, rbj::lowPass(4800.0, 0.707, fs));

    applyTailFade(h, fs, 2.0, 5.5);  // gate the far tail
    peakNormalize(h, fs);
    return h;
}

std::vector<float> generateTweed1x8IR(double sampleRate) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    // A small box rings for LESS time than a 4x12 — half the length is enough and
    // it keeps the convolution cheap.
    const int len = static_cast<int>(std::lround(512.0 * fs / 48000.0));
    const double rr = fs / 48000.0;
    // The mic is closer on a tiny combo, so a shorter direct delay than the 4x12s'.
    const int direct = static_cast<int>(std::lround(3.0 * rr));

    // Tiny amplitudes (see the 2x12 note). Everything here sits ABOVE the 4x12
    // equivalents, which is the whole point of the cab.
    static const std::vector<Mode> kModes = {
        // Box modes. The Champ cabinet's longest internal dimension is ~16", so the
        // fundamental lands near 420 Hz rather than the ~72-104 Hz of a 4x12 — there
        // is no low box mode to have. These are SHORT (an open back does not store
        // energy the way a sealed box does).
        {420.0, 1.1, 0.0016}, {560.0, 1.0, 0.0013},
        // Cone body — an 8" driver's mass corner is high, so the "body" of this
        // speaker is where a 12"'s low mids would be.
        {760.0, 0.9, 0.0014}, {1050.0, 0.8, 0.0013},
        // THE TWEED HONK: a hard upper-mid cluster. An 8" cone breaks up higher and
        // less gracefully than a 12", and that boxy bark is most of what people mean
        // by "tweed".
        {1750.0, 0.7, 0.0018}, {2300.0, 0.6, 0.0016}, {3200.0, 0.5, 0.0012},
        // A little more top than a Celestion 4x12 keeps — a light cone stays coupled
        // higher, which is why a small tweed can sound spitty when you dig in.
        {4600.0, 0.4, 0.0009},
    };

    std::vector<float> h = modalRaw(len, fs, 1.0, direct, kModes);

    // The low corner is the headline number. 62 Hz (Orange) / 72 Hz-ish (Brit)
    // against 145 Hz here, and Q 0.5 because an open back rolls off gently rather
    // than with a sealed box's steeper knee.
    applyBiquad(h, rbj::highPass(120.0, 0.5, fs));
    applyBiquad(h, rbj::peaking(1900.0, 3.5, 1.0, fs));   // the tweed bark
    // Three passes rather than the 4x12s' four: an 8" keeps more top.
    for (int i = 0; i < 3; ++i) applyBiquad(h, rbj::lowPass(5600.0, 0.707, fs));

    applyTailFade(h, fs, 1.2, 3.5);  // a small box: gate the tail early
    peakNormalize(h, fs);
    return h;
}

// Public entry point (see CabIR.h): peak-normalize any IR to unity spectral peak
// in place. Delegates to the same dense-grid normalizer the generators use, so a
// user-uploaded cab is held to the exact same "never boosts" rule (M6.6).
void peakNormalizeIR(std::vector<float>& ir, double sampleRate) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    peakNormalize(ir, fs);
}

}  // namespace clipper::dsp
