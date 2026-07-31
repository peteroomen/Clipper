// Measurement harness for the AC30 dynamic-sag slice (docs §55, audit finding 4).
// NOT registered in CMake and NOT in the artifact's compile closure (core/tools/ is
// excluded from the build stamp by design, docs §29.1) — build it ad hoc:
//
//   cmake -S core -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
//   c++ -std=c++17 -O2 -I core/include core/tools/ac30_sag_probe.cpp //       build/libclipper_dsp.a build/libclipper_core.a -o build/ac30_sag_probe
//   ./build/ac30_sag_probe [A|B|B2|C|D|E|E2|F|G|H|I]     (no arg = all)
//
// Sections, and what each one is for:
//   A   idle operating point at 7 figures — the check that a supply change did not move
//       the DC point (this slice's kVsupply is derived to keep it fixed)
//   B   sag depth / bloom / recovery in the shared JCM/Twin burst convention, all three
//       power sections side by side, plus the AC30's Vk swing
//   B2  the same, swept over drive — B alone cannot say whether the bias dynamics reach
//       the published 10.0 → 12.5 V, because 4 V at the PI grid is nowhere near full out
//   C   the PI-grid drive sweep: the brick-wall table
//   D   low-end thickness: 82–220 Hz band level + attack-vs-settled compression under a
//       low-E pluck through the composed amp
//   E   the PROTECTED harmonic voicing, h2..h8 at fixed input (the player's view)
//   E2  the same LEVEL-MATCHED (input bisected to a fixed output f0) — separates "the
//       character changed" from "it got louder"
//   F   fizz at max volume: 5–16 kHz harmonic vs non-harmonic (alias/IM) content
//   G   the cranked peak window, the §42.6 kFullScaleSecV convention, at three rates
//   H   per-frequency fundamental level AND THD — the direct test of "thin low end",
//       which distinguishes a spectral tilt from low-frequency-specific distortion
//   I   the M11 A1/A4 level convention, so a level move is attributable to a knob range
//       rather than guessed at
#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/Ac30PowerAmp.h"
#include "clipper/dsp/Jcm800PowerAmp.h"
#include "clipper/dsp/TwinPowerAmp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <complex>
#include <algorithm>

using namespace clipper::dsp;
static constexpr double kTwoPi = 6.283185307179586;
static double toDb(double r) { return 20.0 * std::log10(std::max(r, 1e-30)); }

// Goertzel-style single-bin magnitude over a window.
static double binMag(const std::vector<float>& x, int i0, int n, double f, double fs) {
    std::complex<double> acc(0.0, 0.0);
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * i / (n - 1));  // Hann
        acc += std::complex<double>(x[(size_t)(i0 + i)] * w, 0.0) *
               std::exp(std::complex<double>(0.0, -kTwoPi * f * i / fs));
    }
    return 2.0 * std::abs(acc) / (0.5 * n);
}

static double rms(const std::vector<float>& x, int i0, int n) {
    double s = 0.0;
    for (int i = 0; i < n; ++i) { const double v = x[(size_t)(i0 + i)]; s += v * v; }
    return std::sqrt(s / n);
}

// ---------------------------------------------------------------------------
// A. Idle operating point (absolute references: Vk 10 V, Icath 200 mA, CT 342 V)
// ---------------------------------------------------------------------------
static void opPoint(double fs) {
    Ac30PowerAmp pa; pa.prepare(fs, 128); pa.setOversampling(4);
    const double iq = pa.tubeQuiescentPlateCurrent(), ig = pa.tubeQuiescentScreenCurrent();
    const double iTot = 4.0 * (iq + ig);
    std::printf("[A] idle @%.0f: Vk=%.7f V  Icath_total=%.7f mA  rail(CT)=%.7f V  screen=%.7f V "
                " Ip/tube=%.5f mA  Ig2/tube=%.5f mA\n",
                fs, pa.cathodeIdle(), 1000.0 * iTot, pa.railIdle(), pa.screenIdle(),
                1000.0 * iq, 1000.0 * ig);
    std::printf("    published AC30 reference: Vk 10.0 V, Icath 200 mA, CT 342 V, screens 325.5 V\n");
}

