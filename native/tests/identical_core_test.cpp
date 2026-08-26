// Clipper native shell — the LOAD-BEARING identical-core test.
//
// Proves the JUCE plugin is a re-wrap, not a re-implementation: it renders a known
// signal through the REAL ClipperAudioProcessor and, independently, through a
// from-scratch chain built from the portable core classes DIRECTLY (RatModel,
// SdModel, AmpModel / Jcm800Amp, CabConvolver x2, OutputLimiter) with identical
// settings, then asserts the plugin's LEFT-channel output matches the reference
// sample-for-sample (within a tight float tolerance).
//
// NATIVE PARITY: the reference chain now walks the SAME user-ordered board the
// engine walks — every pedal type (RAT, SD-1, TS, Muff, Phaser, Gold) composed by
// hand from the core classes, in the order the Params carry — so an arbitrary chain
// must still come out bit-exact. Four board cases join the four amp-voice cases
// below, the last of them carrying the GOLD "Myth" overdrive: a parallel clean/dirt
// blend with germanium clippers, which is architecturally unlike every other pedal
// on the board and therefore the one most likely to be wrapped wrong.
//
// M9.4/M10.1/M10.2: the check now runs for ALL FOUR amp voices. The Clean 120 case
// exercises the linear stereo-chorus platform; the JCM800 case exercises the mono
// valve head (preamp cascade + power section) rendered dual-mono, now WITH its M10.1
// spring reverb engaged; the Twin case exercises the Fender-blackface mono combo
// with its full AB763 chain (spring reverb + optical tremolo) dual-mono into the same
// cab pair; the AC30 case exercises the Vox class-A "top boost" combo (preamp gain +
// top-boost stack + PI/TOP CUT + EL84 quad + spring reverb) dual-mono into the same
// cab pair, with the presence field reused as TOP CUT. All paths must be bit-exact,
// proving the amp-model switch + per-voice param routing are wrapped identically in
// the plugin and the raw engine.
//
// Both paths use the SAME core code and the SAME internal delays (JCM oversampling
// group delay + cab 128 + limiter 64), so their outputs are time-aligned — no
// latency offset is applied in the comparison. The reported plugin latency is
// separately checked against the sum of the models' latency accessors.
//
// Test signal: an M2-style 220 Hz sine under an exponential pluck envelope at
// 48 kHz. The Clean 120 set engages both dirt pedals, cab + bright, chorus and
// reverb, 4x oversampling; the JCM800 set runs an SD-1 boost into a cranked head.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "ClipperEngine.h"
#include "PluginProcessor.h"

#include "clipper/dsp/Ac30Amp.h"
#include "clipper/dsp/RockerverbAmp.h"
#include "clipper/dsp/MesaAmp.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/CompModel.h"
#include "clipper/dsp/DelayModel.h"
#include "clipper/dsp/GateModel.h"
#include "clipper/dsp/GoldModel.h"
#include "clipper/dsp/Jcm800Amp.h"
#include "clipper/dsp/MuffModel.h"
#include "clipper/dsp/OptoModel.h"
#include "clipper/dsp/VibeModel.h"
#include "clipper/dsp/DropModel.h"
#include "clipper/dsp/EqModel.h"
#include "clipper/dsp/PhaserModel.h"
#include "clipper/dsp/OutputLimiter.h"
#include "clipper/dsp/RatModel.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/TsModel.h"
#include "clipper/dsp/TwinAmp.h"

using clipper::native::Params;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 128;
constexpr int kNumFrames = 48000;  // 1.0 s
constexpr int kJcmOversampling = 4;  // matches ClipperEngine/C ABI (docs §18)
constexpr int kTwinOversampling = 4; // matches ClipperEngine/C ABI (docs §20)
constexpr int kAc30Oversampling = 4; // matches ClipperEngine/C ABI (docs §23)
constexpr int kAmpJcm800 = 1;        // Params::ampModel value for the JCM head
constexpr int kAmpTwin = 2;          // Params::ampModel value for the Twin combo
constexpr int kAmpAc30 = 3;          // Params::ampModel value for the AC30 combo
constexpr int kAmpRockerverb = 5;    // Params::ampModel value for the Rockerverb
constexpr int kAmpMesa = 6;          // Params::ampModel value for the Mesa (M10.4)

// The CLEAN 120 parameter set (both pedals on, cab + bright, stereo chorus + spring
// reverb, 4x OS) — the linear clean-platform path.
Params cleanParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = true;  p.ratDist = 0.7f; p.ratFilter = 0.4f; p.ratLevel = 0.8f;
    p.sdOn = true;   p.sdDrive = 0.5f; p.sdTone = 0.5f;    p.sdLevel = 0.7f;
    p.ampModel = 0;  // Clean 120
    p.ampOn = true;  p.volume = 0.5f;  p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.bright = true; p.cab = true;
    p.chorusMode = 1;  // chorus (stereo bloom)
    p.chorusSpeed = 0.3f; p.chorusDepth = 0.5f;
    p.reverb = 0.5f;   // M6.7 spring reverb engaged (exercises the wet stereo path)
    p.oversampling = 4;
    return p;
}

// The JCM800 parameter set — the canonical SD-1 boost into a cranked Marshall head.
// RAT off; SD-1 on as a clean-ish boost (low drive, higher level) shoving the JCM's
// front end. The JCM makes its own distortion (gain/master), so it is the amp doing
// the work. bass/middle/treble are the SHARED tone knobs; the JCM ignores volume/
// bright/chorus/reverb. Cab on (would pair with brit412 in the app, but the test's
// bit-exactness is IR-agnostic, so the default IR is fine here).
Params jcmParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = false; p.ratDist = 0.7f; p.ratFilter = 0.4f; p.ratLevel = 0.8f;
    p.sdOn = true;   p.sdDrive = 0.25f; p.sdTone = 0.6f;   p.sdLevel = 0.85f;  // boost
    p.ampModel = kAmpJcm800;  // JCM800
    p.ampOn = true;
    p.bass = 0.55f; p.middle = 0.45f; p.treble = 0.65f;  // shared tone stack
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    p.reverb = 0.4f;       // M10.1: exercise the JCM's spring reverb (usability add)
    p.jcmGain = 0.7f;      // cranked preamp
    p.jcmMaster = 0.5f;    // power-amp pushed
    p.jcmPresence = 0.6f;  // HF lift
    p.oversampling = 4;
    return p;
}

// The TWIN parameter set (M10.1) — the clean benchmark, exercising the full AB763
// chain: spring reverb + optical tremolo. RAT on as a light pedal in front (the Twin
// itself stays clean); the Twin makes NO preamp gain, so this is a glassy clean voice
// with a moving throb and a spring tail. Reuses the shared knobs — volume/bright,
// bass/mid/treble, reverb, and speed/depth (routed to the tremolo SPEED/INTENSITY).
Params twinParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = true;  p.ratDist = 0.3f; p.ratFilter = 0.5f; p.ratLevel = 0.8f;  // light edge
    p.sdOn = false;  p.sdDrive = 0.5f; p.sdTone = 0.5f;    p.sdLevel = 0.7f;
    p.ampModel = kAmpTwin;  // Twin
    p.ampOn = true;  p.volume = 0.55f; p.bass = 0.5f; p.middle = 0.55f; p.treble = 0.6f;
    p.bright = true; p.cab = true;
    p.chorusMode = 1;            // Twin: chorusMode slot reused as TREMOLO ON (docs §20)
                                 // — ON here so this case still exercises the throb.
    p.chorusSpeed = 0.6f;        // → tremolo SPEED (~5 Hz)
    p.chorusDepth = 0.6f;        // → tremolo INTENSITY
    p.reverb = 0.35f;            // period-correct spring reverb
    p.oversampling = 4;
    return p;
}

