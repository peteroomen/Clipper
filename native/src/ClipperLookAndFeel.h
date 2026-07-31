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
// Visual pass 3 adds the second token context: the editor now ships BOTH themes,
// exactly as the web does (tokens.css light `:root` + the dark
// `prefers-color-scheme` root). The bench, the amp panel, the rail, the top bar and
// the input card all resolve the current theme; the PEDALS stay pinned dark in both,
// which is the web behaviour (pedal.css pins .pedal to the dark values on every
// theme). See skin::ThemeMode below. No font-file assets: the condensed hero
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

// ---------------------------------------------------------------------------
// THEME (visual pass 3). The web resolves two token roots — the light `:root` and
// the dark `prefers-color-scheme` root — and the native editor now does the same.
// Two facts make that cheap rather than a rewrite:
//   * the DARK root's control tokens are byte-for-byte the values pedal.css pins
//     the .pedal chassis to on EVERY theme, so darkIsland() already IS the dark
//     token context and the pedals genuinely do not move between themes;
//   * only the BENCH-level values (--ground, the cast shadow, the cable, the bench
//     ink triplet) and the ACCENTS actually differ between the two roots.
// `ThemeMode` is the user's choice; Auto follows the OS via
// juce::Desktop::isDarkModeActive() (fed in through setSystemDark). isDark() is the
// RESOLVED answer that every recipe below reads. The choice is persisted in a plain
// juce::PropertiesFile in the app config dir — it is a look, not an audio
// parameter, so it is deliberately NOT in the APVTS and never reaches a host.
enum class ThemeMode { Auto, Light, Dark };
void setThemeMode(ThemeMode);
ThemeMode themeMode();
void setSystemDark(bool osIsDark);
bool isDark();  // the resolved theme — what the recipes paint
juce::String themeModeName(ThemeMode);
ThemeMode loadThemeMode();
void saveThemeMode(ThemeMode);

// The BENCH, theme-resolved (tokens.css light `:root` / dark root).
juce::Colour ground();       // --ground
juce::Colour groundDeep();   // --ground-deep
juce::Colour benchWell();    // --well in the bench context
juce::Colour benchInk();     // --ink
juce::Colour benchInkDim();  // --ink-dim
juce::Colour benchFaint();   // --ink-faint
// The cast shadow a dark island throws on the bench. Light theme: the warm
// .pedal.raised 16px 18px 34px rgba(54,50,44,.3). Dark theme: the base .raised
// --sh-dark, because a warm brown shadow is invisible on a charcoal bench.
juce::Colour castShadow();
// Patch-cable palette (board.css --cable / --cable-hi / --cable-plug), per theme.
juce::Colour cable();
juce::Colour cableHi();
juce::Colour cablePlug();

// The dark chassis island interior (pedal.css: pinned to dark-theme values on
// every theme so cards read as dark hardware on the light bench). These are also,
// byte-for-byte, the dark-theme root values — see benchScheme().
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

// ---------------------------------------------------------------------------
// Token SCHEMES (visual pass 2). The web resolves every control recipe in a CSS
// custom-property context, and there are exactly two: the LIGHT bench (:root
// light values) and the DARK chassis island (pedal.css pins .pedal to the dark
// values on every theme). The AMP's controls resolve LIGHT on the web — the
// pinning is .pedal-scoped — which is why the web amp reads as porcelain. The
// native amp paints in lightBench() since this pass; pedals stay darkIsland().
// ---------------------------------------------------------------------------
struct Scheme {
    juce::Colour capTop, capBot;          // --cap 145deg
    juce::Colour capEdgeTop, capEdgeBot;  // --cap-edge 145deg
    juce::Colour well;                    // --well
    juce::Colour panelTop, panelBot;      // --panel-grad 160deg
    juce::Colour ink, inkDim, inkFaint;   // --ink / --ink-dim / --ink-faint
    juce::Colour shDark, shDarker, shLight;  // sculpting shadows
    juce::Colour arcTrack;                // --arc-track
};
const Scheme& darkIsland();   // the pedal chassis (today's constants, unchanged)
const Scheme& lightBench();   // tokens.css :root light values, verbatim
// The token context the BENCH-side surfaces resolve in — the amp panel, its
// knobs/levers/rocker/mode switch, the rail, the scrollbar. lightBench() in light
// theme, darkIsland() in dark (the dark root and the .pedal pinning are the same
// token set, which is exactly why the pedals are theme-invariant).
const Scheme& benchScheme();