// ---------------------------------------------------------------------------
// B. Sag depth / bloom / recovery in the JCM/Twin conventions + Vk swing
// ---------------------------------------------------------------------------
struct SagResult { double depthDb, bloomMs, recMs, railIdle, railMin, vkIdle, vkMax, attack, settled; };

template <class Amp>
static SagResult sagBurst(Amp& amp, double fs, double amp_v, bool probeVk) {
    amp.setParameter(Amp::PARAM_DRIVE, 1.0f);
    const double f0 = 400.0;
    const int nsil = (int)(0.02 * fs), nb = (int)(0.30 * fs), nt = (int)(0.25 * fs);
    const int n = nsil + nb + nt;
    std::vector<float> in((size_t)n, 0.0f), out((size_t)n, 0.0f);
    for (int i = 0; i < nb; ++i)
        in[(size_t)(nsil + i)] = (float)(amp_v * std::sin(kTwoPi * f0 * i / fs));
    std::vector<double> rail, vkv;
    int off = 0;
    while (off < n) {
        const int b = std::min(32, n - off);
        amp.process(in.data() + off, out.data() + off, b);
        rail.push_back(amp.railNow());
        if (probeVk) vkv.push_back(((Ac30PowerAmp&)amp).cathodeNow());
        off += b;
    }
    const int per = (int)(fs / f0);
    std::vector<double> env;
    for (int i = nsil; i + per <= nsil + nb; i += per) {
        double pk = 0.0;
        for (int j = 0; j < per; ++j) pk = std::max(pk, std::fabs((double)out[(size_t)(i + j)]));
        env.push_back(pk);
    }
    double attack = 0.0;
    for (size_t k = 0; k < env.size() && k < 3; ++k) attack = std::max(attack, env[k]);
    const double settled = env.back();
    SagResult r{};
    r.depthDb = toDb(attack / settled);
    r.attack = attack; r.settled = settled;
    int atkIdx = 0;
    for (size_t k = 0; k < env.size(); ++k) if (env[k] >= attack - 1e-12) { atkIdx = (int)k; break; }
    int setIdx = atkIdx;
    for (size_t k = (size_t)atkIdx; k < env.size(); ++k)
        if (std::fabs(env[k] - settled) <= 0.05 * settled) { setIdx = (int)k; break; }
    r.bloomMs = 1000.0 * (setIdx - atkIdx) * per / fs;
    double railMin = 1e9; for (double v : rail) railMin = std::min(railMin, v);
    r.railIdle = amp.railIdle(); r.railMin = railMin;
    if (probeVk) { r.vkIdle = ((Ac30PowerAmp&)amp).cathodeIdle();
                   double m = -1e9; for (double v : vkv) m = std::max(m, v); r.vkMax = m; }
    // recovery: rail back to 63.2 % of the way home after burst end
    const int endBlk = (nsil + nb) / 32;
    const double railEnd = rail[(size_t)std::min<size_t>((size_t)endBlk, rail.size() - 1)];
    const double target = railEnd + 0.632 * (amp.railIdle() - railEnd);
    int recBlk = -1;
    for (size_t k = (size_t)endBlk; k < rail.size(); ++k) if (rail[k] >= target) { recBlk = (int)k; break; }
    r.recMs = recBlk > 0 ? 1000.0 * (recBlk - endBlk) * 32 / fs : -1.0;
    return r;
}