// The AC30 parameter set (M10.2) — the Vox "top boost" chime, exercising the full
// class-A chain (preamp gain stage + top-boost tone stack + PI/TOP CUT + EL84 quad +
// spring reverb) dual-mono into the same cab pair. RAT on as a light edge in front;
// the AC30's own VOLUME is the overdrive. Reuses the shared knobs — volume + bass/
// treble + reverb — and REUSES the presence field (jcmPresence) as the AC30's TOP CUT.
// The 'middle' slot is UNUSED (top-boost has no mid).
Params ac30Params() {
    Params p;
    p.inputTrim = 0.5f;
    p.ratOn = true;  p.ratDist = 0.3f; p.ratFilter = 0.5f; p.ratLevel = 0.8f;  // light edge
    p.sdOn = false;  p.sdDrive = 0.5f; p.sdTone = 0.5f;    p.sdLevel = 0.7f;
    p.ampModel = kAmpAc30;  // AC30
    p.ampOn = true;  p.volume = 0.7f;  p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;            // no chorus on the AC30
    p.reverb = 0.25f;           // usability spring reverb
    p.jcmPresence = 0.3f;       // → AC30 TOP CUT (presence field reused)
    p.oversampling = 4;
    return p;
}

// NATIVE PARITY case 1 — a MULTI-PEDAL board: RAT -> Muff -> Twin. Two very
// different nonlinearities in series (the LM308 diode clipper into the four-
// transistor fuzz) ahead of the blackface clean platform, in a chain order the old
// fixed RAT/SD-1 pair could not express at all. The SD-1 is deliberately left ON
// but OFF the board, so this also proves an off-board pedal contributes nothing to
// either the audio or the reported latency.
Params ratMuffTwinParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_RAT;
    p.chain[1] = clipper::native::PEDAL_MUFF;
    p.chainLength = 2;
    p.ratOn = true;  p.ratDist = 0.55f; p.ratFilter = 0.45f; p.ratLevel = 0.7f;
    p.sdOn = true;   // ON, but NOT on the board — must be inaudible
    p.muffOn = true; p.muffSustain = 0.6f; p.muffTone = 0.5f; p.muffVolume = 0.55f;
    p.ampModel = kAmpTwin;
    p.ampOn = true;  p.volume = 0.5f; p.bass = 0.5f; p.middle = 0.55f; p.treble = 0.6f;
    p.bright = false; p.cab = true;
    p.chorusMode = 1;      // Twin: the slot is TREMOLO ON
    p.chorusSpeed = 0.45f; p.chorusDepth = 0.5f;
    p.reverb = 0.3f;
    p.oversampling = 4;
    return p;
}

// NATIVE PARITY case 2 — TS -> Phaser -> JCM800. A screamer boost into the LINEAR
// phaser (no oversampling, no group delay) into the cranked Marshall head: it
// exercises a three-unit board, a pedal type with no latency contribution, and a
// chain whose ORDER matters (phasing the overdrive, not overdriving the phase).
Params tsPhaserJcmParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_TS;
    p.chain[1] = clipper::native::PEDAL_PHASER;
    p.chainLength = 2;
    p.ratOn = false;
    p.sdOn = false;
    p.tsOn = true;     p.tsDrive = 0.4f; p.tsTone = 0.55f; p.tsLevel = 0.8f;
    p.phaserOn = true; p.phaserSpeed = 0.35f;
    p.ampModel = kAmpJcm800;
    p.ampOn = true;
    p.bass = 0.55f; p.middle = 0.45f; p.treble = 0.65f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    p.reverb = 0.35f;
    p.jcmGain = 0.65f; p.jcmMaster = 0.5f; p.jcmPresence = 0.55f;
    p.oversampling = 4;
    return p;
}

// NATIVE PARITY case 3 — the same three pedals with the MUFF added and the order
// reversed, on the Clean 120: phaser first, then muff, then TS. Proves the routing
// is genuinely order-driven rather than a fixed sequence that happens to be
// re-labelled, and exercises a four-deep board.
Params reorderedBoardParams() {
    Params p;
    p.inputTrim = 0.4f;
    p.chain[0] = clipper::native::PEDAL_PHASER;
    p.chain[1] = clipper::native::PEDAL_MUFF;
    p.chain[2] = clipper::native::PEDAL_TS;
    p.chain[3] = clipper::native::PEDAL_RAT;
    p.chainLength = 4;
    p.phaserOn = true; p.phaserSpeed = 0.5f;
    p.muffOn = true;   p.muffSustain = 0.45f; p.muffTone = 0.6f; p.muffVolume = 0.5f;
    p.tsOn = true;     p.tsDrive = 0.3f; p.tsTone = 0.5f; p.tsLevel = 0.6f;
    p.ratOn = false;   // on the board but BYPASSED — a true pass-through
    p.ampModel = 0;    // Clean 120
    p.ampOn = true;    p.volume = 0.45f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.bright = true;   p.cab = true;
    p.chorusMode = 1;  p.chorusSpeed = 0.3f; p.chorusDepth = 0.5f;
    p.reverb = 0.2f;
    p.oversampling = 4;
    return p;
}

// NATIVE PARITY case 4 (v1.1 item 6) — a board carrying the GOLD "Myth" overdrive:
// Gold -> RAT -> Phaser -> AC30. The gold box is the odd architecture out (a parallel
// clean/dirt blend cross-faded by the dual-ganged gain pot, germanium clippers, its
// own oversampled section), so it gets a board of its own to prove the native wrap
// routes its three knobs, its engaged flag, its chain slot and its oversampling group
// delay exactly as the hand-composed core chain does. It sits FIRST — the always-on
// boost position the pedal is actually used in — with the RAT bypassed-but-on-board
// behind it and the linear phaser after, into the class-A chime.
Params goldBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_GOLD;
    p.chain[1] = clipper::native::PEDAL_RAT;
    p.chain[2] = clipper::native::PEDAL_PHASER;
    p.chainLength = 3;
    p.goldOn = true;  p.goldGain = 0.45f; p.goldTreble = 0.6f; p.goldLevel = 0.75f;
    p.ratOn = false;  // on the board but BYPASSED — a true pass-through
    p.phaserOn = true; p.phaserSpeed = 0.4f;
    p.muffOn = true;   // ON, but NOT on the board — must be inaudible
    p.tsOn = true;     // likewise
    p.sdOn = true;     // likewise
    p.ampModel = kAmpAc30;
    p.ampOn = true;  p.volume = 0.65f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.55f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    p.reverb = 0.2f;
    p.jcmPresence = 0.35f;  // → AC30 TOP CUT
    p.oversampling = 4;
    return p;
}

