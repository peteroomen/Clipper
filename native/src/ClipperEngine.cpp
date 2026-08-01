#include "ClipperEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clipper::native {

namespace {
// Mirrors web/src/params.ts INPUT_TRIM_MIN_DB / MAX_DB.
constexpr float kTrimMinDb = -12.0f;
constexpr float kTrimMaxDb = 24.0f;
// Chain-edit declick fade, each way. Mirrors DECLICK_SECONDS in
// web/worklet/clipper-processor.js — long enough to be inaudible as a transient,
// short enough that a reorder feels instant.
constexpr double kDeclickSeconds = 0.006;
// The zero HOLD between the topology swap and the fade back in, during which the
// reordered pedals settle into their new input with the output muted (see the
// ClipperEngine.h note). Six milliseconds covers the allpass/oversampler ring-out
// the chain-edit test measures.
constexpr double kDeclickHoldSeconds = 0.006;
constexpr float  kPi = 3.14159265358979323846f;
// The stable serialization keys, indexed by PedalType.
const char* const kPedalKeys[PEDAL_TYPE_COUNT] = {"rat",    "sd1",  "ts",
                                                  "muff",   "phaser", "gold",
                                                  "wah",   "comp",
                                                  "delay", "gate", "chorus"};
constexpr int   kCabPartition = 128;  // == worklet render quantum / cab partition
// Extra samples to HOLD the output at zero after a CAB swap, before the fade back
// in. Mirrors CAB_SWAP_DEAD_SAMPLES in web/worklet/clipper-processor.js and exists
// for the same reason: CabConvolver::prepare() zeroes the frequency-domain delay
// line, so a freshly swapped-in cab emits silence until its FDL refills (up to two
// partitions). Fading INTO that gap would ramp silence and then jump; holding at
// zero across it and only then ramping means the fade-in always ramps real audio.
constexpr int   kCabSwapDeadSamples = 256;
// A hard cap on the message thread's wait for the audio thread's two-instruction
// commit window (CabCommitting). It cannot spin forever by construction; the cap is
// belt-and-braces so a stopped audio thread can never wedge the UI.
constexpr int   kCabPrepareSpinLimit = 1000000;
// The JCM's fixed internal oversampling — matches the C ABI (docs §18: 4× ships;
// 8× buys nothing at the composed max-gain floor). Independent of the pedal OS.
constexpr int   kJcmOversampling = 4;
constexpr int   kTwinOversampling = 4;  // matches the C ABI (docs §20: 4× ships)
constexpr int   kAc30Oversampling = 4;  // matches the C ABI (docs §23: 4× ships)
constexpr int   kOrangeOversampling = 4;  // matches the C ABI (docs §57: 4× ships)
constexpr int   kAmpJcm800 = 1;  // Params::ampModel value for the JCM
constexpr int   kAmpTwin = 2;    // Params::ampModel value for the Twin
constexpr int   kAmpAc30 = 3;    // Params::ampModel value for the AC30
constexpr int   kAmpOrange = 4;  // Params::ampModel value for the Orange OR120
}  // namespace

float trimKnobToGain(float knob01) {
    const float k = std::min(1.0f, std::max(0.0f, knob01));
    const float db = kTrimMinDb + (kTrimMaxDb - kTrimMinDb) * k;
    return std::pow(10.0f, db / 20.0f);
}

const char* pedalTypeKey(int type) {
    if (type < 0 || type >= PEDAL_TYPE_COUNT) return nullptr;
    return kPedalKeys[type];
}

int pedalTypeFromKey(const char* key) {
    if (key == nullptr) return -1;
    for (int i = 0; i < PEDAL_TYPE_COUNT; ++i)
        if (std::strcmp(key, kPedalKeys[i]) == 0) return i;
    return -1;
}

bool Params::pedalOn(int type) const {
    switch (type) {
        case PEDAL_RAT:    return ratOn;
        case PEDAL_SD:     return sdOn;
        case PEDAL_TS:     return tsOn;
        case PEDAL_MUFF:   return muffOn;
        case PEDAL_PHASER: return phaserOn;
        case PEDAL_GOLD:   return goldOn;
        case PEDAL_WAH:    return wahOn;
        case PEDAL_COMP:   return compOn;
        case PEDAL_GATE:   return gateOn;
        case PEDAL_CHORUS: return ce1On;
        case PEDAL_DELAY:  return delayOn;
        default:           return false;
    }
}

ClipperEngine::ClipperEngine() = default;

