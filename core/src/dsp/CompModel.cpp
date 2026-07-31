// Clipper — portable DSP core.
//
// CompModel: the Dyna Comp / Ross voice as a CompressorConfig. Every number
// below is a component value off the schematic or something derived from
// component values in this file, in the open, with the arithmetic visible. There
// is no fitted constant in this file. Sources and the full derivation are in
// docs §59.
//
// ============================================================================
//  THE NETLIST THIS FILE ENCODES
// ============================================================================
// Reference designators follow the LTspice transcription (docs §59.1); the
// ElectroSmash numbering differs but the values agree.
//
//   Supply         V1 = 9 V; bias divider R1 56 k / R2 27 k -> 2.928 V
//                  (the schematic's own net is named V3P0 — the divider agrees)
//   Input          C2 10 nF -> R4 10 k -> Q1 base (R5 1 M to the bias rail),
//                  Q1 = 2N3904 emitter follower, R6 10 k emitter
//   OTA drive      C3 1 µF -> the CA3080's INVERTING input (R7 1 M to ground).
//                  The NON-inverting input is AC-terminated by R12 15 k ∥ C4
//                  10 nF in series with C5 1 µF, and the two inputs are bridged
//                  by the 2 k offset TRIMMER (R9+R10). That trimmer against the
//                  15 k termination is the whole input attenuator: the CA3080
//                  sees only ~12 % of the signal as a DIFFERENTIAL voltage.
//   Gain cell      U1 CA3080. Iout = Iabc·tanh(Vd / 2Vt)
//   Load           R13 150 k to the bias rail, C6 1 nF across it
//   Splitter       Q2 2N3904, R15 10 k collector, R14 10 k emitter (EQUAL, so
//                  two anti-phase copies of equal amplitude). The EMITTER is the
//                  audio output; BOTH legs feed the detector.
//   Detector       C7/C8 10 nF -> nodes with R16/R17 1 M to ground and D1/D2
//                  1N4148 clamping the NEGATIVE excursion (anodes grounded);
//                  Q3/Q4 2N3904, emitters grounded, collectors on the envelope
//                  node. Full-wave, and a DC restorer rather than a plain
//                  rectifier.
//   Envelope       C9 10 µF, R18 150 k to +9 V.  R18·C9 = 1.5 s  <- the RELEASE
//   Control        Q5 2N3904 emitter follower -> RV1 500 k SUSTAIN rheostat ->
//                  R23 27 k -> the CA3080's Iabc pin (~0.7 V above the negative
//                  rail). R23 alone sets the MAXIMUM control current.
//   Output         C10 50 nF -> R20 10 k -> RV2 50 k LEVEL pot
//
// ============================================================================
//  THE FINDING THAT IS NOT IN ANY PROSE SOURCE
// ============================================================================
// The differential-drive network and the load pole very nearly CANCEL. The
// drive network rises with frequency (C4 shunts the 15 k termination, so more of
// the signal survives as a difference) while the 150 k ∥ 1 nF load falls. Net
// gain-cell response, measured off this config: 100 Hz 0.1176, 1 kHz 0.1238,
// 5 kHz 0.1166, 10 kHz 0.0896 — FLAT within ±0.5 dB from 100 Hz to 5 kHz, then
// a gentle top rolloff. That is why a Dyna Comp is "not very bright" without
// sounding filtered, and it is why neither network may be simplified away
// without the other.

#include "clipper/dsp/CompModel.h"

#include <cmath>

#include "clipper/dsp/CompressorEngine.h"

namespace clipper::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

// --- Component values (the schematic) ---------------------------------------
constexpr double kVsupply = 9.0;       // V1
constexpr double kRdivTop = 56.0e3;    // R1
constexpr double kRdivBot = 27.0e3;    // R2
constexpr double kCin = 10.0e-9;       // C2
constexpr double kRinSeries = 10.0e3;  // R4
constexpr double kRinBias = 1.0e6;     // R5
constexpr double kRe1 = 10.0e3;        // R6
constexpr double kRotaBias = 1.0e6;    // R7 (the inverting input's leg to ground)
constexpr double kRtrim = 2.0e3;       // R9 + R10, the offset trimmer, end to end
constexpr double kRnInvBias = 1.0e6;   // R8
constexpr double kRterm = 15.0e3;      // R12
constexpr double kCterm = 10.0e-9;     // C4
constexpr double kCtermSeries = 1.0e-6;  // C5
constexpr double kRload = 150.0e3;     // R13
constexpr double kCload = 1.0e-9;      // C6
constexpr double kRc2 = 10.0e3;        // R15 (splitter collector)
constexpr double kRe2 = 10.0e3;        // R14 (splitter emitter)
constexpr double kCdet = 10.0e-9;      // C7 / C8
constexpr double kRdet = 1.0e6;        // R16 / R17
constexpr double kCenv = 10.0e-6;      // C9
constexpr double kRenv = 150.0e3;      // R18
constexpr double kRsustain = 500.0e3;  // RV1 (a RHEOSTAT, not a divider)
constexpr double kRabc = 27.0e3;       // R23
constexpr double kCout = 50.0e-9;      // C10
constexpr double kRoutSeries = 10.0e3; // R20
constexpr double kRlevel = 50.0e3;     // RV2