static void sagTable(double fs) {
    TwinPowerAmp tpa; tpa.prepare(fs, 128); tpa.setOversampling(4);
    Jcm800PowerAmp jpa; jpa.prepare(fs, 128); jpa.setOversampling(4);
    Ac30PowerAmp apa; apa.prepare(fs, 128); apa.setOversampling(4);
    auto t = sagBurst(tpa, fs, 2.0, false);
    auto j = sagBurst(jpa, fs, 2.0, false);
    auto a = sagBurst(apa, fs, 2.0, true);
    std::printf("[B] sag @%.0f (400 Hz, 2.0 V into DRIVE 1.0 = 4 V at the grid):\n", fs);
    std::printf("    Twin  depth %.2f dB  bloom %6.1f ms  rec %6.1f ms  rail %.1f->%.1f\n",
                t.depthDb, t.bloomMs, t.recMs, t.railIdle, t.railMin);
    std::printf("    JCM   depth %.2f dB  bloom %6.1f ms  rec %6.1f ms  rail %.1f->%.1f\n",
                j.depthDb, j.bloomMs, j.recMs, j.railIdle, j.railMin);
    std::printf("    AC30  depth %.2f dB  bloom %6.1f ms  rec %6.1f ms  rail %.1f->%.1f  "
                "Vk %.2f->%.2f V (published 10.0->12.5)\n",
                a.depthDb, a.bloomMs, a.recMs, a.railIdle, a.railMin, a.vkIdle, a.vkMax);
}