// NATIVE PARITY case 5 (M10.7) — a board carrying the ROCKERVERB: TS -> Squash ->
// Rocker Verb. The Rockerverb is the first amp voice whose GAIN and its MASTER are
// BOTH live at once (the JCM's are too, but the Rockerverb reads them off the same
// two shared fields with a DIFFERENT meaning for the master's position in the
// circuit), and it is the first Orange with a MID, so this case is what proves the
// native wrap routes jcmGain -> PARAM_GAIN, jcmMaster -> PARAM_VOLUME and the
// shared middle exactly as the hand-composed core chain does. jcmPresence is set
// to a NON-default value on purpose: this voice has no presence control, so if the
// wrap ever routed it the two renders would diverge.
Params rockerverbBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_TS;
    p.chain[1] = clipper::native::PEDAL_COMP;
    p.chainLength = 2;
    p.tsOn = true;   p.tsDrive = 0.35f; p.tsTone = 0.55f; p.tsLevel = 0.7f;
    p.compOn = true; p.compSustain = 0.4f; p.compLevel = 0.6f;
    p.ratOn = true;  // ON, but NOT on the board — must be inaudible
    p.sdOn = true;   // likewise
    p.ampModel = kAmpRockerverb;
    p.ampOn = true;
    p.volume = 0.9f;   // slot 0 — this voice must IGNORE it entirely
    p.bass = 0.45f; p.middle = 0.65f; p.treble = 0.55f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    p.reverb = 0.2f;          // a REAL spring on this amp
    p.jcmGain = 0.35f;        // -> Rockerverb GAIN
    p.jcmMaster = 0.08f;      // -> Rockerverb VOLUME (a linear master: keep it low)
    p.jcmPresence = 0.85f;    // must NOT reach this voice
    p.oversampling = 4;
    return p;
}

// M2-style 220 Hz sine * exponential pluck envelope.
std::vector<float> makeSignal() {
    std::vector<float> x(static_cast<size_t>(kNumFrames));
    const double f0 = 220.0;
    const double tau = 0.25;   // pluck decay time constant (s)
    const double amp = 0.3;    // hot-humbucker-ish reference level
    for (int i = 0; i < kNumFrames; ++i) {
        const double t = i / kFs;
        const double env = std::exp(-t / tau);
        x[static_cast<size_t>(i)] =
            static_cast<float>(amp * env * std::sin(2.0 * M_PI * f0 * t));
    }
    return x;
}

