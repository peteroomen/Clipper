// Plain-assert tests for the M10.10 Fender tweed Champ 5F1 (docs §72) — the small
// tweed, the lineup's first Fender that distorts, and THE FIRST SINGLE-ENDED OUTPUT
// STAGE IN THE PROJECT.
//
// Covers ChampPreamp (two 12AX7 stages either side of the 1 MΩ log volume pot, and
// NO tone stack — this amp has one knob), ChampPowerAmp (one cathode-biased 6V6GT
// into a small single-ended OT, no phase inverter, no feedback) and the composed
// ChampAmp.
//
// No framework: int main + <cassert>.
//
// THE LOAD-BEARING TESTS ARE testFenderMeasuredVoltages, testSingleEndedH2,
// testScreenFitAgainstDatasheet and testPlateKneeIsPhysical.
//
//   * testFenderMeasuredVoltages compares against an ABSOLUTE external reference —
//     Fender's own published operating point for this amp — rather than against an
//     analytic form derived from the same netlist (docs §29's standing complaint).
//     Only ONE constant (kVsupply) is fitted to it; Vk, Ip, Ik and Vpk are the
//     device card's own predictions and this is what checks them.
//   * testSingleEndedH2 is the voice's acceptance bar and it is TOPOLOGY: with one
//     tube there is no opposite leg, so the even harmonics that a push-pull pair
//     cancels survive. Asserted as a CONTRAST against the balanced Twin on the
//     identical stimulus, because "this amp makes h2" is not by itself a property
//     that distinguishes it from a re-voice.
//   * testScreenFitAgainstDatasheet is audit finding 10 on a third tube, FIXED —
//     and it asserts the contrast against the published fits both reachable sources
//     carry, so nobody "restores" kg2 = 4500 thinking the derived value is the bug.
//   * testPlateKneeIsPhysical is audit finding 9 from the other side.

#include "clipper/dsp/ChampAmp.h"
#include "clipper/dsp/ChampPowerAmp.h"
#include "clipper/dsp/ChampPreamp.h"
#include "clipper/dsp/TwinAmp.h"

#include "support/AssertsLive.h"
#include "support/DcOffset.h"
// NO support/Xfail.h — this suite registers ZERO known-bad properties.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace clipper::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 128;
constexpr double kSr = 48000.0;

// Goertzel amplitude of `f` over x[n0..end). Rectangular window over an integer
// number of periods — the right window for reading HARMONIC amplitudes (§70's
// lesson: pick the window for the quantity you are measuring).
double bin(const std::vector<float>& x, double sr, double f, size_t n0) {
    const double w = 2.0 * kPi * f / sr;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t n = n0; n < x.size(); ++n) {
        const double s = static_cast<double>(x[n]) + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double m = s1 * s1 + s2 * s2 - c * s1 * s2;
    return std::sqrt(m > 0.0 ? m : 0.0) * 2.0 / static_cast<double>(x.size() - n0);
}

std::vector<float> tone(double sr, int n, double f, double amp) {
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * f * i / sr));
    return v;
}

template <typename Amp>
void runBlocks(Amp& a, const std::vector<float>& in, std::vector<float>& out) {
    out.assign(in.size(), 0.0f);
    for (size_t off = 0; off + kBlock <= in.size(); off += kBlock)
        a.process(in.data() + off, out.data() + off, kBlock);
}

double rmsOf(const std::vector<float>& x, size_t n0) {
    double s = 0.0;
    for (size_t i = n0; i < x.size(); ++i) s += double(x[i]) * double(x[i]);
    return std::sqrt(s / double(x.size() - n0));
}

// THD over harmonics 2..8 of f0.
double thdPercent(const std::vector<float>& y, double f0, size_t n0) {
    const double h1 = bin(y, kSr, f0, n0);
    double d = 0.0;
    for (int k = 2; k <= 8; ++k) {
        const double h = bin(y, kSr, f0 * k, n0);
        d += h * h;
    }
    return 100.0 * std::sqrt(d) / h1;
}

void setUpChamp(ChampAmp& a, double sr, double volume) {
    a.prepare(sr, kBlock);
    a.setOversampling(4);
    a.setParameter(ChampAmp::PARAM_VOLUME, static_cast<float>(volume));
    a.setParameter(ChampAmp::PARAM_REVERB, 0.0f);
}