// ---------------------------------------------------------------------------
// B2. Sag / bias-shift vs DRIVE LEVEL. The single-level [B] probe cannot say
//     whether the bias dynamics reach the published 10.0 -> 12.5 V swing, because
//     4 V at the PI grid is nowhere near full output. Sweep it.
// ---------------------------------------------------------------------------
static void sagVsDrive(double fs) {
    std::printf("[B2] AC30 sag vs drive @%.0f (400 Hz burst, DRIVE 1.0 so grid = 2x the listed V):\n"
                "     gridV   depth(dB)  bloom(ms)   Vk idle->max (V)   rail idle->min (V)  outPk\n", fs);
    for (double v : {0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
        Ac30PowerAmp apa; apa.prepare(fs, 128); apa.setOversampling(4);
        auto r = sagBurst(apa, fs, v, true);
        std::printf("     %6.1f  %8.2f  %9.1f   %6.3f -> %6.3f     %7.2f -> %7.2f   %.4f\n",
                    2.0 * v, r.depthDb, r.bloomMs, r.vkIdle, r.vkMax, r.railIdle, r.railMin,
                    r.attack);
    }
}

// ---------------------------------------------------------------------------
// C. Drive sweep — the audit's brick-wall table
// ---------------------------------------------------------------------------
static void driveSweep(double fs) {
    const double vs[] = {0.05, 0.1, 0.2, 0.5, 1.0, 3.0, 8.0, 25.0};
    std::printf("[C] PI-grid drive sweep @%.0f (1 kHz, peak out):\n     V   ", fs);
    for (double v : vs) std::printf("%8.2f", v);
    std::printf("\n    pk   ");
    for (double v : vs) {
        Ac30PowerAmp pa; pa.prepare(fs, 128); pa.setOversampling(4);
        pa.setParameter(Ac30PowerAmp::PARAM_DRIVE, 0.5f);   // ×1
        const int n = (int)(0.25 * fs);
        std::vector<float> in((size_t)n), out((size_t)n);
        for (int i = 0; i < n; ++i) in[(size_t)i] = (float)(v * std::sin(kTwoPi * 1000.0 * i / fs));
        pa.process(in.data(), out.data(), n);
        double pk = 0.0;
        for (int i = n / 2; i < n; ++i) pk = std::max(pk, std::fabs((double)out[(size_t)i]));
        std::printf("%8.3f", pk);
    }
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// D. LOW-END THICKNESS: 82-220 Hz band level + compression ratio under a pluck
//    at VOLUME 0.7 / 0.8 / 0.85 through the COMPOSED amp.
// ---------------------------------------------------------------------------
static void thickness(double fs) {
    std::printf("[D] low-end thickness (composed Ac30Amp, low-E pluck 82.41 Hz + 2nd/3rd partials,\n"
                "    0.30 V peak pick attack with a 900 ms decay; band = 82-220 Hz):\n");
    std::printf("    VOL   bandRMS(dBFS)  attackPk(dBFS)  settled300ms(dBFS)  compress(dB)  fullRMS(dBFS)\n");
    for (float vol : {0.70f, 0.80f, 0.85f}) {
        Ac30Amp amp; amp.prepare(fs, 128); amp.setOversampling(4);
        amp.setParameter(Ac30Amp::PARAM_VOLUME, vol);
        amp.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_TREBLE, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
        const int nsil = (int)(0.05 * fs), n = (int)(1.0 * fs) + nsil;
        std::vector<float> in((size_t)n, 0.0f), out((size_t)n, 0.0f);
        for (int i = 0; i < n - nsil; ++i) {
            const double t = i / fs;
            const double envl = std::exp(-t / 0.9);
            const double s = std::sin(kTwoPi * 82.41 * t) + 0.45 * std::sin(kTwoPi * 164.82 * t) +
                             0.22 * std::sin(kTwoPi * 247.2 * t);
            in[(size_t)(nsil + i)] = (float)(0.30 * envl * s / 1.67);
        }
        // settle smoothers then render
        amp.process(in.data(), out.data(), n);
        // band energy 82-220 Hz over the first 400 ms of the note
        const int b0 = nsil, bn = (int)(0.40 * fs);
        double bandE = 0.0;
        for (double f = 82.0; f <= 220.0; f += 4.0) {
            const double m = binMag(out, b0, bn, f, fs);
            bandE += m * m;
        }
        const double bandRms = std::sqrt(bandE / 2.0);
        // attack peak: first 25 ms; settled: 25 ms window centred 300 ms in
        double atk = 0.0;
        for (int i = nsil; i < nsil + (int)(0.025 * fs); ++i) atk = std::max(atk, std::fabs((double)out[(size_t)i]));
        const double set300 = rms(out, nsil + (int)(0.2875 * fs), (int)(0.025 * fs)) * std::sqrt(2.0);
        std::printf("    %.2f  %12.2f  %14.2f  %18.2f  %12.2f  %13.2f\n",
                    vol, toDb(bandRms), toDb(atk), toDb(set300), toDb(atk / set300),
                    toDb(rms(out, nsil, (int)(0.5 * fs))));
    }
}

// ---------------------------------------------------------------------------
// E. PROTECTED harmonic voicing: h2..h8 at VOLUME 0.6 and 0.85, matched drive
// ---------------------------------------------------------------------------
static void spectrum(double fs) {
    std::printf("[E] PROTECTED harmonic voicing (composed, 220 Hz, 0.15 V peak, knobs noon,\n"
                "    steady-state window 400-900 ms; dBc relative to the fundamental):\n");
    std::printf("    VOL    f0(dBFS)      h2      h3      h4      h5      h6      h7      h8   THD%%\n");
    for (float vol : {0.60f, 0.85f}) {
        Ac30Amp amp; amp.prepare(fs, 128); amp.setOversampling(4);
        amp.setParameter(Ac30Amp::PARAM_VOLUME, vol);
        amp.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_TREBLE, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
        const int n = (int)(1.0 * fs);
        std::vector<float> in((size_t)n), out((size_t)n);
        for (int i = 0; i < n; ++i) in[(size_t)i] = (float)(0.15 * std::sin(kTwoPi * 220.0 * i / fs));
        amp.process(in.data(), out.data(), n);
        const int i0 = (int)(0.40 * fs), nn = (int)(0.50 * fs);
        const double f1 = binMag(out, i0, nn, 220.0, fs);
        std::printf("    %.2f  %8.2f", vol, toDb(f1));
        double hsum = 0.0;
        for (int h = 2; h <= 8; ++h) {
            const double m = binMag(out, i0, nn, 220.0 * h, fs);
            hsum += m * m;
            std::printf("%8.2f", toDb(m / f1));
        }
        std::printf("%7.2f\n", 100.0 * std::sqrt(hsum) / f1);
    }
}

// ---------------------------------------------------------------------------
// E2. The PROTECTED voicing, LEVEL-MATCHED. [E] holds the INPUT fixed, which is the
//     player's view but confounds "the character changed" with "the brick wall came
//     off so it is louder". Here the input is bisected until the fundamental lands on
//     a FIXED output target (the pre-slice [E] levels), so h2..h8 in dBc compare two
//     renders at the same acoustic loudness — the only comparison that can say whether
//     the DISTORTION CHARACTER moved.
// ---------------------------------------------------------------------------
static void spectrumMatched(double fs) {
    std::printf("[E2] PROTECTED voicing, LEVEL-MATCHED (composed, 220 Hz, knobs noon; input bisected\n"
                "     so f0 hits the pre-slice target; dBc re the fundamental):\n");
    std::printf("     VOL  targetF0  inV      f0(dBFS)      h2      h3      h4      h5      h6      h7      h8   THD%%\n");
    struct Case { float vol; double targetDb; };
    for (Case c : {Case{0.60f, -11.32}, Case{0.85f, -8.38}}) {
        auto render = [&](double vin, double& f0Db, double harms[7], double& thd) {
            Ac30Amp amp; amp.prepare(fs, 128); amp.setOversampling(4);
            amp.setParameter(Ac30Amp::PARAM_VOLUME, c.vol);
            amp.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
            amp.setParameter(Ac30Amp::PARAM_TREBLE, 0.5f);
            amp.setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
            amp.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
            const int n = (int)(1.0 * fs);
            std::vector<float> in((size_t)n), out((size_t)n);
            for (int i = 0; i < n; ++i) in[(size_t)i] = (float)(vin * std::sin(kTwoPi * 220.0 * i / fs));
            amp.process(in.data(), out.data(), n);
            const int i0 = (int)(0.40 * fs), nn = (int)(0.50 * fs);
            const double f1 = binMag(out, i0, nn, 220.0, fs);
            f0Db = toDb(f1);
            double hsum = 0.0;
            for (int h = 2; h <= 8; ++h) {
                const double m = binMag(out, i0, nn, 220.0 * h, fs);
                hsum += m * m;
                harms[h - 2] = toDb(m / f1);
            }
            thd = 100.0 * std::sqrt(hsum) / f1;
        };
        double lo = 0.005, hi = 3.0, f0Db = 0.0, thd = 0.0, h[7] = {0};
        for (int it = 0; it < 40; ++it) {
            const double mid = std::sqrt(lo * hi);
            render(mid, f0Db, h, thd);
            if (f0Db < c.targetDb) lo = mid; else hi = mid;
        }
        const double vin = std::sqrt(lo * hi);
        render(vin, f0Db, h, thd);
        std::printf("     %.2f  %8.2f  %.5f  %8.2f", c.vol, c.targetDb, vin, f0Db);
        for (int i = 0; i < 7; ++i) std::printf("%8.2f", h[i]);
        std::printf("%7.2f\n", thd);
    }
}

// ---------------------------------------------------------------------------
// F. FIZZ at max volume: high-band / alias floor (record, not a target)
// ---------------------------------------------------------------------------
static void fizz(double fs) {
    Ac30Amp amp; amp.prepare(fs, 128); amp.setOversampling(4);
    amp.setParameter(Ac30Amp::PARAM_VOLUME, 1.0f);
    amp.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
    amp.setParameter(Ac30Amp::PARAM_TREBLE, 0.5f);
    amp.setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
    amp.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
    const double f0 = 220.0;
    const int n = (int)(1.0 * fs);
    std::vector<float> in((size_t)n), out((size_t)n);
    for (int i = 0; i < n; ++i) in[(size_t)i] = (float)(0.30 * std::sin(kTwoPi * f0 * i / fs));
    amp.process(in.data(), out.data(), n);
    const int i0 = (int)(0.40 * fs), nn = (int)(0.50 * fs);
    const double f1 = binMag(out, i0, nn, f0, fs);
    // high band 5-16 kHz: harmonic bins vs non-harmonic (alias/IM) bins
    double harm = 0.0, nonh = 0.0;
    for (int h = 23; h * f0 <= 16000.0; ++h) {          // 5.06 kHz up
        const double m = binMag(out, i0, nn, h * f0, fs);
        harm += m * m;
    }
    for (double f = 5000.0; f <= 16000.0; f += 37.0) {   // 37 Hz is not a 220 divisor
        const double r = std::fmod(f, f0);
        if (r < 25.0 || r > f0 - 25.0) continue;         // skip near-harmonic bins
        const double m = binMag(out, i0, nn, f, fs);
        nonh += m * m;
    }
    std::printf("[F] fizz @ VOLUME 1.0 (%.0f Hz, 0.30 V): f0 %.2f dBFS; 5-16 kHz harmonic %.2f dBc, "
                "non-harmonic (alias/IM) floor %.2f dBc\n",
                fs, toDb(f1), toDb(std::sqrt(harm) / f1), toDb(std::sqrt(nonh) / f1));
}

// ---------------------------------------------------------------------------
// G. Cranked peak window (the §42.6 kFullScaleSecV convention)
// ---------------------------------------------------------------------------
static void cranked(double fs) {
    Ac30Amp amp; amp.prepare(fs, 128); amp.setOversampling(4);
    amp.setParameter(Ac30Amp::PARAM_VOLUME, 1.0f);
    amp.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
    amp.setParameter(Ac30Amp::PARAM_TREBLE, 0.5f);
    amp.setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
    amp.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
    const int n = (int)(0.5 * fs);
    std::vector<float> in((size_t)n), out((size_t)n);
    for (int i = 0; i < n; ++i) in[(size_t)i] = (float)(0.5 * std::sin(kTwoPi * 110.0 * i / fs));
    amp.process(in.data(), out.data(), n);
    double pk = 0.0;
    for (int i = n / 2; i < n; ++i) pk = std::max(pk, std::fabs((double)out[(size_t)i]));
    std::printf("[G] cranked peak (VOL 1.0, 0.5 V @ 110 Hz) = %.4f  "
                "(secondary V = pk*kFullScaleSecV = %.3f; target window ~0.9)\n",
                pk, pk * Ac30PowerAmp::kFullScaleSecV);
}

// ---------------------------------------------------------------------------
// H. THE LOW-END CLAIM, measured directly. "Thin low end" is a per-frequency gain
//    statement, so measure per-frequency gain: one steady sine at a time, the SAME
//    peak amplitude at every frequency, composed amp, and report the FUNDAMENTAL's
//    output level. A wideband envelope compressor whose attack (2.6 ms) is short
//    against a low-E period (12.1 ms) ducks inside each half-cycle, which costs the
//    LOW notes fundamental level that it does not cost the high ones — so if that was
//    the mechanism, this table tilts.
// ---------------------------------------------------------------------------
static void lowEnd(double fs) {
    std::printf("[H] per-frequency fundamental level (composed Ac30Amp, knobs noon, one sine at a\n"
                "    time, SAME 0.25 V peak input at every frequency; f0 out in dBFS, and dB re the\n"
                "    440 Hz row = the spectral TILT that a 'thin low end' would show as):\n");
    std::printf("    VOL     82.41   110.0   164.8   220.0   440.0   880.0  |  tilt re 440: 82Hz  110Hz  165Hz\n");
    for (float vol : {0.70f, 0.80f, 0.85f}) {
        const double freqs[6] = {82.41, 110.0, 164.81, 220.0, 440.0, 880.0};
        double lv[6], th[6];
        for (int k = 0; k < 6; ++k) {
            Ac30Amp amp; amp.prepare(fs, 128); amp.setOversampling(4);
            amp.setParameter(Ac30Amp::PARAM_VOLUME, vol);
            amp.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
            amp.setParameter(Ac30Amp::PARAM_TREBLE, 0.5f);
            amp.setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
            amp.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
            const int n = (int)(1.0 * fs);
            std::vector<float> in((size_t)n), out((size_t)n);
            for (int i = 0; i < n; ++i)
                in[(size_t)i] = (float)(0.25 * std::sin(kTwoPi * freqs[k] * i / fs));
            amp.process(in.data(), out.data(), n);
            const int i0 = (int)(0.40 * fs), nn = (int)(0.50 * fs);
            const double f1 = binMag(out, i0, nn, freqs[k], fs);
            lv[k] = toDb(f1);
            double hs = 0.0;
            for (int h = 2; h <= 12 && freqs[k] * h < 0.45 * fs; ++h) {
                const double m = binMag(out, i0, nn, freqs[k] * h, fs);
                hs += m * m;
            }
            th[k] = 100.0 * std::sqrt(hs) / f1;
        }
        std::printf("    %.2f  ", vol);
        for (int k = 0; k < 6; ++k) std::printf("%7.2f ", lv[k]);
        std::printf(" | %16.2f %6.2f %6.2f\n", lv[0] - lv[4], lv[1] - lv[4], lv[2] - lv[4]);
        std::printf("     THD%%  ");
        for (int k = 0; k < 6; ++k) std::printf("%7.2f ", th[k]);
        std::printf(" | THD ratio 82Hz/440Hz = %.2fx\n", th[0] / th[4]);
    }
}


// ---------------------------------------------------------------------------
// I. The M11 A1/A4 convention: composed Ac30Amp at the suite's opening defaults
//    (VOLUME 0.4, BASS 0.5, TREBLE 0.6, CUT 0.5, REVERB 0), driven by the same
//    standard pluck (220 Hz + 6 partials, 4 ms attack, peak normalized to 0.1),
//    1.5 s. Reports the RMS the suite reports, so the level move is attributable.
// ---------------------------------------------------------------------------
static void m11Level(double fs) {
    const int n = (int)(1.5 * fs);
    std::vector<float> in((size_t)n), out((size_t)n, 0.0f);
    double pk = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        const double attack = 1.0 - std::exp(-t / 0.004);
        double sgn = 0.0;
        for (int h = 1; h <= 6; ++h)
            sgn += (1.0 / h) * std::exp(-t * (2.0 + 1.2 * h)) * std::sin(kTwoPi * 220.0 * h * t);
        in[(size_t)i] = (float)(attack * sgn);
        pk = std::max(pk, std::fabs((double)in[(size_t)i]));
    }
    for (float& v : in) v *= (float)(0.1 / pk);
    const double inRms = rms(in, 0, n);
    for (float vol : {0.30f, 0.40f, 0.50f, 0.70f}) {
        Ac30Amp amp; amp.prepare(fs, 128); amp.setOversampling(4);
        amp.setParameter(Ac30Amp::PARAM_VOLUME, vol);
        amp.setParameter(Ac30Amp::PARAM_BASS, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_TREBLE, 0.6f);
        amp.setParameter(Ac30Amp::PARAM_TOPCUT, 0.5f);
        amp.setParameter(Ac30Amp::PARAM_REVERB, 0.0f);
        std::fill(out.begin(), out.end(), 0.0f);
        int off = 0;
        while (off < n) { const int b = std::min(128, n - off);
                          amp.process(in.data() + off, out.data() + off, b); off += b; }
        double opk = 0.0; for (float v : out) opk = std::max(opk, (double)std::fabs(v));
        std::printf("[I] M11 pluck @%.0f, VOL %.2f: out rms %.2f dBFS  peak %.4f  "
                    "A4 delta %+.2f dB\n", fs, vol, toDb(rms(out, 0, n)), opk,
                    toDb(rms(out, 0, n)) - toDb(inRms));
    }
}

int main(int argc, char** argv) {
    const double fs = 48000.0;
    std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "A") opPoint(fs);
    if (only.empty() || only == "B") { sagTable(48000.0); sagTable(44100.0); sagTable(96000.0); }
    if (only.empty() || only == "B2") sagVsDrive(fs);
    if (only.empty() || only == "C") driveSweep(fs);
    if (only.empty() || only == "D") thickness(fs);
    if (only.empty() || only == "E") spectrum(fs);
    if (only.empty() || only == "E2") spectrumMatched(fs);
    if (only.empty() || only == "F") fizz(fs);
    if (only.empty() || only == "G") { cranked(44100.0); cranked(48000.0); cranked(96000.0); }
    if (only.empty() || only == "H") lowEnd(fs);
    if (only.empty() || only == "I") m11Level(fs);
    return 0;
}
