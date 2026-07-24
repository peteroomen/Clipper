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
const char* const kPedalKeys[PEDAL_TYPE_COUNT] = {"rat", "sd1", "ts", "muff", "phaser"};
constexpr int   kCabPartition = 128;  // == worklet render quantum / cab partition
// The JCM's fixed internal oversampling — matches the C ABI (docs §18: 4× ships;
// 8× buys nothing at the composed max-gain floor). Independent of the pedal OS.
constexpr int   kJcmOversampling = 4;
constexpr int   kTwinOversampling = 4;  // matches the C ABI (docs §20: 4× ships)
constexpr int   kAc30Oversampling = 4;  // matches the C ABI (docs §23: 4× ships)
constexpr int   kAmpJcm800 = 1;  // Params::ampModel value for the JCM
constexpr int   kAmpTwin = 2;    // Params::ampModel value for the Twin
constexpr int   kAmpAc30 = 3;    // Params::ampModel value for the AC30
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
    using clipper::dsp::AmpModel;
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

    // Amp knobs + toggles. VOLUME feeds clean120 + twin + ac30.
    if (p.volume != o.volume) {
        amp_.setParameter(AmpModel::PARAM_VOLUME, p.volume);
        twin_.setParameter(TwinAmp::PARAM_VOLUME, p.volume);
        ac30_.setParameter(Ac30Amp::PARAM_VOLUME, p.volume);
    }
    // bass/treble are SHARED across ALL FOUR amp voices; middle feeds all but the AC30
    // (top-boost has no mid) — update every tone stack so the inactive voices are
    // already correct at a live switch.
    if (p.bass != o.bass) {
        amp_.setParameter(AmpModel::PARAM_BASS, p.bass);
        jcm_.setParameter(Jcm800Amp::PARAM_BASS, p.bass);
        twin_.setParameter(TwinAmp::PARAM_BASS, p.bass);
        ac30_.setParameter(Ac30Amp::PARAM_BASS, p.bass);
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
    }

    // JCM800-only knobs. The presence field is SHARED: it is the JCM's presence AND
    // the AC30's TOP CUT (both are power-amp HF controls, docs §23).
    if (p.jcmGain != o.jcmGain)         jcm_.setParameter(Jcm800Amp::PARAM_GAIN, p.jcmGain);
    if (p.jcmMaster != o.jcmMaster)     jcm_.setParameter(Jcm800Amp::PARAM_MASTER, p.jcmMaster);
    if (p.jcmPresence != o.jcmPresence) {
        jcm_.setParameter(Jcm800Amp::PARAM_PRESENCE, p.jcmPresence);
        ac30_.setParameter(Ac30Amp::PARAM_TOPCUT, p.jcmPresence);  // reused as TOP CUT
    }

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
    // The phaser is a LINEAR time-varying stage (allpass sweep) — no nonlinearity,
    // so it has no oversampler and no group delay. The web C ABI's
    // phaser_set_oversampling is likewise a no-op.
}

void ClipperEngine::processPedal(int type, const float* in, float* out, int numFrames) {
    switch (type) {
        case PEDAL_RAT:    rat_.process(in, out, numFrames); break;
        case PEDAL_SD:     sd_.process(in, out, numFrames); break;
        case PEDAL_TS:     ts_.process(in, out, numFrames); break;
        case PEDAL_MUFF:   muff_.process(in, out, numFrames); break;
        case PEDAL_PHASER: phaser_.process(in, out, numFrames); break;
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

    setPedalOversampling(params_.oversampling);

    // Declick step: one full fade takes ~6 ms, exactly like the worklet.
    declickStep_ = 1.0f / static_cast<float>(std::max(
                              1L, std::lround(kDeclickSeconds * sampleRate_)));
    declickHoldLen_ = static_cast<int>(std::lround(kDeclickHoldSeconds * sampleRate_));
    declickPhase_ = Declick::Idle;
    declickGain_ = 1.0f;
    declickHold_ = 0;
    commitChain();  // prepare() adopts the requested board with no fade

    const std::vector<float> ir = clipper::dsp::generateDefaultCab2x12IR(sampleRate_);
    cabL_.prepare(sampleRate_, ir.data(), static_cast<int>(ir.size()), sampleRate_,
                  kCabPartition);
    cabR_.prepare(sampleRate_, ir.data(), static_cast<int>(ir.size()), sampleRate_,
                  kCabPartition);

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
        } else {
            amp_.processStereo(cur, outL, outR, numFrames);
        }
        if (p.cab) {
            cabL_.process(outL, outL, numFrames);  // in-place ok
            cabR_.process(outR, outR, numFrames);
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
                    declickHold_ = declickHoldLen_;
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
    if (p.ampOn && p.cab) n += kCabPartition;      // cab adds one partition (128)
    n += limiter_.latencySamples();                // lookahead (64)
    return n;
}

}  // namespace clipper::native