// ---------------------------------------------------------------------------
// 1. FENDER'S OWN MEASURED OPERATING POINT — the absolute external reference.
//
// Fender publish this amp's DC point: 19 V across the 470 Ω cathode resistor
// (= 40.4 mA of cathode current), a plate node of ~305 V and 37 mA of plate
// current, leaving ~3.4 mA of screen current by difference.
//
// EXACTLY ONE constant is fitted to this — kVsupply, which pins the 305 V node.
// Vk, Ip, Ig2 and Vpk below are the DEVICE CARD's own predictions, so this test is
// a real external check on the 6V6 fit rather than a restatement of it. If the card
// or the supply moves, these are the numbers that say whether it moved correctly.
// ---------------------------------------------------------------------------
void testFenderMeasuredVoltages() {
    ChampPowerAmp p;
    p.prepare(kSr, kBlock);

    const double rail = p.railIdle();
    const double vk = p.cathodeIdle();
    const double ip = p.tubeQuiescentPlateCurrent();
    const double ig2 = p.tubeQuiescentScreenCurrent();
    const double ik = ip + ig2;
    const double vpk = rail - vk;

    std::printf("    plate node %8.3f V  (Fender 305)    %6.4fx\n", rail, rail / 305.0);
    std::printf("    cathode    %8.3f V  (Fender  19)    %6.4fx\n", vk, vk / 19.0);
    std::printf("    Ip         %8.3f mA (Fender  37)    %6.4fx\n", ip * 1e3, ip / 0.037);
    std::printf("    Ig2        %8.3f mA (Fender ~3.4)   %6.4fx\n", ig2 * 1e3, ig2 / 0.0034);
    std::printf("    Ik         %8.3f mA (Fender  40.4)  %6.4fx\n", ik * 1e3, ik / 0.0404);
    std::printf("    Vpk        %8.3f V  (Fender 286)    %6.4fx\n", vpk, vpk / 286.0);

    // The pinned one.
    assert(std::fabs(rail - 305.0) < 0.5);
    // The PREDICTIONS. Windows are ±5 % except the screen, whose reference is a
    // by-difference figure rather than a published one — recorded at ±20 %.
    assert(std::fabs(vk / 19.0 - 1.0) < 0.05);
    assert(std::fabs(ip / 0.037 - 1.0) < 0.05);
    assert(std::fabs(ik / 0.0404 - 1.0) < 0.05);
    assert(std::fabs(vpk / 286.0 - 1.0) < 0.05);
    assert(std::fabs(ig2 / 0.0034 - 1.0) < 0.20);

    // Dissipation, against the 6V6GT's own ratings. A Champ runs its tube HOT and
    // that is correct — but not over the line.
    const double pa = vpk * ip, pg2 = vpk * ig2;
    std::printf("    plate diss %8.3f W  (rating 14)     %5.1f %%\n", pa, 100.0 * pa / 14.0);
    std::printf("    screen diss%8.3f W  (rating 2.75)   %5.1f %%\n", pg2, 100.0 * pg2 / 2.75);
    assert(pa < 14.0 && pa > 8.0);
    assert(pg2 < 2.75);
}