// Per-section ACCENTS. tokens.css defines a light and a dark value for each
// (`--accent-*`), and unlike the chassis surfaces the accents are NOT pinned by
// pedal.css — they resolve at the root, so a RAT LED really is a different red in
// the web's dark theme. accent() resolves the current theme.
//
// One deliberate exception, unchanged from visual pass 2: --accent-gold takes the
// DARK token in BOTH themes. The gold pedal's accent only ever paints on the
// pinned-dark chassis, where the light-theme #8F6A22 sits at ~2.8:1 — a muddy brown
// that loses the one thing this pedal is famous for. Champagne gold, ~6.9:1.
enum class AccentId { Rat, Sd, Ts, Muff, Phaser, Gold, Jcm, Twin, Ac30, Orange, Clean };
juce::Colour accent(AccentId);

// Diagonal fill approximating a CSS linear-gradient(~150deg) across a rect.
void fillDiagGradient(juce::Graphics&, juce::Rectangle<float>, juce::Colour from,
                      juce::Colour to);

// A raised dark-chassis card: cast shadow(s) on the bench + body gradient +
// inset light top rim + inset dark edge. `radius` in px.
//
// SPLIT IN TWO on purpose (visual pass 3). JUCE clips a component's paint to its
// own bounds, so a card that is a CHILD component cannot paint its own cast shadow
// — it lands entirely under itself and the card reads flat. That is why the pedal
// cards had no shadow while the editor-painted input/amp cards did. A child card
// therefore paints drawChassisBody() and its PARENT paints
// drawIslandCastShadow() at the card's bounds before the children draw.
// drawChassisCard() is the two together, for cards the editor paints itself.
void drawIslandCastShadow(juce::Graphics&, juce::Rectangle<float>, float radius);
void drawChassisBody(juce::Graphics&, juce::Rectangle<float>, float radius);
void drawChassisCard(juce::Graphics&, juce::Rectangle<float>, float radius);

// A raised BENCH card — the web `.raised` recipe verbatim (base.css): --panel-grad
// body + 10px 10px 24px sh-dark + -10px -10px 22px sh-light + inset top light rim,
// resolved in benchScheme(). The amp panel (web parity: the amp is a bench panel,
// not a dark island — amp.css resolves in the page token context, light or dark).
void drawBenchCard(juce::Graphics&, juce::Rectangle<float>, float radius);

// A recessed well carved into a surface (inset dark TL + light BR). Defaults to
// the dark chassis; pass a scheme for a light-bench well (the amp's slots).
void drawWell(juce::Graphics&, juce::Rectangle<float>, float radius,
              const Scheme& s = darkIsland());

// A small round jewel/LED. `on` lights it (accent + glow); off is a recessed dot.
//
// PAINT ORDER MATTERS, twice over. (1) A lit jewel's glow spreads to ~3× its
// radius, so it must be drawn LAST in its component — after every neighbouring
// body and, above all, after their juce::DropShadow passes, which will otherwise
// eat the halo (docs → Native pedal-board parity). (2) JUCE clips a component's
// paint to its bounds, so the CALLER must leave the glow's spread INSIDE its own
// component — a jewel parked on a bounds edge renders a square-clipped halo,
// which is exactly the 2026-07-31 "draw order issues on amp on/off switches"
// report. glowSpread() says how much head-room a lit jewel of diameter d needs
// on every side.
//
// The glow itself is the web recipe (0 0 16px + 0 0 5px accent-glow) as two
// smooth radial gradients — the old four stacked alpha discs compounded into
// visibly banded steps. `off` takes the scheme so the recessed dot mixes into
// the right well colour.
float glowSpread(float jewelDiameter);
void drawJewel(juce::Graphics&, juce::Rectangle<float>, juce::Colour accent, bool on,
               const Scheme& s = darkIsland());

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
    // Which token context the dial paints in (web parity: amp knobs resolve the
    // LIGHT tokens, pedal knobs the pinned-dark ones). Default dark.
    void setScheme(const skin::Scheme&);
    juce::Slider& slider() { return slider_; }
    void resized() override;

private:
    void refreshReadout();
    juce::Slider slider_;
    juce::Label nameLabel_, valueLabel_;
    juce::Colour accent_{skin::accent(skin::AccentId::Rat)};
    bool dimmed_{false};
    const skin::Scheme* scheme_{nullptr};  // set in ctor (darkIsland)
    double lastTickValue_{0.0};            // ui-sound detent tracking
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
    void mouseUp(const juce::MouseEvent&) override;  // the release thunk (web parity)