void ClipperEngine::applyParamsToModels() {
    const Params& p = params_;

    // Dirt pedals: id 0/1/2 map to (dist/drive, filter/tone, level) — identical
    // positional ABI in RatModel and SdModel.
    rat_.setParameter(clipper::dsp::RatModel::PARAM_DISTORTION, p.ratDist);
    rat_.setParameter(clipper::dsp::RatModel::PARAM_FILTER, p.ratFilter);
    rat_.setParameter(clipper::dsp::RatModel::PARAM_LEVEL, p.ratLevel);

    sd_.setParameter(clipper::dsp::SdModel::PARAM_DRIVE, p.sdDrive);
    sd_.setParameter(clipper::dsp::SdModel::PARAM_TONE, p.sdTone);
    sd_.setParameter(clipper::dsp::SdModel::PARAM_LEVEL, p.sdLevel);

    // The parity pedals — same positional 0/1/2 slot ABI as the RAT/SD-1, reading
    // as Drive/Tone/Level (TS), Sustain/Tone/Volume (Muff) and Speed (phaser: the
    // shared slot 0; slots 1/2 are unused and therefore never exposed natively).
    ts_.setParameter(clipper::dsp::TsModel::PARAM_DRIVE, p.tsDrive);
    ts_.setParameter(clipper::dsp::TsModel::PARAM_TONE, p.tsTone);
    ts_.setParameter(clipper::dsp::TsModel::PARAM_LEVEL, p.tsLevel);

    muff_.setParameter(clipper::dsp::MuffModel::PARAM_SUSTAIN, p.muffSustain);
    muff_.setParameter(clipper::dsp::MuffModel::PARAM_TONE, p.muffTone);
    muff_.setParameter(clipper::dsp::MuffModel::PARAM_VOLUME, p.muffVolume);

    phaser_.setParameter(clipper::dsp::PhaserModel::PARAM_SPEED, p.phaserSpeed);

    // GOLD "Myth" (v1.1 item 6) — the same positional 0/1/2 slot ABI, reading as
    // Gain/Treble/Output. Its measurement hooks (diode flavour, clean-blend defeat,
    // ideal op-amp) are NOT user knobs and are deliberately left at their defaults.
    gold_.setParameter(clipper::dsp::GoldModel::PARAM_GAIN, p.goldGain);
    gold_.setParameter(clipper::dsp::GoldModel::PARAM_TREBLE, p.goldTreble);
    gold_.setParameter(clipper::dsp::GoldModel::PARAM_OUTPUT, p.goldLevel);

    // Wah "Weeper" (docs §58) — same positional 0/1/2 slot ABI, reading as
    // POSITION / SENSITIVITY / VOICE.
    wah_.setParameter(clipper::dsp::WahModel::PARAM_POSITION, p.wahPosition);
    wah_.setParameter(clipper::dsp::WahModel::PARAM_SENSITIVITY, p.wahSense);
    wah_.setParameter(clipper::dsp::WahModel::PARAM_VOICE, p.wahVoice);
    // "Squash" OTA compressor (M13.1) — the same positional slot ABI, but only
    // slots 0 and 2 are real knobs (SUSTAIN / LEVEL). Slot 1 is not written at
    // all: a compressor has no tone control and the model ignores it.
    comp_.setParameter(clipper::dsp::CompModel::PARAM_SUSTAIN, p.compSustain);
    comp_.setParameter(clipper::dsp::CompModel::PARAM_LEVEL, p.compLevel);

    // "Curfew" noise gate (M13.6a) — same positional slot ABI, slots 0 and 2 only
    // (THRESHOLD / DECAY). Slot 1 is not written at all: the reference gate has
    // no third audio control and the model ignores it.
    gate_.setParameter(clipper::dsp::GateModel::PARAM_THRESHOLD, p.gateThreshold);
    gate_.setParameter(clipper::dsp::GateModel::PARAM_DECAY, p.gateDecay);
    // CE-1 "Ensemble" chorus (M13.7) — the same positional slot ABI, reading as
    // RATE / DEPTH / MODE. MODE is DISCRETE inside the model (< 0.5 chorus,
    // >= 0.5 vibrato); it is a float here only because the slot is.
    ce1_.setParameter(clipper::dsp::Ce1Model::PARAM_RATE, p.ce1Rate);
    ce1_.setParameter(clipper::dsp::Ce1Model::PARAM_DEPTH, p.ce1Depth);
    ce1_.setParameter(clipper::dsp::Ce1Model::PARAM_MODE, p.ce1Mode);
    // "Echoman" BBD analog delay (M13.4) — three real knobs on the shared slots.
    delay_.setParameter(clipper::dsp::DelayModel::PARAM_DELAY, p.delayTime);
    delay_.setParameter(clipper::dsp::DelayModel::PARAM_FEEDBACK, p.delayFeedback);
    delay_.setParameter(clipper::dsp::DelayModel::PARAM_BLEND, p.delayBlend);

    // Amp tone stack + volume + bright, then the routed chorus params.
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_VOLUME, p.volume);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_BASS, p.bass);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_MIDDLE, p.middle);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_TREBLE, p.treble);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_CHORUS_SPEED, p.chorusSpeed);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_CHORUS_DEPTH, p.chorusDepth);
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_CHORUS_MODE,
                      static_cast<float>(p.chorusMode));
    amp_.setParameter(clipper::dsp::AmpModel::PARAM_REVERB, p.reverb);

    // JCM800 (M9.4): kept current alongside the Clean 120 so a live model switch is
    // instant. bass/middle/treble are SHARED (same knob values feed both tone
    // stacks); gain/master/presence are JCM-only.
    using J = clipper::dsp::Jcm800Amp;
    jcm_.setParameter(J::PARAM_GAIN, p.jcmGain);
    jcm_.setParameter(J::PARAM_MASTER, p.jcmMaster);
    jcm_.setParameter(J::PARAM_BASS, p.bass);
    jcm_.setParameter(J::PARAM_MID, p.middle);
    jcm_.setParameter(J::PARAM_TREBLE, p.treble);
    jcm_.setParameter(J::PARAM_PRESENCE, p.jcmPresence);
    jcm_.setParameter(J::PARAM_REVERB, p.reverb);  // M10.1 usability add

    // Twin (M10.1): kept current alongside the others. Reuses the shared knobs —
    // volume/bright + bass/mid/treble + reverb + speed/depth (→ tremolo).
    using T = clipper::dsp::TwinAmp;
    twin_.setParameter(T::PARAM_VOLUME, p.volume);
    twin_.setParameter(T::PARAM_BASS, p.bass);
    twin_.setParameter(T::PARAM_MID, p.middle);
    twin_.setParameter(T::PARAM_TREBLE, p.treble);
    twin_.setParameter(T::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
    twin_.setParameter(T::PARAM_REVERB, p.reverb);
    twin_.setParameter(T::PARAM_SPEED, p.chorusSpeed);
    twin_.setParameter(T::PARAM_INTENSITY, p.chorusDepth);
    // The Twin has no chorus → the 'chorusMode' slot is reused as its TREMOLO ON/OFF
    // (docs §20). 0 = off (bit-exact bypass), ≥1 = on. Matches the C-ABI routing.
    twin_.setParameter(T::PARAM_TREMOLO_ENABLE, p.chorusMode >= 1 ? 1.0f : 0.0f);

    // AC30 (M10.2): kept current alongside the others. Reuses the shared knobs —
    // volume + bass/treble + reverb — and REUSES the presence field as its TOP CUT
    // (docs §23). The AC30 top-boost has NO mid control, so 'middle' never routes here.
    using X = clipper::dsp::Ac30Amp;
    ac30_.setParameter(X::PARAM_VOLUME, p.volume);
    ac30_.setParameter(X::PARAM_BASS, p.bass);
    ac30_.setParameter(X::PARAM_TREBLE, p.treble);
    ac30_.setParameter(X::PARAM_TOPCUT, p.jcmPresence);  // presence slot reused as TOP CUT
    ac30_.setParameter(X::PARAM_REVERB, p.reverb);

    // Orange OR120 (M10.3): reuses volume + bass/treble + reverb from the shared
    // fields and REUSES the presence field as its H.F. BOOST (docs §57). It has NO
    // mid control (a James stack is bass + treble), so 'middle' never routes here,
    // and NO master (VOLUME is the whole amp). Its one own field is the F.A.C.
    using O = clipper::dsp::OrangeAmp;
    orange_.setParameter(O::PARAM_VOLUME, p.volume);
    orange_.setParameter(O::PARAM_BASS, p.bass);
    orange_.setParameter(O::PARAM_TREBLE, p.treble);
    orange_.setParameter(O::PARAM_FAC, p.orangeFac);
    orange_.setParameter(O::PARAM_HF_DRIVE, p.jcmPresence);  // presence slot = HF DRIVE
    orange_.setParameter(O::PARAM_REVERB, p.reverb);
}

