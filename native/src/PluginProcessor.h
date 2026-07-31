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

#include <atomic>
#include <vector>

#include "ClipperEngine.h"

namespace clipper::native {

// Parameter identifiers (APVTS). Stable strings — the saved state keys off these,
// so do not rename without a migration. NATIVE PARITY added the ts*/muff*/phaser*
// ids; every pre-existing id is untouched, so old sessions still load.
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
// Native parity: the three pedals the fixed chain never had. Same 0..1 knob shape;
// names/defaults mirror web PEDAL_KNOB_DEFAULTS.
inline constexpr const char* tsOn        = "tsOn";
inline constexpr const char* tsDrive     = "tsDrive";
inline constexpr const char* tsTone      = "tsTone";
inline constexpr const char* tsLevel     = "tsLevel";
inline constexpr const char* muffOn      = "muffOn";
inline constexpr const char* muffSustain = "muffSustain";
inline constexpr const char* muffTone    = "muffTone";
inline constexpr const char* muffVolume  = "muffVolume";
inline constexpr const char* phaserOn    = "phaserOn";
inline constexpr const char* phaserSpeed = "phaserSpeed";
// v1.1 item 6: the GOLD "Myth" transparent overdrive. Same three-knob shape as the
// other dirt boxes; defaults mirror web GOLD_KNOB_DEFAULTS.
inline constexpr const char* goldOn      = "goldOn";
inline constexpr const char* goldGain    = "goldGain";
inline constexpr const char* goldTreble  = "goldTreble";
inline constexpr const char* goldLevel   = "goldLevel";
inline constexpr const char* ampOn       = "ampOn";
inline constexpr const char* volume      = "volume";
inline constexpr const char* bass        = "bass";
inline constexpr const char* middle      = "middle";
inline constexpr const char* treble      = "treble";
inline constexpr const char* bright      = "bright";
inline constexpr const char* cab         = "cab";
// The cab/IR PICKER (2026-07-31): which cabinet IR is loaded. A choice parameter —
// automatable, and it round-trips with the session like every other parameter. The
// custom IR's FILE PATH is deliberately NOT a parameter (a host cannot automate a
// path, and a path is not a control); it lives in the state tree, next to the board.
inline constexpr const char* cabModel    = "cabModel";
inline constexpr const char* chorusMode  = "chorusMode";
inline constexpr const char* chorusSpeed = "chorusSpeed";
inline constexpr const char* chorusDepth = "chorusDepth";
inline constexpr const char* reverb      = "reverb";
// M9.4 JCM800: the amp-voice choice + the three JCM-only knobs.
inline constexpr const char* ampModel    = "ampModel";
inline constexpr const char* jcmGain     = "jcmGain";
inline constexpr const char* jcmMaster   = "jcmMaster";
inline constexpr const char* jcmPresence = "jcmPresence";
inline constexpr const char* oversampling = "oversampling";
}  // namespace pid

