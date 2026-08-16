// M11 "Player Expectations Suite" — the missing test layer between "the circuit
// math is right" and "a player plugs in and it sounds right".
//
// Four field bugs shipped while every circuit metric was green (docs §26):
//   1. RAT "no balls"      — level calibration + shelf voicing at player levels.
//   2. Cab fizz            — correct convolution of a wrong-sounding IR, plus an
//                            in-place aliasing bug the separate-buffer null test missed.
//   3. Muff Pi hum blowout — nothing ever tested MIN-knob settings or a realistic
//                            single-coil noise floor (docs §24 postmortem).
//   4. AC30 "muddy"        — circuit right, knob-taper default wrong.
//
// This suite therefore tests THE PLAYER'S EXPECTATIONS, not the circuit:
//   A. Universal gear invariants — one parameterized harness over EVERY dirt pedal
//      (rat/sd1/ts/muff/gold) and EVERY amp voice (clean120/jcm800/twin/ac30):
//      min-knob usability, the hum-torture standard, knob monotonicity
//      spot-checks, and default-settings level sanity.
//   B. Live-convention processing — every C-ABI entry point exercised the way the
//      WORKLET actually calls it (in-place buffers, 128-frame blocks, 48 kHz) and
//      compared against the test-convenient way (separate buffers, one big block).
//      This is the test that would have caught the in-place convolver bug.
//   C. Golden program renders — the "first five minutes" rigs a new player will
//      actually try, compared against committed reference renders within a
//      perceptual-ish per-third-octave-band gate (±1.5 dB), so lossless refactors
//      pass but voicing drift fails. Regenerate ONLY via scripts/update-goldens.sh
//      — drift must be a conscious decision, and that script is what makes it one
//      (clean tree, printed dB summary, a human at a terminal, a written
//      justification in core/tests/goldens/GOLDENS.md).
//
// All measurements run at 48 kHz — the web AudioWorklet's fixed rate, i.e. the
// rate at which every one of the four field bugs was heard. (Per-rate circuit
// behavior is already pinned by the per-gear suites at 44.1/48/96 k.)
//
// No framework: int main + <cassert>, same as every other suite. Every bar below
// carries the MEASURED per-gear numbers it was derived from (frozen 2026-07,
// commit of this file) so a future failure can be read against them.

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/FFT.h"
#include "clipper/dsp/GoldModel.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/PhaserModel.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/ReverbModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"
#include "clipper/dsp/TwinAmp.h"

// Golden WAV IO. dr_wav lives in tools/third_party and is included ONLY by
// tools/tests, never by the DSP core.
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "support/AssertsLive.h"
#include "support/Xfail.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// --- The worklet's C ABI (clipper_c_api.cpp) ---------------------------------
// Block B drives the EXACT entry points the browser calls, so the prototypes are
// declared verbatim here and the test links clipper_c_api.
extern "C" {
void* rat_create(float);   void rat_destroy(void*);
void  rat_set_param(void*, int, float);
void  rat_set_oversampling(void*, int);
void  rat_process(void*, const float*, float*, int);
void* sd_create(float);    void sd_destroy(void*);
void  sd_set_param(void*, int, float);
void  sd_set_oversampling(void*, int);
void  sd_process(void*, const float*, float*, int);
void* ts_create(float);    void ts_destroy(void*);
void  ts_set_param(void*, int, float);
void  ts_set_oversampling(void*, int);
void  ts_process(void*, const float*, float*, int);
void* muff_create(float);  void muff_destroy(void*);
void  muff_set_param(void*, int, float);
void  muff_set_oversampling(void*, int);
void  muff_process(void*, const float*, float*, int);
void* gold_create(float);  void gold_destroy(void*);
void  gold_set_param(void*, int, float);
void  gold_set_oversampling(void*, int);
void  gold_process(void*, const float*, float*, int);
void* phaser_create(float); void phaser_destroy(void*);
void  phaser_set_param(void*, int, float);
void  phaser_process(void*, const float*, float*, int);
void* amp_create(float);   void amp_destroy(void*);
void  amp_set_model(void*, int);
void  amp_set_cab_builtin(void*, int);
void  amp_set_param(void*, int, float);
void  amp_process(void*, const float*, float*, int);
void  amp_process_stereo(void*, const float*, float*, float*, int);
}

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kFs = 48000.0;  // the AudioWorklet rate — where the bugs were heard

// Amp-chain C ABI param ids (mirror web/src/params.ts / clipper_c_api.cpp).
constexpr int kAmpVolume = 0, kAmpBass = 1, kAmpMid = 2, kAmpTreble = 3,
              kAmpBright = 4, kAmpCab = 5, kAmpSpeed = 6, kAmpDepth = 7,
              kAmpChorusMode = 8, kAmpReverb = 9, kAmpJcmGain = 10,
              kAmpJcmPresence = 11, kAmpJcmMaster = 12;

// --- known-bad properties (2026-07-24 audit) --------------------------------------
// Found by this slice, when block B's vacuous 128-only comparison was replaced with a
// RAGGED block size. It is the audit's Medium/DSP item, now measured end to end through
// the C ABI the worklet actually calls.
constexpr clipper::test::XfailDecl kXfControlRateBlockSize{
    "control-rate-param-sampling-block-size",
    "2026-07-24 audit, Medium/DSP: 'control-rate parameter sampling defeats the 5 ms "
    "smoother at DAW block sizes' (OverdriveEngine.cpp:147, MuffModel.cpp:161, "
    "GoldModel.cpp:487)",
    "a dirt pedal's output does not depend on the host block size AT ALL, including during "
    "the initial parameter snap — the smoothers advance per sample but only the CHUNK-END "
    "value is kept, so the trajectory differs with chunk size (worst: Muff, 1.6 absolute at "
    "25 ms; four converge to EXACTLY 0 over the render's last quarter and the Muff to "
    "3.3e-05, its docs-§53 diode caps having a ~5 s discharge; the GOLD needs ~300 ms "
    "to settle where the others need ~50, its slowest drive-path state)",
    "the control-rate sampling fix: apply the smoothed value PER SAMPLE instead of keeping "
    "the chunk-end value (RatModel's own inner loop is the pattern)"};

// --- signal helpers ----------------------------------------------------------

double toDb(double a) { return 20.0 * std::log10(a + 1e-12); }

double rmsOf(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : std::sqrt(s / x.size());
}

double peakOf(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, std::fabs(static_cast<double>(v)));
    return p;
}

bool allFinite(const std::vector<float>& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

std::vector<float> sine(double f, float amp, double secs, double fs) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> s(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
    return s;
}

// THE STANDARD PLUCK: the same plucked-string burst the render CLI generates
// (fast attack, 6 harmonics, brighter partials decay faster), 220 Hz, scaled so
// the burst PEAKS at −20 dBFS (0.1) — a normal single-note guitar DI level, the
// signal every "first five minutes" player feeds every piece of gear.
std::vector<float> standardPluck(double secs, double fs, double f0 = 220.0,
                                 float peakTarget = 0.1f) {
    const int n = static_cast<int>(secs * fs);
    std::vector<float> sig(static_cast<size_t>(n));
    const double attackS = 0.004;  // 4 ms pick attack
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        const double attack = 1.0 - std::exp(-t / attackS);
        double s = 0.0;
        for (int h = 1; h <= 6; ++h) {
            const double decay = std::exp(-t * (2.0 + 1.2 * h));
            s += (1.0 / h) * decay * std::sin(kTwoPi * f0 * h * t);
        }
        sig[static_cast<size_t>(i)] = static_cast<float>(attack * s);
    }
    const double pk = peakOf(sig);
    const float norm = pk > 1e-12 ? static_cast<float>(peakTarget / pk) : 1.0f;
    for (float& v : sig) v *= norm;
    return sig;
}

// Hann-windowed Goertzel magnitude over [start, start+n).
double goertzelAmp(const std::vector<float>& x, size_t start, size_t n, double f,
                   double fs) {
    const double w = kTwoPi * f / fs;
    const double cw = std::cos(w), sw = std::sin(w), coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0, winSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double win = 0.5 * (1.0 - std::cos(kTwoPi * i / (n - 1)));
        winSum += win;
        const double s0 = x[start + i] * win + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return 2.0 * std::sqrt(re * re + im * im) / winSum;
}

// Goertzel over the settled last half of the buffer.
double toneLevel(const std::vector<float>& x, double f, double fs) {
    const size_t n = x.size(), w = n / 2, st = n - w;
    return goertzelAmp(x, st, w, f, fs);
}

// THD (harmonics 2..8 vs fundamental) over the settled last half.
double thdOf(const std::vector<float>& o, double f0, double fs) {
    const size_t n = o.size(), w = n / 2, st = n - w;
    const double f1 = goertzelAmp(o, st, w, f0, fs);
    double hh = 0.0;
    for (int k = 2; k <= 8; ++k) {
        const double a = goertzelAmp(o, st, w, k * f0, fs);
        hh += a * a;
    }
    return std::sqrt(hh) / (f1 + 1e-12);
}

// High-band harmonic energy of a nonlinear device's output for a 220 Hz sine
// drive: summed harmonic power in [1 kHz, 10 kHz] relative to the fundamental,
// in dB. The spectral-centroid statement a tone knob makes, in its most
// sensitive form: a knob that "brightens" must raise this; one that "darkens"
// must lower it. (A plain power-weighted centroid is fundamental-dominated and
// barely moves — measured: the SD-1 tone sweep moved it only 4%.)
double hfHarmonicDb(const std::vector<float>& o, double f0, double fs) {
    const size_t n = o.size(), w = n / 2, st = n - w;
    const double f1 = goertzelAmp(o, st, w, f0, fs);
    double hh = 0.0;
    for (int k = 2; k * f0 < std::min(10000.0, fs * 0.45); ++k) {
        if (k * f0 < 1000.0) continue;
        const double a = goertzelAmp(o, st, w, k * f0, fs);
        hh += a * a;
    }
    return 10.0 * std::log10(hh / (f1 * f1 + 1e-30) + 1e-30);
}

// --- third-octave band spectrum (the golden-render gate) ---------------------

struct BandSpectrum {
    std::vector<double> centerHz;
    std::vector<double> rmsDb;  // per-band RMS in dBFS-ish (relative, consistent)
};

// Hann-windowed FFT of the whole buffer, bin powers summed into third-octave
// bands (centers 50 Hz · 2^(k/3) up to ~12.7 kHz).
BandSpectrum thirdOctaveSpectrum(const std::vector<float>& x, double fs) {
    int n = 1;
    while (n < static_cast<int>(x.size())) n <<= 1;
    clipper::dsp::FFT fft;
    fft.prepare(n);
    std::vector<double> re(static_cast<size_t>(n), 0.0), im(static_cast<size_t>(n), 0.0);
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = 0.5 * (1.0 - std::cos(kTwoPi * i / (x.size() - 1)));
        re[i] = x[i] * w;
    }
    fft.transform(re.data(), im.data(), false);
    const double binHz = fs / n;
    BandSpectrum bs;
    const double kEdge = std::pow(2.0, 1.0 / 6.0);
    for (double c = 50.0; c < 12800.0 && c < fs * 0.45; c *= std::pow(2.0, 1.0 / 3.0)) {
        const int b0 = static_cast<int>(std::ceil(c / kEdge / binHz));
        const int b1 = static_cast<int>(std::floor(c * kEdge / binHz));
        double p = 0.0;
        int cnt = 0;
        for (int b = std::max(1, b0); b <= b1 && b < n / 2; ++b, ++cnt)
            p += re[static_cast<size_t>(b)] * re[static_cast<size_t>(b)] +
                 im[static_cast<size_t>(b)] * im[static_cast<size_t>(b)];
        if (cnt == 0) continue;
        bs.centerHz.push_back(c);
        bs.rmsDb.push_back(10.0 * std::log10(p / cnt + 1e-30));
    }
    return bs;
}