// ---------------------------------------------------------------------------
// 2. AUDIT FINDING 10, ON A THIRD TUBE, FIXED.
//
// The Koren form makes Ig2/Ip = (kg1/kg2)/atan(Vp/kvb). The 6V6GT datasheet's
// class-A1 point (Vp 250, Vg2 250, Vg1 −12.5) gives Ip 45 mA and Ig2 5 mA, i.e.
// a TRUE ratio of 0.111. Both reachable published Koren 6V6 fits carry kg2 = 4500,
// which predicts 0.237–0.244 — 2.1–2.2x too high, exactly the pattern finding 10
// measured for the EL84 (0.363) and the 6L6 (0.210).
//
// This asserts BOTH halves: the shipped card lands on the datasheet, AND the
// published kg2 does not — so restoring 4500 goes red rather than passing quietly.
// ---------------------------------------------------------------------------
void testScreenFitAgainstDatasheet() {
    const El34Params card = to6V6(Tube6V6Params{});
    // Datasheet class-A1 point.
    const double vp = 250.0, vg2 = 250.0, vg1 = -12.5;
    const double ip = el34PlateCurrent(vp, vg1, vg2, card);
    const double ig2 = el34ScreenCurrent(vg1, vg2, card);
    std::printf("    datasheet P1: Ip %7.3f mA (45)  Ig2 %6.3f mA (5)  Ig2/Ip %6.4f (0.1111)\n",
                ip * 1e3, ig2 * 1e3, ig2 / ip);
    assert(std::fabs(ip / 0.045 - 1.0) < 0.06);    // plate: the PUBLISHED fit, unmodified
    assert(std::fabs(ig2 / 0.005 - 1.0) < 0.02);   // screen: the DERIVED kg2, pinned here

    // Second datasheet point (Vp 315, Vg2 225, Vg1 −13 -> Ip 34 mA). Different Vp
    // AND different Vg2, so agreeing here is a real check on the SHAPE rather than
    // on the scaling. Errors of opposite sign at the two points is why kg1 was left
    // unmodified rather than trimmed to P1.
    const double ip2 = el34PlateCurrent(315.0, -13.0, 225.0, card);
    std::printf("    datasheet P2: Ip %7.3f mA (34)  %6.4fx\n", ip2 * 1e3, ip2 / 0.034);
    assert(std::fabs(ip2 / 0.034 - 1.0) < 0.06);

    // THE CONTRAST. With the published kg2 the screen is 2.2x the datasheet.
    Tube6V6Params pub{};
    pub.kg2 = kPublished6V6Kg2;
    const El34Params published = to6V6(pub);
    const double ig2Pub = el34ScreenCurrent(vg1, vg2, published);
    const double ipPub = el34PlateCurrent(vp, vg1, vg2, published);
    std::printf("    published kg2=4500: Ig2 %6.3f mA  Ig2/Ip %6.4f  (%4.2fx the datasheet)\n",
                ig2Pub * 1e3, ig2Pub / ipPub, (ig2Pub / ipPub) / 0.1111);
    assert(ig2Pub / ipPub > 0.20);   // i.e. the published fit really is ~2x out
    assert(ig2 / ip < 0.13);         // and ours is not

    // And at THIS AMP's own idle the published fit sits at the tube's screen rating.
    ChampPowerAmp p;
    p.prepare(kSr, kBlock);
    const double vpkIdle = p.railIdle() - p.cathodeIdle();
    const double pg2Pub = vpkIdle * el34ScreenCurrent(-p.cathodeIdle(), vpkIdle, published);
    const double pg2Ours = vpkIdle * p.tubeQuiescentScreenCurrent();
    std::printf("    at idle: screen diss OURS %5.3f W vs PUBLISHED %5.3f W (rating 2.75)\n",
                pg2Ours, pg2Pub);
    assert(pg2Pub > 2.4);      // the finding-10 pathology, reproduced
    assert(pg2Ours < 1.6);     // and absent here
}

// ---------------------------------------------------------------------------
// 3. THE ACCEPTANCE BAR: A SINGLE-ENDED STAGE DOES NOT CANCEL EVEN HARMONICS.
//
// Asserted as a CONTRAST against the balanced Twin on the IDENTICAL stimulus,
// because "the Champ makes 2nd harmonic" is true of every amp here and would not
// distinguish this voice from a re-voice of existing machinery. What distinguishes
// it is that its h2 does not CANCEL, because there is no opposite leg to cancel
// against — and that is topology, not voicing.
// ---------------------------------------------------------------------------
void testSingleEndedH2() {
    const double f0 = 220.0;
    const int n = 48000;
    const std::vector<float> in = tone(kSr, n, f0, 0.15);

    ChampAmp champ;
    setUpChamp(champ, kSr, 0.25);
    std::vector<float> yc;
    runBlocks(champ, in, yc);

    TwinAmp twin;
    twin.prepare(kSr, kBlock);
    twin.setParameter(TwinAmp::PARAM_VOLUME, 0.5f);
    twin.setParameter(TwinAmp::PARAM_BASS, 0.5f);
    twin.setParameter(TwinAmp::PARAM_MID, 0.5f);
    twin.setParameter(TwinAmp::PARAM_TREBLE, 0.5f);
    twin.setParameter(TwinAmp::PARAM_REVERB, 0.0f);
    std::vector<float> yt;
    runBlocks(twin, in, yt);

    const size_t n0 = 24000;
    auto ratioDb = [&](const std::vector<float>& y) {
        const double h1 = bin(y, kSr, f0, n0);
        const double h2 = bin(y, kSr, f0 * 2.0, n0);
        const double h3 = bin(y, kSr, f0 * 3.0, n0);
        return std::make_pair(20.0 * std::log10(h2 / h1), 20.0 * std::log10(h3 / h1));
    };
    const auto c = ratioDb(yc);
    const auto t = ratioDb(yt);
    std::printf("    Champ (single-ended): h2 %7.2f dBc   h3 %7.2f dBc\n", c.first, c.second);
    std::printf("    Twin  (push-pull)   : h2 %7.2f dBc   h3 %7.2f dBc\n", t.first, t.second);
    std::printf("    h2 CONTRAST         : %7.2f dB\n", c.first - t.first);

    // The Champ's h2 must be well above the Twin's leakage. Bar recorded, not
    // snugged — the measurement is printed above so a future change is visible.
    assert(c.first - t.first > 12.0);
    // And h2 must DOMINATE h3 in this amp: an unbalanced stage's signature.
    assert(c.first > c.second);
}

