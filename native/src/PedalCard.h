// Clipper native shell — one PEDAL CARD on the board (native parity).
//
// The native translation of web/src/components/Pedal.tsx + the FACES table: a dark
// neumorphic chassis carrying, top to bottom,
//
// (the GOLD 'plate' face reshuffles this: its wordmark is ENGRAVED into a milled
// nameplate band between the knobs and the stomp, so it has no hero line of its own.)
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
    // The body layout — the native sibling of the web FACES `layout` field.
    //   Stack    the reference vertical stack: eyebrow, hero wordmark, knob row, stomp.
    //   Triangle the Muff's three knobs in their classic triangle.
    //   Single   the phaser's ONE big centred knob.
    //   Plate    the GOLD box: knob row over an ENGRAVED NAMEPLATE band carrying the
    //            wordmark (so this face has no hero wordmark of its own), round stomp.
    //   Bank     M13.6's ten-band EQ: the ten band knobs in a 2x5 grid with
    //            GAIN/VOLUME on a row beneath. The only face with more than three
    //            controls.
    enum class Layout { Stack, Triangle, Single, Plate, Bank };

    const char* eyebrow;      // the small model line (.pedal-model)
    const char* wordmark;     // the hero name (.pedal-logo) / the nameplate engraving
    // The small-area identity colour, as a TOKEN rather than a value: tokens.css
    // gives every --accent-* a light and a dark value, and the accents (unlike the
    // chassis surfaces pedal.css pins) resolve at the root, so they follow the
    // theme. skin::accent() answers for the theme in force when we paint.
    skin::AccentId accent;
    Footswitch::Shape shape;  // the ONE morphology cue
    Layout layout;
    float wordmarkSize;
    struct Knob {
        const char* name;
        const char* paramId;
        // A DISCRETE slot's detent labels, in order (nullptr => a continuous pot).
        // The models quantize these slots — the drop-tune's nine positions, the
        // three two-state MODE switches — but the card drew all of them as 0-100
        // dials, so the readout named nothing. NeuKnob::setPositions turns the
        // dial into a detented selector reading the position's name.
        std::vector<const char*> positions{};
    };
    // In display order. 1..3 for every face except Bank, which carries TWELVE
    // (the ten bands, then GAIN and VOLUME) — see Layout::Bank.
    std::vector<Knob> knobs;
    const char* onParamId;    // the engaged-flag parameter
    // The PedalType this face belongs to. It is a FIELD rather than the entry's
    // POSITION for two measured reasons found by M13.6a (docs §61.10): the table
    // was a `[PEDAL_TYPE_COUNT]` array indexed by type, and (1) the wah's and the
    // compressor's entries were in the wrong ORDER, so the native editor drew the
    // Squash face on a Weeper and vice versa; (2) the slot-reservation commit
    // widened PEDAL_TYPE_COUNT to 11 while the table still had 8 entries, so
    // types 8/9/10 resolved to a value-initialized face with NULL strings. Both
    // are impossible once the type is written down next to the face.
    int type;
};

// The face for a PedalType. Falls back to the RAT face for a type whose slice has
// not landed yet — see `pedalHasFace`, which is what the menus filter on.
const PedalFace& pedalFace(int type);
// FALSE for a reserved-but-unfilled PedalType. The gear tray and the swap menu
// must not offer one: it would be an item that adds a pedal with no face.
bool pedalHasFace(int type);
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

    // Re-resolve every theme-dependent colour this card holds (the knob accents,
    // the remove chip's tint) and repaint. The editor calls this on a theme flip;
    // the chassis itself is pinned dark in both themes, exactly like the web.
    void applyTheme();

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

    juce::Rectangle<int> ledBounds_, headerBounds_, wordmarkBounds_, plateBounds_;
    int posWidth_ = 18;  // 0 when a narrow card drops the position number

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalCard)
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_PEDAL_CARD_H
