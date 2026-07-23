// Clipper native shell — JUCE AudioProcessor.
//
// Thin wrapper: it owns an AudioProcessorValueTreeState (the parameter store +
// state save/restore) and a ClipperEngine (the shared DSP that uses the portable
// core directly). Every block it snapshots the APVTS atomics into a
// clipper::native::Params and hands them to the engine, then runs mono-in ->
// stereo-out. No DSP lives here — this file is purely the host/JUCE glue.

#ifndef CLIPPER_NATIVE_PLUGIN_PROCESSOR_H
#define CLIPPER_NATIVE_PLUGIN_PROCESSOR_H

#include <juce_audio_processors/juce_audio_processors.h>

#include "ClipperEngine.h"

namespace clipper::native {

// Parameter identifiers (APVTS). Stable strings — the saved state keys off these,
// so do not rename without a migration.
namespace pid {
inline constexpr const char* inputTrim   = "inputTrim";
inline constexpr const char* ratOn       = "ratOn";
inline constexpr const char* ratDist     = "ratDist";
inline constexpr const char* ratFilter   = "ratFilter";
inline constexpr const char* ratLevel    = "ratLevel";
inline constexpr const char* sdOn        = "sdOn";
inline constexpr const char* sdDrive     = "sdDrive";
inline constexpr const char* sdTone      = "sdTone";
inline constexpr const char* sdLevel     = "sdLevel";
inline constexpr const char* ampOn       = "ampOn";
inline constexpr const char* volume      = "volume";
inline constexpr const char* bass        = "bass";
inline constexpr const char* middle      = "middle";
inline constexpr const char* treble      = "treble";
inline constexpr const char* bright      = "bright";
inline constexpr const char* cab         = "cab";
inline constexpr const char* chorusMode  = "chorusMode";
inline constexpr const char* chorusSpeed = "chorusSpeed";
inline constexpr const char* chorusDepth = "chorusDepth";
inline constexpr const char* oversampling = "oversampling";
}  // namespace pid

class ClipperAudioProcessor : public juce::AudioProcessor {
public:
    ClipperAudioProcessor();
    ~ClipperAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Read the current APVTS values into a Params snapshot (used by processBlock
    // and reusable by the editor / tests).
    Params snapshotParams() const;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    // Recompute + publish latency to the host from the current param snapshot.
    void updateLatency(const Params& p);

    ClipperEngine engine_;
    int lastReportedLatency_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipperAudioProcessor)
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_PLUGIN_PROCESSOR_H