// ---------------------------------------------------------------------------
// 4. AUDIT FINDING 9, FROM THE OTHER SIDE: THE PLATE KNEE IS AT A PHYSICAL CURRENT.
//
// Finding 9 measured that the push-pull sections need ~530 mA from ONE EL34 before
// the plate reaches the knee — twice a real tube's peak cathode current — so
// plate-load saturation, which those headers name as the clipping mechanism, never
// actually happens there. `Vp = rail − (i − iq)·Rload` is a SINGLE-ENDED relation,
// and here it is applied to a genuinely single-ended stage, so the knee should
// arrive inside a 6V6's real capability.
// ---------------------------------------------------------------------------
void testPlateKneeIsPhysical() {
    ChampPowerAmp p;
    p.prepare(kSr, kBlock);
    const double rail = p.railIdle();
    const double vk = p.cathodeIdle();
    const double railK = rail - vk;

    // Drive the grid up from the bias point and find where the plate first falls
    // to the knee (kvb is the Koren knee scale; "at the knee" == Vp <= 2·kvb, i.e.
    // atan() has left its linear region and the tube is running out of plate volts).
    const double kneeV = 2.0 * p.tube().kvb;
    double ipAtKnee = -1.0, vgAtKnee = 0.0;
    for (double vg = -vk; vg <= 5.0; vg += 0.05) {
        double ip = 0.0;
        const double vp = p.plateAtCurrent(vg, railK, railK, ip);
        if (vp <= kneeV) { ipAtKnee = ip; vgAtKnee = vg; break; }
    }
    std::printf("    plate reaches the knee (Vp <= %.1f V) at Ip = %.1f mA, Vgk = %+.2f V\n",
                kneeV, ipAtKnee * 1e3, vgAtKnee);
    assert(ipAtKnee > 0.0);
    // A 6V6GT's peak cathode current capability is in the low hundreds of mA. The
    // knee must arrive well inside that — finding 9's push-pull figure was 530 mA
    // from ONE EL34, which is unphysical.
    assert(ipAtKnee < 0.200);
    assert(ipAtKnee > 0.050);
}

// ---------------------------------------------------------------------------
// 5. THERE IS NO NEGATIVE FEEDBACK — the AC30's anti-NFB catcher, verbatim.
// A render with the (inert) feedback seam on and off must be BIT-IDENTICAL, so a
// later slice cannot quietly add a loop to tidy the distortion up.
// ---------------------------------------------------------------------------
void testNoFeedbackIsBitIdentical() {
    const std::vector<float> in = tone(kSr, 24000, 220.0, 0.30);
    std::vector<float> a, b;

    ChampAmp amp1;
    setUpChamp(amp1, kSr, 0.7);
    amp1.powerMutable().setFeedbackEnabled(false);
    runBlocks(amp1, in, a);

    ChampAmp amp2;
    setUpChamp(amp2, kSr, 0.7);
    amp2.powerMutable().setFeedbackEnabled(true);
    runBlocks(amp2, in, b);

    size_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++diff;
    std::printf("    feedback on vs off: %zu of %zu samples differ\n", diff, a.size());
    assert(diff == 0);
    assert(!amp1.power().feedbackEnabled());
}