void ClipperEngine::setParams(const Params& p) {
    params_ = p;
    applyParamsToModels();
    // Setup path (not a live edit): adopt the topology immediately and leave the
    // declick idle, so the first processed block is already the requested chain and
    // is NOT multiplied by any envelope (the bit-exactness contract).
    commitChain();
    declickPhase_ = Declick::Idle;
    declickGain_ = 1.0f;
    declickHold_ = 0;
}

void ClipperEngine::commitChain() {
    const Params& p = params_;
    activeLength_ = std::max(0, std::min(kMaxChain, p.chainLength));
    for (int i = 0; i < activeLength_; ++i) activeChain_[i] = p.chain[i];
    for (int t = 0; t < PEDAL_TYPE_COUNT; ++t) activeOn_[t] = p.pedalOn(t);
}

bool ClipperEngine::chainEditPending() const {
    const Params& p = params_;
    const int want = std::max(0, std::min(kMaxChain, p.chainLength));
    if (want != activeLength_) return true;
    for (int i = 0; i < want; ++i)
        if (p.chain[i] != activeChain_[i]) return true;
    // An engage/bypass toggle is a topology change too: switching a high-gain pedal
    // in or out is a hard step the core's knob smoothing does NOT cover, so the web
    // worklet's immediate flip can tick. Native brackets it with the same fade —
    // only for pedals actually ON the board (an off-board flag changes nothing).
    for (int i = 0; i < want; ++i)
        if (p.pedalOn(p.chain[i]) != activeOn_[p.chain[i]]) return true;
    return false;
}

