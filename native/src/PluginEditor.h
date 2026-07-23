// Clipper native shell — custom flat editor (v1).
//
// A tidy, flat, dark editor grouped into four columns: Pedals | Amp | Chorus |
// Output. Plain JUCE sliders/toggles, no image assets — the neumorphic web design
// is roadmapped for a later native pass. The build git hash is shown bottom-right
// (configured via CMake as CLIPPER_GIT_HASH).

#ifndef CLIPPER_NATIVE_PLUGIN_EDITOR_H
#define CLIPPER_NATIVE_PLUGIN_EDITOR_H

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

#include "PluginProcessor.h"

namespace clipper::native {

class ClipperAudioProcessorEditor : public juce::AudioProcessorEditor {
public:
    explicit ClipperAudioProcessorEditor(ClipperAudioProcessor&);
    ~ClipperAudioProcessorEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    // A labelled rotary knob bound to an APVTS float parameter.
    struct Knob {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAttach> attach;
    };
    // A labelled toggle bound to an APVTS bool parameter.
    struct Toggle {
        juce::ToggleButton button;
        std::unique_ptr<ButtonAttach> attach;
    };

    void addKnob(std::unique_ptr<Knob>& slot, const char* paramId, const juce::String& name);
    void addToggle(std::unique_ptr<Toggle>& slot, const char* paramId, const juce::String& name);

    ClipperAudioProcessor& proc_;

    // Column headers.
    juce::Label pedalsHeader_, ampHeader_, chorusHeader_, outputHeader_, title_;

    // Input / pedals column.
    std::unique_ptr<Knob> inputTrim_;
    std::unique_ptr<Toggle> ratOn_;
    std::unique_ptr<Knob> ratDist_, ratFilter_, ratLevel_;
    std::unique_ptr<Toggle> sdOn_;
    std::unique_ptr<Knob> sdDrive_, sdTone_, sdLevel_;

    // Amp column.
    std::unique_ptr<Toggle> ampOn_;
    std::unique_ptr<Knob> volume_, bass_, middle_, treble_;
    std::unique_ptr<Toggle> bright_, cab_;

    // Chorus column.
    juce::ComboBox chorusMode_;
    std::unique_ptr<ComboAttach> chorusModeAttach_;
    juce::Label chorusModeLabel_;
    std::unique_ptr<Knob> chorusSpeed_, chorusDepth_;

    // Output column.
    juce::ComboBox oversampling_;
    std::unique_ptr<ComboAttach> oversamplingAttach_;
    juce::Label oversamplingLabel_;

    juce::Label buildStamp_;  // git hash, bottom-right.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipperAudioProcessorEditor)
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_PLUGIN_EDITOR_H
