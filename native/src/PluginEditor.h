// Clipper native shell — the neumorphic PEDAL BOARD editor (native parity).
//
// A doctrine-compliant (docs §17) board, laid out LEFT TO RIGHT exactly like the
// web's Board.tsx — the arrangement the user asked to keep:
//
//   [INPUT] ~~ [pedal] ~~ [pedal] ~~ ... ~~ [+ ADD] ~~ [AMP]
//
// joined by neumorphic PATCH CABLES: sagging rubber tubes running jack to jack,
// drawn behind the enclosures so each plug reads as entering its socket.
//
// The board is DYNAMIC (this is the parity work). Cards come from the processor's
// chain order, and the user can:
//   * ADD    — the gear tray lists every pedal type not already on the board;
//   * REMOVE — the ✕ chip on each card;
//   * SWAP   — the ⇄ chip, listing the types not on the board;
//   * MOVE   — the ◀ ▶ chips, or by DRAGGING a card's ⠿ grip: the card reorders
//              live under the pointer, exactly as the web board does.
// Every edit goes through the processor's chain state, so it round-trips with the
// session, and the engine applies it through the 6 ms declick fade.
//
// The AMP stays fixed at the end of the chain, a single card whose FACE switches
// with the amp-voice choice (Eight Hundred gold / Twin silver-blue / Thirty copper
// / Clean 120 red), mirroring the web Amp faces.
//
// All controls stay 100% APVTS-attached; the drawing lives in ClipperLookAndFeel
// and PedalCard. Light-bench look only (a dark theme is still future work).

#ifndef CLIPPER_NATIVE_PLUGIN_EDITOR_H
#define CLIPPER_NATIVE_PLUGIN_EDITOR_H

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>
#include <vector>

#include "ClipperLookAndFeel.h"
#include "PedalCard.h"
#include "PluginProcessor.h"

namespace clipper::native {

class ClipperAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer {
public:
    explicit ClipperAudioProcessorEditor(ClipperAudioProcessor&);
    ~ClipperAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Re-sync every widget (the board cards, amp face + labels/visibility, dim
    // states, toggle positions, combo selections) from the current APVTS state.
    // Called on construction and whenever a bound parameter changes; also used by
    // the headless snapshot tool to force a synchronous refresh after setting
    // params.
    void refreshFromState();

    // --- board editing (also the snapshot tool's hooks) ----------------------
    void addPedal(int type);           // append before the amp
    void removePedalAt(int index);
    void swapPedalAt(int index, int newType);
    void movePedal(int from, int to);  // live-reorder, declicked by the engine

private:
    using ComboAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ParamAttach = juce::ParameterAttachment;

    void timerCallback() override;  // notices an EXTERNAL board change (host load)

    void rebuildBoard();          // recreate the cards from the processor's chain
    void scheduleBoardRefresh();  // ...but never from inside a card's own callback
    void applyBoardSizeFloor();   // the window is never narrower than the board
    void showTrayMenu();      // the gear tray's "add a pedal" popup
    void updateAmpFace();     // rebuild the visible amp control set for the voice
    void updateEnablement();  // dim bypassed sections' knobs
    void layoutAmpCard(juce::Rectangle<int>);
    void dragCardTo(int cardIndex, int parentX);  // pointer-driven live reorder

    ClipperAudioProcessor& proc_;
    ClipperLookAndFeel lnf_;

    // Top-bar selectors.
    juce::ComboBox ampVoiceBox_, oversampleBox_;
    std::unique_ptr<ComboAttach> ampVoiceAttach_, oversampleAttach_;

    // INPUT card.
    NeuKnob inputTrim_;
    std::unique_ptr<SliderAttach> inputTrimAttach_;

    // THE BOARD: one card per chain entry, plus the gear tray.
    juce::OwnedArray<PedalCard> cards_;
    ChipButton trayAdd_{"+ ADD PEDAL"};
    int boardVersion_ = -1;
    int draggingCard_ = -1;
    bool sizingFloor_ = false;  // re-entrancy guard for applyBoardSizeFloor

    // AMP card — the full superset of knobs; visibility/labels set per voice.
    NeuKnob volume_, bass_, middle_, treble_, presence_, master_, gain_, reverb_;
    NeuKnob modSpeed_, modDepth_;  // chorus/tremolo speed + depth/intensity
    std::unique_ptr<SliderAttach> volumeAttach_, bassAttach_, middleAttach_, trebleAttach_,
        presenceAttach_, masterAttach_, gainAttach_, reverbAttach_, modSpeedAttach_,
        modDepthAttach_;
    LeverToggle bright_, cab_;
    PowerControl power_;
    ModeSwitch chorusMode_;
    std::unique_ptr<ParamAttach> brightAttach_, cabAttach_, ampOnAttach_, chorusModeAttach_,
        ampModelListen_;

    // Card rectangles (computed in resized(), painted in paint()).
    juce::Rectangle<int> cardInput_, cardAmp_, trayBounds_;
    juce::Rectangle<int> ampModRowCaption_;  // caption box for the chorus/tremolo row
    int ampModDividerY_ = 0;                 // the divider's OWN line, in its own gap

    // Per-voice presentation state.
    int ampModel_ = 0;
    juce::String ampWordmark_{"Clean 120"}, ampEyebrow_{"Solid State · Stereo"};
    juce::Colour ampAccent_{skin::accentClean};
    std::vector<NeuKnob*> ampPrimaryKnobs_;  // ordered tone knobs for this voice
    std::vector<NeuKnob*> ampModKnobs_;      // speed/depth (chorus/tremolo), if any
    juce::String modCaption_;                // "Chorus" / "Tremolo" / ""
    bool showBright_ = true;
    bool showMode_ = true;

    juce::Label buildStamp_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipperAudioProcessorEditor)
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_PLUGIN_EDITOR_H
