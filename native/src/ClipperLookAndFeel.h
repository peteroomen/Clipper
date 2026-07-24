// Clipper native shell — the neumorphic LookAndFeel + widget kit (native visual
// pass). This is the native translation of the web design language the user loves
// (web/src/styles/{tokens,pedal,amp,board}.css, doctrine docs/DEVELOPMENT.md §17):
// a LIGHT porcelain BENCH on which sit DARK-CHASSIS neumorphic "island" cards.
// Identity per section is carried by a small-area ACCENT colour (RAT red, SD-1
// yellow, Eight Hundred gold, Twin silver-blue, Thirty copper, Clean 120 red), a
// morphology cue and a knowing name — never a full-body hue (§17).
//
// CSS→JUCE recipe translation (values taken verbatim from the CSS):
//   * The bench            = --ground #E5E3DE (light theme root).
//   * A raised card        = the pedal's dark-island interior: --panel-grad
//                            linear-gradient(160deg,#292c32,#222529); the light-
//                            theme .pedal.raised box-shadow (16px18px34 warm cast +
//                            3px4px10 tighter cast + inset 1px light top rim + inset
//                            1px dark edge) → two juce::DropShadow passes + rim/edge
//                            strokes.
//   * A recessed well      = inset dark(top-left) + light(bottom-right) shadow pair.
//   * A sculpted knob      = --cap-edge body (dual outer shadow) + knurl ring +
//                            --cap dome (inset rim) + ink pointer + the floating
//                            270° VALUE ARC (accent fill over --arc-track) with the
//                            numeric readout beneath — the web Knob anatomy.
//   * Lit LED / jewel      = accent fill + a soft outer glow (box-shadow 0 0 14px).
//
// This pass ships the LIGHT-bench look only; a dark theme is future work (see
// docs "Native app (JUCE)" → Visual pass). No font-file assets: the condensed hero
// wordmarks are approximated with a boldened, horizontally-compressed system font
// (JUCE built-in Font handling), which reads as the web's Anton marks without
// bundling/decompressing a woff2.

#ifndef CLIPPER_NATIVE_LOOK_AND_FEEL_H
#define CLIPPER_NATIVE_LOOK_AND_FEEL_H

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace clipper::native {

// ---------------------------------------------------------------------------
// skin — the palette + shared drawing recipes, translated 1:1 from the CSS
// tokens (light-theme root, with the pedal's dark-island interior overrides).
// ---------------------------------------------------------------------------
namespace skin {

// The bench (light porcelain) — tokens.css :root light values.
inline const juce::Colour ground      {0xffE5E3DE};
inline const juce::Colour groundDeep  {0xffDBD8D2};
inline const juce::Colour benchWell   {0xffD6D3CC};
inline const juce::Colour benchInkDim {0xff71736F};
inline const juce::Colour benchFaint  {0xff9B9D98};

// The dark chassis island interior (pedal.css: pinned to dark-theme values on
// every theme so cards read as dark hardware on the light bench).
inline const juce::Colour panelTop    {0xff292C32};  // --panel-grad 160deg start
inline const juce::Colour panelBot    {0xff222529};  // --panel-grad 160deg end
inline const juce::Colour capTop      {0xff2E3238};  // --cap 145deg start
inline const juce::Colour capBot      {0xff212429};  // --cap 145deg end
inline const juce::Colour capEdgeTop  {0xff1E2126};  // --cap-edge 145deg start
inline const juce::Colour capEdgeBot  {0xff2C3036};  // --cap-edge 145deg end
inline const juce::Colour well        {0xff1D2025};  // --well (on the dark chassis)
inline const juce::Colour ink         {0xffE8E9EB};  // --ink
inline const juce::Colour inkDim      {0xff969BA3};  // --ink-dim
inline const juce::Colour inkFaint    {0xff6A6F77};  // --ink-faint
inline const juce::Colour arcTrack    {0x12FFFFFF};  // --arc-track rgba(255,255,255,.07)

// Interior sculpting shadows on the dark chassis (pedal.css light-theme override).
inline const juce::Colour shDark      {0xd9090B0E};  // rgba(9,11,14,.85)
inline const juce::Colour shDarker    {0x99050608};  // rgba(5,6,8,.6)
inline const juce::Colour shLight     {0x0EFFFFFF};  // rgba(255,255,255,.055)

// The warm cast shadow the dark enclosure throws on the light bench
// (.pedal.raised light-theme: 16px 18px 34px rgba(54,50,44,.3)).
inline const juce::Colour castShadow  {0x4D36322C};  // rgba(54,50,44,.30)

// Per-section ACCENTS — light-theme root values (readable on the light bench and
// on the dark chassis). tokens.css :root / [data-theme=light].
inline const juce::Colour accentRat   {0xffF03B24};  // --led / --accent-rat  (red)
inline const juce::Colour accentSd    {0xffB58900};  // --accent-sd           (yellow/gold)
inline const juce::Colour accentJcm   {0xffA87A18};  // --accent-jcm          (brass gold)
inline const juce::Colour accentTwin  {0xff4E7BA8};  // --accent-twin         (silver-blue)
inline const juce::Colour accentAc30  {0xffB4612C};  // --accent-ac30         (copper)
inline const juce::Colour accentClean {0xffF03B24};  // Clean 120 → red accent

// Diagonal fill approximating a CSS linear-gradient(~150deg) across a rect.
void fillDiagGradient(juce::Graphics&, juce::Rectangle<float>, juce::Colour from,
                      juce::Colour to);

// A raised dark-chassis card: warm cast shadow(s) on the bench + body gradient +
// inset light top rim + inset dark edge. `radius` in px.
void drawChassisCard(juce::Graphics&, juce::Rectangle<float>, float radius);

// A recessed well carved into the dark chassis (inset dark TL + light BR).
void drawWell(juce::Graphics&, juce::Rectangle<float>, float radius);

// A small round jewel/LED. `on` lights it (accent + glow); off is a recessed dot.
void drawJewel(juce::Graphics&, juce::Rectangle<float>, juce::Colour accent, bool on);

// Condensed hero wordmark font (Anton-substitute: boldened + horizontally
// compressed system font). `height` in px.
juce::Font wordmarkFont(float height);
// Monospaced eyebrow/label font (the web's ui-monospace small caps labels).
juce::Font monoFont(float height);
}  // namespace skin

// ---------------------------------------------------------------------------
// ClipperLookAndFeel — rotary knobs (sculpted cap + value arc + readout), combo
// boxes (neumorphic pills) and label fonts, in the shared language.
// ---------------------------------------------------------------------------
class ClipperLookAndFeel : public juce::LookAndFeel_V4 {
public:
    ClipperLookAndFeel();

    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider&) override;

    void drawComboBox(juce::Graphics&, int w, int h, bool isDown, int buttonX,
                      int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    void drawPopupMenuBackground(juce::Graphics&, int w, int h) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipperLookAndFeel)
};

