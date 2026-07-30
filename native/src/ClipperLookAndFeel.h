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

// Patch-cable palette (board.css light theme: --cable / --cable-hi / --cable-plug).
inline const juce::Colour cable       {0xff35383E};
inline const juce::Colour cableHi     {0x80FFFFFF};  // rgba(255,255,255,.5)
inline const juce::Colour cablePlug   {0xffC4C0B8};

// Per-section ACCENTS — light-theme root values (readable on the light bench and
// on the dark chassis). tokens.css :root / [data-theme=light].
inline const juce::Colour accentRat   {0xffF03B24};  // --led / --accent-rat  (red)
inline const juce::Colour accentSd    {0xffB58900};  // --accent-sd           (yellow/gold)
inline const juce::Colour accentTs    {0xff1E9E5A};  // --accent-ts           (green box)
inline const juce::Colour accentMuff  {0xff7A3FBF};  // --accent-muff         (violet)
inline const juce::Colour accentPhaser{0xffC4611A};  // --accent-phaser       (burnt orange)
// --accent-gold. Every other accent here is the LIGHT-theme root value, because
// every other accent also has to survive on the light bench. This one does not: the
// gold pedal's accent only ever paints on the pinned-DARK pedal chassis, and the
// light-theme #8F6A22 sits at ~2.8:1 there — a muddy brown that loses the one thing
// this pedal is famous for. So it takes the web's DARK-theme token, exactly as the
// chassis surfaces already do (pedal.css pins its island to dark values on every
// theme). Champagne gold, ~6.9:1 on the charcoal.
inline const juce::Colour accentGold  {0xffD9B36B};  // --accent-gold (dark-theme token)
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
//
// PAINT ORDER MATTERS: a lit jewel's glow spreads to ~3× its radius, so it must be
// drawn LAST in its component — after every neighbouring body and, above all, after
// their juce::DropShadow passes, which are translucent black over a wide blur and
// will otherwise eat the halo. That inversion is exactly what made the LEDs look
// "covered over" before native parity (see docs → Native pedal-board parity).
void drawJewel(juce::Graphics&, juce::Rectangle<float>, juce::Colour accent, bool on);

// A carved side JACK socket (board.css .jack): a recessed ring with a darker bore.
// Drawn ON TOP of the chassis so a cable end reads as entering the socket.
void drawJack(juce::Graphics&, juce::Point<float> centre, float diameter);

// One neumorphic PATCH CABLE between two jack centres (board.css .cable): a
// gravity-sagging cubic (the sag grows with the span, matching Board.tsx's
// cablePath), a soft cast shadow under the tube, the rubber body, a specular top
// highlight, and a plug disc at each end.
void drawCable(juce::Graphics&, juce::Point<float> from, juce::Point<float> to);

// The GOLD pedal's milled NAMEPLATE (pedal.css .name-plate / .name-plate-text): a
// recessed band cut into the chassis, hairline accent rules along its top and bottom
// lips, and the wordmark ENGRAVED into it — cut in from above, catching light on the
// lower edge, the inverse of the RAT logo's raised emboss. Type only: the original's
// engraved figure is its trademark, so nothing but letters goes on this plate.
void drawNamePlate(juce::Graphics&, juce::Rectangle<float>, juce::Colour accent,
                   const juce::String& text, bool engaged);

// The PEDALBOARD RAIL the cards stand on (the scrollable board's floor). A channel
// milled into the porcelain bench — the same inset recipe as board.css's .board-source
// well — carrying a ribbed rubber MAT. No photo texture: the ribs are drawn, the
// depth is shadow. It spans the whole scrolling content, so it slides with the pedals
// and gives the board somewhere to run off to.
void drawBoardRail(juce::Graphics&, juce::Rectangle<float>);

// Condensed hero wordmark font (Anton-substitute: boldened + horizontally
// compressed system font). `height` in px.
juce::Font wordmarkFont(float height);
// Monospaced eyebrow/label font (the web's ui-monospace small caps labels).
juce::Font monoFont(float height);
// Serif face for the engraved nameplate (the web asks for Optima/Palatino; native
// takes the platform's default serif, which is the same gesture).
juce::Font serifFont(float height);
// Draw `text` LETTER-SPACED and centred in `area` at the current font/colour — the
// engraved plate's .42em tracking, which JUCE fonts have no direct notion of.
void drawTracked(juce::Graphics&, const juce::String& text, juce::Rectangle<float> area,
                 float tracking);
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
    // The board viewport's slim horizontal scrollbar: a carved track with a soft
    // capsule thumb, so the overflow affordance belongs to the bench rather than
    // arriving as a stock grey OS bar across the pedalboard.
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width,
                       int height, bool isScrollbarVertical, int thumbStartPosition,
                       int thumbSize, bool isMouseOver, bool isMouseDown) override;
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

