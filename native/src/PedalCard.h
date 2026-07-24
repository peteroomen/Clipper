// Clipper native shell — one PEDAL CARD on the board (native parity).
//
// The native translation of web/src/components/Pedal.tsx + the FACES table: a dark
// neumorphic chassis carrying, top to bottom,
//
//   [rack]      the chain-position chips — grip (drag to reorder) · position ·
//               move-earlier · move-later · swap · remove. The web floats these
//               above the enclosure (.unit-rack); native keeps them INSIDE the
//               card's top band, where they cannot collide with the row above.
//   [header]    the model eyebrow line + the status LED at the top-right, exactly
//               where .pedal-top puts them.
//   [wordmark]  the knowing name (Rodent / Super Drive / Screamer / Pi / Ninety).
//   [knobs]     the type's 1-3 sculpted knobs (the Muff's three sit in its classic
//               triangle — its own morphology cue).
//   [stomp]     the footswitch, in that type's morphology (round / treadle / pad).
//
// Doctrine (docs §17): the chassis is the SAME dark charcoal for every pedal;
// identity is carried by a small-area ACCENT, ONE morphology cue, and the name.
//
// The card owns its APVTS attachments, so the knobs and the LED stay bound to the
// same parameter ids the host and the web build use. It knows nothing about the
// chain — it just reports the user's intent through its callbacks.

#ifndef CLIPPER_NATIVE_PEDAL_CARD_H
#define CLIPPER_NATIVE_PEDAL_CARD_H

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

#include "ClipperLookAndFeel.h"
#include "PluginProcessor.h"

namespace clipper::native {

// The static per-type presentation table — the native sibling of the web FACES
// record. One entry per PedalType.
struct PedalFace {
    const char* eyebrow;      // the small model line (.pedal-model)
    const char* wordmark;     // the hero name (.pedal-logo)
    juce::Colour accent;      // the small-area identity colour
    Footswitch::Shape shape;  // the ONE morphology cue
    bool triangleKnobs;       // the Muff's three-knob triangle
    float wordmarkSize;
    struct Knob {
        const char* name;
        const char* paramId;
    };
    std::vector<Knob> knobs;  // 1..3, in display order
    const char* onParamId;    // the engaged-flag parameter
};

// The face for a PedalType (PEDAL_RAT..PEDAL_PHASER).
const PedalFace& pedalFace(int type);
// The human label used in the gear tray / swap menus.
juce::String pedalMenuLabel(int type);

class PedalCard : public juce::Component {
public:
    PedalCard(ClipperAudioProcessor&, int type);
    ~PedalCard() override;

    int type() const { return type_; }
    // Chain position (1-based) shown in the rack, and which move chips are live.
    void setPosition(int index, int total);

    // Where this card's side jacks sit, in PARENT coordinates — the cable layer
    // measures the board through these (the web measures .jack-in / .jack-out).
    juce::Point<float> inJack() const;
    juce::Point<float> outJack() const;

    // User intent, reported to the editor (which owns the chain).
    std::function<void()> onMoveLeft, onMoveRight, onRemove;
    std::function<void(int newType)> onSwap;
    std::function<void(int parentX)> onDragTo;
    std::function<void()> onDragEnd;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void refreshEngaged();  // LED + knob dimming from the engaged parameter

    ClipperAudioProcessor& proc_;
    int type_;
    int index_ = 0, total_ = 1;
    bool engaged_ = true;

    std::vector<std::unique_ptr<NeuKnob>> knobs_;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>>
        knobAttach_;
    Footswitch stomp_;
    std::unique_ptr<juce::ParameterAttachment> onAttach_;

    ChipButton grip_{juce::String::fromUTF8("\xe2\xa0\xbf")};  // ⠿ drag handle
    ChipButton left_{juce::String::fromUTF8("\xe2\x97\x80")};  // ◀
    ChipButton right_{juce::String::fromUTF8("\xe2\x96\xb6")}; // ▶
    ChipButton swap_{juce::String::fromUTF8("\xe2\x87\x84")};  // ⇄
    ChipButton remove_{juce::String::fromUTF8("\xe2\x9c\x95")};// ✕

    juce::Rectangle<int> ledBounds_, headerBounds_, wordmarkBounds_;
    int posWidth_ = 18;  // 0 when a narrow card drops the position number

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalCard)
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_PEDAL_CARD_H