// --- the universal gear harness (block A) ------------------------------------

// A knob as the PLAYER sees it. `kind` states what the knob PROMISES:
//   GAIN  — more of it = more distortion (THD must not decrease 0 → 0.5/def → 1).
//   LEVEL — more of it = louder; min may be honestly (near-)silent.
//   TONE  — brightens (dirUp=true) or darkens (dirUp=false) as it goes up.
//   AUX   — everything else (reverb mix, presence, bright, …): only the universal
//           min-knob bars apply (finite, audible, bounded).
struct KnobSpec {
    const char* name;
    int slot;          // model param id
    float def;         // the shipped opening value (mirrors web/src/rig.ts)
    enum Kind { GAIN, LEVEL, TONE, AUX } kind;
    bool dirUp = true;      // TONE: true = clockwise brightens (documented sense)
    float monoLo = 0.0f;    // GAIN monotonicity probe points (lo/mid/hi)
    float monoMid = 0.5f;
    float monoHi = 1.0f;
};

// One gear unit under the harness: a factory returning a fresh, prepared
// instance behind a uniform setKnob/process interface. Fresh instance per
// render — no state bleed between measurements.
struct Gear {
    std::string name;
    bool isPedal;  // pedals output volts (peak may exceed 1); amps are normalized
    std::vector<KnobSpec> knobs;
    // A4 level-sanity window: the allowed output-vs-input RMS delta (dB) for the
    // standard pluck at defaults. Per-gear because gear CLASSES honestly differ
    // (a fuzz turns a decaying pluck into a sustained wall — its RMS delta is
    // legitimately +20 dB+; a valve amp at its 0.4 opening volume is legitimately
    // clean-quiet). Each window is measured-value ± ~10 dB — tight enough that
    // the next "no balls" (−20 dB drift) or "blows the mix apart" fails.
    double lvlDeltaLo = -12.0, lvlDeltaHi = 12.0;
    // process(knobs-values, in, out): renders through a FRESH instance.
    std::function<void(const std::vector<float>& knobVals, const std::vector<float>& in,
                       std::vector<float>& out)> render;
};

// Let every smoothed parameter settle at its TARGET before measuring: a fresh
// instance ramps knobs from their construction state over a few ms, and a
// front-loaded pluck would otherwise leak through the ramp (measured: clean120
// volume=0 read −58 dBFS instead of silence without this).
constexpr double kSettleSecs = 0.25;

template <typename Model>
Gear makePedalGear(const char* name, std::vector<KnobSpec> knobs) {
    Gear g;
    g.name = name;
    g.isPedal = true;
    g.knobs = std::move(knobs);
    auto knobsCopy = g.knobs;
    g.render = [knobsCopy](const std::vector<float>& vals, const std::vector<float>& in,
                           std::vector<float>& out) {
        Model m;
        m.prepare(kFs, 128);
        m.setOversampling(4);
        for (size_t i = 0; i < knobsCopy.size(); ++i)
            m.setParameter(knobsCopy[i].slot, vals[i]);
        std::vector<float> sil(static_cast<size_t>(kSettleSecs * kFs), 0.0f);
        std::vector<float> tmp(sil.size(), 0.0f);
        m.process(sil.data(), tmp.data(), static_cast<int>(sil.size()));  // settle
        out.assign(in.size(), 0.0f);
        if (!in.empty()) m.process(in.data(), out.data(), static_cast<int>(in.size()));
    };
    return g;
}

// SFINAE helper: the linear clean120 AmpModel has no setOversampling; the valve
// amps do (and run at the C ABI's fixed 4×).
template <typename M>
auto trySetOversampling(M& m, int f, int) -> decltype(m.setOversampling(f), void()) {
    m.setOversampling(f);
}
template <typename M>
void trySetOversampling(M&, int, long) {}

template <typename Model>
Gear makeAmpGear(const char* name, std::vector<KnobSpec> knobs, bool oversampled) {
    Gear g;
    g.name = name;
    g.isPedal = false;
    g.knobs = std::move(knobs);
    auto knobsCopy = g.knobs;
    g.render = [knobsCopy, oversampled](const std::vector<float>& vals,
                                        const std::vector<float>& in,
                                        std::vector<float>& out) {
        Model m;
        if (oversampled) trySetOversampling(m, 4, 0);
        m.prepare(kFs, 128);
        for (size_t i = 0; i < knobsCopy.size(); ++i)
            m.setParameter(knobsCopy[i].slot, vals[i]);
        std::vector<float> sil(static_cast<size_t>(kSettleSecs * kFs), 0.0f);
        std::vector<float> tmp(sil.size(), 0.0f);
        m.process(sil.data(), tmp.data(), static_cast<int>(sil.size()));  // settle
        out.assign(in.size(), 0.0f);
        if (!in.empty()) m.process(in.data(), out.data(), static_cast<int>(in.size()));
    };
    return g;
}

std::vector<float> defaults(const Gear& g) {
    std::vector<float> v;
    for (const auto& k : g.knobs) v.push_back(k.def);
    return v;
}

// The full lineup. Knob defaults mirror web/src/rig.ts (PEDAL_KNOB_DEFAULTS /
// AMP_KNOB_DEFAULTS) — the exact opening state of the app.
std::vector<Gear> allGear() {
    using clipper::dsp::Ac30Amp;
    using clipper::dsp::AmpModel;
    using clipper::dsp::GoldModel;
    using clipper::dsp::Jcm800Amp;
    using clipper::dsp::MuffModel;
    using clipper::dsp::RatModel;
    using clipper::dsp::SdModel;
    using clipper::dsp::TsModel;
    using clipper::dsp::TwinAmp;
    using K = KnobSpec;
    std::vector<Gear> gear;
    // A4 level-sanity windows (RMS delta dB, standard pluck at defaults), each
    // measured value ± ~10 dB. Measured 2026-07 @ 48 kHz (input RMS −33.5 dBFS):
    //   rat +18.6   sd1 +15.6   ts +13.1   muff +27.7 (fuzz sustain wall — by design;
    //   was +29.4 pre-§53, before the clip stages' DC-blocking diode caps)
    //   gold +12.4 (§56, was +12.5 after §54, +19.3 after §52, +7.3 after §50 and
    //   +13.2 before it: §52
    //   replaced the FITTED dirt summing weight 0.65 with the schematic's
    //   R20/(R16·kSumGain) = 4.1702 and the pedal got LOUDER, which was the opposite
    //   of the field report — §54 then fixed the node that weight multiplies and it
    //   came back down 6.8 dB. The window itself did NOT need re-centring: 12.5 sits
    //   comfortably inside the 9..29 §52 opened, so it is left alone)
    //   clean120 −6.0   jcm800 −2.5 (was +1.7 pre-§51: the re-derived GAIN taper
    //   delivers 4.1 dB less drive at the 0.5 default — docs §51)   twin −13.8
    //   ac30 −7.5 (0.4 opening volume). RE-BASELINED 2026-07-31 (docs §55, the dynamic
    //   supply). Two things to know before reading the move as a loss. (1) The
    //   previously-recorded "+1.6" had ROTTED: the pre-§55 code measures −4.1 here, so
    //   the slice's own contribution is −3.4 dB, not −9.1. (2) That −3.4 is almost
    //   entirely the §42.6 re-normalization (kFullScaleSecV 12.2 → 18.733 = −3.72 dB),
    //   and it lands ONLY on the clean end of the knob: at VOLUME 0.70 the same probe
    //   moves +13.64 → +13.36, i.e. 0.28 dB. The amp did not get quieter, it got a
    //   WIDER knob — which is the same fact the drive sweep reports as 0.82 → 2.37 dB
    //   of dynamic range. Measured A4 delta vs VOLUME, pre → post:
    //     0.30  −11.25 → −14.87   0.40  −4.09 → −7.46
    //     0.50   +2.53 →  −0.34   0.70 +13.64 → +13.36
    auto window = [&](const char* name, double lo, double hi) {
        for (auto& g : gear)
            if (g.name == name) { g.lvlDeltaLo = lo; g.lvlDeltaHi = hi; }
    };
    // RAT: FILTER is a post-clip low-pass — clockwise DARKENS (dirUp=false).
    gear.push_back(makePedalGear<RatModel>("rat", {
        K{"distortion", RatModel::PARAM_DISTORTION, 0.7f, K::GAIN},
        K{"filter", RatModel::PARAM_FILTER, 0.4f, K::TONE, false},
        K{"level", RatModel::PARAM_LEVEL, 0.8f, K::LEVEL},
    }));
    gear.push_back(makePedalGear<SdModel>("sd1", {
        K{"drive", SdModel::PARAM_DRIVE, 0.5f, K::GAIN},
        K{"tone", SdModel::PARAM_TONE, 0.5f, K::TONE, true},
        K{"level", SdModel::PARAM_LEVEL, 0.7f, K::LEVEL},
    }));
    gear.push_back(makePedalGear<TsModel>("ts", {
        K{"drive", TsModel::PARAM_DRIVE, 0.5f, K::GAIN},
        K{"tone", TsModel::PARAM_TONE, 0.5f, K::TONE, true},
        K{"level", TsModel::PARAM_LEVEL, 0.75f, K::LEVEL},
    }));
    // Muff SUSTAIN monotonicity probes 0 / 0.6 / 1.0 — docs §24: the fine THD
    // curve dips slightly near the cold-bias knee, the COARSE min→default→max
    // trend is the contract (measured 44% → 54% → 122%).
    gear.push_back(makePedalGear<MuffModel>("muff", {
        K{"sustain", MuffModel::PARAM_SUSTAIN, 0.6f, K::GAIN, true, 0.0f, 0.6f, 1.0f},
        K{"tone", MuffModel::PARAM_TONE, 0.5f, K::TONE, true},
        K{"volume", MuffModel::PARAM_VOLUME, 0.6f, K::LEVEL},
    }));
    // GOLD (v1.1 item 6): the clean-blend overdrive. GAIN is a true crossfade, so
    // at 0 it is honestly CLEAN (THD ~0) rather than "quiet" — GAIN semantics, and
    // the min-knob bar treats it like any other gain knob (audible, not silenced).
    // TREBLE is a NORMAL-sense treble tilt (clockwise brightens), unlike the RAT's
    // FILTER or the AC30's CUT.
    gear.push_back(makePedalGear<GoldModel>("gold", {
        K{"gain", GoldModel::PARAM_GAIN, 0.35f, K::GAIN},
        K{"treble", GoldModel::PARAM_TREBLE, 0.5f, K::TONE, true},
        K{"output", GoldModel::PARAM_OUTPUT, 0.7f, K::LEVEL},
    }));
    gear.push_back(makeAmpGear<AmpModel>("clean120", {
        K{"volume", AmpModel::PARAM_VOLUME, 0.4f, K::LEVEL},
        K{"bass", AmpModel::PARAM_BASS, 0.5f, K::AUX},
        K{"middle", AmpModel::PARAM_MIDDLE, 0.5f, K::AUX},
        K{"treble", AmpModel::PARAM_TREBLE, 0.6f, K::TONE, true},
    }, /*oversampled=*/false));
    // JCM800 GAIN is the preamp volume pot: at 0 it honestly kills the signal
    // (a real 2204 is silent with the preamp volume at zero) → LEVEL semantics
    // for the min-knob bar, GAIN monotonicity probed separately below.
    gear.push_back(makeAmpGear<Jcm800Amp>("jcm800", {
        K{"gain", Jcm800Amp::PARAM_GAIN, 0.5f, K::GAIN},
        K{"master", Jcm800Amp::PARAM_MASTER, 0.4f, K::LEVEL},
        K{"bass", Jcm800Amp::PARAM_BASS, 0.5f, K::AUX},
        K{"mid", Jcm800Amp::PARAM_MID, 0.5f, K::AUX},
        K{"treble", Jcm800Amp::PARAM_TREBLE, 0.6f, K::TONE, true},
        K{"presence", Jcm800Amp::PARAM_PRESENCE, 0.5f, K::AUX},
        K{"reverb", Jcm800Amp::PARAM_REVERB, 0.0f, K::AUX},
    }, /*oversampled=*/true));
    gear.push_back(makeAmpGear<TwinAmp>("twin", {
        K{"volume", TwinAmp::PARAM_VOLUME, 0.4f, K::LEVEL},
        K{"bass", TwinAmp::PARAM_BASS, 0.5f, K::AUX},
        K{"mid", TwinAmp::PARAM_MID, 0.5f, K::AUX},
        K{"treble", TwinAmp::PARAM_TREBLE, 0.6f, K::TONE, true},
        K{"reverb", TwinAmp::PARAM_REVERB, 0.0f, K::AUX},
    }, /*oversampled=*/true));
    // AC30 TOP CUT: clockwise DARKENS (the authentic inverted sense, docs §23).
    gear.push_back(makeAmpGear<Ac30Amp>("ac30", {
        K{"volume", Ac30Amp::PARAM_VOLUME, 0.4f, K::LEVEL},
        K{"bass", Ac30Amp::PARAM_BASS, 0.5f, K::AUX},
        K{"treble", Ac30Amp::PARAM_TREBLE, 0.6f, K::TONE, true},
        K{"cut", Ac30Amp::PARAM_TOPCUT, 0.5f, K::TONE, false},
        K{"reverb", Ac30Amp::PARAM_REVERB, 0.0f, K::AUX},
    }, /*oversampled=*/true));
    window("rat", 7.0, 27.0);
    window("sd1", 4.0, 24.0);
    window("ts", 1.0, 21.0);
    // RE-BASELINED 2026-08-10 (docs §67, the output-pot taper slice) — and it is
    // the ONLY one of the five dirt rows that had to move, which is itself the
    // scope check. The VOLUME pot is now the A250k audio taper its BOM says it
    // is, and at the shipped 0.6 default that is worth a DERIVED
    // 20·log10(audioTaper(0.6)/0.6) = -10.13 dB. Measured +27.7 -> +17.6, i.e.
    // -10.1 dB: the move is the pot law to within 0.03 dB and nothing else.
    // The window is SHIFTED by that same 10 dB, not widened: 18..38 -> 8..28,
    // still exactly 20 dB wide, so it is no easier to pass than it was. The other
    // four dirt rows stayed inside their existing windows and were NOT touched:
    // rat +18.6 -> +16.9, sd1 +15.6 -> +7.1, ts +13.1 -> +5.8, gold +12.4 -> +12.2.
    window("muff", 8.0, 28.0);
    window("gold", 9.0, 29.0);  // §52 summing network re-centred this from −3..17
                                // (itself re-centred from 3..23 by §50) on a measured
                                // +19.3 dB. §54's clipping-stage trio measures +12.5,
                                // still inside it — LEFT ALONE rather than re-snugged
                                // around the newest number (docs §54.6).
    window("clean120", -17.0, 3.0);
    window("jcm800", -10.0, 10.0);
    window("twin", -25.0, -5.0);
    window("ac30", -18.0, 2.0);  // §55: re-centred on the measured −7.5 (was −8..+12
                                 // around a stale +1.6 that the pre-slice code already
                                 // measured at −4.1). The file's convention is the
                                 // measurement ± ~10 dB; the old window left 0.5 dB of
                                 // margin, which is not a guard.
    return gear;
}