// ---------------------------------------------------------------------------
// 6. IT BREAKS UP ALMOST IMMEDIATELY — and it CLEANS UP when you play softer.
//
// Two properties, and the second is the one that matters musically: a Champ has no
// master volume and no tone stack, so the only thing standing between the player
// and the distortion is how hard they hit it.
// ---------------------------------------------------------------------------
void testBreaksUpEarlyAndCleansUp() {
    const double f0 = 220.0;
    auto thdAt = [&](double volume, double level) {
        ChampAmp a;
        setUpChamp(a, kSr, volume);
        const std::vector<float> in = tone(kSr, 48000, f0, level);
        std::vector<float> y;
        runBlocks(a, in, y);
        return thdPercent(y, f0, 24000);
    };

    // At the house unity-trim level, breakup is essentially immediate.
    const double t05 = thdAt(0.05, 0.15);
    const double t10 = thdAt(0.10, 0.15);
    const double t30 = thdAt(0.30, 0.15);
    std::printf("    0.15 V in: VOL 0.05 -> %6.2f %%   0.10 -> %6.2f %%   0.30 -> %6.2f %%\n",
                t05, t10, t30);
    assert(t05 < 5.0);          // there IS a clean setting, but only just
    assert(t10 > 5.0);          // and it is gone by 1 on the dial
    assert(t30 > 20.0);
    assert(t10 > t05 && t30 > t10);   // monotone

    // TOUCH SENSITIVITY: at a fixed volume, playing softer cleans it up. This is
    // the whole point of an amp with no master volume.
    const double hard = thdAt(0.20, 0.15);
    const double soft = thdAt(0.20, 0.05);
    std::printf("    VOL 0.20: hard pick (0.15 V) %6.2f %%   soft (0.05 V) %6.2f %%   ratio %5.2fx\n",
                hard, soft, hard / soft);
    assert(hard / soft > 2.0);

    // And it breaks up FAR earlier than the clean Fender in the lineup. The Twin's
    // documented breakup onset is VOLUME ~0.9 (docs §44); the Champ is past 5 % by
    // 0.10. Asserted as the ordering rather than as two absolute numbers.
    TwinAmp twin;
    twin.prepare(kSr, kBlock);
    twin.setParameter(TwinAmp::PARAM_VOLUME, 0.10f);
    twin.setParameter(TwinAmp::PARAM_BASS, 0.5f);
    twin.setParameter(TwinAmp::PARAM_MID, 0.5f);
    twin.setParameter(TwinAmp::PARAM_TREBLE, 0.5f);
    twin.setParameter(TwinAmp::PARAM_REVERB, 0.0f);
    std::vector<float> yt;
    const std::vector<float> in = tone(kSr, 48000, f0, 0.15);
    runBlocks(twin, in, yt);
    const double twinThd = thdPercent(yt, f0, 24000);
    std::printf("    at VOL 0.10: Champ %6.2f %%  vs  Twin %6.2f %%   (%5.2fx)\n",
                t10, twinThd, t10 / twinThd);
    assert(t10 > 3.0 * twinThd);
}