// The FOOTSWITCH — the pedal's morphology cue, translated from the web recipes in
// pedal.css. Four shapes, one behaviour:
//
//   Round     .fsw           — the RAT/phaser stomp: cap-edge disc, inset cap dome,
//                              a ring of grip dots, "STOMP" caption beneath.
//   BigRound  [data-face=wide] .fsw — the Muff's larger stomp (104 vs 88 px).
//   Treadle   .fsw-treadle    — the Boss-compact rubber pad: matte tread face,
//                              pebble texture, grip ribs at the toe, embossed
//                              wordmark, no caption.
//   Pad       .fsw-pad        — the Ibanez-format hinged metal plate: brushed
//                              face + a hinge line across the toe, no caption.
//
// Pressing THUNKS (`.fsw:active` — the body drops 2 px and its outer shadow
// collapses into an inset one) for ~130 ms, exactly like the web's transient
// press; the engaged STATE is shown by the card's LED, never by this widget. It
// therefore no longer draws an LED of its own — the old stacked-LED layout is
// what the stomp's drop shadow was painting over.
class Footswitch : public juce::Component, private juce::Timer {
public:
    enum class Shape { Round, BigRound, Treadle, Pad };

    Footswitch();
    void setAccent(juce::Colour c) { accent_ = c; repaint(); }
    void setShape(Shape s) { shape_ = s; repaint(); }
    void setCaption(const juce::String& c) { caption_ = c; repaint(); }
    // The name embossed on a treadle/pad face (the web's .treadle-wordmark).
    void setWordmark(const juce::String& w) { wordmark_ = w; repaint(); }
    // Dim the embossed wordmark when the pedal is bypassed (.pedal:not(.on)).
    void setEngaged(bool e) { engaged_ = e; repaint(); }
    std::function<void()> onClick;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void paintRound(juce::Graphics&, juce::Rectangle<float>);
    void paintTreadle(juce::Graphics&, juce::Rectangle<float>);
    void paintPad(juce::Graphics&, juce::Rectangle<float>);

    juce::Colour accent_{skin::accentRat};
    Shape shape_{Shape::Round};
    bool pressed_{false};
    bool engaged_{true};
    juce::String caption_{"Stomp"};
    juce::String wordmark_;
};

// A small neumorphic CHIP button — the board.css `.rack-btn` / `.tray-add` family:
// a rounded cap-edge pill with a dual shadow that inverts to an inset one while
// held. Used for the per-card reorder/swap/remove rack and the gear tray.
class ChipButton : public juce::Component {
public:
    explicit ChipButton(const juce::String& text = {});
    void setText(const juce::String& t) { text_ = t; repaint(); }
    void setTint(juce::Colour c) { tint_ = c; repaint(); }
    void setChipEnabled(bool e) { enabled_ = e; setInterceptsMouseClicks(e, false); repaint(); }
    bool chipEnabled() const { return enabled_; }
    // Optional drag reporting (the grip): x is in the PARENT's coordinate space.
    std::function<void()> onClick;
    std::function<void(int parentX)> onDrag;
    std::function<void()> onDragEnd;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    juce::String text_;
    juce::Colour tint_{skin::inkDim};
    bool enabled_{true};
    bool held_{false};
    bool dragged_{false};
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
    // Clamped to the label count: the shared chorusMode param can hold Vibrato (2)
    // from a Clean 120 session while the Twin shows the two-state trem switch —
    // index >= 1 is "on" engine-side (ClipperEngine.cpp), so displaying the last
    // label is the correct reading, not a lie.
    void setSelected(int idx) {
        selected_ = juce::jlimit(0, labels_.size() - 1, idx);
        repaint();
    }
    int selected() const { return selected_; }
    // The switch is shared between amp panels (Clean 120 chorus, Twin tremolo) —
    // each panel sets its own labels and accent when it takes the switch over.
    void setLabels(juce::StringArray labels) {
        labels_ = std::move(labels);
        selected_ = juce::jlimit(0, labels_.size() - 1, selected_);
        repaint();
    }
    void setAccent(juce::Colour accent) { accent_ = accent; repaint(); }
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