// The CA3080's Iabc pin sits one base-emitter drop above the negative rail (a
// single diode-connected mirror transistor). NOTE for anyone comparing against
// the LTspice transcription: it substitutes an LM13700, whose bias node sits at
// ~1.3 V instead, which moves the maximum control current by 8 %. The pedal has
// a CA3080, so 0.7 V is the right number here.
constexpr double kVabcPin = 0.7;
// The CA3080's output compliance: it runs out roughly a volt from each rail.
constexpr double kOtaHeadroom = 1.0;
constexpr double kVt = 0.02585;  // kT/q at 300 K, the project's shared value

// --- Derivations (all in the open) ------------------------------------------

// Ebers-Moll / Gummel-Poon base current for the shared 2N3904 card. Used here
// only to derive small-signal input impedances at the DC operating points; the
// runtime copy lives in CompressorEngine.cpp.
double ibAt(double vbe, const NpnDevice& d) {
    return (d.Is / d.betaF) * (std::exp(vbe / d.Vt) - 1.0) +
           d.Ise * (std::exp(vbe / (d.Ne * d.Vt)) - 1.0);
}

// hFE at a collector current, from the card alone. At 0.23 mA this returns ~106
// — the 2N3904 datasheet's own low-current figure — with nothing chosen by hand.
double hfeAt(double ic, const NpnDevice& d) {
    const double vbe = d.Vt * std::log(ic / d.Is + 1.0);
    return ic / ibAt(vbe, d);
}

// The bias rail the whole circuit references.
double biasVolts() { return kVsupply * kRdivBot / (kRdivTop + kRdivBot); }

// The OTA's DIFFERENTIAL drive network, second order with two REAL poles:
//
//   Zterm(s) = Rb ∥ [ Rt/(1 + s·Rt·Ct) + 1/(s·Cs) ]
//   H(s)     = −Rtrim / (Rtrim + Zterm(s))
//
// Multiplying out gives
//   H(s) = −Rtrim·(1 + b1 s + b2 s²) / (a0 + a1 s + a2 s²)
// whose roots are computed here rather than pasted in, so the network can never
// drift away from the component values above.
DriveNetwork makeDriveNetwork() {
    const double Rb = kRnInvBias, Rt = kRterm, Ct = kCterm, Cs = kCtermSeries;
    const double b1 = Rb * Cs + Rt * (Ct + Cs);
    const double b2 = Rb * Rt * Ct * Cs;
    const double a0 = kRtrim + Rb;
    const double a1 = kRtrim * Rb * Cs + (kRtrim + Rb) * Rt * (Ct + Cs);
    const double a2 = kRtrim * Rb * Rt * Ct * Cs;

    // Both quadratics have hugely separated real roots (the discriminants are
    // dominated by the linear term), so the numerically stable factorisation is
    // the "big root first, small root by the product" form.
    auto roots = [](double c2, double c1, double c0, double& lo, double& hi) {
        const double disc = std::sqrt(c1 * c1 - 4.0 * c2 * c0);
        const double big = (c1 + disc) / (2.0 * c2);  // magnitude of the fast root
        hi = big;
        lo = (c0 / c2) / big;  // product of roots = c0/c2
    };
    double zLo = 0.0, zHi = 0.0, pLo = 0.0, pHi = 0.0;
    roots(b2, b1, 1.0, zLo, zHi);
    roots(a2, a1, a0, pLo, pHi);

    DriveNetwork d{};
    d.zeroHz[0] = zLo / (2.0 * kPi);
    d.zeroHz[1] = zHi / (2.0 * kPi);
    d.poleHz[0] = pLo / (2.0 * kPi);
    d.poleHz[1] = pHi / (2.0 * kPi);
    // s -> infinity: the termination shorts, the trimmer dominates, and the OTA
    // sees the WHOLE signal as a difference. Negative because the drive lands on
    // the inverting input — the pedal is phase-inverting, like the real one.
    d.hfGain = -kRtrim * b2 / a2;
    return d;
}