bool isDirtPedal(const Gear& g) { return g.isPedal; }

// ---------------------------------------------------------------------------
// A.1 — MIN-KNOB USABILITY. The Muff bug generalized: at defaults AND with any
// single knob at its minimum, the standard −20 dBFS pluck must come out finite,
// bounded, and (unless the knob is an output-level pot) audible.
//
// Bars (measured 2026-07, 48 kHz, standard pluck peak −20 dBFS / RMS −33.5 dBFS;
// every render lets smoothers SETTLE 0.25 s at the target first):
//   audible floor  −70 dBFS RMS   quietest legit min-knob output measured:
//                                  twin mid=0 at −51.3 dBFS (18.7 dB margin);
//                                  next-quietest rat distortion=0 −50.4, then
//                                  ac30 bass=0 −50.4 (the passive Vox stack still
//                                  guts a low-heavy pluck with BASS at zero, but
//                                  the §23-second-amendment gain-structure fix
//                                  lifted it 13.7 dB — it used to be the quietest
//                                  legit output at −64.1). The floor separates
//                                  "authentically quiet" from "silenced" (a true
//                                  kill measures −240).
//   pedal peak ceiling 2.0 V      hottest measured: muff tone=0 (dark, LP leg)
//                                  1.637 V; muff defaults 1.170 V (a Muff is LOUD —
//                                  the downstream limiter owns the ceiling).
//                                  (§53 re-baseline: 1.69 / 1.41 before the
//                                  DC-blocked diode branch.)
//   amp peak ceiling  1.05        amps are normalized, 1.0 == full scale; hottest
//                                  measured at defaults: jcm800 0.095 (was 0.126
//                                  pre-§51 — the re-derived GAIN taper, docs §51).
//   LEVEL knob at min             must actually attenuate ≥ 6 dB below default —
//                                  measured: every level/volume/master pot (and
//                                  the JCM's preamp-volume GAIN) kills to −240.
//   Defaults RMS per gear: rat −16.6, sd1 −19.6, ts −22.1, muff −7.4 (§53: was
//   −5.8, before the clip stages' DC-blocking diode caps), gold −22.7
//   (§52 took it −27.8 → −15.9 with the derived summing weight; §54's clipping-stage
//   trio takes it back to −22.6, because the reference implementation's measured
//   germanium fit clamps the diode node 5.7–8.5 dB lower than this model's old
//   datasheet-shaped pair and the 495 Hz summing pole trims the dirt's harmonics;
//   §56's netlist input divider then moves it 0.1 dB to −22.7.
//   Default peak 1.285 → 0.443 → 0.429 V, well inside the 2.0 V pedal ceiling),
//   clean120 −41.1, jcm800 −37.6 (was −33.5 pre-§51: −4.1 dB of GAIN-taper drive
//   at the 0.5 default, docs §51), twin −49.0, ac30 −33.6 dBFS (ac30 was −46.9
//   before the §23 second amendment: its PI was starved, so the VOLUME knob
//   could not reach the power section — see docs §23).
// ---------------------------------------------------------------------------
void testMinKnobUsability(const std::vector<Gear>& gear) {
    const auto pluck = standardPluck(1.5, kFs);
    const double inRms = rmsOf(pluck);
    constexpr double kAudibleFloorDb = -70.0;
    for (const auto& g : gear) {
        const double peakCeiling = g.isPedal ? 2.0 : 1.05;
        std::vector<float> out;
        g.render(defaults(g), pluck, out);
        assert(allFinite(out) && "defaults render produced NaN/inf");
        const double defRms = rmsOf(out), defPeak = peakOf(out);
        std::printf("  [A1] %-9s defaults: rms %6.1f dBFS  peak %5.3f\n", g.name.c_str(),
                    toDb(defRms), defPeak);
        std::fflush(stdout);
        assert(toDb(defRms) > kAudibleFloorDb && "defaults render inaudible");
        assert(defPeak < peakCeiling && "defaults render exceeds the peak ceiling");
        for (size_t i = 0; i < g.knobs.size(); ++i) {
            auto vals = defaults(g);
            vals[i] = 0.0f;
            std::vector<float> o;
            g.render(vals, pluck, o);
            assert(allFinite(o) && "min-knob render produced NaN/inf");
            const double r = rmsOf(o), p = peakOf(o);
            std::printf("  [A1] %-9s   %s=0: rms %6.1f dBFS  peak %5.3f\n", g.name.c_str(),
                        g.knobs[i].name, toDb(r), p);
            std::fflush(stdout);
            assert(p < peakCeiling && "min-knob render exceeds the peak ceiling");
            if (g.knobs[i].kind == KnobSpec::LEVEL ||
                (std::strcmp(g.name.c_str(), "jcm800") == 0 && g.knobs[i].kind == KnobSpec::GAIN)) {
                // An output-level pot (or the JCM's preamp-volume GAIN) at min is
                // honestly quiet — but it must genuinely attenuate.
                assert(toDb(r) < toDb(defRms) - 6.0 &&
                       "level knob at min does not attenuate vs default");
            } else {
                assert(toDb(r) > kAudibleFloorDb &&
                       "non-level knob at MIN silenced the gear (the Muff-class bug)");
            }
        }
        (void)inRms;
    }
    std::printf("  [ok] A1 min-knob usability: every knob min + defaults finite, bounded, "
                "audible (floor %.0f dBFS)\n", kAudibleFloorDb);
}