// ---------------------------------------------------------------------------
// Widget kit — small custom Components sharing the language. All are driven by an
// external `on`/value + a click callback; they store no parameter state (the
// editor binds them to the APVTS).
// ---------------------------------------------------------------------------

// A labelled sculpted knob: a rotary Slider (drawn by the LnF) with the knob NAME
// and the numeric readout beneath, both tinted by the section accent. The value
// readout mirrors the web (round(value*100)).
class NeuKnob : public juce::Component {
public:
    NeuKnob();
    void setName(const juce::String&);
    void setAccent(juce::Colour);
    void setDimmed(bool);  // bypassed → arc + readout dim (.pedal:not(.on))
    juce::Slider& slider() { return slider_; }
    void resized() override;

private:
    void refreshReadout();
    juce::Slider slider_;
    juce::Label nameLabel_, valueLabel_;
    juce::Colour accent_{skin::accentRat};
    bool dimmed_{false};
};

// The round stomp FOOTSWITCH (the RAT's morphology) with an LED above and a
// caption below. Toggles `on` via onClick.
class Footswitch : public juce::Component {
public:
    Footswitch();
    void setAccent(juce::Colour c) { accent_ = c; repaint(); }
    void setOn(bool o) { on_ = o; repaint(); }
    bool isOn() const { return on_; }
    void setCaption(const juce::String& c) { caption_ = c; repaint(); }
    std::function<void()> onClick;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    juce::Colour accent_{skin::accentRat};
    bool on_{false};
    juce::String caption_{"Stomp"};
};

// A carved-slot lever toggle (the amp Bright/Cab levers): a recessed well with a
// cap lever that slides down + lights (accent) when on. Caption beneath.
class LeverToggle : public juce::Component {
public:
    LeverToggle();
    void setAccent(juce::Colour c) { accent_ = c; repaint(); }
    void setOn(bool o) { on_ = o; repaint(); }
    bool isOn() const { return on_; }
    void setCaption(const juce::String& c) { caption_ = c; repaint(); }
    std::function<void()> onClick;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    juce::Colour accent_{skin::accentRat};
    bool on_{false};
    juce::String caption_{"Bright"};
};

// The amp POWER control: a glowing jewel over a rocker, caption "Power".
class PowerControl : public juce::Component {
public:
    PowerControl();
    void setAccent(juce::Colour c) { accent_ = c; repaint(); }
    void setOn(bool o) { on_ = o; repaint(); }
    bool isOn() const { return on_; }
    std::function<void()> onClick;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    juce::Colour accent_{skin::accentRat};
    bool on_{true};
};

// The 3-way chorus MODE switch: three carved segments stacked in a well, the
// active one lit. Reports the chosen index (0 Off / 1 Chorus / 2 Vibrato).
class ModeSwitch : public juce::Component {
public:
    ModeSwitch();
    void setSelected(int idx) { selected_ = idx; repaint(); }
    int selected() const { return selected_; }
    std::function<void(int)> onSelect;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    juce::StringArray labels_{"Off", "Chorus", "Vibrato"};
    int selected_{0};
    juce::Colour accent_{skin::accentClean};
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_LOOK_AND_FEEL_H