// The REFERENCE chain: mirrors ClipperEngine::process using core classes directly.
// Set params -> prepare (snaps smoothers) -> set oversampling, exactly as the
// engine does, then render the whole signal block by block. Handles BOTH amp voices:
// the Clean 120 splits to a stereo pair (chorus); the JCM800 is a mono head whose
// single output is mirrored to both sides (dual-mono) ahead of the identical cabs.
void renderReference(const Params& p, const std::vector<float>& in,
                     std::vector<float>& outL, std::vector<float>& outR,
                     int& latencyOut) {
    using namespace clipper::dsp;
    const bool jcm800 = p.ampModel == kAmpJcm800;
    const bool twin = p.ampModel == kAmpTwin;
    const bool ac30 = p.ampModel == kAmpAc30;
    const bool rockerverb = p.ampModel == kAmpRockerverb;
    const bool mesa = p.ampModel == kAmpMesa;

    RatModel rat;
    SdModel sd;
    TsModel ts;
    MuffModel muff;
    PhaserModel phaser;
    GoldModel gold;      // the "Myth" transparent overdrive
    CompModel comp;      // the "Squash" OTA compressor (M13.1)
    GateModel gate;      // the "Curfew" noise gate (M13.6a)
    OptoModel opto;      // the "Lumen" optical compressor (M13.3)
    VibeModel vibe;      // the "Swirl" Uni-Vibe (M13.5)
    DropModel drop;      // the "Cellar" drop-tune (M13.10)
    EqModel eq;          // the "Decade" ten-band graphic EQ (M13.6)
    DelayModel delayFx;  // the "Echoman" BBD analog delay (M13.4)
    AmpModel amp;        // Clean 120
    Jcm800Amp jcm;       // JCM800 head
    TwinAmp twinAmp;     // Twin combo
    Ac30Amp ac30Amp;     // AC30 combo
    RockerverbAmp rvAmp; // Rockerverb 100 dirty channel (M10.7)
    MesaAmp mesaAmp;     // Mesa Dual Rectifier Solo Head (M10.4)
    CabConvolver cabL, cabR;
    OutputLimiter limiter;

    rat.setParameter(RatModel::PARAM_DISTORTION, p.ratDist);
    rat.setParameter(RatModel::PARAM_FILTER, p.ratFilter);
    rat.setParameter(RatModel::PARAM_LEVEL, p.ratLevel);
    sd.setParameter(SdModel::PARAM_DRIVE, p.sdDrive);
    sd.setParameter(SdModel::PARAM_TONE, p.sdTone);
    sd.setParameter(SdModel::PARAM_LEVEL, p.sdLevel);
    // The parity pedals, in ClipperEngine::applyParamsToModels' exact order.
    ts.setParameter(TsModel::PARAM_DRIVE, p.tsDrive);
    ts.setParameter(TsModel::PARAM_TONE, p.tsTone);
    ts.setParameter(TsModel::PARAM_LEVEL, p.tsLevel);
    muff.setParameter(MuffModel::PARAM_SUSTAIN, p.muffSustain);
    muff.setParameter(MuffModel::PARAM_TONE, p.muffTone);
    muff.setParameter(MuffModel::PARAM_VOLUME, p.muffVolume);
    phaser.setParameter(PhaserModel::PARAM_SPEED, p.phaserSpeed);
    gold.setParameter(GoldModel::PARAM_GAIN, p.goldGain);
    gold.setParameter(GoldModel::PARAM_TREBLE, p.goldTreble);
    gold.setParameter(GoldModel::PARAM_OUTPUT, p.goldLevel);
    comp.setParameter(CompModel::PARAM_SUSTAIN, p.compSustain);
    comp.setParameter(CompModel::PARAM_LEVEL, p.compLevel);
    gate.setParameter(GateModel::PARAM_THRESHOLD, p.gateThreshold);
    gate.setParameter(GateModel::PARAM_DECAY, p.gateDecay);
    opto.setParameter(OptoModel::PARAM_PEAK_REDUCTION, p.optoPeakReduction);
    opto.setParameter(OptoModel::PARAM_MODE, p.optoMode);
    opto.setParameter(OptoModel::PARAM_GAIN, p.optoGain);
    vibe.setParameter(VibeModel::PARAM_SPEED, p.vibeSpeed);
    vibe.setParameter(VibeModel::PARAM_INTENSITY, p.vibeIntensity);
    vibe.setParameter(VibeModel::PARAM_MODE, p.vibeMode);
    drop.setParameter(DropModel::PARAM_AMOUNT, p.dropAmount);
    eq.setParameter(EqModel::PARAM_GAIN, p.eqGain);
    eq.setParameter(EqModel::PARAM_VOLUME, p.eqVolume);
    for (int i = 0; i < EqModel::kNumBands; ++i) eq.setBand(i, p.eqBand[i]);
    delayFx.setParameter(DelayModel::PARAM_DELAY, p.delayTime);
    delayFx.setParameter(DelayModel::PARAM_FEEDBACK, p.delayFeedback);
    delayFx.setParameter(DelayModel::PARAM_BLEND, p.delayBlend);

    if (jcm800) {
        // Mirror ClipperEngine::applyParams' JCM order exactly.
        jcm.setParameter(Jcm800Amp::PARAM_GAIN, p.jcmGain);
        jcm.setParameter(Jcm800Amp::PARAM_MASTER, p.jcmMaster);
        jcm.setParameter(Jcm800Amp::PARAM_BASS, p.bass);
        jcm.setParameter(Jcm800Amp::PARAM_MID, p.middle);
        jcm.setParameter(Jcm800Amp::PARAM_TREBLE, p.treble);
        jcm.setParameter(Jcm800Amp::PARAM_PRESENCE, p.jcmPresence);
        jcm.setParameter(Jcm800Amp::PARAM_REVERB, p.reverb);  // M10.1 usability add
    } else if (twin) {
        // Mirror ClipperEngine::applyParams' Twin routing exactly.
        twinAmp.setParameter(TwinAmp::PARAM_VOLUME, p.volume);
        twinAmp.setParameter(TwinAmp::PARAM_BASS, p.bass);
        twinAmp.setParameter(TwinAmp::PARAM_MID, p.middle);
        twinAmp.setParameter(TwinAmp::PARAM_TREBLE, p.treble);
        twinAmp.setParameter(TwinAmp::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
        twinAmp.setParameter(TwinAmp::PARAM_REVERB, p.reverb);
        twinAmp.setParameter(TwinAmp::PARAM_SPEED, p.chorusSpeed);
        twinAmp.setParameter(TwinAmp::PARAM_INTENSITY, p.chorusDepth);
        twinAmp.setParameter(TwinAmp::PARAM_TREMOLO_ENABLE, p.chorusMode >= 1 ? 1.0f : 0.0f);
    } else if (ac30) {
        // Mirror ClipperEngine::applyParams' AC30 routing exactly: volume/bass/treble
        // + reverb from the shared knobs, and the presence field reused as TOP CUT.
        // The AC30 top-boost has NO mid — 'middle' is not routed.
        ac30Amp.setParameter(Ac30Amp::PARAM_VOLUME, p.volume);
        ac30Amp.setParameter(Ac30Amp::PARAM_BASS, p.bass);
        ac30Amp.setParameter(Ac30Amp::PARAM_TREBLE, p.treble);
        ac30Amp.setParameter(Ac30Amp::PARAM_TOPCUT, p.jcmPresence);  // presence → TOP CUT
        ac30Amp.setParameter(Ac30Amp::PARAM_REVERB, p.reverb);
    } else if (rockerverb) {
        // Mirror ClipperEngine::applyParamsToModels' Rockerverb routing exactly:
        // jcmGain is its GAIN, jcmMaster its post-tone-stack VOLUME, and
        // bass/middle/treble/reverb the shared fields. NOTHING else routes here —
        // in particular not volume (slot 0) and not jcmPresence.
        rvAmp.setParameter(RockerverbAmp::PARAM_GAIN, p.jcmGain);
        rvAmp.setParameter(RockerverbAmp::PARAM_VOLUME, p.jcmMaster);
        rvAmp.setParameter(RockerverbAmp::PARAM_BASS, p.bass);
        rvAmp.setParameter(RockerverbAmp::PARAM_MID, p.middle);
        rvAmp.setParameter(RockerverbAmp::PARAM_TREBLE, p.treble);
        rvAmp.setParameter(RockerverbAmp::PARAM_REVERB, p.reverb);
    } else if (mesa) {
        // Mirror ClipperEngine::applyParamsToModels' Mesa routing exactly: jcmGain
        // is its GAIN, jcmMaster its MASTER, jcmPresence its PRESENCE, and
        // bass/middle/treble/reverb the shared fields. NOT volume (slot 0).
        mesaAmp.setParameter(MesaAmp::PARAM_GAIN, p.jcmGain);
        mesaAmp.setParameter(MesaAmp::PARAM_MASTER, p.jcmMaster);
        mesaAmp.setParameter(MesaAmp::PARAM_PRESENCE, p.jcmPresence);
        mesaAmp.setParameter(MesaAmp::PARAM_BASS, p.bass);
        mesaAmp.setParameter(MesaAmp::PARAM_MID, p.middle);
        mesaAmp.setParameter(MesaAmp::PARAM_TREBLE, p.treble);
        mesaAmp.setParameter(MesaAmp::PARAM_REVERB, p.reverb);
        // The three switches, quantized the SAME way ClipperEngine::applyMesaSwitches
        // does. Written out here rather than shared, deliberately: this driver's
        // whole job is to be an independent reconstruction of the engine's routing,
        // and calling the engine's own helper would make the comparison circular.
        {
            const float m = p.mesaMode < 0.0f ? 0.0f : (p.mesaMode > 1.0f ? 1.0f : p.mesaMode);
            const int n = static_cast<int>(clipper::dsp::MesaMode::MESA_MODE_COUNT);
            int idx = static_cast<int>(m * static_cast<float>(n - 1) + 0.5f);
            if (idx < 0) idx = 0;
            if (idx > n - 1) idx = n - 1;
            mesaAmp.setMode(static_cast<clipper::dsp::MesaMode>(idx));
        }
        mesaAmp.setRectifier(p.mesaRectifier < 0.5f ? clipper::dsp::MesaRectifier::Silicon
                                                    : clipper::dsp::MesaRectifier::Valve5U4);
        mesaAmp.setPowerMode(p.mesaPowerMode < 0.5f ? clipper::dsp::MesaPowerMode::Bold
                                                    : clipper::dsp::MesaPowerMode::Spongy);
    } else {
        amp.setParameter(AmpModel::PARAM_VOLUME, p.volume);
        amp.setParameter(AmpModel::PARAM_BASS, p.bass);
        amp.setParameter(AmpModel::PARAM_MIDDLE, p.middle);
        amp.setParameter(AmpModel::PARAM_TREBLE, p.treble);
        amp.setParameter(AmpModel::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
        amp.setParameter(AmpModel::PARAM_CHORUS_SPEED, p.chorusSpeed);
        amp.setParameter(AmpModel::PARAM_CHORUS_DEPTH, p.chorusDepth);
        amp.setParameter(AmpModel::PARAM_CHORUS_MODE, static_cast<float>(p.chorusMode));
        amp.setParameter(AmpModel::PARAM_REVERB, p.reverb);
    }

    rat.prepare(kFs, kBlock);
    sd.prepare(kFs, kBlock);
    ts.prepare(kFs, kBlock);
    muff.prepare(kFs, kBlock);
    phaser.prepare(kFs);   // linear: no block-size scratch
    gold.prepare(kFs, kBlock);
    comp.prepare(kFs, kBlock);
    gate.prepare(kFs, kBlock);
    opto.prepare(kFs, kBlock);
    vibe.prepare(kFs, kBlock);
    drop.prepare(kFs, kBlock);
    eq.prepare(kFs);
    delayFx.prepare(kFs, kBlock);
    amp.prepare(kFs, kBlock);
    // The JCM runs at its fixed 4x internally (set BEFORE prepare so its stages size
    // to it), independent of the pedal OS selector — matches ClipperEngine.
    jcm.setOversampling(kJcmOversampling);
    jcm.prepare(kFs, kBlock);
    twinAmp.setOversampling(kTwinOversampling);
    twinAmp.prepare(kFs, kBlock);
    ac30Amp.setOversampling(kAc30Oversampling);
    ac30Amp.prepare(kFs, kBlock);
    rvAmp.setOversampling(kAc30Oversampling);  // every tube amp here runs 4x
    rvAmp.prepare(kFs, kBlock);
    mesaAmp.prepare(kFs, kBlock);
    rat.setOversampling(p.oversampling);
    sd.setOversampling(p.oversampling);
    ts.setOversampling(p.oversampling);
    muff.setOversampling(p.oversampling);
    gold.setOversampling(p.oversampling);
    opto.setOversampling(p.oversampling);
    vibe.setOversampling(p.oversampling);
    drop.setOversampling(p.oversampling);
    eq.setOversampling(p.oversampling);
    // (the phaser has no oversampler — it is linear)

    const std::vector<float> ir = generateDefaultCab2x12IR(kFs);
    cabL.prepare(kFs, ir.data(), static_cast<int>(ir.size()), kFs, 128);
    cabR.prepare(kFs, ir.data(), static_cast<int>(ir.size()), kFs, 128);
    limiter.prepare(kFs);

    outL.assign(in.size(), 0.0f);
    outR.assign(in.size(), 0.0f);

    std::vector<float> a(kBlock), b(kBlock), l(kBlock), r(kBlock);
    const float g = clipper::native::trimKnobToGain(p.inputTrim);

    int off = 0;
    while (off < kNumFrames) {
        const int n = std::min(kBlock, kNumFrames - off);
        for (int i = 0; i < n; ++i) a[static_cast<size_t>(i)] = in[static_cast<size_t>(off + i)] * g;
        float* cur = a.data();
        float* other = b.data();
        // Walk the BOARD in order, running each engaged pedal — the routing the
        // engine performs, composed here by hand from the core models.
        for (int c = 0; c < p.chainLength; ++c) {
            const int type = p.chain[c];
            if (!p.pedalOn(type)) continue;
            switch (type) {
                case clipper::native::PEDAL_RAT:    rat.process(cur, other, n); break;
                case clipper::native::PEDAL_SD:     sd.process(cur, other, n); break;
                case clipper::native::PEDAL_TS:     ts.process(cur, other, n); break;
                case clipper::native::PEDAL_MUFF:   muff.process(cur, other, n); break;
                case clipper::native::PEDAL_PHASER: phaser.process(cur, other, n); break;
                case clipper::native::PEDAL_GOLD:   gold.process(cur, other, n); break;
                case clipper::native::PEDAL_COMP:   comp.process(cur, other, n); break;
                case clipper::native::PEDAL_GATE:   gate.process(cur, other, n); break;
                case clipper::native::PEDAL_OPTO:   opto.process(cur, other, n); break;
                case clipper::native::PEDAL_VIBE:   vibe.process(cur, other, n); break;
                case clipper::native::PEDAL_DROP:   drop.process(cur, other, n); break;
                case clipper::native::PEDAL_EQ:     eq.process(cur, other, n); break;
                case clipper::native::PEDAL_DELAY:  delayFx.process(cur, other, n); break;
                default: continue;
            }
            std::swap(cur, other);
        }
        if (!p.ampOn) {
            for (int i = 0; i < n; ++i) { l[static_cast<size_t>(i)] = cur[i]; r[static_cast<size_t>(i)] = cur[i]; }
        } else {
            if (jcm800) {
                jcm.process(cur, l.data(), n);                   // mono head
                for (int i = 0; i < n; ++i) r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];  // dual-mono
            } else if (twin) {
                twinAmp.process(cur, l.data(), n);               // mono combo
                for (int i = 0; i < n; ++i) r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];  // dual-mono
            } else if (ac30) {
                ac30Amp.process(cur, l.data(), n);               // mono combo
                for (int i = 0; i < n; ++i) r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];  // dual-mono
            } else if (rockerverb) {
                rvAmp.process(cur, l.data(), n);                 // mono head
                for (int i = 0; i < n; ++i) r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];  // dual-mono
            } else if (mesa) {
                mesaAmp.process(cur, l.data(), n);               // mono head
                for (int i = 0; i < n; ++i) r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];  // dual-mono
            } else {
                amp.processStereo(cur, l.data(), r.data(), n);   // stereo split
            }
            if (p.cab) {
                cabL.process(l.data(), l.data(), n);             // per-side cab
                cabR.process(r.data(), r.data(), n);
            }
        }
        limiter.processStereo(l.data(), r.data(), n);            // safety limiter
        for (int i = 0; i < n; ++i) {
            outL[static_cast<size_t>(off + i)] = l[static_cast<size_t>(i)];
            outR[static_cast<size_t>(off + i)] = r[static_cast<size_t>(i)];
        }
        off += n;
    }

    int pedalLatency = 0;
    for (int c = 0; c < p.chainLength; ++c) {
        const int type = p.chain[c];
        if (!p.pedalOn(type)) continue;
        switch (type) {
            case clipper::native::PEDAL_RAT:  pedalLatency += rat.latencySamples(); break;
            case clipper::native::PEDAL_SD:   pedalLatency += sd.latencySamples(); break;
            case clipper::native::PEDAL_TS:   pedalLatency += ts.latencySamples(); break;
            case clipper::native::PEDAL_MUFF: pedalLatency += muff.latencySamples(); break;
            case clipper::native::PEDAL_GOLD: pedalLatency += gold.latencySamples(); break;
            case clipper::native::PEDAL_COMP: pedalLatency += comp.latencySamples(); break;
            // The gate is not oversampled: zero group delay, like the phaser.
            case clipper::native::PEDAL_GATE: break;
            case clipper::native::PEDAL_OPTO: pedalLatency += opto.latencySamples(); break;
            case clipper::native::PEDAL_VIBE: pedalLatency += vibe.latencySamples(); break;
            case clipper::native::PEDAL_DROP: pedalLatency += drop.latencySamples(); break;
            case clipper::native::PEDAL_EQ:   pedalLatency += eq.latencySamples(); break;
            case clipper::native::PEDAL_DELAY: pedalLatency += delayFx.latencySamples(); break;
            default: break;  // the phaser is linear — no group delay
        }
    }
    latencyOut = pedalLatency +
                 (p.ampOn && jcm800 ? jcm.latencySamples() : 0) +
                 (p.ampOn && twin ? twinAmp.latencySamples() : 0) +
                 (p.ampOn && ac30 ? ac30Amp.latencySamples() : 0) +
                 (p.ampOn && rockerverb ? rvAmp.latencySamples() : 0) +
                 (p.ampOn && mesa ? mesaAmp.latencySamples() : 0) +
                 (p.ampOn && p.cab ? cabL.latencySamples() : 0) +
                 limiter.latencySamples();
}