void ClipperEngine::updateParams(const Params& p) {
    using clipper::dsp::Ac30Amp;
    using clipper::dsp::OrangeAmp;
    using clipper::dsp::AmpModel;
    using clipper::dsp::GoldModel;
    using clipper::dsp::WahModel;
    using clipper::dsp::CompModel;
    using clipper::dsp::DelayModel;
    using clipper::dsp::GateModel;
    using clipper::dsp::Jcm800Amp;
    using clipper::dsp::MuffModel;
    using clipper::dsp::PhaserModel;
    using clipper::dsp::RatModel;
    using clipper::dsp::SdModel;
    using clipper::dsp::TsModel;
    using clipper::dsp::TwinAmp;
    const Params& o = params_;  // old snapshot

    // Dirt-pedal knobs (only the changed ones — never re-seed a steady smoother).
    if (p.ratDist != o.ratDist)     rat_.setParameter(RatModel::PARAM_DISTORTION, p.ratDist);
    if (p.ratFilter != o.ratFilter) rat_.setParameter(RatModel::PARAM_FILTER, p.ratFilter);
    if (p.ratLevel != o.ratLevel)   rat_.setParameter(RatModel::PARAM_LEVEL, p.ratLevel);
    if (p.sdDrive != o.sdDrive)     sd_.setParameter(SdModel::PARAM_DRIVE, p.sdDrive);
    if (p.sdTone != o.sdTone)       sd_.setParameter(SdModel::PARAM_TONE, p.sdTone);
    if (p.sdLevel != o.sdLevel)     sd_.setParameter(SdModel::PARAM_LEVEL, p.sdLevel);
    if (p.tsDrive != o.tsDrive)     ts_.setParameter(TsModel::PARAM_DRIVE, p.tsDrive);
    if (p.tsTone != o.tsTone)       ts_.setParameter(TsModel::PARAM_TONE, p.tsTone);
    if (p.tsLevel != o.tsLevel)     ts_.setParameter(TsModel::PARAM_LEVEL, p.tsLevel);
    if (p.muffSustain != o.muffSustain) muff_.setParameter(MuffModel::PARAM_SUSTAIN, p.muffSustain);
    if (p.muffTone != o.muffTone)   muff_.setParameter(MuffModel::PARAM_TONE, p.muffTone);
    if (p.muffVolume != o.muffVolume) muff_.setParameter(MuffModel::PARAM_VOLUME, p.muffVolume);
    if (p.phaserSpeed != o.phaserSpeed) phaser_.setParameter(PhaserModel::PARAM_SPEED, p.phaserSpeed);
    if (p.goldGain != o.goldGain)     gold_.setParameter(GoldModel::PARAM_GAIN, p.goldGain);
    if (p.goldTreble != o.goldTreble) gold_.setParameter(GoldModel::PARAM_TREBLE, p.goldTreble);
    if (p.goldLevel != o.goldLevel)   gold_.setParameter(GoldModel::PARAM_OUTPUT, p.goldLevel);
    if (p.wahPosition != o.wahPosition) wah_.setParameter(WahModel::PARAM_POSITION, p.wahPosition);
    if (p.wahSense != o.wahSense)     wah_.setParameter(WahModel::PARAM_SENSITIVITY, p.wahSense);
    if (p.wahVoice != o.wahVoice)     wah_.setParameter(WahModel::PARAM_VOICE, p.wahVoice);
    if (p.compSustain != o.compSustain) comp_.setParameter(CompModel::PARAM_SUSTAIN, p.compSustain);
    if (p.compLevel != o.compLevel)   comp_.setParameter(CompModel::PARAM_LEVEL, p.compLevel);
    if (p.gateThreshold != o.gateThreshold)
        gate_.setParameter(GateModel::PARAM_THRESHOLD, p.gateThreshold);
    if (p.gateDecay != o.gateDecay) gate_.setParameter(GateModel::PARAM_DECAY, p.gateDecay);
    if (p.delayTime != o.delayTime)         delay_.setParameter(DelayModel::PARAM_DELAY, p.delayTime);
    if (p.delayFeedback != o.delayFeedback) delay_.setParameter(DelayModel::PARAM_FEEDBACK, p.delayFeedback);
    if (p.delayBlend != o.delayBlend)       delay_.setParameter(DelayModel::PARAM_BLEND, p.delayBlend);

    // Amp knobs + toggles. VOLUME feeds clean120 + twin + ac30.
    if (p.volume != o.volume) {
        amp_.setParameter(AmpModel::PARAM_VOLUME, p.volume);
        twin_.setParameter(TwinAmp::PARAM_VOLUME, p.volume);
        ac30_.setParameter(Ac30Amp::PARAM_VOLUME, p.volume);
        orange_.setParameter(OrangeAmp::PARAM_VOLUME, p.volume);
    }
    // bass/treble are SHARED across ALL FOUR amp voices; middle feeds all but the AC30
    // (top-boost has no mid) — update every tone stack so the inactive voices are
    // already correct at a live switch.
    if (p.bass != o.bass) {
        amp_.setParameter(AmpModel::PARAM_BASS, p.bass);
        jcm_.setParameter(Jcm800Amp::PARAM_BASS, p.bass);
        twin_.setParameter(TwinAmp::PARAM_BASS, p.bass);
        ac30_.setParameter(Ac30Amp::PARAM_BASS, p.bass);
        orange_.setParameter(OrangeAmp::PARAM_BASS, p.bass);
    }
    if (p.middle != o.middle) {
        amp_.setParameter(AmpModel::PARAM_MIDDLE, p.middle);
        jcm_.setParameter(Jcm800Amp::PARAM_MID, p.middle);
        twin_.setParameter(TwinAmp::PARAM_MID, p.middle);
        // AC30 top-boost has NO mid control — 'middle' never reaches it.
    }
    if (p.treble != o.treble) {
        amp_.setParameter(AmpModel::PARAM_TREBLE, p.treble);
        jcm_.setParameter(Jcm800Amp::PARAM_TREBLE, p.treble);
        twin_.setParameter(TwinAmp::PARAM_TREBLE, p.treble);
        ac30_.setParameter(Ac30Amp::PARAM_TREBLE, p.treble);
        orange_.setParameter(OrangeAmp::PARAM_TREBLE, p.treble);
    }
    // BRIGHT feeds clean120 + twin.
    if (p.bright != o.bright) {
        amp_.setParameter(AmpModel::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
        twin_.setParameter(TwinAmp::PARAM_BRIGHT, p.bright ? 1.0f : 0.0f);
    }
    // SPEED/DEPTH feed clean120 chorus + twin tremolo SPEED/INTENSITY.
    if (p.chorusSpeed != o.chorusSpeed) {
        amp_.setParameter(AmpModel::PARAM_CHORUS_SPEED, p.chorusSpeed);
        twin_.setParameter(TwinAmp::PARAM_SPEED, p.chorusSpeed);
    }
    if (p.chorusDepth != o.chorusDepth) {
        amp_.setParameter(AmpModel::PARAM_CHORUS_DEPTH, p.chorusDepth);
        twin_.setParameter(TwinAmp::PARAM_INTENSITY, p.chorusDepth);
    }
    if (p.chorusMode != o.chorusMode) {
        amp_.setParameter(AmpModel::PARAM_CHORUS_MODE, static_cast<float>(p.chorusMode));
        // chorusMode reused as the Twin's TREMOLO ON/OFF (docs §20); live toggle is
        // click-free (the OptoTremolo enable-ramp).
        twin_.setParameter(TwinAmp::PARAM_TREMOLO_ENABLE, p.chorusMode >= 1 ? 1.0f : 0.0f);
    }
    // REVERB feeds all four voices (clean120 + jcm + twin + ac30).
    if (p.reverb != o.reverb) {
        amp_.setParameter(AmpModel::PARAM_REVERB, p.reverb);
        jcm_.setParameter(Jcm800Amp::PARAM_REVERB, p.reverb);
        twin_.setParameter(TwinAmp::PARAM_REVERB, p.reverb);
        ac30_.setParameter(Ac30Amp::PARAM_REVERB, p.reverb);
        orange_.setParameter(OrangeAmp::PARAM_REVERB, p.reverb);
    }

    // JCM800-only knobs. The presence field is SHARED: it is the JCM's presence AND
    // the AC30's TOP CUT (both are power-amp HF controls, docs §23).
    if (p.jcmGain != o.jcmGain)         jcm_.setParameter(Jcm800Amp::PARAM_GAIN, p.jcmGain);
    if (p.jcmMaster != o.jcmMaster)     jcm_.setParameter(Jcm800Amp::PARAM_MASTER, p.jcmMaster);
    if (p.jcmPresence != o.jcmPresence) {
        jcm_.setParameter(Jcm800Amp::PARAM_PRESENCE, p.jcmPresence);
        ac30_.setParameter(Ac30Amp::PARAM_TOPCUT, p.jcmPresence);  // reused as TOP CUT
        orange_.setParameter(OrangeAmp::PARAM_HF_DRIVE, p.jcmPresence);  // …and HF DRIVE
    }

    // M10.3 Orange-only knob: the F.A.C. rotary.
    if (p.orangeFac != o.orangeFac)
        orange_.setParameter(OrangeAmp::PARAM_FAC, p.orangeFac);

    // Oversampling change: reset only the pedals' OS filter state (like the web
    // worklet's per-node setOversampling), not a full re-prepare.
    if (p.oversampling != o.oversampling) setPedalOversampling(p.oversampling);

    // The remaining fields (inputTrim gain, the per-pedal engaged flags, the chain,
    // ampOn/cab) are consumed directly in process()/latencySamples(), so just store
    // them...
    params_ = p;

    // ...and if the store changed the TOPOLOGY (board membership, order, or an
    // engaged flag), start the declick fade. The swap itself happens at the fade
    // zero inside process(); nothing is allocated or freed here or there, since
    // every pedal instance is a plain member that simply stops being visited.
    if (chainEditPending() && declickPhase_ != Declick::Out) declickPhase_ = Declick::Out;
}

void ClipperEngine::setPedalOversampling(int factor) {
    rat_.setOversampling(factor);
    sd_.setOversampling(factor);
    ts_.setOversampling(factor);
    muff_.setOversampling(factor);
    gold_.setOversampling(factor);
    // The wah's RESONATOR is linear and runs at base rate, but its common-emitter
    // OUTPUT stage is a real transistor, so it oversamples like a dirt box and
    // reports real latency (docs §58.4) — unlike the phaser below.
    wah_.setOversampling(factor);
    comp_.setOversampling(factor);
    // The gate accepts and IGNORES this, like the phaser: its signal path is a
    // multiply, so there is nothing for an oversampler to band-limit and it
    // carries no group delay (docs §61.7).
    gate_.setOversampling(factor);
    // ce1_ has no nonlinearity: setOversampling would be a no-op, so it is not called.
    // NOTE: the delay is deliberately NOT re-factored here. Its oversampling is
    // set by the DEVICE (the BBD clock reaches 136.5 kHz, so the internal rate
    // must clear 273 kHz), not by the chain's quality switch — docs §60.5 / ADR 022.
    // The phaser is a LINEAR time-varying stage (allpass sweep) — no nonlinearity,
    // so it has no oversampler and no group delay. The web C ABI's
    // phaser_set_oversampling is likewise a no-op.
}

// ---------------------------------------------------------------------------
// THE CAB / IR PICKER — the message-thread half.
//
// Everything in this block allocates (IR synthesis, peak normalization, one FFT
// spectrum per partition) and is therefore MESSAGE THREAD ONLY. It never touches
// the live pair, never touches the declick state machine, and never blocks on the
// audio thread for longer than its two-instruction commit window.
// ---------------------------------------------------------------------------

bool ClipperEngine::beginCabPrepare() {
    for (int spin = 0; spin < kCabPrepareSpinLimit; ++spin) {
        int s = cabState_.load(std::memory_order_acquire);
        if (s == CabCommitting) continue;     // the audio thread is mid-flip (ns)
        if (s == CabPreparing) return false;  // another prepare already owns it
        if (s == CabSwapped) {                // reclaim the retired pair first
            retireCab();
            continue;
        }
        // Idle or Armed. Taking it back from Armed is deliberate: a second cab
        // change arriving before the audio thread reached its fade zero simply
        // rewrites the pending IR, and the CAS is what stops the audio thread from
        // swapping into a pair we are about to overwrite.
        if (cabState_.compare_exchange_strong(s, CabPreparing, std::memory_order_acq_rel,
                                              std::memory_order_acquire))
            return true;
        // s moved under us — re-read and decide again.
    }
    return false;
}

void ClipperEngine::loadCurrentCabIntoPair(int pair) {
    std::vector<float> ir;
    double irRate = sampleRate_;
    if (cabSource_ == CAB_CUSTOM && !customIr_.empty()) {
        ir = customIr_;
        irRate = customIrRate_ > 0.0 ? customIrRate_ : sampleRate_;
        // M6.6: the engine NEVER trusts a file's level — a cab may colour the tone
        // but must never boost a band past the level entering it, or it pushes the
        // output limiter and fizzes. Same call, same place, as the C ABI's
        // amp_prepare_cab_custom. The BUILT-INS are deliberately NOT re-normalized:
        // their generators already normalize, and a second pass would perturb the
        // low mantissa bits of an IR the identical-core test renders bit-exactly.
        clipper::dsp::peakNormalizeIR(ir, sampleRate_);
    } else {
        // The native CabChoice ids and the C ABI's built-in ids diverge at 2 (see
        // the enum's comment) — mapped HERE, in code, rather than by index maths.
        ir = cabSource_ == CAB_BRIT412   ? clipper::dsp::generateBrit4x12IR(sampleRate_)
           : cabSource_ == CAB_ORANGE412 ? clipper::dsp::generateOrange4x12IR(sampleRate_)
                                         : clipper::dsp::generateDefaultCab2x12IR(sampleRate_);
    }
    if (ir.empty()) return;
    const int len = static_cast<int>(ir.size());
    // The partition stays 128 in every case — the cab's contribution to reported
    // latency must not move when the user changes cabs (asserted in the tests).
    cab_[pair][0].prepare(sampleRate_, ir.data(), len, irRate, kCabPartition);
    cab_[pair][1].prepare(sampleRate_, ir.data(), len, irRate, kCabPartition);
}

bool ClipperEngine::prepareCabBuiltin(int which) {
    const int w = (which == CAB_BRIT412)     ? CAB_BRIT412
                : (which == CAB_ORANGE412)  ? CAB_ORANGE412
                                            : CAB_CLEAN212;
    if (!beginCabPrepare()) return false;
    cabSource_ = w;
    loadCurrentCabIntoPair(activeCabPair_.load(std::memory_order_relaxed) ^ 1);
    cabState_.store(CabArmed, std::memory_order_release);
    return true;
}

bool ClipperEngine::prepareCabCustom(const float* ir, int irLength, double irSampleRate) {
    if (ir == nullptr || irLength <= 0) return false;
    if (!beginCabPrepare()) return false;
    customIr_.assign(ir, ir + irLength);
    customIrRate_ = irSampleRate > 0.0 ? irSampleRate : sampleRate_;
    cabSource_ = CAB_CUSTOM;
    loadCurrentCabIntoPair(activeCabPair_.load(std::memory_order_relaxed) ^ 1);
    cabState_.store(CabArmed, std::memory_order_release);
    return true;
}

bool ClipperEngine::cabSwapArmed() const {
    const int s = cabState_.load(std::memory_order_acquire);
    return s == CabArmed || s == CabCommitting;
}

void ClipperEngine::commitCabNow() {
    // SETUP ONLY (see the header). With no audio thread running, commitCabIfArmed()
    // is simply performed here instead of at a fade zero, and the retirement can
    // follow immediately.
    commitCabIfArmed();
    retireCab();
}

void ClipperEngine::retireCab() {
    int s = CabSwapped;
    if (!cabState_.compare_exchange_strong(s, CabIdle, std::memory_order_acq_rel,
                                           std::memory_order_acquire))
        return;
    // The CAS above acquires the audio thread's release store, so the flipped
    // activeCabPair_ is visible here — the pair we are about to clear is genuinely
    // the retired one and not the live one.
    const int retired = activeCabPair_.load(std::memory_order_relaxed) ^ 1;
    // Retirement: drop the retired convolvers' running state on the MESSAGE thread.
    // Its heap buffers are released (and reused) by the next prepare into this pair,
    // which is also message thread — so no free ever reaches the render path.
    cab_[retired][0].reset();
    cab_[retired][1].reset();
}

bool ClipperEngine::commitCabIfArmed() {
    // AUDIO THREAD. One CAS to claim the swap, one integer flip, one release store.
    int s = CabArmed;
    if (!cabState_.compare_exchange_strong(s, CabCommitting, std::memory_order_acq_rel,
                                           std::memory_order_relaxed))
        return false;
    activeCabPair_.store(activeCabPair_.load(std::memory_order_relaxed) ^ 1,
                         std::memory_order_relaxed);
    cabCommits_.fetch_add(1, std::memory_order_relaxed);
    cabState_.store(CabSwapped, std::memory_order_release);
    return true;
}

void ClipperEngine::processPedal(int type, const float* in, float* out, int numFrames) {
    switch (type) {
        case PEDAL_RAT:    rat_.process(in, out, numFrames); break;
        case PEDAL_SD:     sd_.process(in, out, numFrames); break;
        case PEDAL_TS:     ts_.process(in, out, numFrames); break;
        case PEDAL_MUFF:   muff_.process(in, out, numFrames); break;
        case PEDAL_PHASER: phaser_.process(in, out, numFrames); break;
        case PEDAL_GOLD:   gold_.process(in, out, numFrames); break;
        case PEDAL_WAH:    wah_.process(in, out, numFrames); break;
        case PEDAL_COMP:   comp_.process(in, out, numFrames); break;
        case PEDAL_GATE:   gate_.process(in, out, numFrames); break;
        case PEDAL_CHORUS: ce1_.process(in, out, numFrames); break;
        case PEDAL_DELAY:  delay_.process(in, out, numFrames); break;
        default: break;
    }
}

void ClipperEngine::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(1, maxBlockSize);

    // Push targets first, then prepare so each model snaps its smoothers to them.
    applyParamsToModels();

    rat_.prepare(sampleRate_, maxBlock_);
    sd_.prepare(sampleRate_, maxBlock_);
    ts_.prepare(sampleRate_, maxBlock_);
    muff_.prepare(sampleRate_, maxBlock_);
    phaser_.prepare(sampleRate_);  // linear: no block-size scratch to size
    gold_.prepare(sampleRate_, maxBlock_);
    wah_.prepare(sampleRate_, maxBlock_);
    comp_.prepare(sampleRate_, maxBlock_);
    gate_.prepare(sampleRate_, maxBlock_);
    ce1_.prepare(sampleRate_, maxBlock_);
    delay_.prepare(sampleRate_, maxBlock_);
    amp_.prepare(sampleRate_, maxBlock_);
    // The JCM runs at its fixed 4× internally (set BEFORE prepare so its stages
    // size to it), independent of the pedal OS selector — matches the C ABI.
    jcm_.setOversampling(kJcmOversampling);
    jcm_.prepare(sampleRate_, maxBlock_);
    // The Twin likewise runs at its fixed 4× internally (docs §20), independent of
    // the pedal OS selector — matches the C ABI.
    twin_.setOversampling(kTwinOversampling);
    twin_.prepare(sampleRate_, maxBlock_);
    // The AC30 likewise runs at its fixed 4× internally (docs §23), independent of
    // the pedal OS selector — matches the C ABI.
    ac30_.setOversampling(kAc30Oversampling);
    ac30_.prepare(sampleRate_, maxBlock_);
    orange_.setOversampling(kOrangeOversampling);
    orange_.prepare(sampleRate_, maxBlock_);

    setPedalOversampling(params_.oversampling);

    // Declick step: one full fade takes ~6 ms, exactly like the worklet.
    declickStep_ = 1.0f / static_cast<float>(std::max(
                              1L, std::lround(kDeclickSeconds * sampleRate_)));
    declickHoldLen_ = static_cast<int>(std::lround(kDeclickHoldSeconds * sampleRate_));
    declickPhase_ = Declick::Idle;
    declickGain_ = 1.0f;
    declickHold_ = 0;
    commitChain();  // prepare() adopts the requested board with no fade

    // THE CAB. prepare() rebuilds the CURRENTLY SELECTED cab (cabSource_, which the
    // message thread owns and which survives a re-prepare) at the new engine rate,
    // into pair 0, and makes pair 0 live. A default engine has cabSource_ ==
    // CAB_CLEAN212, so this is byte-for-byte the pre-picker call and the
    // identical-core render is bit-identical.
    //
    // Only the ACTIVE pair is prepared here. The spare is never activated without a
    // prepare of its own — that is the C ABI's invariant (ADR 003), and it is what
    // stops a swap from splicing in stale convolution tail.
    cabState_.store(CabIdle, std::memory_order_release);
    activeCabPair_.store(0, std::memory_order_relaxed);
    loadCurrentCabIntoPair(0);

    limiter_.prepare(sampleRate_);

    bufA_.assign(static_cast<size_t>(maxBlock_), 0.0f);
    bufB_.assign(static_cast<size_t>(maxBlock_), 0.0f);
}