// ---------------------------------------------------------------------------
// 7. THE PREAMP: two 12AX7s, a BARE audio taper, and NO tone stack.
// ---------------------------------------------------------------------------
void testPreampAndVolumeLaw() {
    ChampPreamp p;
    p.prepare(kSr, kBlock);
    const double ia = (p.rail1a() - p.v1a().quiescentPlateVoltage()) / ChampPreamp::kRa;
    const double ib = (p.rail1b() - p.v1b().quiescentPlateVoltage()) / ChampPreamp::kRa;
    std::printf("    V1A rail %7.2f V plate %7.2f V  Iq %5.3f mA\n",
                p.rail1a(), p.v1a().quiescentPlateVoltage(), ia * 1e3);
    std::printf("    V1B rail %7.2f V plate %7.2f V  Iq %5.3f mA\n",
                p.rail1b(), p.v1b().quiescentPlateVoltage(), ib * 1e3);
    // Both stages carry a 100 kΩ load, so the healthy window is ABOVE the 0.5–0.9 mA
    // band that applies to 220 kΩ-load stages (§63.3's point).
    assert(ia > 0.7e-3 && ia < 1.3e-3);
    assert(ib > 0.7e-3 && ib < 1.3e-3);
    // The decoupling ladder must drop in the transcribed direction: B1 > B2 > B3.
    assert(p.rail1b() > p.rail1a());

    // The VOLUME pot is its own grid leak, so it is the BARE audio taper: the house
    // law puts the wiper at 1/(e^{k/2}+1) = 11.920 % at half rotation (§68's
    // corrected figure — NOT the 12.15 % that appeared in prose).
    p.setParameter(ChampPreamp::PARAM_VOLUME, 0.5f);
    std::printf("    wiper at half rotation: %.5f %% (audio taper: 11.920 %%)\n",
                p.volumeWiper() * 100.0);
    assert(std::fabs(p.volumeWiper() - 0.11920) < 0.0005);
    p.setParameter(ChampPreamp::PARAM_VOLUME, 1.0f);
    assert(std::fabs(p.volumeWiper() - 1.0) < 1e-9);
    p.setParameter(ChampPreamp::PARAM_VOLUME, 0.0f);
    assert(p.volumeWiper() < 1e-9);
}

// ---------------------------------------------------------------------------
// 8. ONE OVERSAMPLING DOMAIN (§63.14) — asserted against Oversampler's OWN
// documented 0/64/72/76 rather than read back from the amp, so a regression to
// per-stage domains (which measured 216 samples) goes red.
// ---------------------------------------------------------------------------
void testOneOversamplingDomain() {
    ChampAmp a;
    a.prepare(kSr, kBlock);
    const int expect[9] = {0, 0, 64, 0, 72, 0, 0, 0, 76};
    for (int f : {1, 2, 4, 8}) {
        a.setOversampling(f);
        std::printf("    %dx -> %d samples\n", f, a.latencySamples());
        assert(a.latencySamples() == expect[f]);
    }
    a.setOversampling(4);
    assert(a.oversampling() == 4);
    assert(a.latencySamples() == 72);
}

// ---------------------------------------------------------------------------
// 9. The house block.
// ---------------------------------------------------------------------------
void testBlockSizeInvariance() {
    const std::vector<float> in = tone(kSr, 12000, 220.0, 0.20);
    ChampAmp a;
    setUpChamp(a, kSr, 0.5);
    std::vector<float> big(in.size(), 0.0f);
    a.process(in.data(), big.data(), static_cast<int>(in.size()));

    ChampAmp b;
    setUpChamp(b, kSr, 0.5);
    std::vector<float> ragged(in.size(), 0.0f);
    // A RAGGED size — the only segmentation that can catch a block-size bug (§30).
    const int step = 100;
    for (size_t off = 0; off + step <= in.size(); off += step)
        b.process(in.data() + off, ragged.data() + off, step);

    double worst = 0.0;
    const size_t settle = static_cast<size_t>(0.25 * kSr);
    for (size_t i = settle; i + step < in.size(); ++i)
        worst = std::max(worst, std::fabs(double(big[i]) - double(ragged[i])));
    std::printf("    ragged 100 vs one big block: worst |diff| %.3e\n", worst);
    assert(worst < 2e-3);
}

void testResetAndNaN() {
    const std::vector<float> in = tone(kSr, 24000, 220.0, 0.20);
    ChampAmp a;
    setUpChamp(a, kSr, 0.5);
    std::vector<float> fresh;
    runBlocks(a, in, fresh);

    // Play it, poison it, reset it, replay it.
    ChampAmp b;
    setUpChamp(b, kSr, 0.5);
    std::vector<float> junk;
    runBlocks(b, in, junk);
    std::vector<float> bad(kBlock, 0.0f);
    bad[10] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> tmp(kBlock, 0.0f);
    b.process(bad.data(), tmp.data(), kBlock);
    b.reset();
    std::vector<float> after;
    runBlocks(b, in, after);

    double worst = 0.0;
    size_t nonFinite = 0;
    for (size_t i = 0; i < fresh.size(); ++i) {
        worst = std::max(worst, std::fabs(double(fresh[i]) - double(after[i])));
        if (!std::isfinite(after[i])) ++nonFinite;
    }
    std::printf("    reset() vs a fresh model: worst |diff| %.3e, non-finite %zu/%zu\n",
                worst, nonFinite, after.size());
    assert(nonFinite == 0);
    assert(worst < 1e-6);
}