class ClipperAudioProcessor : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener,
                              private juce::AsyncUpdater {
public:
    ClipperAudioProcessor();
    ~ClipperAudioProcessor() override;

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

    // ---- THE BOARD (chain order + membership) --------------------------------
    // Which pedal types are on the board, in signal order. This is deliberately
    // NOT an automatable parameter: it is a topology, not a continuous control, and
    // hosts have no sane way to automate "the RAT moved after the Muff". It lives
    // as a child node of the APVTS state tree, so it round-trips through the DAW
    // session with everything else (getStateInformation is the whole tree).
    //
    // The audio thread never reads the ValueTree: setChainOrder also publishes a
    // PACKED snapshot into an atomic (a 4-bit length + kMaxChain × 4-bit type
    // slots), which snapshotParams() unpacks lock-free. The fields were widened
    // from 3 bits when the GOLD pedal made the board six deep: 3-bit slots would
    // still have fitted six types, but only just, and a seventh would have silently
    // aliased. Four bits keeps a whole spare type per slot and still fits a single
    // uint32 — so the publish stays ONE relaxed atomic store, never a lock.
    std::vector<int> chainOrder() const;
    void setChainOrder(const std::vector<int>& types);
    // Bumped on every board change (including a host state load), so the editor can
    // notice an external edit and rebuild its cards.
    int chainVersion() const { return chainVersion_.load(); }

    // The board a FRESH instance opens with — the web app's DEFAULT_RIG: one RAT.
    static std::vector<int> defaultChain() { return {PEDAL_RAT}; }
    // The board a PRE-PARITY saved session migrates to: the old fixed pair, so a
    // session saved by the two-pedal build reloads sounding exactly the same (its
    // ratOn/sdOn flags still say which of the two were engaged).
    static std::vector<int> legacyChain() { return {PEDAL_RAT, PEDAL_SD}; }

    // ---- THE CAB / IR PICKER (MESSAGE THREAD ONLY) ---------------------------
    // The engine owns the real-time discipline (ClipperEngine's banner); this side
    // owns the state: which cab is chosen (an APVTS parameter) and, for a custom
    // IR, WHERE it came from (a state-tree property, not a parameter).
    //
    // The path lives in a `cab` child node of the APVTS state, exactly like the
    // board's `order` — so it travels with the DAW session, survives a preset
    // switch, and never appears in a host's automation lane.
    int cabChoice() const;                     // CAB_CLEAN212 / _BRIT412 / _CUSTOM
    void setCabChoice(int choice);             // set the parameter + rebuild the cab
    juce::String customIrPath() const;
    void setCustomIrPath(const juce::String&);
    // Decode `file`, select it as the custom cab, and rebuild. False (with a note
    // set) if the file cannot be used; the previous cab keeps playing.
    bool loadCustomIrFile(const juce::File& file);

    // What the UI shows: the current cab's name, and a one-line note that is empty
    // unless something needs saying (a missing IR, a truncated one).
    juce::String cabLabel() const;
    juce::String customIrLabel() const { return customIrLabel_; }
    juce::String cabNote() const { return cabNote_; }
    // Bumped on every cab change, so the editor can notice one it did not make
    // (host automation, a session load) the same way it watches the board.
    int cabVersion() const { return cabVersion_.load(); }
    // Give the engine's retired convolver pair back on the message thread. The
    // editor's timer calls this; nothing breaks if it does not (the next prepare
    // reclaims it), it just happens sooner.
    void retireCab() { engine_.retireCab(); }

private:
    // Rebuild the selected cab into the engine's spare pair. `immediate` performs
    // the swap on the spot instead of arming it for the audio thread's fade zero,
    // and is legal ONLY where no audio thread is running (the constructor,
    // prepareToPlay).
    // `force` skips the idempotence check below — a USER action (a menu pick, a
    // file load) must always be honoured, even when the request looks identical to
    // the last one, because the world may have changed under it (the IR file was
    // deleted and the player is asking us to try again).
    void applyCabFromState(bool immediate, bool force = false);
    // APVTS listener: the ONLY reason the processor is one. Host automation of
    // cabModel can arrive on the AUDIO thread, and rebuilding a cab means file I/O
    // and allocation — so this just wakes the message thread.
    void parameterChanged(const juce::String& id, float value) override;
    void handleAsyncUpdate() override;  // ...and here is the message thread.

    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    // Recompute + publish latency to the host from the current param snapshot.
    void updateLatency(const Params& p);

    // Re-read the board out of the state tree into the packed atomic (after a host
    // state load), creating/repairing the node if it is missing or malformed.
    void syncChainFromState(bool legacyFallback);

    ClipperEngine engine_;
    int lastReportedLatency_ = -1;

    // Packed board snapshot for the audio thread (see chainOrder above).
    std::atomic<juce::uint32> packedChain_{0};
    std::atomic<int> chainVersion_{0};

    // Cab-picker presentation state (message thread only).
    juce::String customIrLabel_;  // "" until a custom IR has actually loaded
    juce::String cabNote_;        // "" unless there is something to tell the player
    std::atomic<int> cabVersion_{0};
    double engineRate_ = 48000.0;  // the rate an IR must be resampled to
    // What the engine was last ASKED for ("choice|path"). A user's click reaches
    // applyCabFromState twice — once directly, once through the parameter listener's
    // async hop — and without this the second pass would arm a SECOND swap, which
    // the player would hear as two declick fades on one click.
    juce::String lastCabSig_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipperAudioProcessor)
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_PLUGIN_PROCESSOR_H
