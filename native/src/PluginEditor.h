// Clipper native shell — the neumorphic editor (native visual pass).
//
// A doctrine-compliant (docs §17) rebuild of the flat v1 editor: dark-chassis
// neumorphic "island" cards on a light porcelain bench, translated from the web
// design language (web/src/styles/{tokens,pedal,amp,board}.css). Cards:
//   INPUT  — the input-trim knob (utility).
//   RAT    — "Rodent" (red accent): Dist / Filter / Level + a round on/off stomp.
//   SD-1   — "Super Drive" (yellow accent): Drive / Tone / Level + on/off stomp.
//   AMP    — a single card whose FACE switches with the amp-voice choice, showing
//            exactly the controls that voice has (Eight Hundred gold / Twin silver-
//            blue / Thirty copper / Clean 120 red), mirroring the web Amp faces.
// Plus an amp-voice + oversampling selector pill pair and the build git-hash stamp.
//
// All controls stay 100% APVTS-attached (no behaviour change); the drawing lives in
// ClipperLookAndFeel. Light-bench look only this pass (dark theme is future work).

#ifndef CLIPPER_NATIVE_PLUGIN_EDITOR_H
#define CLIPPER_NATIVE_PLUGIN_EDITOR_H

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>
#include <vector>

#include "ClipperLookAndFeel.h"
#include "PluginProcessor.h"

namespace clipper::native {

class ClipperAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::AsyncUpdater {
public:
    explicit ClipperAudioProcessorEditor(ClipperAudioProcessor&);
    ~ClipperAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Re-sync every widget (amp face + labels/visibility, dim states, toggle
    // positions, combo selections) from the current APVTS state. Called on
    // construction and whenever a bound parameter changes; also used by the
    // headless snapshot tool to force a synchronous refresh after setting params.
    void refreshFromState();

private:
    using ComboAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ParamAttach = juce::ParameterAttachment;

    void handleAsyncUpdate() override {}  // reserved (param marshalling done by attachments)

    void updateAmpFace();     // rebuild the visible amp control set for the current voice
    void updateEnablement();  // dim bypassed sections' knobs
    void layoutAmpCard(juce::Rectangle<int>);

    ClipperAudioProcessor& proc_;
    ClipperLookAndFeel lnf_;

    // Top-bar selectors.
    juce::ComboBox ampVoiceBox_, oversampleBox_;
    std::unique_ptr<ComboAttach> ampVoiceAttach_, oversampleAttach_;

    // INPUT card.
    NeuKnob inputTrim_;
    std::unique_ptr<SliderAttach> inputTrimAttach_;

    // RAT card.
    NeuKnob ratDist_, ratFilter_, ratLevel_;
    std::unique_ptr<SliderAttach> ratDistAttach_, ratFilterAttach_, ratLevelAttach_;
    Footswitch ratSwitch_;
    std::unique_ptr<ParamAttach> ratOnAttach_;

    // SD-1 card.
    NeuKnob sdDrive_, sdTone_, sdLevel_;
    std::unique_ptr<SliderAttach> sdDriveAttach_, sdToneAttach_, sdLevelAttach_;
    Footswitch sdSwitch_;
    std::unique_ptr<ParamAttach> sdOnAttach_;

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
    juce::Rectangle<int> cardInput_, cardRat_, cardSd_, cardAmp_;
    juce::Rectangle<int> ampModRowCaption_;  // caption box for the chorus/tremolo divider

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