// Drive the REAL plugin over the same signal, mono in -> stereo out, for the given
// parameter set (either amp voice).
void renderPlugin(const Params& p, const std::vector<float>& in,
                  std::vector<float>& outL, std::vector<float>& outR,
                  int& latencyOut) {
    clipper::native::ClipperAudioProcessor proc;

    // Push the known set into the APVTS BEFORE prepareToPlay, so the engine snaps
    // its smoothers to these targets (steady state from sample 0). convertTo0to1
    // maps each param's real value (knob 0..1 / bool 0-1 / choice index) correctly.
    auto set = [&](const char* id, float realValue) {
        auto* param = proc.apvts.getParameter(id);
        param->setValueNotifyingHost(param->convertTo0to1(realValue));
    };
    using namespace clipper::native::pid;
    set(inputTrim, p.inputTrim);
    set(ratOn, p.ratOn ? 1.0f : 0.0f); set(ratDist, p.ratDist); set(ratFilter, p.ratFilter); set(ratLevel, p.ratLevel);
    set(sdOn, p.sdOn ? 1.0f : 0.0f);   set(sdDrive, p.sdDrive); set(sdTone, p.sdTone); set(sdLevel, p.sdLevel);
    set(tsOn, p.tsOn ? 1.0f : 0.0f);   set(tsDrive, p.tsDrive); set(tsTone, p.tsTone); set(tsLevel, p.tsLevel);
    set(muffOn, p.muffOn ? 1.0f : 0.0f);
    set(muffSustain, p.muffSustain); set(muffTone, p.muffTone); set(muffVolume, p.muffVolume);
    set(phaserOn, p.phaserOn ? 1.0f : 0.0f); set(phaserSpeed, p.phaserSpeed);
    set(goldOn, p.goldOn ? 1.0f : 0.0f);
    set(goldGain, p.goldGain); set(goldTreble, p.goldTreble); set(goldLevel, p.goldLevel);
    // M13.3: the optical compressor. Pushed explicitly, unlike the other
    // post-v1.1 pedals, because its board case moves all THREE slots away from
    // their APVTS defaults — and slot 1 (MODE) is the first middle slot on this
    // board that carries a real control, so nothing else here would have caught a
    // plugin that never delivered it.
    set(optoOn, p.optoOn ? 1.0f : 0.0f);
    set(optoPeakReduction, p.optoPeakReduction);
    set(optoMode, p.optoMode);
    set(optoGain, p.optoGain);
    // M13.5: the Uni-Vibe. Pushed explicitly for the same reason as the opto —
    // its board case moves all THREE slots away from their APVTS defaults, and
    // slot 2 (MODE) is a DISCRETE control whose two positions are different
    // topologies, so a plugin that never delivered it would render the wrong
    // pedal and every other case here would still pass.
    set(vibeOn, p.vibeOn ? 1.0f : 0.0f);
    set(vibeSpeed, p.vibeSpeed);
    set(vibeIntensity, p.vibeIntensity);
    set(vibeMode, p.vibeMode);
    set(dropOn, p.dropOn ? 1.0f : 0.0f);
    set(dropAmount, p.dropAmount);
    set(eqOn, p.eqOn ? 1.0f : 0.0f);
    set(eqGain, p.eqGain);
    set(eqVolume, p.eqVolume);
    set(eqBand31, p.eqBand[0]);
    set(eqBand63, p.eqBand[1]);
    set(eqBand125, p.eqBand[2]);
    set(eqBand250, p.eqBand[3]);
    set(eqBand500, p.eqBand[4]);
    set(eqBand1k, p.eqBand[5]);
    set(eqBand2k, p.eqBand[6]);
    set(eqBand4k, p.eqBand[7]);
    set(eqBand8k, p.eqBand[8]);
    set(eqBand16k, p.eqBand[9]);
    // M10.4: the Mesa's three switches. Pushed explicitly for the same reason as
    // the opto's and the drop's — a parameter the plugin driver never writes is a
    // parameter this test cannot prove is plumbed.
    set(mesaMode, p.mesaMode);
    set(mesaRectifier, p.mesaRectifier);
    set(mesaPowerMode, p.mesaPowerMode);
    // The BOARD is state, not a parameter: push it through the processor's chain API
    // (which also publishes the packed snapshot the audio thread reads).
    {
        std::vector<int> board;
        for (int c = 0; c < p.chainLength; ++c) board.push_back(p.chain[c]);
        proc.setChainOrder(board);
    }
    set(ampOn, p.ampOn ? 1.0f : 0.0f);
    set(ampModel, static_cast<float>(p.ampModel));  // choice index == model id
    set(volume, p.volume); set(bass, p.bass); set(middle, p.middle); set(treble, p.treble);
    set(bright, p.bright ? 1.0f : 0.0f); set(cab, p.cab ? 1.0f : 0.0f);
    set(chorusMode, static_cast<float>(p.chorusMode));
    set(chorusSpeed, p.chorusSpeed); set(chorusDepth, p.chorusDepth);
    set(reverb, p.reverb);
    set(jcmGain, p.jcmGain); set(jcmMaster, p.jcmMaster); set(jcmPresence, p.jcmPresence);
    set(oversampling, 2.0f);  // choice index 2 == 4x

    proc.setPlayConfigDetails(1, 2, kFs, kBlock);
    proc.prepareToPlay(kFs, kBlock);

    outL.assign(in.size(), 0.0f);
    outR.assign(in.size(), 0.0f);

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    int off = 0;
    while (off < kNumFrames) {
        const int n = std::min(kBlock, kNumFrames - off);
        buffer.clear();
        // Mono source into channel 0 (channel 1 left silent; the processor reads
        // channel 0 as the mono guitar).
        for (int i = 0; i < n; ++i) buffer.setSample(0, i, in[static_cast<size_t>(off + i)]);
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, n);
        proc.processBlock(block, midi);
        for (int i = 0; i < n; ++i) {
            outL[static_cast<size_t>(off + i)] = block.getSample(0, i);
            outR[static_cast<size_t>(off + i)] = block.getSample(1, i);
        }
        off += n;
    }
    latencyOut = proc.getLatencySamples();
}