void testDcOnSignal() {
    // DC asserted ON SIGNAL, and with a DC-offset input — deleting a coupling cap
    // changes nothing on a clean input (§30).
    for (double offset : {0.0, 0.1}) {
        ChampAmp a;
        setUpChamp(a, kSr, 0.5);
        std::vector<float> in = tone(kSr, 24000, 220.0, 0.20);
        for (auto& v : in) v += static_cast<float>(offset);
        std::vector<float> y;
        runBlocks(a, in, y);
        double sum = 0.0, peak = 0.0;
        const size_t n0 = 12000;
        for (size_t i = n0; i < y.size(); ++i) {
            sum += y[i];
            peak = std::max(peak, std::fabs(double(y[i])));
        }
        const double dc = sum / double(y.size() - n0);
        std::printf("    input offset %+.2f V: DC %+.5f = %.4f %% of peak\n",
                    offset, dc, 100.0 * std::fabs(dc) / peak);
        assert(std::fabs(dc) / peak < 0.02);
    }
}

// ADR 006: the scope is decided BY MEASUREMENT, and the measurement came out
// against the tidy answer — this is the §59 / §69 case. These states would rest at
// exactly zero if their input did, but TriodeStage's grid Newton exits at a
// residual tolerance, so the coupling cap carries that floor and re-excites the OT
// pair every sample. They decay and then PLATEAU ~27 decades above subnormal.
//
// So the bar is NOT "the state is exactly zero" — that would be asserting a
// property this amp does not have. It is what the anti-denormal policy is actually
// FOR: no subnormal float leaves the model. Both halves are asserted, plus the
// plateau's stability (a state that is still DECAYING could still reach subnormal
// later, so "settled" is load-bearing and is checked over a further 30 s).
void testRestingState() {
    ChampAmp a;
    setUpChamp(a, kSr, 0.5);
    const std::vector<float> in = tone(kSr, 24000, 220.0, 0.30);
    std::vector<float> y;
    runBlocks(a, in, y);

    std::vector<float> silence(kBlock, 0.0f), out(kBlock, 0.0f);
    for (int i = 0; i < static_cast<int>(8.0 * kSr / kBlock); ++i)
        a.process(silence.data(), out.data(), kBlock);

    const double resting8 = a.power().maxAbsRestingState();
    auto subnormals = [](const std::vector<float>& v) {
        size_t k = 0;
        for (float x : v)
            if (x != 0.0f && std::fabs(x) < std::numeric_limits<float>::min()) ++k;
        return k;
    };
    double peak = 0.0;
    for (float v : out) peak = std::max(peak, std::fabs(double(v)));
    size_t sub = subnormals(out);

    // A further 30 s: the plateau must be SETTLED, not still decaying — a state on
    // its way down could still reach the subnormal range later.
    for (int i = 0; i < static_cast<int>(30.0 * kSr / kBlock); ++i) {
        a.process(silence.data(), out.data(), kBlock);
        sub += subnormals(out);
    }
    const double resting38 = a.power().maxAbsRestingState();
    std::printf("    plateau: %.4e at 8 s, %.4e at 38 s   output peak %.3e\n",
                resting8, resting38, peak);
    std::printf("    subnormal output samples over the whole silent tail: %zu\n", sub);

    // THE PROPERTY THE POLICY IS ABOUT.
    assert(sub == 0);
    // The plateau is settled (unchanged to within a part in 1e6 over 30 s) ...
    assert(std::fabs(resting38 - resting8) <= 1e-6 * resting8);
    // ... and sits decades above the float subnormal boundary, so the flushes in
    // processSampleOS are guard-rails rather than a fix for a measured cliff.
    assert(resting38 > 1e3 * static_cast<double>(std::numeric_limits<float>::min()));
    // Inaudible by a wide margin regardless: ~-525 dBFS.
    assert(peak < 1e-20);
    // The nonzero-resting nodes are NOT guarded and must be at their real DC point.
    std::printf("    rail %.3f V, cathode %.3f V (real operating points, unguarded)\n",
                a.power().railNow(), a.power().cathodeNow());
    assert(a.power().railNow() > 250.0);
    assert(a.power().cathodeNow() > 10.0);
}