// ---------------------------------------------------------------------------
// A.2 — HUM TORTURE STANDARD. The Pi's exact field signal, generalized to every
// dirt pedal: a 220 Hz note at −10 dBFS + 60 Hz single-coil hum at −40 dBFS.
// At min gain AND at default gain, the 60 Hz component must stay well below the
// note in the output.
//
// REGRESSION FLOOR: ≥ 28 dB below the note. Measured (2026-07, 48 kHz):
//   rat  min −36.0 / def −41.6 dB     sd1  min −33.1 / def −39.3 dB
//   ts   min −33.1 / def −38.2 dB     muff min −47.5 / def −55.4 dB (re-measured
//        2026-07-31 twice: docs §49's series base resistors, then docs §53's
//        DC-blocked diode branch — still 19+ dB inside the bar)
//   gold min −30.1 / def −33.6 dB (§56: the default row was −35.6 under §54; the min
//        row is UNCHANGED and always will be — GAIN 0 is bit-exact by contract)
//        ← the TIGHTEST at min, and necessarily so: at GAIN 0
//        this pedal is a LINEAR buffer, so it can only PRESERVE the input's own
//        −30 dB hum-to-note ratio (−30.1 measured == the physical ceiling for a
//        transparent pedal, 2.1 dB of margin). Turning up still improves it, but by
//        2.0 dB LESS than it used to, and that is docs §56 being honest: §50's
//        stand-in for the drive path's input network was a 600 Hz one-pole high-pass,
//        which threw away 7 dB more 41 Hz than the reference divider actually does, so
//        it was flattering this row. The real network passes more hum to the clipper.
// Context: the INPUT hum sits 30 dB below the note, and a perfectly linear pedal
// preserves that — so ~−33 dB at min drive (near-linear SD-1/TS) is the physical
// ceiling of what "min gain" can do; compression + input filtering IMPROVE it at
// default drive. Tightest today is −33.1 dB → a 28 dB bar leaves 5.1 dB margin
// while still catching a Pi-class blowout outright (pre-fix the Pi measured the
// hum at ~−1 dB vs the note — a 30+ dB regression against this floor).
// ---------------------------------------------------------------------------
void testHumTorture(const std::vector<Gear>& gear) {
    constexpr double kHumBarDb = -28.0;
    auto note = sine(220.0, 0.316f, 1.5, kFs);           // −10 dBFS note
    const auto hum = sine(60.0, 0.01f, 1.5, kFs);        // −40 dBFS single-coil hum
    for (size_t i = 0; i < note.size(); ++i) note[i] += hum[i];
    for (const auto& g : gear) {
        if (!isDirtPedal(g)) continue;
        double meas[2] = {0, 0};
        int mi = 0;
        for (float gainVal : {0.0f, -1.0f}) {  // min, then default
            auto vals = defaults(g);
            for (size_t k = 0; k < g.knobs.size(); ++k)
                if (g.knobs[k].kind == KnobSpec::GAIN && gainVal >= 0.0f) vals[k] = gainVal;
            std::vector<float> out;
            g.render(vals, note, out);
            assert(allFinite(out) && "hum-torture render produced NaN/inf");
            const double n220 = toneLevel(out, 220.0, kFs);
            const double h60 = toneLevel(out, 60.0, kFs);
            meas[mi++] = toDb(h60 / (n220 + 1e-30));
        }
        std::printf("  [A2] %-5s hum vs note: min-gain %6.1f dB, default %6.1f dB (bar %.0f)\n",
                    g.name.c_str(), meas[0], meas[1], kHumBarDb);
        std::fflush(stdout);
        assert(meas[0] < kHumBarDb && meas[1] < kHumBarDb &&
               "HUM TORTURE: 60 Hz hum not >=28 dB below the note (the Pi-class blowout)");
    }
    std::printf("  [ok] A2 hum torture: 60 Hz stays ≥%.0f dB below the note on every dirt "
                "pedal at min AND default gain\n", -kHumBarDb);
}

// ---------------------------------------------------------------------------
// A.3 — KNOB MONOTONICITY SPOT-CHECKS.
//   GAIN knobs at lo/mid/hi → monotonically non-decreasing THD (dirt) or output
//   level (level-style pots); TONE knobs at extremes → the spectral centroid /
//   band level moves in the documented direction.
//
// Measured (2026-07, 48 kHz, references for future failures):
//   GAIN THD %:  rat 0.0→22.4→37.1   sd1 0.1→11.5→36.7   ts 0.1→5.7→31.9
//                muff 0.4→32.0→35.7 (probes 0/0.6/1.0; §49's series base resistors
//                made the wall ARTICULATE — max fell 147.7 → 38.5 because the
//                fundamental survives — and §53's DC-blocked diode branch gave the
//                stages a real bias to clip around, so the knob BOTTOM finally
//                cleans up: 2.6 → 0.4 % at SUSTAIN 0, with kClipDriveMax un-fitted
//                6.0 → 1.0 and the taper floor re-derived −70 → −65)
//                jcm800 0.2→6.9→48.5 (was 0.2→10.8→48.5 post-§47, and 0.0→9.3→48.5
//                post-§45; §51 re-derived the GAIN taper — knob 0.5 now delivers
//                the drive the k=4 law delivered at 0.40, so the mid-knob THD
//                falls while GAIN 1.0 is bit-identical: taper(1) = 1 for any k)
//                gold 0.0→4.2→13.1 (0.0 % at GAIN 0 is the crossfade: the clipped
//                half is switched OUT, not merely quiet. §50: was 0.0→23.4→30.6 —
//                the gang law A = 1+422k/((1−g)·100k+17k) spans 4.6→25.8×, 6.1×
//                at the 0.35 default, where the linear law hit 24.3× there — the
//                real unit's knob-0.99 drive at the shipped default. §52: 0.0→2.3
//                →15.3 → 0.0→3.6→19.6, the derived summing weight raising the
//                dirt's share of the sum. §54: → 0.0→4.2→13.1 — the reference's
//                germanium fit clamps 8.4 dB lower and the 495 Hz summing pole
//                takes the harmonics down harder than the fundamental, while the
//                drive amp's C7 network raises the mid-knob drive: max THD falls,
//                mid-knob THD rises slightly. §56: → 0.0→3.7→12.3, the netlist
//                input divider replacing §50's stand-in — it attenuates the 220 Hz
//                probe 0.6-0.8 dB MORE than the stand-in did, so slightly less
//                drive reaches the diodes at every knob position. REPORTED, not
//                aimed at: the owner had said the GOLD is "potentially still a bit
//                gainy by a touch but let's not change for now", and this moves in
//                that direction by accident of the component values)
//   LEVEL dBFS (0/0.5/1): rat −240→−15.6→−9.6   sd1 −240→−12.8→−6.8
//                ts −240→−14.9→−8.9   muff −240→−6.4→−0.4 (§53: was −7.0→−1.0)
//                clean120 −240→−25.9→−23.0   jcm master −240→−21.9→−7.5 (§51: the
//                master pot's own law is untouched at k = 4 — what moved is the
//                GAIN drive reaching it at the 0.5 default)
//                twin −240→−34.5→−16.3   ac30 −240→−17.8→−9.7 (docs §23 second
//                amendment: the AC30 volume travel moved up ~12 dB when the
//                starved phase inverter was fixed — the knob reaches the EL84s now.
//                §55 leaves the TRAVEL essentially alone: the dynamic supply costs the
//                clean end ~3.4 dB and the driven end 0.28 dB, so the knob's authority
//                grew rather than moved.)
//   TONE, pedals (HF harmonic energy dB, dark→bright, gain maxed; bar ≥ +6):
//                rat filter −19.8→−10.6 (inverted knob)   sd1 −16.0→−4.2
//                ts −17.4→−6.2   muff −12.8→−4.1 (§53: was −5.0→+10.1 — the clip
//                stages' 470 pF Miller caps now work against a real base-node
//                impedance, so the pedal is less shrill overall; same +8.7 dB of
//                knob authority)   gold treble −29.4→−18.9
//                (§50: was −17.9→−5.7 — less clipped harmonic energy overall at
//                max gain, same +10.6 dB of knob authority. §52: −25.9→−15.3 →
//                −22.9→−12.3, +3.0 dB from the derived summing weight. §54:
//                → −29.4→−18.9, i.e. −6.5 dB of HF harmonic energy, which is the
//                495 Hz summing pole doing exactly the job it is in the circuit
//                for. §56: → −30.2→−19.7, the netlist input divider's slightly
//                colder 220 Hz drive. The knob's authority is STILL exactly
//                +10.6 dB, at every one of the FIVE re-baselines — it is a linear
//                tilt after the nonlinearity, so nothing upstream can move it)
//   TONE, amps  (3 kHz level dB, dark→bright; bar ≥ +4):
//                clean120 treble −36.7→−27.3   jcm800 treble −29.3→−14.1 (§51)
//                twin treble −52.3→−21.0   ac30 treble −45.1→−21.8
//                ac30 CUT −35.9→−25.5 (inverted knob: 1 = darker — docs §23)
// ---------------------------------------------------------------------------
void testKnobMonotonicity(const std::vector<Gear>& gear) {
    for (const auto& g : gear) {
        // -- GAIN knobs --------------------------------------------------------
        for (size_t i = 0; i < g.knobs.size(); ++i) {
            const auto& k = g.knobs[i];
            if (k.kind != KnobSpec::GAIN) continue;
            // 2026-07-25: this was `g.isPedal ? 0.1f : 0.1f` — two identical branches, i.e.
            // a ternary that reads as if pedals and amps are probed at different levels
            // while doing nothing. 0.1 V is right for both (a realistic single-coil pluck),
            // so the branch is gone rather than the level changed.
            const auto drive = sine(220.0, 0.1f, 1.0, kFs);
            double th[3];
            int idx = 0;
            for (float v : {k.monoLo, k.monoMid, k.monoHi}) {
                auto vals = defaults(g);
                vals[i] = v;
                std::vector<float> out;
                g.render(vals, drive, out);
                assert(allFinite(out));
                th[idx++] = thdOf(out, 220.0, kFs);
            }
            std::printf("  [A3] %-9s %-10s THD %5.1f%% -> %5.1f%% -> %5.1f%%\n", g.name.c_str(),
                        k.name, th[0] * 100, th[1] * 100, th[2] * 100);
            std::fflush(stdout);
            // 2026-07-25: this asserted `th[1] >= th[0] * 0.95`, which permits a 5 % THD
            // DECREASE under the banner "non-decreasing". Turning a gain knob UP must not
            // make a device cleaner — that is a wiring/taper inversion, and 5 % of slack is
            // exactly enough to hide a small one. The contract is now literal (>=), with a
            // tiny ABSOLUTE epsilon for float noise in the Goertzel rather than a
            // proportional one that scales with the measurement it is meant to protect.
            constexpr double kThdEps = 1e-6;  // 0.0001 % THD — far below any real knob step
            assert(th[1] >= th[0] - kThdEps && th[2] >= th[1] - kThdEps &&
                   "GAIN knob THD DECREASED as the knob opened (taper or wiring inverted)");
            // And it must actually DO something across its travel, not merely fail to
            // decrease: a knob wired to nothing satisfies non-decreasing perfectly.
            assert(th[2] > th[0] + 1e-3 &&
                   "GAIN knob barely changed THD across its travel (dead control?)");
        }
        // -- LEVEL knobs (level must not decrease as the pot opens) ------------
        for (size_t i = 0; i < g.knobs.size(); ++i) {
            const auto& k = g.knobs[i];
            if (k.kind != KnobSpec::LEVEL) continue;
            const auto drive = sine(220.0, 0.1f, 0.6, kFs);
            double lv[3];
            int idx = 0;
            for (float v : {0.0f, 0.5f, 1.0f}) {
                auto vals = defaults(g);
                vals[i] = v;
                std::vector<float> out;
                g.render(vals, drive, out);
                assert(allFinite(out));
                lv[idx++] = rmsOf(out);
            }
            std::printf("  [A3] %-9s %-10s level %6.1f -> %6.1f -> %6.1f dBFS\n", g.name.c_str(),
                        k.name, toDb(lv[0]), toDb(lv[1]), toDb(lv[2]));
            std::fflush(stdout);
            // 2026-07-25: `lv[1] >= lv[0]` compared a real level against lv[0], which is
            // ~1e-12 (a closed pot is silence) — true for any lv[1] whatsoever, including a
            // level knob that does nothing at all. And `lv[2] >= lv[1] * 0.98` again let the
            // output DROP as the pot opened. Now: the pot must be genuinely closed at 0,
            // must not lose level as it opens, and must gain REAL level across its travel.
            assert(lv[0] < 1e-6 && "LEVEL knob at 0 is not silent (the pot does not close)");
            assert(lv[2] >= lv[1] && "LEVEL knob output DROPPED from 0.5 to 1.0 (taper "
                                     "inverted, or the top of the travel is dead)");
            // The top half of the travel must be WORTH SOMETHING. Bar: +2 dB from noon to
            // fully open — clearly audible, and it fails a knob whose upper half is dead,
            // which is a real class of defect in this codebase (the audit measured JCM800
            // BASS at "+9.5 dB lower half / +0.2 dB upper half"). Measured across the rig:
            // rat/muff/gold +6.0, jcm master +14.4 (was +9.5 pre-§51 — a less-driven
            // power section decompresses, so the master's top half is worth MORE now),
            // ac30 +8.1, twin +18.2, and clean120 the
            // tightest at +2.9 dB (its volume runs into a compressive output stage).
            assert(lv[2] > lv[1] * 1.2589 &&
                   "LEVEL knob gained under 2 dB from noon to fully open (dead top half)");
        }
        // -- TONE knobs (extremes move the spectrum the documented way) --------
        for (size_t i = 0; i < g.knobs.size(); ++i) {
            const auto& k = g.knobs[i];
            if (k.kind != KnobSpec::TONE) continue;
            if (g.isPedal) {
                // Nonlinear device: high-band energy of the harmonic series it
                // creates, probed with the gain knob MAXED (a rich series) so the
                // tone stage has spectrum to work on.
                const auto drive = sine(220.0, 0.15f, 1.0, kFs);
                double hf[2];
                int idx = 0;
                for (float v : {0.0f, 1.0f}) {
                    auto vals = defaults(g);
                    for (size_t q = 0; q < g.knobs.size(); ++q)
                        if (g.knobs[q].kind == KnobSpec::GAIN) vals[q] = 1.0f;
                    vals[i] = v;
                    std::vector<float> out;
                    g.render(vals, drive, out);
                    hf[idx++] = hfHarmonicDb(out, 220.0, kFs);
                }
                const double hi = k.dirUp ? hf[1] : hf[0];
                const double lo = k.dirUp ? hf[0] : hf[1];
                std::printf("  [A3] %-9s %-10s HF harmonics %6.1f dB (dark) -> %6.1f dB (bright)\n",
                            g.name.c_str(), k.name, lo, hi);
                std::fflush(stdout);
                assert(hi > lo + 6.0 &&
                       "TONE knob extremes do not move the high-band harmonic energy >=6 dB "
                       "in the documented direction");
            } else {
                // Near-linear amp at defaults: probe the 3 kHz band directly.
                auto levelAt3k = [&](float v) {
                    auto vals = defaults(g);
                    vals[i] = v;
                    std::vector<float> out;
                    const auto probe = sine(3000.0, 0.05f, 0.5, kFs);
                    g.render(vals, probe, out);
                    return toDb(toneLevel(out, 3000.0, kFs));
                };
                const double at0 = levelAt3k(0.0f), at1 = levelAt3k(1.0f);
                const double bright = k.dirUp ? at1 : at0;
                const double dark = k.dirUp ? at0 : at1;
                std::printf("  [A3] %-9s %-10s 3 kHz %6.1f dB (dark) -> %6.1f dB (bright)\n",
                            g.name.c_str(), k.name, dark, bright);
                std::fflush(stdout);
                assert(bright > dark + 4.0 &&
                       "amp TONE knob extremes do not move the 3 kHz band >=4 dB in the "
                       "documented direction");
            }
        }
    }
    std::printf("  [ok] A3 knob monotonicity: every gain/level/tone knob moves its promised "
                "metric the promised way\n");
}