// Run the full identical-core comparison for one parameter set (one amp voice).
// Returns true on PASS.
// NATIVE PARITY case 5 (M13.1) — a board carrying the "Squash" OTA compressor:
// Squash -> Screamer -> Twin. The compressor goes FIRST, which is where a player
// actually puts one, and it is the first non-dirt pedal to reach this test: it
// has TWO knobs (slot 1 is not written at all), it carries a feed-back detector
// loop whose state must not be perturbed by the wrapping, and it oversamples, so
// its group delay has to land in the reported latency like any dirt box.
Params compBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_COMP;
    p.chain[1] = clipper::native::PEDAL_TS;
    p.chainLength = 2;
    p.compOn = true;  p.compSustain = 0.7f; p.compLevel = 0.45f;
    p.tsOn = true;    p.tsDrive = 0.35f; p.tsTone = 0.5f; p.tsLevel = 0.6f;
    p.ratOn = true;   // ON, but NOT on the board — must be inaudible
    p.goldOn = true;  // likewise
    p.muffOn = true;  // likewise
    p.ampModel = kAmpTwin;
    p.ampOn = true;  p.volume = 0.55f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    return p;
}

// NATIVE PARITY case 7 (M13.4) — a board carrying the "Echoman" BBD analog
// delay: RAT -> Echoman -> JCM800. The delay goes LAST, which is where a player
// actually puts one, and it is the first pedal to reach this test that carries a
// LONG recursive state (a 550 ms line at 8x = a quarter of a million samples) and
// a FEEDBACK loop. That combination is exactly what a wrapping bug corrupts and a
// short render would hide, so this case is what proves the plugin's chunking does
// not perturb it. It is also the first pedal here that reports ZERO latency while
// still oversampling internally.
Params delayBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_RAT;
    p.chain[1] = clipper::native::PEDAL_DELAY;
    p.chainLength = 2;
    p.ratOn = true;   p.ratDist = 0.5f; p.ratFilter = 0.5f; p.ratLevel = 0.6f;
    p.delayOn = true; p.delayTime = 0.45f; p.delayFeedback = 0.55f; p.delayBlend = 0.4f;
    p.compOn = true;  // ON, but NOT on the board — must be inaudible
    p.goldOn = true;  // likewise
    p.muffOn = true;  // likewise
    p.ampModel = kAmpJcm800;
    p.ampOn = true;  p.volume = 0.5f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.jcmGain = 0.45f; p.jcmMaster = 0.5f; p.jcmPresence = 0.4f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    return p;
}

