#include "UiSound.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace clipper::native::uisound {
namespace {

// One playing blip. Exponential ramps are per-sample multipliers precomputed at
// trigger time from the device sample rate, exactly mirroring the WebAudio
// exponentialRampToValueAtTime curves in ui-sound.ts.
struct Voice {
    bool active = false;
    bool square = false;   // tick = square osc; thunk = sine
    double phase = 0.0;
    double freq = 0.0;
    double freqMul = 1.0;  // per-sample exp ramp (thunk's 60 ms drop to 45 Hz)
    double freqEndAt = 0;  // sample index where the freq ramp stops
    double gain = 0.0;
    double gainMul = 1.0;
    double samplesLeft = 0;
    double t = 0;  // samples since trigger
};

class Engine : private juce::AudioIODeviceCallback {
public:
    static Engine* get() {
        // Standalone only — see UiSound.h. Checked before anything is built, so a
        // hosted plugin never even constructs the device manager.
        if (!juce::JUCEApplicationBase::isStandaloneApp()) return nullptr;
        static Engine engine;
        return engine.ok_ ? &engine : nullptr;
    }

    void trigger(bool isTick, bool down) {
        const juce::ScopedLock sl(lock_);
        // Steal the oldest slot; four voices is plenty for UI blips.
        Voice* v = &voices_[0];
        for (auto& c : voices_) {
            if (!c.active) { v = &c; break; }
            if (c.t > v->t) v = &c;
        }
        const double fs = sampleRate_;
        *v = {};
        v->active = true;
        v->square = isTick;
        if (isTick) {
            v->freq = 2100.0;
            v->gain = 0.015;
            v->gainMul = std::pow(0.0001 / 0.015, 1.0 / (0.015 * fs));
            v->samplesLeft = 0.02 * fs;
        } else {
            v->freq = down ? 95.0 : 120.0;
            v->freqMul = std::pow(45.0 / v->freq, 1.0 / (0.06 * fs));
            v->freqEndAt = 0.06 * fs;
            v->gain = 0.12;
            v->gainMul = std::pow(0.0001 / 0.12, 1.0 / (0.09 * fs));
            v->samplesLeft = 0.1 * fs;
        }
    }

private:
    Engine() {
        // Output-only; initialise returns an error string when there is no usable
        // device (headless container) — in that case the engine stays disabled.
        ok_ = deviceManager_.initialiseWithDefaultDevices(0, 2).isEmpty() &&
              deviceManager_.getCurrentAudioDevice() != nullptr;
        if (ok_) deviceManager_.addAudioCallback(this);
    }
    ~Engine() override {
        if (ok_) deviceManager_.removeAudioCallback(this);
        deviceManager_.closeAudioDevice();
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        sampleRate_ = device->getCurrentSampleRate();
    }
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext(const float* const*, int,
                                          float* const* out, int numOut, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        for (int ch = 0; ch < numOut; ++ch)
            if (out[ch]) juce::FloatVectorOperations::clear(out[ch], numSamples);
        // This is a UI-blip side device, not the processing graph; a missed block
        // under contention would only delay a click, so a try-lock keeps it clean.
        const juce::ScopedTryLock sl(lock_);
        if (!sl.isLocked()) return;
        for (auto& v : voices_) {
            if (!v.active) continue;
            for (int i = 0; i < numSamples; ++i) {
                if (v.samplesLeft-- <= 0) { v.active = false; break; }
                const float s = v.square
                                    ? (std::fmod(v.phase, 1.0) < 0.5 ? 1.0f : -1.0f)
                                    : (float)std::sin(v.phase * juce::MathConstants<double>::twoPi);
                const float y = s * (float)v.gain;
                for (int ch = 0; ch < numOut; ++ch)
                    if (out[ch]) out[ch][i] += y;
                v.phase += v.freq / sampleRate_;
                v.gain *= v.gainMul;
                if (v.t < v.freqEndAt) v.freq *= v.freqMul;
                v.t += 1.0;
            }
        }
    }

    juce::AudioDeviceManager deviceManager_;
    juce::CriticalSection lock_;
    Voice voices_[4];
    double sampleRate_ = 44100.0;
    bool ok_ = false;
};

}  // namespace

void tick() {
    if (auto* e = Engine::get()) e->trigger(true, false);
}

void thunk(bool down) {
    if (auto* e = Engine::get()) e->trigger(false, down);
}

}  // namespace clipper::native::uisound