CompressorConfig makeDynaCompConfig() {
    CompressorConfig c{};
    c.npn = NpnDevice{};
    c.diode = DiodeDevice{};

    const double vBias = biasVolts();  // 2.9277 V

    // --- Input stage ---------------------------------------------------------
    // Q1's emitter current, then its own small-signal numbers from the card.
    const double vbe1 = c.npn.Vt * std::log(2.3e-4 / c.npn.Is + 1.0);
    const double ie1 = (vBias - vbe1) / kRe1;
    const double re1 = c.npn.Vt / ie1;
    const double rLoadAc1 = 1.0 / (1.0 / kRe1 + 1.0 / kRotaBias);
    const double rinBase = hfeAt(ie1, c.npn) * (re1 + rLoadAc1);
    const double rTotIn =
        kRinSeries + 1.0 / (1.0 / kRinBias + 1.0 / rinBase);
    c.in.couplingHz = 1.0 / (2.0 * kPi * rTotIn * kCin);
    c.in.gain = rLoadAc1 / (rLoadAc1 + re1);

    // --- Drive network -------------------------------------------------------
    c.drive = makeDriveNetwork();

    // --- Gain cell -----------------------------------------------------------
    c.cell.kind = GainCellSpec::Kind::kOtaTanh;
    c.cell.otaVt = kVt;

    // --- Load ----------------------------------------------------------------
    // The OTA is a current source, so the node's impedance is R13 in parallel
    // with the splitter's base impedance — and THAT is what sets both the
    // transimpedance and the pole. Leaving Q2's loading out would put the pole
    // 13 % low and the gain 12 % high.
    const double vbe2 = c.npn.Vt * std::log(2.3e-4 / c.npn.Is + 1.0);
    const double ie2 = (vBias - vbe2) / kRe2;
    const double re2 = c.npn.Vt / ie2;
    const double rinQ2 = hfeAt(ie2, c.npn) * (re2 + kRe2);
    c.load.rLoadOhms = 1.0 / (1.0 / kRload + 1.0 / rinQ2);
    c.load.poleHz = 1.0 / (2.0 * kPi * c.load.rLoadOhms * kCload);
    c.load.swingUp = kVsupply - vBias - kOtaHeadroom;
    c.load.swingDown = vBias - kOtaHeadroom;

    // --- Splitter ------------------------------------------------------------
    c.split.gain = kRe2 / (kRe2 + re2);

    // --- Sidechain -----------------------------------------------------------
    c.det.clampCapF = kCdet;
    c.det.clampResOhms = kRdet;
    c.det.envCapF = kCenv;
    c.det.envResOhms = kRenv;
    c.det.supplyV = kVsupply;
    c.det.vceSatV = 0.1;
    // The emitter leg is a follower; the collector leg sits behind R15.
    c.det.srcEmitterOhms = re2 + c.load.rLoadOhms / hfeAt(ie2, c.npn);
    c.det.srcCollectorOhms = kRc2;

    // --- Control map ---------------------------------------------------------
    c.ctl.potOhms = kRsustain;
    c.ctl.seriesOhms = kRabc;
    c.ctl.cellPinV = kVabcPin;

    // --- Output --------------------------------------------------------------
    c.out.potOhms = kRlevel;
    c.out.seriesOhms = kRoutSeries;
    c.out.srcOhms = c.det.srcEmitterOhms;
    c.out.couplingHz =
        1.0 / (2.0 * kPi * (c.out.srcOhms + kRoutSeries + kRlevel) * kCout);

    // The real box is FEED-BACK. Do not change this to make a measurement come
    // out nicer — flip it with setDetectorTap() in a test instead.
    c.detectorFromOutput = true;
    return c;
}

}  // namespace

struct CompModel::Impl {
    Impl() : engine(makeDynaCompConfig()) {}
    CompressorEngine engine;
};

CompModel::CompModel() : impl_(std::make_unique<Impl>()) {}
CompModel::~CompModel() = default;

void CompModel::prepare(double sampleRate, int maxBlockSize) {
    impl_->engine.prepare(sampleRate, maxBlockSize);
}
void CompModel::setOversampling(int factor) {
    impl_->engine.setOversampling(factor);
}
int CompModel::oversampling() const { return impl_->engine.oversampling(); }
int CompModel::latencySamples() const {
    return impl_->engine.latencySamples();
}
void CompModel::setParameter(int paramId, float value) {
    impl_->engine.setParameter(paramId, value);
}
void CompModel::process(const float* in, float* out, int numFrames) {
    impl_->engine.process(in, out, numFrames);
}
void CompModel::reset() { impl_->engine.reset(); }
void CompModel::setDetectorTap(bool fromOutput) {
    impl_->engine.setDetectorTap(fromOutput);
}
bool CompModel::detectorFromOutput() const {
    return impl_->engine.detectorFromOutput();
}
double CompModel::controlCurrent() const {
    return impl_->engine.controlCurrent();
}
double CompModel::envelopeVolts() const {
    return impl_->engine.envelopeVolts();
}
double CompModel::cellGainAtDc() const { return impl_->engine.cellGainAtDc(); }
double CompModel::maxAbsRestingState() const {
    return impl_->engine.maxAbsRestingState();
}

}  // namespace clipper::dsp