// ---------------------------------------------------------------------------
// A.4 — LEVEL SANITY. No gear at its opening defaults may swing the RMS of the
// standard −20 dBFS pluck by more than ±12 dB. This is the "no balls" /
// "blows the mix apart" guard: a new pedal or amp voice that opens 20 dB quiet
// (the RAT field bug) or 15 dB hot fails here before a player ever hears it.
//
// Measured Δ RMS at defaults (2026-07, 48 kHz, pluck peak −20 dBFS / RMS −33.5):
//   rat +18.6   sd1 +15.6   ts +13.1   muff +27.7 (§53: was +29.4; the sustain wall lifts a
//   DECAYING pluck's RMS by design — a fuzz that did NOT would be the bug)
//   clean120 −6.0   jcm800 −2.5 (docs §51)   twin −18.8   ac30 −7.5 (0.4 opening volume;
//   docs §55 re-baselined this — see the note in allGear(). The §55 supply slice moved it
//   −3.4 dB, all of it on the CLEAN end of the knob: at VOLUME 0.70 the same probe moves
//   0.28 dB. The recorded "+1.6" had rotted — the pre-§55 code measures −4.1 — and the
//   twin's recorded "−13.8" has rotted the same way to −18.8, which is NOT this slice's
//   doing and is left for whoever next touches the Twin.)
// One symmetric global bound cannot hold that honest spread, so the windows are
// PER GEAR (measured ± ~10 dB, set in allGear()) — tight enough that the next
// "no balls" (−20 dB drift) or blowout still fails loudly.
// ---------------------------------------------------------------------------
void testLevelSanity(const std::vector<Gear>& gear) {
    const auto pluck = standardPluck(1.5, kFs);
    const double inDb = toDb(rmsOf(pluck));
    for (const auto& g : gear) {
        std::vector<float> out;
        g.render(defaults(g), pluck, out);
        const double d = toDb(rmsOf(out)) - inDb;
        std::printf("  [A4] %-9s default-rig RMS delta %+6.1f dB (window %+.0f..%+.0f)\n",
                    g.name.c_str(), d, g.lvlDeltaLo, g.lvlDeltaHi);
        std::fflush(stdout);
        assert(d > g.lvlDeltaLo && d < g.lvlDeltaHi &&
               "LEVEL SANITY: gear at defaults left its documented level window (the next "
               "'no balls' / 'blows the mix apart')");
    }
    std::printf("  [ok] A4 level sanity: all gear inside its documented default-level window\n");
}

// --- A5 — THE DIRT LINEUP'S STAGING SURVIVES THE OUTPUT-POT LAWS. ------------
//
// Added 2026-08-10 (docs §67). This is the bar the whole output-pot slice turns
// on, and it exists because §66 REFUSED to fix one pedal on exactly this ground.
//
// The argument, as §66.3 made it: the RAT's LEVEL pot is logarithmic and the
// model mapped it linear. Substituting the real law on the RAT ALONE measures
// -11.61 dBFS at the unity-trim probe, which makes it the QUIETEST of the five
// and reproduces the pre-§36 staging to within 0.4 dB — i.e. a correct knob law
// would have silently undone §36's diode fix. The cure was never "leave it
// linear", it was "move all five together", and this test is what proves the
// cure worked rather than asserting that it did.
//
// So: the RAT must NOT be the quietest of the five at the shipped defaults. It
// measures -11.61 dBFS — the SAME absolute number §66 measured for the one-pedal
// version — and yet it holds RANK 2 OF 5, because its siblings moved too. The
// absolute level is not the property; the rank is.
//
// The spread is PRINTED, not asserted. §67.5 records why: it widens 4.5 -> 8.0 dB
// and the cause is the shipped OUTPUT DEFAULTS, which were chosen against linear
// pots and are now the wrong coordinates. That is a named follow-up, not a
// failure of the laws, and snugging a bar around it would hide it.
void testDirtLineupStaging(const std::vector<Gear>& gear) {
    // The docs §66.2 probe, exactly: 220 Hz at the 0.15 V unity trim, 48 kHz,
    // shipped defaults, tail RMS.
    const double f0 = 220.0;
    const size_t n = static_cast<size_t>(kFs);
    std::vector<float> tone(n);
    for (size_t i = 0; i < n; ++i)
        tone[i] = 0.15f * static_cast<float>(std::sin(kTwoPi * f0 * i / kFs));

    const char* kDirt[] = {"rat", "sd1", "ts", "muff", "gold"};
    double ratDb = 0.0, lo = 1e9, hi = -1e9;
    int quieterThanRat = 0, dirtSeen = 0;
    std::printf("  [A5] dirt lineup at shipped defaults, 220 Hz / 0.15 V / 48 kHz:\n");
    for (const char* want : kDirt) {
        for (const auto& g : gear) {
            if (g.name != want) continue;
            std::vector<float> out;
            g.render(defaults(g), tone, out);
            // Skip the smoothing transient, exactly as the pedal suites do.
            const size_t skip = std::min(out.size(), static_cast<size_t>(0.2 * kFs));
            double acc = 0.0;
            size_t cnt = 0;
            for (size_t i = skip; i < out.size(); ++i) { acc += double(out[i]) * out[i]; ++cnt; }
            const double db = toDb(cnt ? std::sqrt(acc / cnt) : 0.0);
            std::printf("       %-5s %7.2f dBFS\n", want, db);
            if (g.name == "rat") ratDb = db;
            else ++dirtSeen, quieterThanRat += (db < ratDb ? 1 : 0);
            lo = std::min(lo, db);
            hi = std::max(hi, db);
            break;
        }
    }
    // `quieterThanRat` is only meaningful once the RAT has been measured, and it
    // is first in kDirt, so every sibling is compared against a real number.
    assert(dirtSeen == 4 && "the dirt lineup did not resolve five pedals");
    assert(quieterThanRat > 0 &&
           "the RAT is the QUIETEST of the five dirt pedals at the shipped "
           "defaults — that is the pre-§36 staging returning through a knob law, "
           "which is exactly what docs §66.3 refused to ship. Some sibling's "
           "output pot has gone back to a linear map.");
    std::printf("  [ok] A5 lineup staging: RAT at %.2f dBFS with %d of 4 siblings "
                "below it (§36 holds); spread %.1f dB — RECORDED, not asserted "
                "(docs §67.5: the OUTPUT defaults are the follow-up)\n",
                ratDb, quieterThanRat, hi - lo);
}

// ---------------------------------------------------------------------------
// B — LIVE-CONVENTION TESTING. Render every C-ABI entry point twice:
//   ref : separate in/out buffers, ONE call over the whole signal (the way tests
//         find convenient), fresh instance;
//   live: out == in (IN-PLACE), 128-frame blocks at 48 kHz (the way the WORKLET
//         actually calls it — see web/worklet/clipper-processor.js), fresh instance.
// The two must agree to float-accumulation tolerance. This is the test that
// would have caught the in-place cab-convolver aliasing bug.
// ---------------------------------------------------------------------------

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    assert(a.size() == b.size());
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    return m;
}