void testRateIndependence() {
    double first = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        ChampAmp a;
        setUpChamp(a, sr, 0.4);
        const std::vector<float> in = tone(sr, static_cast<int>(sr), 220.0, 0.15);
        std::vector<float> y;
        runBlocks(a, in, y);
        const double r = rmsOf(y, static_cast<size_t>(sr / 2));
        const double db = 20.0 * std::log10(r);
        if (first == 0.0) first = db;
        std::printf("    %7.0f Hz: %7.3f dBFS (%+.3f dB)\n", sr, db, db - first);
        assert(std::fabs(db - first) < 0.6);
    }
}

// The power the amp actually makes, REPORTED against its rating (§57.3's rule: do
// not close a shortfall with a constant). The power section's own sine ceiling is
// the reference the composed amp is compared to.
void testRatedPower() {
    // (a) the power section alone, driven with a clean sine.
    ChampPowerAmp p;
    p.prepare(kSr, kBlock);
    p.setOversampling(4);
    const std::vector<float> drive = tone(kSr, 48000, 220.0, 40.0);
    std::vector<float> ys(drive.size(), 0.0f);
    p.process(drive.data(), ys.data(), static_cast<int>(drive.size()));
    const double vsec = rmsOf(ys, 24000) * ChampPowerAmp::kFullScaleSecV;
    const double pSection = vsec * vsec / 8.0;

    // (b) the composed amp, cranked.
    ChampAmp a;
    setUpChamp(a, kSr, 1.0);
    const std::vector<float> in = tone(kSr, 48000, 220.0, 0.30);
    std::vector<float> y;
    runBlocks(a, in, y);
    const double vsec2 = rmsOf(y, 24000) * ChampPowerAmp::kFullScaleSecV;
    const double pAmp = vsec2 * vsec2 / 8.0;

    std::printf("    power section's own sine ceiling: %5.2f W into 8 ohm\n", pSection);
    std::printf("    composed amp, cranked:            %5.2f W  (rated ~5 W)\n", pAmp);
    // The SECTION must reach the tube's rating — that is what says the device card
    // and the load line are right. The COMPOSED figure is lower because the preamp
    // delivers a blocking-limited waveform, and is reported rather than chased.
    assert(pSection > 4.5 && pSection < 6.5);
    assert(pAmp > 3.0);
    // Full-scale normalisation: a cranked render must approach but not exceed 1.0.
    std::printf("    cranked peak (normalised): %.4f\n", double(a.power().lastOutputPeak()));
    assert(a.power().lastOutputPeak() > 0.7 && a.power().lastOutputPeak() < 1.05);
}

}  // namespace

int main() {
    clipper::test::requireAssertsLive();
    std::printf("== Fender tweed Champ 5F1 (M10.10, docs §72) ==\n");
    std::printf("- Fender's own measured operating point (ABSOLUTE reference)\n");
    testFenderMeasuredVoltages();
    std::printf("- the 6V6 screen fit vs the datasheet (audit finding 10)\n");
    testScreenFitAgainstDatasheet();
    std::printf("- BAR: single-ended h2 does not cancel (vs the push-pull Twin)\n");
    testSingleEndedH2();
    std::printf("- the plate knee is at a PHYSICAL current (audit finding 9)\n");
    testPlateKneeIsPhysical();
    std::printf("- there is no negative feedback (bit-identical)\n");
    testNoFeedbackIsBitIdentical();
    std::printf("- breaks up early, and cleans up when you play softer\n");
    testBreaksUpEarlyAndCleansUp();
    std::printf("- the preamp, and the bare audio taper\n");
    testPreampAndVolumeLaw();
    std::printf("- one oversampling domain\n");
    testOneOversamplingDomain();
    std::printf("- block-size invariance (ragged 100)\n");
    testBlockSizeInvariance();
    std::printf("- reset() and NaN recovery\n");
    testResetAndNaN();
    std::printf("- DC on signal\n");
    testDcOnSignal();
    std::printf("- resting state (ADR 006)\n");
    testRestingState();
    std::printf("- rate independence\n");
    testRateIndependence();
    std::printf("- rated power (reported, not chased)\n");
    testRatedPower();
    std::printf("== all Champ tests passed ==\n");
    return 0;
}