private:
    void timerCallback() override;
    void paintRound(juce::Graphics&, juce::Rectangle<float>);
    void paintTreadle(juce::Graphics&, juce::Rectangle<float>);
    void paintPad(juce::Graphics&, juce::Rectangle<float>);

    juce::Colour accent_{skin::accent(skin::AccentId::Rat)};
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
// cap lever that slides down + lights (accent) when on. Caption beneath. Paints
// in benchScheme() (amp-only widget — amp.css .toggle resolves in the page token
// context). The lever SLIDES with the web's 160 ms overshoot spring
// (transition: top .16s cubic-bezier(.34,1.56,.64,1)) rather than snapping — but
// the position is CLAMPED to the slot (visual pass 3): easeOutBack overshoots by
// ~9.9 %, which carried the lever 2.2 px past the slot's 4 px inset at both ends
// ("the bright switch goes too far down"). Clamping keeps the spring's timing and
// gives it a physical stop, which is what a real lever has.
class LeverToggle : public juce::Component, private juce::Timer {
public:
    LeverToggle();
    void setAccent(juce::Colour c) { accent_ = c; repaint(); }
    void setOn(bool o);
    bool isOn() const { return on_; }
    void setCaption(const juce::String& c) { caption_ = c; repaint(); }
    std::function<void()> onClick;

    // Slot width + the head-room the lever's 2px/3px/6px cast shadow needs on each
    // side, so nothing is clipped at the component bounds.
    static int preferredWidth();

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    juce::Colour accent_{skin::accent(skin::AccentId::Rat)};
    bool on_{false};
    bool everSet_{false};    // the first setOn (session sync) snaps, no animation
    float leverPos_{0.0f};   // 0 = top (off) … 1 = bottom (on), animated
    float animFrom_{0.0f};
    double animStartMs_{0.0};
    bool animating_{false};
    juce::String caption_{"Bright"};
};

// The amp POWER control: a glowing jewel over a rocker, caption "Power". Paints in
// benchScheme() (amp-only — amp.css .power/.rocker/.jewel resolve in the page token
// context). The jewel band reserves skin::glowSpread() of head-room so the lit halo
// is never clipped by the component bounds, and preferredWidth() reserves the
// rocker's own 5px/5px/12px cast shadow — pass 2 fixed the vertical clipping and
// left the horizontal, which is why the 2026-07-31 screenshot still showed a halo
// with a hard right edge and a rocker shadow sliced off (visual pass 3).
class PowerControl : public juce::Component {
public:
    PowerControl();
    void setAccent(juce::Colour c) { accent_ = c; repaint(); }
    void setOn(bool o) { on_ = o; repaint(); }
    bool isOn() const { return on_; }
    std::function<void()> onClick;

    // The height the full web anatomy needs: jewel band + gap + 64 px rocker +
    // caption. layoutAmpCard sizes the cluster from this instead of guessing.
    static int preferredHeight();
    // The width it needs: the 46 px rocker plus its cast-shadow head-room (which
    // also covers the jewel's glow radius).
    static int preferredWidth();

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    juce::Colour accent_{skin::accent(skin::AccentId::Rat)};
    bool on_{true};
};

// The chorus/tremolo MODE switch — the web `.mode-switch` geometry exactly
// (rebuilt in visual pass 3, the owner's "the chorus/trem is broken as well"):
// every `.mode-opt` has the SAME footprint, they stack flush with the CSS's −1 px
// overlap, and ONLY the stack's outer corners round (12 px). The active segment is
// raised and lit at that same footprint — it must never read wider or squarer than
// its neighbours, which is what the old per-segment full-rounded-rect inset strokes
// and the middle segment's corner-less path together produced. Works for both the
// 3-state (Clean 120 chorus) and 2-state (Twin tremolo) forms.
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

    // The width the web's 78 px segment plus the active segment's cast-shadow
    // head-room needs (the shadow is clipped at the component bounds otherwise).
    static int preferredWidth();

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    juce::StringArray labels_{"Off", "Chorus", "Vibrato"};
    int selected_{0};
    juce::Colour accent_{skin::accent(skin::AccentId::Clean)};
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_LOOK_AND_FEEL_H