// 2026-07-25 (test/assert-real-properties). This helper's tolerance used to be 2e-5 with a
// comment about "float accumulation across ~60k samples of tube solves", and every single
// unit it drove measured a max |Δ| of EXACTLY 0.000e+00. That was not luck: every unit
// already chunks internally at exactly 128, so a 128-frame outer loop cannot produce a
// different internal call pattern from one big call. The tolerance was ~94 dB looser than
// the property needed, so the test could not have failed short of gross corruption — it was
// structurally vacuous, in the same way audit finding 3's `testConvolverChunking` was.
//
// It is now two different, both-real assertions depending on the block size:
//
//   ALIGNED (a multiple of the internal 128-sample chunk) -> BIT-IDENTICAL, tol 0.0.
//       This is what is actually true, and pinning it at 0 makes it a regression detector.
//
//   RAGGED (not a multiple) -> the only segmentation that can catch a block-size bug at
//       all. Finding 3's CabConvolver was exact at 128 and produced an error LARGER than
//       the signal at 100; the old 128-only test could not see it. Measured now, at 100 and
//       127 frames:
//         * phaser, all four amp voices, CabConvolver and ReverbModel are EXACTLY 0 at
//           every block size from 1 to 256 -> asserted bit-identical here too.
//         * the five DIRT PEDALS diverge during the startup ramp and then CONVERGE (four
//           to exactly 0, the Muff to 3.3e-05 — see kRaggedTailBar). How long that takes
//           is set by the slowest recursive state the
//           differing parameter trajectory excites, not by the smoother: four of them are
//           inside 1.0e-3 relative by ~50 ms, and the GOLD needs ~300 ms because docs §56
//           put a 1 uF/15 k (~30 ms) corner in its drive path. That is the audit's
//           Medium/DSP item
//           "control-rate parameter sampling defeats the 5 ms smoother at DAW block sizes":
//           the smoother advances per sample but only its CHUNK-END value is kept, so the
//           parameter TRAJECTORY during the initial snap depends on the chunk size. Worst
//           case measured: Muff, 1.6 absolute at 25 ms. So the SETTLED output is asserted
//           for real, the CONVERGENCE is asserted separately and much harder (the bar that
//           actually rules out finding 3's class), and the startup transient is an XFAIL
//           naming that item.
struct LiveDiff {
    double whole = 0.0;     // max |Δ| over the entire render
    double settled = 0.0;   // max |Δ| past kBlockSettleSecs
    double relative = 0.0;  // settled, relative to the reference's own peak there
    double tail = 0.0;      // max |Δ| over the last kBlockTailFrac, relative
};
// The settle window. RE-DERIVED 2026-07-31 (docs §56) — it was 0.05 s, justified as
// "past every parameter smoother's ~5-8 ms settle", and that derivation was measurably
// INCOMPLETE. What has to settle is not the smoother: it is the slowest RECURSIVE STATE
// that the differing parameter trajectory excites on its way through. The GOLD's §56
// drive-path input divider carries a 1 uF cap (C16) across R19 = 15 k, a ~30 ms time
// constant — five times anything the old 0.65*HP600 stand-in had — so its trajectory
// difference needs ~7 tau to wash out, not one. Measured on the GOLD, 100-frame blocks
// vs one big call, max |Δ| relative to the reference's own peak in 10 ms buckets:
//     50 ms 1.3e-2 | 110 ms 1.4e-3 | 200 ms 1.3e-5 | 300 ms 2.0e-7 | 400 ms EXACTLY 0
// so 0.25 s is ~3 orders of magnitude inside the bar for the slowest unit in the file.
constexpr double kBlockSettleSecs = 0.25;

// The settled bar: 2e-3 relative is about -54 dB, tight enough that finding 3's class of
// bug (error larger than the signal) is caught by three orders of magnitude, and loose
// enough to cover the residual smoother-trajectory difference.
constexpr double kRaggedSettledBar = 2e-3;

// ...and because widening a window can only ever make a test easier, the same change
// adds a bar that makes it HARDER overall, and which is the one that actually separates
// "smoothed differently" from "stream corrupted": the difference must CONVERGE. A
// finding-3-class defect (a permanently misaligned stream) never decays, so it fails
// this at any window length, where a trajectory transient decays to nothing.
// Measured over the final quarter of every render in this file: EXACTLY 0.0 for every
// unit — all four amp voices, the phaser, the convolver, the reverb, and four of the
// five dirt pedals — with ONE exception, the Muff at 3.34e-05. That exception is
// physics and it is already documented: docs §53 put a 1 uF cap in series with the
// clip stages' diodes, and it can only discharge through the diodes' own leakage
// (tau ~ 5 s), so a trajectory difference cannot wash out inside a 1 s render. The bar
// is therefore 1e-4: 20x tighter than the settled bar, 3x above the one unit that is
// not exactly zero, and unreachable by a stream that never converges at all.
constexpr double kBlockTailFrac = 0.25;
constexpr double kRaggedTailBar = 1e-4;

LiveDiff liveVsRef(const char* name, const std::vector<float>& in,
                   const std::function<void(const float*, float*, int)>& refProc,
                   const std::function<void(const float*, float*, int)>& liveProc,
                   int blockSize, bool expectBitExact,
                   const clipper::test::XfailDecl* startupXfail) {
    const int n = static_cast<int>(in.size());
    std::vector<float> ref(in.size(), 0.0f);
    refProc(in.data(), ref.data(), n);          // separate buffers, one big call
    std::vector<float> live = in;               // IN-PLACE at the host quantum
    for (int off = 0; off < n; off += blockSize) {
        const int c = std::min(blockSize, n - off);
        liveProc(live.data() + off, live.data() + off, c);
    }
    assert(allFinite(live) && "live-convention render produced NaN/inf");

    LiveDiff d;
    d.whole = maxAbsDiff(ref, live);
    const int st = std::min(n, static_cast<int>(kBlockSettleSecs * kFs));
    double pk = 0.0;
    for (int i = st; i < n; ++i) {
        d.settled = std::max(d.settled, std::fabs(static_cast<double>(ref[i]) - live[i]));
        pk = std::max(pk, std::fabs(static_cast<double>(ref[i])));
    }
    d.relative = pk > 0.0 ? d.settled / pk : 0.0;
    // Convergence: the same comparison over the FINAL kBlockTailFrac of the render.
    const int tailStart = n - static_cast<int>(kBlockTailFrac * n);
    double tailD = 0.0, tailPk = 0.0;
    for (int i = std::max(0, tailStart); i < n; ++i) {
        tailD = std::max(tailD, std::fabs(static_cast<double>(ref[i]) - live[i]));
        tailPk = std::max(tailPk, std::fabs(static_cast<double>(ref[i])));
    }
    d.tail = tailPk > 0.0 ? tailD / tailPk : 0.0;

    std::printf("  [B ] %-24s in-place/%4d-frame vs big-block: max |Δ| %.3e (settled %.3e = "
                "%.2e rel, tail %.2e rel)\n", name, blockSize, d.whole, d.settled,
                d.relative, d.tail);
    std::fflush(stdout);

    if (expectBitExact) {
        assert(d.whole == 0.0 &&
               "LIVE-CONVENTION: in-place blocked processing is not BIT-IDENTICAL to "
               "separate-buffer big-block processing at a 128-aligned block size");
        return d;
    }
    // Ragged. The settled output must be block-size independent for real.
    assert(pk > 0.0 && "no settled signal to compare");
    assert(d.relative <= kRaggedSettledBar &&
           "RAGGED BLOCK SIZE: the SETTLED output depends on the host block size — the "
           "stream is being corrupted, not merely smoothed differently (finding 3's class)");
    assert(d.tail <= kRaggedTailBar &&
           "RAGGED BLOCK SIZE: the block-size difference does NOT converge — it is still "
           "there at the end of the render, so it is a misaligned stream (finding 3's "
           "class), not a parameter-trajectory transient (docs §56)");
    if (startupXfail) {
        char detail[256];
        std::snprintf(detail, sizeof detail,
                      "%s at %d-frame blocks: max |Δ| %.3e during the first %.0f ms (settled "
                      "%.2e rel, bar for the whole render is %.0e)",
                      name, blockSize, d.whole, 1000.0 * kBlockSettleSecs, d.relative,
                      kRaggedSettledBar);
        clipper::test::expectXfail(d.whole <= kRaggedSettledBar * pk, *startupXfail, detail);
    } else {
        assert(d.whole == 0.0 &&
               "this unit is bit-identical at every block size from 1 to 256; it just "
               "stopped being (a new block-size dependence)");
    }
    return d;
}