// NATIVE PARITY case 6 (M13.6a) — a board carrying the "Curfew" noise gate:
// RAT -> Curfew -> JCM800. The gate goes AFTER the dirt, which is where a gate
// belongs and the opposite of where the compressor belongs. It is the first pedal
// to reach this test that reports ZERO latency while a pedal BEFORE it reports a
// nonzero one, so the chain's latency accounting has to add exactly the RAT's.
Params gateBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_RAT;
    p.chain[1] = clipper::native::PEDAL_GATE;
    p.chainLength = 2;
    p.ratOn = true;   p.ratDist = 0.7f; p.ratFilter = 0.5f; p.ratLevel = 0.5f;
    p.gateOn = true;  p.gateThreshold = 0.3f; p.gateDecay = 0.45f;
    p.compOn = true;  // ON, but NOT on the board — must be inaudible
    p.goldOn = true;  // likewise
    p.muffOn = true;  // likewise
    p.ampModel = kAmpJcm800;
    p.ampOn = true;  p.volume = 0.55f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    return p;
}

// NATIVE PARITY case 8 (M13.3) — a board carrying the "Lumen" optical
// compressor: Lumen -> RAT -> Twin. It is the first pedal to reach this test
// whose MIDDLE slot carries a real control (MODE), so it is the case that proves
// slot 1 is plumbed end to end rather than silently dropped the way every other
// two-knob pedal's is. It also carries a feed-back loop with a MULTI-SECOND state
// (the cell's trap occupancy), which a wrapping bug would perturb.
Params optoBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_OPTO;
    p.chain[1] = clipper::native::PEDAL_RAT;
    p.chainLength = 2;
    p.optoOn = true;  p.optoPeakReduction = 0.7f; p.optoMode = 1.0f; p.optoGain = 0.55f;
    p.ratOn = true;   p.ratDist = 0.4f; p.ratFilter = 0.5f; p.ratLevel = 0.5f;
    p.compOn = true;  // ON, but NOT on the board — must be inaudible
    p.goldOn = true;  // likewise
    p.muffOn = true;  // likewise
    p.ampModel = kAmpTwin;
    p.ampOn = true;  p.volume = 0.5f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    return p;
}

// NATIVE PARITY case 9 (M13.5) — a board carrying the "Swirl" Uni-Vibe:
// RAT -> Swirl -> JCM800. Two things make this case worth its runtime rather than
// being a ninth copy of the same shape. (1) It is the second pedal to reach this
// test that uses all three slots, and its slot 2 (MODE) selects between two
// different TOPOLOGIES (dry+wet against wet alone), so a plugin that dropped it
// would render an audibly different pedal while every other case still passed.
// (2) It carries a LAMP with a real thermal state and four photocells with their
// own, so the two sides have to agree on a multi-hundred-millisecond history, not
// just on a filter's last two samples.
Params vibeBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_RAT;
    p.chain[1] = clipper::native::PEDAL_VIBE;
    p.chainLength = 2;
    p.ratOn = true;   p.ratDist = 0.35f; p.ratFilter = 0.5f; p.ratLevel = 0.6f;
    // Every slot away from its APVTS default, MODE in the VIBRATO position.
    p.vibeOn = true;  p.vibeSpeed = 0.62f; p.vibeIntensity = 0.85f; p.vibeMode = 1.0f;
    p.optoOn = true;  // ON, but NOT on the board — must be inaudible
    p.compOn = true;  // likewise
    p.goldOn = true;  // likewise
    p.ampModel = kAmpJcm800;
    p.ampOn = true;  p.volume = 0.5f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.jcmGain = 0.45f;  p.jcmMaster = 0.4f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    return p;
}


// NATIVE PARITY case 11 (M13.6) — a board carrying the "Decade" ten-band EQ:
// Decade -> RAT -> JCM800. This is the case that proves the parameter-path
// WIDENING, which is what the whole slice rests on: the EQ is the first pedal on
// this board whose controls are not the three shared slots, so ten of its twelve
// parameters travel by ids 3..12 that no previous case exercises at all. A
// plugin that dropped them, or that mapped a band name to the wrong id, would
// render a different CURVE while every other case still passed.
//
// The sliders are set AWAY from flat and asymmetrically — a scoop, the setting
// this pedal is actually bought for — because every band at 0.5 is EXACT
// transparency (docs §72.3), so a case with flat sliders would pass against a
// plugin whose band plumbing did nothing at all.
Params eqBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_EQ;
    p.chain[1] = clipper::native::PEDAL_RAT;
    p.chainLength = 2;
    p.eqOn = true;
    p.eqGain = 0.6f; p.eqVolume = 0.45f;
    // A mid scoop with a low and a presence push: every band different, and none
    // of them at the default.
    const float bands[10] = {0.35f, 0.70f, 0.62f, 0.55f, 0.25f,
                             0.20f, 0.48f, 0.72f, 0.58f, 0.40f};
    for (int i = 0; i < 10; ++i) p.eqBand[i] = bands[i];
    p.ratOn = true;   p.ratDist = 0.4f; p.ratFilter = 0.5f; p.ratLevel = 0.6f;
    p.dropOn = true;  // ON, but NOT on the board — must be inaudible
    p.vibeOn = true;  // likewise
    p.ampModel = kAmpJcm800;
    p.ampOn = true;  p.volume = 0.5f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.jcmGain = 0.5f;  p.jcmMaster = 0.35f;
    p.bright = false; p.cab = true;
    return p;
}

// NATIVE PARITY case 10 (M13.10) — a board carrying the "Cellar" drop-tune:
// Cellar -> RAT -> Mesa. Two things make it worth its runtime. (1) It is the
// FIRST case whose pedal has ONE knob and whose knob is QUANTIZED, so a plugin
// that rounded slot 0 differently from the core would render a different
// INTERVAL — the loudest possible disagreement — while every other case passed.
// (2) It runs the pitch shifter FIRST, before the dirt, which is where the
// coaching puts it: the shifter's grain splice has to agree across a
// multi-hundred-millisecond search history and then be amplified by a clipper
// and five triodes, so any per-block divergence is magnified rather than hidden.
// The amp is the MESA, which §70 could not use: this driver had no Mesa case at
// all until the §71 slice added one, so naming it then would have compared the
// plugin against a reference that could not build it. It can now, and this is
// also the ONLY case that drives the Mesa's three switches end to end.
Params dropBoardParams() {
    Params p;
    p.inputTrim = 0.5f;
    p.chain[0] = clipper::native::PEDAL_DROP;
    p.chain[1] = clipper::native::PEDAL_RAT;
    p.chainLength = 2;
    // AMOUNT away from its APVTS default (0.0 = DROP 1): 0.25 is the DROP 3
    // detent, three semitones down.
    p.dropOn = true;  p.dropAmount = 0.25f;
    p.ratOn = true;   p.ratDist = 0.4f; p.ratFilter = 0.5f; p.ratLevel = 0.6f;
    p.vibeOn = true;  // ON, but NOT on the board — must be inaudible
    p.optoOn = true;  // likewise
    p.ampModel = kAmpMesa;
    p.ampOn = true;  p.volume = 0.5f; p.bass = 0.5f; p.middle = 0.5f; p.treble = 0.6f;
    p.jcmGain = 0.5f;  p.jcmMaster = 0.35f;  p.jcmPresence = 0.6f;
    // All three Mesa switches AWAY from their defaults, so a plugin that dropped
    // any one of them diverges: RED VINTAGE (not the default RED MODERN), the 5U4
    // valve rectifier (not silicon) and SPONGY (not bold).
    p.mesaMode = 0.75f;  p.mesaRectifier = 1.0f;  p.mesaPowerMode = 1.0f;
    p.bright = false; p.cab = true;
    p.chorusMode = 0;
    return p;
}