void ClipperEngine::process(const float* in, float* outL, float* outR,
                            int numFrames) {
    if (numFrames <= 0) return;
    if (numFrames > maxBlock_) {
        // Chunk oversized blocks so scratch never overflows (mirrors the core's own
        // internal chunking contract).
        int off = 0;
        while (off < numFrames) {
            const int chunk = std::min(maxBlock_, numFrames - off);
            process(in + off, outL + off, outR + off, chunk);
            off += chunk;
        }
        return;
    }

    const Params& p = params_;

    // 0. A CAB SWAP prepared on the message thread waits for the declick fade's
    // zero, exactly like a chain edit. It is armed HERE rather than in
    // updateParams() so the message thread never has to touch the declick state
    // machine — it publishes one atomic and the audio thread does the rest. When an
    // edit is already in flight the swap simply rides that same fade to its zero
    // (the worklet's _stageEdit behaviour).
    if (declickPhase_ == Declick::Idle &&
        cabState_.load(std::memory_order_acquire) == CabArmed)
        declickPhase_ = Declick::Out;

    // 1. Input trim into ping-pong buffer A.
    const float g = trimKnobToGain(p.inputTrim);
    float* cur = bufA_.data();
    float* other = bufB_.data();
    for (int i = 0; i < numFrames; ++i) cur[i] = in[i] * g;

    // 2. The USER-ORDERED pedal board: walk the COMMITTED chain in order and run
    // each engaged pedal. Ping-pong so a bypassed pedal is a true pass-through (its
    // buffer is simply not swapped in) — identical to the worklet's `continue`.
    // Reading activeChain_ (not params_.chain) is what makes a reorder land only at
    // the declick zero, never mid-block.
    for (int i = 0; i < activeLength_; ++i) {
        const int type = activeChain_[i];
        if (!activeOn_[type]) continue;
        processPedal(type, cur, other, numFrames);
        std::swap(cur, other);
    }

    // 3. Amp stage. Powered off => stereo passthrough of the mono chain signal.
    // Clean 120 splits to a stereo pair (chorus); the JCM is a MONO head, so its
    // single output is mirrored to both sides (dual-mono) before the identical cab
    // pair — matching the C ABI's amp_process_stereo.
    if (!p.ampOn) {
        for (int i = 0; i < numFrames; ++i) {
            outL[i] = cur[i];
            outR[i] = cur[i];
        }
    } else {
        if (p.ampModel == kAmpJcm800) {
            jcm_.process(cur, outL, numFrames);
            for (int i = 0; i < numFrames; ++i) outR[i] = outL[i];
        } else if (p.ampModel == kAmpTwin) {
            // The Twin is a mono combo head → dual-mono into the identical cab pair.
            twin_.process(cur, outL, numFrames);
            for (int i = 0; i < numFrames; ++i) outR[i] = outL[i];
        } else if (p.ampModel == kAmpAc30) {
            // The AC30 is a mono combo head → dual-mono into the identical cab pair.
            ac30_.process(cur, outL, numFrames);
            for (int i = 0; i < numFrames; ++i) outR[i] = outL[i];
        } else if (p.ampModel == kAmpOrange) {
            // The OR120 is a mono HEAD → dual-mono into the identical cab pair.
            orange_.process(cur, outL, numFrames);
            for (int i = 0; i < numFrames; ++i) outR[i] = outL[i];
        } else {
            amp_.processStereo(cur, outL, outR, numFrames);
        }
        if (p.cab) {
            // The LIVE pair. Reading the index once per block is the entire
            // audio-thread cost of the cab picker.
            const int pair = activeCabPair_.load(std::memory_order_relaxed);
            cab_[pair][0].process(outL, outL, numFrames);  // in-place ok
            cab_[pair][1].process(outR, outR, numFrames);
        }
    }

    // 4. Chain-edit DECLICK, applied to the amp output BEFORE the limiter (exactly
    // where the worklet applies it). When nothing is in flight the envelope is
    // skipped entirely rather than multiplied by 1.0 — a steady chain must stay
    // bit-for-bit identical to a single-shot core render.
    if (declickPhase_ != Declick::Idle || declickGain_ < 1.0f) {
        float dg = declickGain_;
        for (int i = 0; i < numFrames; ++i) {
            if (declickPhase_ == Declick::Out) {
                dg -= declickStep_;
                if (dg <= 0.0f) {
                    dg = 0.0f;
                    commitChain();  // the topology swap happens exactly at zero
                    // ...and so does the CAB swap: one CAS + one integer flip, the
                    // whole audio-thread cost of a cab change (ADR 003). A swapped-in
                    // convolver's FDL is empty, so the zero HOLD is widened to cover
                    // the worklet's CAB_SWAP_DEAD_SAMPLES as well as the settling
                    // window the chain edits need.
                    const bool cabSwapped = commitCabIfArmed();
                    declickHold_ = cabSwapped
                                       ? std::max(declickHoldLen_, kCabSwapDeadSamples)
                                       : declickHoldLen_;
                    declickPhase_ = Declick::Hold;
                }
            } else if (declickPhase_ == Declick::Hold) {
                // Output stays at zero while the just-reordered pedals settle into
                // their new input; they are still being PROCESSED, just not heard.
                if (--declickHold_ <= 0) declickPhase_ = Declick::In;
            } else if (declickPhase_ == Declick::In) {
                dg += declickStep_;
                if (dg >= 1.0f) {
                    dg = 1.0f;
                    declickPhase_ = Declick::Idle;
                }
            }
            // Raised-cosine map of the linear ramp (C1-smooth at both ends).
            const float env = dg >= 1.0f ? 1.0f : 0.5f - 0.5f * std::cos(kPi * dg);
            outL[i] *= env;
            outR[i] *= env;
        }
        declickGain_ = dg;
    }

    // 5. Final safety limiter (stereo, one shared gain — image-preserving).
    limiter_.processStereo(outL, outR, numFrames);
}