void testLiveConvention() {
    // 1.2 s of the standard pluck (long enough to cover cab partition boundaries,
    // reverb tails building up, and the tube solvers' warm-start paths). Trimmed by 37
    // samples so the length (57563 at 48 kHz) is a multiple of NEITHER block size below —
    // every pass therefore ends on a partial block, which is its own code path.
    const auto full = standardPluck(1.2, kFs);
    const std::vector<float> pluck(full.begin(), full.end() - 37);

    // 128 is the worklet's render quantum: BIT-IDENTICAL is required. 100 is RAGGED — not a
    // multiple of the internal 128-sample chunk, and the size at which audit finding 3's
    // CabConvolver bug produced an error LARGER than the signal while the 128-only test
    // passed. Any unit that keeps hidden state indexed by its own chunk counter fails at
    // 100 and not at 128, which is exactly why both are here.
    struct BlockCase { int block; bool bitExact; };
    constexpr BlockCase kBlocks[] = {{128, true}, {100, false}};

    // -- pedal C ABIs (defaults, 4× oversampling — the worklet's settings) -----
    struct PedalAbi {
        const char* name;
        void* (*create)(float);
        void (*destroy)(void*);
        void (*setParam)(void*, int, float);
        void (*setOs)(void*, int);
        void (*process)(void*, const float*, float*, int);
        float p0, p1, p2;
        // Non-null = this unit's output depends on the host block size during the initial
        // smoother snap (the audit's control-rate parameter-sampling item). Null = exact at
        // every block size, asserted bit-identical even at a ragged one.
        const clipper::test::XfailDecl* startupXfail;
    };
    const PedalAbi pedals[] = {
        {"rat_ ABI", rat_create, rat_destroy, rat_set_param, rat_set_oversampling,
         rat_process, 0.7f, 0.4f, 0.8f, &kXfControlRateBlockSize},
        {"sd_ ABI", sd_create, sd_destroy, sd_set_param, sd_set_oversampling,
         sd_process, 0.5f, 0.5f, 0.7f, &kXfControlRateBlockSize},
        {"ts_ ABI", ts_create, ts_destroy, ts_set_param, ts_set_oversampling,
         ts_process, 0.5f, 0.5f, 0.75f, &kXfControlRateBlockSize},
        {"muff_ ABI", muff_create, muff_destroy, muff_set_param, muff_set_oversampling,
         muff_process, 0.6f, 0.5f, 0.6f, &kXfControlRateBlockSize},
        {"gold_ ABI", gold_create, gold_destroy, gold_set_param, gold_set_oversampling,
         gold_process, 0.35f, 0.5f, 0.7f, &kXfControlRateBlockSize},
        // The phaser has no per-chunk parameter sampling: exact at every block size.
        {"phaser_ ABI", phaser_create, phaser_destroy, phaser_set_param, nullptr,
         phaser_process, 0.35f, 0.5f, 0.5f, nullptr},
    };
    for (const auto& p : pedals) {
        auto mk = [&]() {
            void* h = p.create(static_cast<float>(kFs));
            if (p.setOs) p.setOs(h, 4);
            p.setParam(h, 0, p.p0);
            p.setParam(h, 1, p.p1);
            p.setParam(h, 2, p.p2);
            return h;
        };
        for (const auto& bc : kBlocks) {
            void* hRef = mk();
            void* hLive = mk();
            liveVsRef(p.name, pluck,
                      [&](const float* i, float* o, int n) { p.process(hRef, i, o, n); },
                      [&](const float* i, float* o, int n) { p.process(hLive, i, o, n); },
                      bc.block, bc.bitExact, p.startupXfail);
            p.destroy(hRef);
            p.destroy(hLive);
        }
    }

    // -- the AmpChain C ABI, every voice, cab ON + reverb engaged --------------
    // (reverb 0.25 so the spring path is exercised in-place too; chorus stays at
    // the shipped default OFF — its stereo path is covered below).
    auto mkAmp = [&](int model) {
        void* h = amp_create(static_cast<float>(kFs));
        amp_set_model(h, model);
        amp_set_cab_builtin(h, model == 1 ? 1 : 0);  // JCM pairs with brit412
        amp_set_param(h, kAmpVolume, 0.4f);
        amp_set_param(h, kAmpBass, 0.5f);
        amp_set_param(h, kAmpMid, 0.5f);
        amp_set_param(h, kAmpTreble, 0.6f);
        amp_set_param(h, kAmpBright, 0.0f);
        amp_set_param(h, kAmpCab, 1.0f);
        amp_set_param(h, kAmpSpeed, 0.3f);
        amp_set_param(h, kAmpDepth, 0.5f);
        amp_set_param(h, kAmpChorusMode, 0.0f);
        amp_set_param(h, kAmpReverb, 0.25f);
        amp_set_param(h, kAmpJcmGain, 0.5f);
        amp_set_param(h, kAmpJcmPresence, 0.5f);
        amp_set_param(h, kAmpJcmMaster, 0.4f);
        return h;
    };
    const char* ampNames[] = {"amp_ ABI clean120", "amp_ ABI jcm800", "amp_ ABI twin",
                              "amp_ ABI ac30"};
    // All four amp voices are bit-identical at every block size (measured 1..256): the valve
    // amps have no parameter smoothing at all (audit finding 6), and the clean amp's
    // smoothers are advanced per sample without a per-chunk resample. So no XFAIL here —
    // ragged sizes are asserted bit-identical too.
    for (int model = 0; model < 4; ++model) {
        for (const auto& bc : kBlocks) {
            void* hRef = mkAmp(model);
            void* hLive = mkAmp(model);
            liveVsRef(ampNames[model], pluck,
                      [&](const float* i, float* o, int n) { amp_process(hRef, i, o, n); },
                      [&](const float* i, float* o, int n) { amp_process(hLive, i, o, n); },
                      bc.block, bc.bitExact, nullptr);
            amp_destroy(hRef);
            amp_destroy(hLive);
        }
    }

    // -- the STEREO amp path (the worklet's real render call), chorus ON -------
    // in_ptr may not alias the outputs (documented), so the live convention here
    // is DISTINCT out_l/out_r at 128 frames vs one big block.
    {
        const int n = static_cast<int>(pluck.size());
        auto mkC = [&]() {
            void* h = mkAmp(0);
            amp_set_param(h, kAmpChorusMode, 1.0f);  // chorus ON -> genuinely stereo
            return h;
        };
        for (const auto& bc : kBlocks) {
            void* hRef = mkC();
            void* hLive = mkC();
            std::vector<float> refL(pluck.size(), 0.0f), refR(pluck.size(), 0.0f);
            amp_process_stereo(hRef, pluck.data(), refL.data(), refR.data(), n);
            std::vector<float> liveL(pluck.size(), 0.0f), liveR(pluck.size(), 0.0f);
            for (int off = 0; off < n; off += bc.block) {
                const int c = std::min(bc.block, n - off);
                amp_process_stereo(hLive, pluck.data() + off, liveL.data() + off,
                                   liveR.data() + off, c);
            }
            const double dL = maxAbsDiff(refL, liveL), dR = maxAbsDiff(refR, liveR);
            std::printf("  [B ] %-24s %4d-frame vs big-block stereo: max |Δ| L %.3e R %.3e\n",
                        "amp_ ABI stereo+chorus", bc.block, dL, dR);
            assert(allFinite(liveL) && allFinite(liveR));
            // Bit-identical at BOTH sizes, ragged included, and the two channels must be
            // checked separately: the chorus makes them genuinely different signals, so a
            // block-size bug in one channel's delay-line indexing cannot hide in the other.
            assert(dL == 0.0 && dR == 0.0 &&
                   "stereo amp path is not BIT-IDENTICAL between blocked and big-block "
                   "rendering");
            amp_destroy(hRef);
            amp_destroy(hLive);
        }
    }

    // -- the cab convolver + reverb, bare (the exact units the worklet chains) --
    // CabConvolver: BIT-IDENTICAL at a ragged size too, since fix/cab-block-size. This is
    // the assertion audit finding 3 needed and did not have.
    for (const auto& bc : kBlocks) {
        const auto ir = clipper::dsp::generateDefaultCab2x12IR(kFs);
        clipper::dsp::CabConvolver cabRef, cabLive;
        cabRef.prepare(kFs, ir.data(), static_cast<int>(ir.size()), kFs, 128);
        cabLive.prepare(kFs, ir.data(), static_cast<int>(ir.size()), kFs, 128);
        liveVsRef("CabConvolver", pluck,
                  [&](const float* i, float* o, int n) { cabRef.process(i, o, n); },
                  [&](const float* i, float* o, int n) { cabLive.process(i, o, n); }, bc.block,
                  bc.bitExact, nullptr);
    }
    for (const auto& bc : kBlocks) {
        clipper::dsp::ReverbModel rvRef, rvLive;
        rvRef.prepare(kFs);
        rvRef.setMix(0.5f);
        rvLive.prepare(kFs);
        rvLive.setMix(0.5f);
        liveVsRef("ReverbModel", pluck,
                  [&](const float* i, float* o, int n) { rvRef.process(i, o, n); },
                  [&](const float* i, float* o, int n) { rvLive.process(i, o, n); }, bc.block,
                  bc.bitExact, nullptr);
    }
    std::printf("  [ok] B live-convention: BIT-IDENTICAL in-place at the worklet's 128-frame "
                "quantum for every entry point; at a RAGGED 100 frames the phaser, all four "
                "amp voices, CabConvolver and ReverbModel are still bit-identical and the "
                "five dirt pedals agree to <= %.0e relative once settled\n",
                kRaggedSettledBar);
}

// ---------------------------------------------------------------------------
// C — GOLDEN PROGRAM RENDERS. The "first five minutes" rigs, at the app's
// DEFAULT knobs, rendered through the SAME C-ABI chain the worklet drives and
// compared against committed 16-bit mono 48 kHz references:
//   1. rat_jcm800      RAT → JCM800 + brit412 cab
//   2. sd1_twin_reverb SD-1 → Twin + clean212 cab + spring reverb 0.25
//   3. muff_twin       Muff → CLEAN Twin + clean212 cab (the Gilmour move)
//   4. ts_ac30         Screamer → AC30 + clean212 cab
//   5. clean120_chorus clean 120 + chorus ON + clean212 cab (stereo → mono mix)
//
// Gate: per-third-octave-band RMS difference ≤ 1.5 dB on every band whose golden
// level is within 55 dB of the golden's loudest band (quieter bands are noise;
// ≥ 6 live bands required — measured 7..13 per rig), plus broadband RMS within
// ±1.0 dB. 16-bit storage quantization stays well below the quietest compared
// band — irrelevant to a 1.5 dB gate (measured round-trip worst band Δ 0.11 dB,
// the quantization + windowing floor of the gate itself). Lossless
// refactors (solver iteration-order changes, chunking changes) pass; voicing
// drift (a re-tapered knob, a re-voiced shelf, a changed IR) fails.
//
// Regenerating goldens is DELIBERATE ONLY, and goes through
//   bash scripts/update-goldens.sh
// which requires a clean tree, prints the per-golden dB deltas, requires a
// confirmation typed at a terminal, and records a justification in
// core/tests/goldens/GOLDENS.md. The raw
//   ./build/clipper_player_expectations_tests --update-goldens
// still exists (the script drives it) but bypasses all of that — don't.
// ---------------------------------------------------------------------------

struct RigRender {
    const char* name;
    std::vector<float> audio;
};

std::vector<float> renderPedalChain(
    void* (*create)(float), void (*destroy)(void*), void (*setParam)(void*, int, float),
    void (*setOs)(void*, int), void (*process)(void*, const float*, float*, int),
    float p0, float p1, float p2, const std::vector<float>& in) {
    void* h = create(static_cast<float>(kFs));
    if (setOs) setOs(h, 4);
    setParam(h, 0, p0);
    setParam(h, 1, p1);
    setParam(h, 2, p2);
    std::vector<float> out(in.size(), 0.0f);
    process(h, in.data(), out.data(), static_cast<int>(in.size()));
    destroy(h);
    return out;
}

// The amp chain at rig.ts AMP_KNOB_DEFAULTS, with the given voice/cab/reverb.
std::vector<float> renderAmpChain(int model, int cab, float reverb, int chorusMode,
                                  const std::vector<float>& in) {
    void* h = amp_create(static_cast<float>(kFs));
    amp_set_model(h, model);
    amp_set_cab_builtin(h, cab);
    amp_set_param(h, kAmpVolume, 0.4f);
    amp_set_param(h, kAmpBass, 0.5f);
    amp_set_param(h, kAmpMid, 0.5f);
    amp_set_param(h, kAmpTreble, 0.6f);
    amp_set_param(h, kAmpBright, 0.0f);
    amp_set_param(h, kAmpCab, 1.0f);
    amp_set_param(h, kAmpSpeed, 0.3f);
    amp_set_param(h, kAmpDepth, 0.5f);
    amp_set_param(h, kAmpChorusMode, static_cast<float>(chorusMode));
    amp_set_param(h, kAmpReverb, reverb);
    amp_set_param(h, kAmpJcmGain, 0.5f);
    amp_set_param(h, kAmpJcmPresence, 0.5f);
    amp_set_param(h, kAmpJcmMaster, 0.4f);
    std::vector<float> out(in.size(), 0.0f);
    if (chorusMode > 0) {
        // The chorus path is stereo; the golden stores the mono mix (L+R)/2.
        std::vector<float> l(in.size(), 0.0f), r(in.size(), 0.0f);
        amp_process_stereo(h, in.data(), l.data(), r.data(), static_cast<int>(in.size()));
        for (size_t i = 0; i < in.size(); ++i) out[i] = 0.5f * (l[i] + r[i]);
    } else {
        amp_process(h, in.data(), out.data(), static_cast<int>(in.size()));
    }
    amp_destroy(h);
    return out;
}

std::vector<RigRender> renderFirstFiveMinutesRigs() {
    // 2.0 s standard pluck — long enough for attack + body + decaying tail
    // through reverb, short enough to keep 5 goldens < 1 MB total.
    const auto pluck = standardPluck(2.0, kFs);
    std::vector<RigRender> rigs;
    rigs.push_back({"rat_jcm800",
        renderAmpChain(1, 1, 0.0f, 0,
            renderPedalChain(rat_create, rat_destroy, rat_set_param, rat_set_oversampling,
                             rat_process, 0.7f, 0.4f, 0.8f, pluck))});
    rigs.push_back({"sd1_twin_reverb",
        renderAmpChain(2, 0, 0.25f, 0,
            renderPedalChain(sd_create, sd_destroy, sd_set_param, sd_set_oversampling,
                             sd_process, 0.5f, 0.5f, 0.7f, pluck))});
    rigs.push_back({"muff_twin",
        renderAmpChain(2, 0, 0.0f, 0,
            renderPedalChain(muff_create, muff_destroy, muff_set_param,
                             muff_set_oversampling, muff_process, 0.6f, 0.5f, 0.6f, pluck))});
    rigs.push_back({"ts_ac30",
        renderAmpChain(3, 0, 0.0f, 0,
            renderPedalChain(ts_create, ts_destroy, ts_set_param, ts_set_oversampling,
                             ts_process, 0.5f, 0.5f, 0.75f, pluck))});
    rigs.push_back({"clean120_chorus", renderAmpChain(0, 0, 0.0f, 1, pluck)});
    return rigs;
}