bool runCase(const char* label, const Params& p, const std::vector<float>& in) {
    std::printf("\n--- case: %s ---\n", label);

    std::vector<float> refL, refR, plL, plR;
    int refLat = 0, plLat = 0;
    renderReference(p, in, refL, refR, refLat);
    renderPlugin(p, in, plL, plR, plLat);

    // Secondary cross-check: render straight through ClipperEngine (no JUCE glue),
    // set once + prepared (snapped), and confirm it too is bit-exact against the
    // hand-written core reference — isolating the engine from the plugin wrapper.
    double engMax = 0.0;
    {
        clipper::native::ClipperEngine eng;
        eng.setParams(p);
        eng.prepare(kFs, kBlock);
        int off = 0;
        std::vector<float> il(kBlock), ol(kBlock), orr(kBlock);
        while (off < kNumFrames) {
            const int n = std::min(kBlock, kNumFrames - off);
            for (int i = 0; i < n; ++i) il[static_cast<size_t>(i)] = in[static_cast<size_t>(off + i)];
            eng.process(il.data(), ol.data(), orr.data(), n);
            for (int i = 0; i < n; ++i)
                engMax = std::max(engMax, std::fabs(static_cast<double>(ol[static_cast<size_t>(i)]) -
                                                    static_cast<double>(refL[static_cast<size_t>(off + i)])));
            off += n;
        }
    }

    // Compare the LEFT channel (the task's spec) sample-for-sample.
    double maxDiff = 0.0, sumSq = 0.0, refPeak = 0.0;
    int maxIdx = -1;
    for (int i = 0; i < kNumFrames; ++i) {
        const double d = std::fabs(static_cast<double>(plL[static_cast<size_t>(i)]) -
                                   static_cast<double>(refL[static_cast<size_t>(i)]));
        if (d > maxDiff) { maxDiff = d; maxIdx = i; }
        sumSq += d * d;
        refPeak = std::max(refPeak, std::fabs(static_cast<double>(refL[static_cast<size_t>(i)])));
    }
    const double rms = std::sqrt(sumSq / kNumFrames);

    // Also confirm the RIGHT channel matches (proves the stereo path — chorus bloom
    // for the Clean 120, dual-mono for the JCM — is wired identically).
    double maxDiffR = 0.0;
    for (int i = 0; i < kNumFrames; ++i)
        maxDiffR = std::max(maxDiffR,
                            std::fabs(static_cast<double>(plR[static_cast<size_t>(i)]) -
                                      static_cast<double>(refR[static_cast<size_t>(i)])));

    std::printf("reference L peak      : %.6f\n", refPeak);
    std::printf("max |engine-ref| L    : %.3e\n", engMax);
    std::printf("max |plugin-ref| L    : %.3e (at sample %d)\n", maxDiff, maxIdx);
    std::printf("max |plugin-ref| R    : %.3e\n", maxDiffR);
    std::printf("RMS diff L            : %.3e\n", rms);
    std::printf("reported latency      : plugin=%d  reference=%d\n", plLat, refLat);

    // Tolerance: the plugin wraps the identical core with no re-seeding, so the
    // whole chain is expected BIT-EXACT. Allow only a hair of float slack.
    const double kTol = 1e-6;
    bool ok = true;
    if (refPeak < 1e-4) { std::printf("FAIL: reference produced no signal\n"); ok = false; }
    if (engMax > kTol)  { std::printf("FAIL: engine vs core reference exceeds %.1e\n", kTol); ok = false; }
    if (maxDiff > kTol) { std::printf("FAIL: L channel exceeds tolerance %.1e\n", kTol); ok = false; }
    if (maxDiffR > kTol) { std::printf("FAIL: R channel exceeds tolerance %.1e\n", kTol); ok = false; }
    if (plLat != refLat) { std::printf("FAIL: latency mismatch\n"); ok = false; }

    if (ok) std::printf("PASS: %s output is sample-identical to the direct core chain.\n", label);
    return ok;
}

}  // namespace

int main() {
    std::printf("=== Clipper identical-core test ===\n");
    std::printf("signal: 220 Hz sine + pluck, %d samples @ %.0f Hz, block %d\n",
                kNumFrames, kFs, kBlock);

    const std::vector<float> in = makeSignal();

    bool ok = true;
    ok &= runCase("Clean 120", cleanParams(), in);
    ok &= runCase("JCM800", jcmParams(), in);
    ok &= runCase("Twin", twinParams(), in);
    ok &= runCase("AC30", ac30Params(), in);
    // Native parity: multi-pedal BOARDS, in user-chosen order.
    ok &= runCase("Board: RAT -> Muff -> Twin", ratMuffTwinParams(), in);
    ok &= runCase("Board: TS -> Phaser -> JCM800", tsPhaserJcmParams(), in);
    ok &= runCase("Board: Phaser -> Muff -> TS -> (RAT bypassed) -> Clean 120",
                  reorderedBoardParams(), in);
    // v1.1 item 6: the GOLD "Myth" overdrive on the native board.
    ok &= runCase("Board: Gold -> (RAT bypassed) -> Phaser -> AC30", goldBoardParams(), in);
    ok &= runCase("Board: Squash -> TS -> Twin", compBoardParams(), in);
    ok &= runCase("Board: RAT -> Curfew -> JCM800", gateBoardParams(), in);
    ok &= runCase("Board: RAT -> Echoman -> JCM800", delayBoardParams(), in);
    // M10.7: the Rockerverb 100 on the native board.
    ok &= runCase("Board: TS -> Squash -> Rocker Verb", rockerverbBoardParams(), in);
    // M13.3: the Lumen optical compressor on the native board.
    ok &= runCase("Board: Lumen -> RAT -> Twin", optoBoardParams(), in);
    // M13.5: the Uni-Vibe on the native board.
    ok &= runCase("Board: RAT -> Swirl -> JCM800", vibeBoardParams(), in);
    // M13.10: the drop-tune on the native board, FIRST in the chain.
    ok &= runCase("Board: Cellar -> RAT -> Mesa (all three Recto switches off-default)", dropBoardParams(), in);
    // M13.6: the ten-band EQ, FIRST in the chain, with every slider off flat.
    ok &= runCase("Board: Decade -> RAT -> JCM800", eqBoardParams(), in);

    if (ok) {
        std::printf(
            "\nPASS: all four amp voices AND every multi-pedal board (including the "
            "GOLD box) are sample-identical across plugin + engine + core.\n");
        return 0;
    }
    std::printf("\nFAIL: identical-core mismatch (see cases above).\n");
    return 1;
}