int ClipperEngine::latencySamples() const {
    const Params& p = params_;
    int n = 0;
    // Every ENGAGED pedal ON THE BOARD adds its oversampling group delay (they run
    // in series). Off-board pedals contribute nothing however their flags read —
    // this is the parity change: latency now follows the chain, not two fixed slots.
    // Read from the TARGET chain so the host learns about an edit immediately.
    const int len = std::max(0, std::min(kMaxChain, p.chainLength));
    for (int i = 0; i < len; ++i) {
        const int type = p.chain[i];
        if (!p.pedalOn(type)) continue;
        switch (type) {
            case PEDAL_RAT:  n += rat_.latencySamples(); break;
            case PEDAL_SD:   n += sd_.latencySamples(); break;
            case PEDAL_TS:   n += ts_.latencySamples(); break;
            case PEDAL_MUFF: n += muff_.latencySamples(); break;
            // The gold overdrive oversamples its germanium clipper like the other
            // dirt boxes, so it carries the same OS group delay.
            case PEDAL_GOLD: n += gold_.latencySamples(); break;
            // The wah's transistor output stage oversamples too (§58.4), so it
            // carries the same group delay as a dirt box. Its resonator does not.
            case PEDAL_WAH:  n += wah_.latencySamples(); break;
            // The compressor oversamples its OTA gain cell, so it carries the
            // same OS group delay as the dirt boxes.
            case PEDAL_COMP: n += comp_.latencySamples(); break;
            // The gate is not oversampled — zero group delay, like the phaser.
            case PEDAL_GATE: break;
            // The CE-1 chorus is linear time-varying — no oversampling, and its
            // modulated delay IS the effect rather than a compensable latency.
            case PEDAL_CHORUS: break;
            // The delay reports 0 by design: its dry path never enters the
            // oversampled domain (docs §60), so it adds no latency to the chain.
            case PEDAL_DELAY: n += delay_.latencySamples(); break;
            // The phaser is linear — zero group delay.
            case PEDAL_PHASER: break;
            default: break;
        }
    }
    // The JCM/Twin add their own oversampling group delay when the powered voice;
    // the linear Clean 120 adds nothing.
    if (p.ampOn && p.ampModel == kAmpJcm800) n += jcm_.latencySamples();
    if (p.ampOn && p.ampModel == kAmpTwin) n += twin_.latencySamples();
    if (p.ampOn && p.ampModel == kAmpAc30) n += ac30_.latencySamples();
    if (p.ampOn && p.ampModel == kAmpOrange) n += orange_.latencySamples();
    if (p.ampOn && p.cab) n += kCabPartition;      // cab adds one partition (128)
    n += limiter_.latencySamples();                // lookahead (64)
    return n;
}

}  // namespace clipper::native