std::string goldenPath(const char* rigName) {
#ifndef CLIPPER_GOLDEN_DIR
#error "CLIPPER_GOLDEN_DIR must be defined by the build (core/CMakeLists.txt)"
#endif
    return std::string(CLIPPER_GOLDEN_DIR) + "/" + rigName + ".wav";
}

void writeGolden(const RigRender& rig) {
    drwav_data_format fmt;
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 1;
    fmt.sampleRate = static_cast<drwav_uint32>(kFs + 0.5);
    fmt.bitsPerSample = 16;
    drwav wav;
    const std::string path = goldenPath(rig.name);
    if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr)) {
        std::fprintf(stderr, "error: cannot write golden %s\n", path.c_str());
        std::exit(1);
    }
    std::vector<drwav_int16> pcm(rig.audio.size());
    drwav_f32_to_s16(pcm.data(), rig.audio.data(), rig.audio.size());
    drwav_write_pcm_frames(&wav, pcm.size(), pcm.data());
    drwav_uninit(&wav);
    std::printf("  [C ] wrote golden %s (%zu frames, peak %.3f, rms %6.1f dBFS)\n",
                path.c_str(), rig.audio.size(), peakOf(rig.audio), toDb(rmsOf(rig.audio)));
}

// The 16-bit storage quantization + windowing floor of the gate itself: a render
// written and read straight back measures this much and no more. Anything at or
// below it means "the same audio", which is what makes it the right threshold for
// deciding whether a golden is actually being CHANGED by a re-bless.
constexpr double kQuantizationFloorDb = 0.15;

enum class GoldenStatus { Ok, Missing, FormatDrift, LengthDrift };

struct GoldenDelta {
    GoldenStatus status = GoldenStatus::Ok;
    double rmsDeltaDb = 0.0;
    double worstBandDb = 0.0;
    double worstHz = 0.0;
    int bandsCompared = 0;
    drwav_uint64 goldenFrames = 0;
};

// Measure a fresh render against the golden ON DISK. Pure measurement: it asserts
// nothing about the gate, so it can be used both to enforce the gate (compareGolden)
// and to report what a re-bless would change (--golden-report / --update-goldens).
GoldenDelta measureAgainstGolden(const RigRender& rig) {
    GoldenDelta d;
    // A fresh render must not blow up before we even get to voicing. True in every
    // mode, including update: never bless a NaN.
    assert(allFinite(rig.audio) && "rig render produced NaN/inf");

    const std::string path = goldenPath(rig.name);
    unsigned int ch = 0, sr = 0;
    drwav_uint64 frames = 0;
    float* raw = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &frames,
                                                         nullptr);
    if (!raw) {
        d.status = GoldenStatus::Missing;
        return d;
    }
    d.goldenFrames = frames;
    if (ch != 1 || sr != static_cast<unsigned>(kFs)) {
        drwav_free(raw, nullptr);
        d.status = GoldenStatus::FormatDrift;
        return d;
    }
    if (frames != rig.audio.size()) {
        drwav_free(raw, nullptr);
        d.status = GoldenStatus::LengthDrift;
        return d;
    }
    std::vector<float> gold(raw, raw + frames);
    drwav_free(raw, nullptr);

    d.rmsDeltaDb = toDb(rmsOf(rig.audio)) - toDb(rmsOf(gold));

    // Per-third-octave-band comparison on every band within 55 dB of the golden's
    // loudest band (quieter bands are noise).
    const BandSpectrum a = thirdOctaveSpectrum(gold, kFs);
    const BandSpectrum b = thirdOctaveSpectrum(rig.audio, kFs);
    assert(a.centerHz.size() == b.centerHz.size());
    double loudest = -1e9;
    for (double v : a.rmsDb) loudest = std::max(loudest, v);
    for (size_t i = 0; i < a.centerHz.size(); ++i) {
        if (a.rmsDb[i] < loudest - 55.0) continue;  // noise-floor band — skip
        const double delta = std::fabs(b.rmsDb[i] - a.rmsDb[i]);
        if (delta > d.worstBandDb) { d.worstBandDb = delta; d.worstHz = a.centerHz[i]; }
        ++d.bandsCompared;
    }
    return d;
}

const char* statusWord(const GoldenDelta& d) {
    switch (d.status) {
        case GoldenStatus::Missing:     return "NEW";
        case GoldenStatus::FormatDrift: return "FORMAT-DRIFT";
        case GoldenStatus::LengthDrift: return "LENGTH-DRIFT";
        case GoldenStatus::Ok: break;
    }
    return d.worstBandDb <= kQuantizationFloorDb ? "UNCHANGED" : "CHANGED";
}

// One machine-readable line per rig, parsed by scripts/update-goldens.sh to build
// the reviewer's before/after table. Keep the field order stable:
//   GOLDEN-DELTA <name> <status> <rmsDb> <worstBandDb> <worstHz> <bands>
void printDeltaLine(const RigRender& rig, const GoldenDelta& d) {
    std::printf("GOLDEN-DELTA %s %s %+.2f %.2f %.0f %d\n",
                rig.name, statusWord(d), d.rmsDeltaDb, d.worstBandDb, d.worstHz,
                d.bandsCompared);
    std::fflush(stdout);
}

// The voicing gate.
void compareGolden(const RigRender& rig) {
    const GoldenDelta d = measureAgainstGolden(rig);
    if (d.status == GoldenStatus::Missing) {
        std::fprintf(stderr,
                     "GOLDEN MISSING: %s — run with --update-goldens (deliberately) first\n",
                     goldenPath(rig.name).c_str());
        assert(false && "golden render file missing");
        return;
    }
    assert(d.status != GoldenStatus::FormatDrift && "golden format drifted");
    assert(d.status != GoldenStatus::LengthDrift && "golden length drifted");

    std::printf("  [C ] %-16s vs golden: rms Δ %+5.2f dB, worst band Δ %.2f dB @ %.0f Hz "
                "(%d bands compared)\n", rig.name, d.rmsDeltaDb, d.worstBandDb, d.worstHz,
                d.bandsCompared);
    std::fflush(stdout);
    assert(std::fabs(d.rmsDeltaDb) < 1.0 && "GOLDEN: broadband RMS drifted > 1 dB");
    assert(d.bandsCompared >= 6 && "golden comparison degenerated (too few live bands)");
    assert(d.worstBandDb <= 1.5 &&
           "GOLDEN: a third-octave band drifted > 1.5 dB — the rig's VOICE changed. If "
           "deliberate, regenerate via scripts/update-goldens.sh and commit with a "
           "justification in core/tests/goldens/GOLDENS.md.");
}

// --- The three golden modes.
//
// Until 2026-07-25 there were two, and the update one was broken: it did
//     if (update) writeGolden(r);
//     compareGolden(r);
// i.e. it compared the fresh render against THE FILE IT HAD JUST WRITTEN. That can
// only ever measure 16-bit quantization (≤0.11 dB) against a 1.5 dB gate, so it was
// a guaranteed pass that silently rewrote the references — and because
// compareGolden also asserts the frame count, a change that altered the render
// LENGTH sailed through too. The measurement now always happens against the
// PREVIOUS golden, before anything is written.

// Report-only: measure against the committed goldens and print, write nothing,
// assert no gate. This is what scripts/update-goldens.sh runs BEFORE asking for
// confirmation, so the reviewer sees the dB deltas of a re-bless in advance.
void reportGoldenDeltas() {
    const auto rigs = renderFirstFiveMinutesRigs();
    for (const auto& r : rigs) printDeltaLine(r, measureAgainstGolden(r));
    std::printf("  [--] golden report only: nothing written.\n");
}

void testGoldenRenders(bool update) {
    const auto rigs = renderFirstFiveMinutesRigs();
    for (const auto& r : rigs) {
        if (!update) {
            compareGolden(r);
            continue;
        }
        // Measure against the PREVIOUS golden first — that delta is the whole
        // point of a re-bless — then overwrite.
        printDeltaLine(r, measureAgainstGolden(r));
        writeGolden(r);
        // Now that the reference IS this render, a round-trip must land within the
        // storage floor. This is a check of the WRITE PATH (wav format, bit depth,
        // channel count), NOT the voicing gate: it is bounded by 16-bit
        // quantization by construction and can never catch drift.
        const GoldenDelta after = measureAgainstGolden(r);
        assert(after.status == GoldenStatus::Ok && "golden write-back is unreadable");
        assert(after.worstBandDb <= kQuantizationFloorDb &&
               "golden write path is lossy beyond 16-bit quantization");
    }
    if (update) {
        std::printf("  [C ] rewrote %zu goldens; deltas above are vs the PREVIOUS goldens\n",
                    rigs.size());
    } else {
        std::printf("  [ok] C golden renders: %zu first-five-minutes rigs within the "
                    "per-third-octave ±1.5 dB voicing gate\n", rigs.size());
    }
}

// Known defects this binary exercises under XFAIL. Printed by --xfail-ledger, which ctest
// surfaces as ***Skipped in its default summary (see core/CMakeLists.txt).
const clipper::test::XfailDecl kLedger[] = {kXfControlRateBlockSize};

}  // namespace

int main(int argc, char** argv) {
    const int ledger = clipper::test::ledgerMain(argc, argv, kLedger,
                                                 sizeof kLedger / sizeof kLedger[0],
                                                 "clipper_player_expectations_tests");
    if (ledger >= 0) return ledger;
    clipper::test::requireAssertsLive();
    bool update = false;
    bool report = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--update-goldens")) update = true;
        // Measure the goldens and print the deltas without touching a file.
        // scripts/update-goldens.sh runs this to show the reviewer what a
        // re-bless would change, before asking for confirmation.
        if (!std::strcmp(argv[i], "--golden-report")) report = true;
    }
    assert(!(update && report) && "--update-goldens and --golden-report are exclusive");

    if (report) {
        std::printf("Measuring golden deltas (report only — nothing will be written)...\n");
        reportGoldenDeltas();
        return 0;
    }

    std::printf("Running clipper M11 player-expectations tests (48 kHz — the worklet rate)...\n");
    const auto gear = allGear();
    testMinKnobUsability(gear);
    testHumTorture(gear);
    testKnobMonotonicity(gear);
    testLevelSanity(gear);
    testDirtLineupStaging(gear);
    testLiveConvention();
    // In update mode the blocks above still run: never bless a render that fails
    // min-knob usability or the hum torture.
    testGoldenRenders(update);
    std::printf("All M11 player-expectations tests passed (XFAILs listed below are known open "
                "defects, not regressions).\n");
    return clipper::test::reportXfails();
}
