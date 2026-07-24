#include "ClipperLookAndFeel.h"

namespace clipper::native {

namespace {
// The web knob arc: a conic sweep `from 225deg` spanning 270°. In JUCE angle terms
// (0 = 12 o'clock, clockwise, radians) that is start 1.25π → end 2.75π.
constexpr float kArcStart = juce::MathConstants<float>::pi * 1.25f;
constexpr float kArcEnd   = juce::MathConstants<float>::pi * 2.75f;

juce::Path roundedRectPath(juce::Rectangle<float> r, float radius) {
    juce::Path p;
    p.addRoundedRectangle(r, radius);
    return p;
}
}  // namespace

// ===========================================================================
// skin drawing recipes
// ===========================================================================
namespace skin {

void fillDiagGradient(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour from,
                      juce::Colour to) {
    // ~150deg: from top-left toward bottom-right.
    juce::ColourGradient grad(from, r.getTopLeft(), to, r.getBottomRight(), false);
    g.setGradientFill(grad);
    g.fillRect(r);
}

void drawChassisCard(juce::Graphics& g, juce::Rectangle<float> r, float radius) {
    auto path = roundedRectPath(r, radius);

    // Two warm cast shadows on the bench (the .pedal.raised dual box-shadow):
    // 16px 18px 34px @ .30, then a tighter 3px 4px 10px @ .22.
    juce::DropShadow(castShadow, 22, {11, 13}).drawForPath(g, path);
    juce::DropShadow(castShadow.withAlpha(0.22f), 8, {3, 4}).drawForPath(g, path);

    // Body — the dark island (--panel-grad 160deg).
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(path);
        fillDiagGradient(g, r, panelTop, panelBot);
    }

    // inset 0 0 0 1px rgba(0,0,0,.38) — crisp dark inner edge.
    g.setColour(juce::Colour(0x61000000));
    g.strokePath(roundedRectPath(r.reduced(0.5f), radius), juce::PathStrokeType(1.0f));
    // inset 0 1px 0 rgba(255,255,255,.07) — faint light rim caught along the TOP edge
    // only (a short highlight between the rounded corners, not a full outline).
    g.setColour(juce::Colour(0x12FFFFFF));
    g.drawLine(r.getX() + radius, r.getY() + 1.5f, r.getRight() - radius, r.getY() + 1.5f,
               1.2f);
}

void drawWell(juce::Graphics& g, juce::Rectangle<float> r, float radius) {
    auto path = roundedRectPath(r, radius);
    g.setColour(well);
    g.fillPath(path);
    // inset dark top-left, inset light bottom-right (the carved recess).
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(path);
        g.setColour(shDarker);
        g.strokePath(roundedRectPath(r.translated(1.6f, 1.8f), radius),
                     juce::PathStrokeType(3.0f));
        g.setColour(shLight);
        g.strokePath(roundedRectPath(r.translated(-1.4f, -1.4f), radius),
                     juce::PathStrokeType(2.0f));
    }
}

void drawJewel(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent, bool on) {
    auto c = r.getCentre();
    float rad = juce::jmin(r.getWidth(), r.getHeight()) * 0.5f;
    if (on) {
        // Outer glow (box-shadow 0 0 14px accent-glow).
        for (int i = 4; i >= 1; --i) {
            g.setColour(accent.withAlpha(0.16f));
            float gr = rad + (float)i * rad * 0.55f;
            g.fillEllipse(c.x - gr, c.y - gr, gr * 2.0f, gr * 2.0f);
        }
        // The lit jewel — a bright specular highlight to a deep core.
        juce::ColourGradient grad(accent.brighter(0.9f), c.x - rad * 0.35f,
                                  c.y - rad * 0.4f, accent.darker(0.7f), c.x + rad,
                                  c.y + rad, true);
        g.setGradientFill(grad);
        g.fillEllipse(c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
    } else {
        // Recessed dark dot (color-mix accent 18% into the well).
        g.setColour(well.interpolatedWith(accent, 0.18f));
        g.fillEllipse(c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
        g.setColour(shDarker);
        g.drawEllipse(c.x - rad + 0.5f, c.y - rad + 0.5f, rad * 2.0f - 1.0f,
                      rad * 2.0f - 1.0f, 1.0f);
    }
}

void drawJack(juce::Graphics& g, juce::Point<float> centre, float diameter) {
    const float r = diameter * 0.5f;
    juce::Rectangle<float> body(centre.x - r, centre.y - r, diameter, diameter);
    // The socket ring: a recessed well (inset dark TL + light BR, board.css .jack).
    g.setColour(well);
    g.fillEllipse(body);
    g.setColour(shDarker);
    g.drawEllipse(body.reduced(0.5f).translated(0.7f, 0.8f), 1.6f);
    g.setColour(shLight);
    g.drawEllipse(body.reduced(0.5f).translated(-0.6f, -0.6f), 1.2f);
    // The bore (.jack::after) — a darker inner disc.
    auto bore = body.reduced(diameter * 0.25f);
    g.setColour(groundDeep.darker(0.55f));
    g.fillEllipse(bore);
    g.setColour(shDarker);
    g.drawEllipse(bore.reduced(0.4f), 0.9f);
}

void drawCable(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to) {
    // Sag law lifted from Board.tsx cablePath(): grows with the span, clamped, and
    // hung from the LOWER of the two ends so the tube always droops.
    const float dx = to.x - from.x;
    const float sag = juce::jlimit(16.0f, 70.0f, std::abs(dx) * 0.16f + 14.0f);
    const float lowY = juce::jmax(from.y, to.y) + sag;
    juce::Path p;
    p.startNewSubPath(from);
    p.cubicTo({from.x + dx * 0.28f, lowY}, {from.x + dx * 0.72f, lowY}, to);

    // .cable filter: drop-shadow(1px 3px 2.5px var(--sh-dark)) — the tube's cast
    // shadow on the surface below it.
    {
        juce::Path shadow = p;
        shadow.applyTransform(juce::AffineTransform::translation(1.0f, 3.0f));
        g.setColour(castShadow.withAlpha(0.30f));
        g.strokePath(shadow, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }
    // .cable-body — the rubber tube.
    g.setColour(cable);
    g.strokePath(p, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved,
                                         juce::PathStrokeType::rounded));
    // .cable-hi — the specular edge hugging the top of the tube (translateY -1.4).
    {
        juce::Path hi = p;
        hi.applyTransform(juce::AffineTransform::translation(0.0f, -1.4f));
        g.setColour(cableHi.withAlpha(0.22f));
        g.strokePath(hi, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    }
    // .cable-plug — the plug disc at each end.
    g.setColour(cablePlug);
    g.fillEllipse(from.x - 5.0f, from.y - 5.0f, 10.0f, 10.0f);
    g.fillEllipse(to.x - 5.0f, to.y - 5.0f, 10.0f, 10.0f);
}

juce::Font wordmarkFont(float height) {
    // Anton substitute: a heavy, horizontally-compressed system sans.
    return juce::Font(juce::FontOptions(height, juce::Font::bold)).withHorizontalScale(0.82f);
}

juce::Font monoFont(float height) {
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), height,
                                        juce::Font::plain));
}
}  // namespace skin

// ===========================================================================
// ClipperLookAndFeel
// ===========================================================================
ClipperLookAndFeel::ClipperLookAndFeel() {
    setColour(juce::PopupMenu::backgroundColourId, skin::panelBot);
    setColour(juce::PopupMenu::textColourId, skin::ink);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, skin::well);
    setColour(juce::PopupMenu::highlightedTextColourId, skin::ink);
}

void ClipperLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
                                          float sliderPos, float /*startAngle*/,
                                          float /*endAngle*/, juce::Slider& s) {
    auto bounds = juce::Rectangle<int>(x, y, w, h).toFloat();
    const float side = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const float bodyR = side * 0.5f * 0.80f;  // knob body
    const float arcR = side * 0.5f * 0.95f;   // arc floats outside the body
    const float angle = kArcStart + sliderPos * (kArcEnd - kArcStart);

    auto accent = s.findColour(juce::Slider::rotarySliderFillColourId);
    const bool dim = !s.isEnabled();
    if (dim) accent = accent.withAlpha(0.30f);

    // --- body cast shadow (6px 6px 14 dark / -5 -5 12 light dual) ---
    {
        juce::Path bp;
        bp.addEllipse(centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
        juce::DropShadow(skin::shDark, (int)(bodyR * 0.55f), {3, 4}).drawForPath(g, bp);
    }
    // --- body (--cap-edge 145deg: #1E2126 → #2C3036) ---
    {
        juce::Rectangle<float> br(centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f,
                                  bodyR * 2.0f);
        g.setGradientFill(juce::ColourGradient(skin::capEdgeTop, br.getTopLeft(),
                                               skin::capEdgeBot, br.getBottomRight(),
                                               false));
        g.fillEllipse(br);
    }
    // --- knurled skirt (repeating conic dark ticks, opacity ~.35) ---
    {
        g.setColour(skin::shDark.withAlpha(0.35f));
        const int teeth = 40;
        const float rOut = bodyR * 0.99f;
        const float rIn = bodyR * 0.86f;
        for (int i = 0; i < teeth; ++i) {
            float a = (float)i / (float)teeth * juce::MathConstants<float>::twoPi;
            float ca = std::cos(a), sa = std::sin(a);
            g.drawLine(centre.x + ca * rIn, centre.y + sa * rIn, centre.x + ca * rOut,
                       centre.y + sa * rOut, 1.4f);
        }
    }
    // --- cap dome (--cap 145deg: #2E3238 → #212429) + rim highlights ---
    const float capR = bodyR * 0.80f;
    {
        juce::Rectangle<float> cr(centre.x - capR, centre.y - capR, capR * 2.0f,
                                  capR * 2.0f);
        g.setGradientFill(juce::ColourGradient(skin::capTop, cr.getTopLeft(), skin::capBot,
                                               cr.getBottomRight(), false));
        g.fillEllipse(cr);
        g.setColour(juce::Colour(0x18FFFFFF));  // inset light top-left rim
        g.drawEllipse(cr.reduced(0.6f).translated(-0.4f, -0.4f), 1.1f);
        g.setColour(skin::shDarker);  // inset dark bottom-right
        g.drawEllipse(cr.reduced(0.6f).translated(0.6f, 0.8f), 1.0f);
    }
    // --- ink pointer ---
    {
        juce::Path ptr;
        float pw = juce::jmax(2.4f, bodyR * 0.10f);
        ptr.addRoundedRectangle(-pw * 0.5f, -capR * 0.92f, pw, capR * 0.52f, pw * 0.5f);
        g.setColour(skin::ink.withAlpha(dim ? 0.4f : 0.85f));
        g.fillPath(ptr, juce::AffineTransform::rotation(angle).translated(centre));
    }
    // --- the floating value ARC (track + accent fill) ---
    {
        const float thick = juce::jmax(2.6f, side * 0.055f);
        juce::Path track;
        track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, kArcStart, kArcEnd, true);
        g.setColour(skin::arcTrack);
        g.strokePath(track, juce::PathStrokeType(thick, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
        if (sliderPos > 0.0008f) {
            juce::Path val;
            val.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, kArcStart, angle, true);
            g.setColour(accent);
            g.strokePath(val, juce::PathStrokeType(thick, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }
    }
}

void ClipperLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool /*isDown*/,
                                      int /*bx*/, int /*by*/, int /*bw*/, int /*bh*/,
                                      juce::ComboBox& box) {
    auto r = juce::Rectangle<float>(0, 0, (float)w, (float)h).reduced(1.0f);
    const float radius = juce::jmin(10.0f, h * 0.4f);
    // A raised neumorphic pill (cap-edge) with a dual outer shadow.
    juce::Path p = roundedRectPath(r, radius);
    juce::DropShadow(skin::shDark, 8, {3, 3}).drawForPath(g, p);
    skin::fillDiagGradient(g, r, skin::capEdgeBot, skin::capEdgeTop);
    g.setColour(juce::Colour(0x14FFFFFF));
    g.strokePath(roundedRectPath(r.reduced(0.5f), radius), juce::PathStrokeType(1.0f));
    // caret
    auto ca = juce::Rectangle<float>(r.getRight() - 20.0f, r.getCentreY() - 3.0f, 10.0f,
                                     6.0f);
    juce::Path tri;
    tri.addTriangle(ca.getX(), ca.getY(), ca.getRight(), ca.getY(), ca.getCentreX(),
                    ca.getBottom());
    g.setColour(skin::inkDim);
    g.fillPath(tri);
    juce::ignoreUnused(box);
}

juce::Font ClipperLookAndFeel::getComboBoxFont(juce::ComboBox&) {
    return skin::monoFont(13.0f);
}

void ClipperLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label) {
    label.setBounds(10, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont(skin::monoFont(13.0f));
    label.setColour(juce::Label::textColourId, skin::ink);
    label.setJustificationType(juce::Justification::centredLeft);
}

void ClipperLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h) {
    auto r = juce::Rectangle<float>(0, 0, (float)w, (float)h);
    g.setColour(skin::panelBot);
    g.fillRoundedRectangle(r.reduced(1.0f), 8.0f);
    g.setColour(juce::Colour(0x22FFFFFF));
    g.drawRoundedRectangle(r.reduced(1.0f), 8.0f, 1.0f);
}

// ===========================================================================
// NeuKnob
// ===========================================================================
NeuKnob::NeuKnob() {
    slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider_.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                juce::MathConstants<float>::pi * 2.75f, true);
    slider_.setColour(juce::Slider::rotarySliderFillColourId, accent_);
    addAndMakeVisible(slider_);

    nameLabel_.setJustificationType(juce::Justification::centred);
    nameLabel_.setColour(juce::Label::textColourId, skin::inkDim);
    nameLabel_.setFont(skin::monoFont(10.5f));
    nameLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel_);

    valueLabel_.setJustificationType(juce::Justification::centred);
    valueLabel_.setColour(juce::Label::textColourId, accent_);
    valueLabel_.setFont(skin::monoFont(11.5f).boldened());
    valueLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(valueLabel_);

    slider_.onValueChange = [this] { refreshReadout(); };
    refreshReadout();
}

void NeuKnob::setName(const juce::String& n) {
    nameLabel_.setText(n.toUpperCase(), juce::dontSendNotification);
}

void NeuKnob::setAccent(juce::Colour c) {
    accent_ = c;
    slider_.setColour(juce::Slider::rotarySliderFillColourId, c);
    valueLabel_.setColour(juce::Label::textColourId, dimmed_ ? c.withAlpha(0.45f) : c);
    repaint();
}

void NeuKnob::setDimmed(bool d) {
    dimmed_ = d;
    slider_.setEnabled(!d);  // the LnF dims the arc + pointer for a disabled slider
    valueLabel_.setColour(juce::Label::textColourId,
                          d ? accent_.withAlpha(0.45f) : accent_);
    nameLabel_.setColour(juce::Label::textColourId,
                         d ? skin::inkDim.withAlpha(0.5f) : skin::inkDim);
    repaint();
}

void NeuKnob::refreshReadout() {
    valueLabel_.setText(juce::String(juce::roundToInt(slider_.getValue() * 100.0)),
                        juce::dontSendNotification);
}

void NeuKnob::resized() {
    auto r = getLocalBounds();
    const int valueH = 15;
    const int nameH = 14;
    valueLabel_.setBounds(r.removeFromBottom(valueH));
    nameLabel_.setBounds(r.removeFromBottom(nameH));
    // The dial fills the remaining square-ish area.
    slider_.setBounds(r);
}

// ===========================================================================
// Footswitch — the four web morphologies (pedal.css .fsw / -treadle / -pad)
// ===========================================================================
Footswitch::Footswitch() {}

void Footswitch::mouseDown(const juce::MouseEvent&) {
    // The thunk: press for ~130 ms (the web's transient `.stomped`), then release.
    pressed_ = true;
    repaint();
    startTimer(130);
    if (onClick) onClick();
}

void Footswitch::timerCallback() {
    stopTimer();
    pressed_ = false;
    repaint();
}

void Footswitch::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    // `.fsw:active { transform: translateY(2px) }` — the whole switch drops.
    if (pressed_) r = r.translated(0.0f, 2.0f);
    switch (shape_) {
        case Shape::Treadle: paintTreadle(g, r); break;
        case Shape::Pad:     paintPad(g, r); break;
        default:             paintRound(g, r); break;
    }
}

void Footswitch::paintRound(juce::Graphics& g, juce::Rectangle<float> r) {
    const float capH = caption_.isEmpty() ? 0.0f : 16.0f;
    // The web disc is 88 px (104 on the Muff's wide face); scale down only if the
    // slot is genuinely smaller than that.
    const float want = shape_ == Shape::BigRound ? 104.0f : 88.0f;
    float d = juce::jmin(want, juce::jmin(r.getWidth(), r.getHeight() - capH - 4.0f));
    d = juce::jmax(44.0f, d);
    // Centre the disc + its caption in the zone: a stomp pinned to the top of a
    // tall slot leaves the enclosure looking half-empty below it.
    const float top = r.getY() + juce::jmax(0.0f, (r.getHeight() - d - capH - 4.0f) * 0.5f);
    juce::Rectangle<float> stomp(r.getCentreX() - d * 0.5f, top, d, d);

    juce::Path sp;
    sp.addEllipse(stomp);
    // 8px 8px 18px --sh-dark (raised) collapsing to 3/3/8 while pressed.
    juce::DropShadow(skin::shDark, pressed_ ? 8 : 16, pressed_ ? juce::Point<int>{3, 3}
                                                               : juce::Point<int>{5, 6})
        .drawForPath(g, sp);
    g.setGradientFill(juce::ColourGradient(skin::capEdgeTop, stomp.getTopLeft(),
                                           skin::capEdgeBot, stomp.getBottomRight(), false));
    g.fillEllipse(stomp);
    // .fsw::before — the inset cap dome (inset 10px on an 88px disc).
    auto inner = stomp.reduced(d * 0.114f);
    g.setGradientFill(juce::ColourGradient(skin::capTop, inner.getTopLeft(), skin::capBot,
                                           inner.getBottomRight(), false));
    g.fillEllipse(inner);
    g.setColour(juce::Colour(0x14FFFFFF));
    g.drawEllipse(inner.reduced(0.5f), 1.0f);
    if (pressed_) {  // inset 2px 2px 6px --sh-darker
        g.setColour(skin::shDarker);
        g.drawEllipse(stomp.reduced(1.4f), 2.6f);
    }
    // .fsw::after — the fine grip ring (repeating conic, opacity .3).
    g.setColour(skin::shDarker.withAlpha(0.3f));
    auto grip = stomp.reduced(d * 0.34f);
    for (int i = 0; i < 16; ++i) {
        const float a = (float)i / 16.0f * juce::MathConstants<float>::twoPi;
        const float gx = grip.getCentreX() + std::cos(a) * grip.getWidth() * 0.5f;
        const float gy = grip.getCentreY() + std::sin(a) * grip.getHeight() * 0.5f;
        g.fillEllipse(gx - 1.0f, gy - 1.0f, 2.0f, 2.0f);
    }
    if (capH > 0.0f) {  // .fsw-label
        g.setColour(skin::inkFaint);
        g.setFont(skin::monoFont(9.5f));
        g.drawText(caption_.toUpperCase(),
                   juce::Rectangle<float>(r.getX(), stomp.getBottom() + 4.0f, r.getWidth(),
                                          capH),
                   juce::Justification::centred);
    }
}

void Footswitch::paintTreadle(juce::Graphics& g, juce::Rectangle<float> r) {
    // .fsw-treadle: a wide BLACK RUBBER pad owning the LOWER body — the web drops
    // it to the bottom of the enclosure with nothing beneath it, and that placement
    // is half the Boss-compact read. Explicitly darker/mattes than the metal knobs.
    const float h = juce::jmin(r.getHeight(), 150.0f);
    auto body = r.withTop(r.getBottom() - h);
    juce::Path bp;
    bp.addRoundedRectangle(body, 18.0f);
    juce::DropShadow(skin::shDark, pressed_ ? 10 : 20, pressed_ ? juce::Point<int>{3, 4}
                                                                : juce::Point<int>{7, 9})
        .drawForPath(g, bp);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff26282D), body.getTopLeft(),
                                           juce::Colour(0xff17181C), body.getBottomRight(),
                                           false));
    g.fillRoundedRectangle(body, 18.0f);
    // ::before — the matte rubber tread face (inset 6px) + pebble texture.
    auto face = body.reduced(6.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff1F2125), face.getTopLeft(),
                                           juce::Colour(0xff131417), face.getBottomRight(),
                                           false));
    g.fillRoundedRectangle(face, 13.0f);
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(roundedRectPath(face, 13.0f));
        g.setColour(juce::Colour(0x0AFFFFFF));
        for (float y = face.getY() + 2.0f; y < face.getBottom(); y += 5.0f)
            for (float x = face.getX() + 2.0f; x < face.getRight(); x += 5.0f)
                g.fillEllipse(x, y, 1.6f, 1.6f);
    }
    // ::after — the raised grip ribs across the toe.
    {
        juce::Rectangle<float> ribs(body.getX() + 22.0f, body.getBottom() - 32.0f,
                                    juce::jmax(0.0f, body.getWidth() - 44.0f), 18.0f);
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(roundedRectPath(ribs, 4.0f));
        for (float x = ribs.getX(); x < ribs.getRight(); x += 8.0f) {
            g.setColour(juce::Colour(0x8C000000));
            g.fillRect(x, ribs.getY(), 2.0f, ribs.getHeight());
            g.setColour(juce::Colour(0x0FFFFFFF));
            g.fillRect(x + 2.0f, ribs.getY(), 1.0f, ribs.getHeight());
        }
    }
    // .treadle-wordmark — embossed light lettering, dimmed when bypassed.
    if (wordmark_.isNotEmpty()) {
        auto tw = juce::Rectangle<float>(body.getX(), body.getBottom() - 62.0f,
                                         body.getWidth(), 26.0f);
        g.setFont(juce::Font(juce::FontOptions(19.0f, juce::Font::bold)));
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.drawText(wordmark_.toUpperCase(), tw.translated(0.0f, 1.0f),
                   juce::Justification::centred);
        g.setColour(juce::Colour(0xffD9DBDF).withAlpha(engaged_ ? 0.92f : 0.55f));
        g.drawText(wordmark_.toUpperCase(), tw, juce::Justification::centred);
    }
}

void Footswitch::paintPad(juce::Graphics& g, juce::Rectangle<float> r) {
    // .fsw-pad: the Ibanez-format hinged METAL plate — wide, low, brushed, and (like
    // the treadle) owning the bottom of the enclosure with nothing below it.
    const float h = juce::jmin(r.getHeight(), 96.0f);
    auto body = r.withTop(r.getBottom() - h);
    juce::Path bp;
    bp.addRoundedRectangle(body, 12.0f);
    juce::DropShadow(skin::shDark, pressed_ ? 10 : 20, pressed_ ? juce::Point<int>{3, 4}
                                                                : juce::Point<int>{7, 9})
        .drawForPath(g, bp);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff2C3036), body.getTopLeft(),
                                           juce::Colour(0xff1C1E22), body.getBottomRight(),
                                           false));
    g.fillRoundedRectangle(body, 12.0f);
    // ::before — the brushed plate face (vertical micro-striping).
    auto face = body.reduced(8.0f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff33373D), face.getTopLeft(),
                                           juce::Colour(0xff232529), face.getBottomRight(),
                                           false));
    g.fillRoundedRectangle(face, 8.0f);
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(roundedRectPath(face, 8.0f));
        g.setColour(juce::Colour(0x08FFFFFF));
        for (float x = face.getX(); x < face.getRight(); x += 3.0f)
            g.fillRect(x, face.getY(), 1.0f, face.getHeight());
    }
    // ::after — the hinge line across the toe.
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillRoundedRectangle(body.getX() + 18.0f, body.getBottom() - 22.0f,
                           juce::jmax(0.0f, body.getWidth() - 36.0f), 3.0f, 1.5f);
    if (wordmark_.isNotEmpty()) {
        auto tw = juce::Rectangle<float>(body.getX(), body.getY() + 16.0f, body.getWidth(),
                                         24.0f);
        g.setFont(skin::monoFont(11.0f));
        g.setColour(skin::inkFaint.withAlpha(engaged_ ? 1.0f : 0.55f));
        g.drawText(wordmark_.toUpperCase(), tw, juce::Justification::centred);
    }
}

// ===========================================================================
// ChipButton (the .rack-btn / .tray-add pill)
// ===========================================================================
ChipButton::ChipButton(const juce::String& text) : text_(text) {}

void ChipButton::mouseDown(const juce::MouseEvent&) {
    if (!enabled_) return;
    held_ = true;
    dragged_ = false;
    repaint();
}

void ChipButton::mouseDrag(const juce::MouseEvent& e) {
    if (!enabled_ || !onDrag) return;
    // Only start dragging past a small threshold, so a click never reorders.
    if (!dragged_ && e.getDistanceFromDragStart() < 4) return;
    dragged_ = true;
    onDrag(getX() + e.x);
}

void ChipButton::mouseUp(const juce::MouseEvent&) {
    const bool wasDragged = dragged_;
    held_ = false;
    dragged_ = false;
    repaint();
    if (!enabled_) return;
    if (wasDragged) {
        if (onDragEnd) onDragEnd();
    } else if (onClick) {
        onClick();
    }
}

void ChipButton::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    const float radius = juce::jmin(7.0f, r.getHeight() * 0.35f);
    if (held_) {
        // :active — the raised pill inverts to a recess.
        g.setColour(skin::well);
        g.fillRoundedRectangle(r, radius);
        g.setColour(skin::shDarker);
        g.drawRoundedRectangle(r.reduced(0.5f).translated(0.6f, 0.7f), radius, 1.6f);
    } else {
        juce::Path p = roundedRectPath(r, radius);
        juce::DropShadow(skin::shDark, 5, {2, 2}).drawForPath(g, p);
        skin::fillDiagGradient(g, r, skin::capEdgeTop, skin::capEdgeBot);
        g.setColour(juce::Colour(0x12FFFFFF));
        g.strokePath(roundedRectPath(r.reduced(0.5f), radius), juce::PathStrokeType(1.0f));
    }
    g.setColour(enabled_ ? tint_ : tint_.withAlpha(0.35f));
    g.setFont(skin::monoFont(juce::jmin(12.0f, r.getHeight() * 0.62f)));
    g.drawText(text_, r, juce::Justification::centred);
}

// ===========================================================================
// LeverToggle (carved slot + sliding cap lever)
// ===========================================================================
LeverToggle::LeverToggle() {}

void LeverToggle::mouseDown(const juce::MouseEvent&) {
    if (onClick) onClick();
}

void LeverToggle::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    const float slotW = 30.0f, slotH = 54.0f;
    juce::Rectangle<float> slot(r.getCentreX() - slotW * 0.5f, r.getY(), slotW, slotH);
    skin::drawWell(g, slot, 15.0f);
    // lever cap (top when off, bottom when on; lit accent when on).
    auto lever = juce::Rectangle<float>(slot.getX() + 4.0f, slot.getY() + (on_ ? 26.0f : 4.0f),
                                        slotW - 8.0f, 24.0f);
    if (on_) {
        g.setGradientFill(juce::ColourGradient(accent_.interpolatedWith(skin::ground, 0.4f),
                                               lever.getTopLeft(),
                                               accent_.interpolatedWith(juce::Colours::black,
                                                                        0.15f),
                                               lever.getBottomRight(), false));
    } else {
        g.setGradientFill(juce::ColourGradient(skin::capTop, lever.getTopLeft(), skin::capBot,
                                               lever.getBottomRight(), false));
    }
    g.fillRoundedRectangle(lever, 12.0f);
    g.setColour(juce::Colour(0x18FFFFFF));
    g.drawRoundedRectangle(lever.reduced(0.5f), 12.0f, 1.0f);
    // caption.
    g.setColour(skin::inkDim);
    g.setFont(skin::monoFont(9.5f));
    g.drawText(caption_.toUpperCase(), r.withTop(slot.getBottom() + 3.0f),
               juce::Justification::centred);
}

// ===========================================================================
// PowerControl (jewel + rocker)
// ===========================================================================
PowerControl::PowerControl() {}

void PowerControl::mouseDown(const juce::MouseEvent&) {
    if (onClick) onClick();
}

void PowerControl::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    const float jewelD = 15.0f;
    auto jewel = juce::Rectangle<float>(r.getCentreX() - jewelD * 0.5f, r.getY(), jewelD,
                                        jewelD);
    // The jewel is drawn LAST (see below): the rocker's DropShadow reaches back over
    // this band and used to paint out the lit halo — the "lights are covered over"
    // bug. The rocker also gets a wider gap so the shadow starts clear of the glow.
    auto rockArea = r.withTrimmedTop(jewelD + 14.0f).withTrimmedBottom(16.0f);
    float rw = 44.0f, rh = juce::jmin(60.0f, rockArea.getHeight());
    juce::Rectangle<float> rocker(rockArea.getCentreX() - rw * 0.5f, rockArea.getY(), rw, rh);
    juce::Path rp = roundedRectPath(rocker, 11.0f);
    juce::DropShadow(skin::shDark, 8, {3, 3}).drawForPath(g, rp);
    skin::fillDiagGradient(g, rocker, skin::capEdgeTop, skin::capEdgeBot);
    // two rocker halves; the pressed half is recessed.
    auto top = rocker.withHeight(rocker.getHeight() * 0.5f).reduced(6.0f, 5.0f);
    auto bot = top.translated(0, rocker.getHeight() * 0.5f);
    auto drawHalf = [&](juce::Rectangle<float> h, bool raised) {
        if (raised) {
            g.setGradientFill(juce::ColourGradient(skin::capTop, h.getTopLeft(), skin::capBot,
                                                   h.getBottomRight(), false));
        } else {
            g.setColour(skin::well);
        }
        g.fillRoundedRectangle(h, 7.0f);
    };
    drawHalf(top, !on_);  // when on, the TOP is pressed in (down)
    drawHalf(bot, on_);
    g.setColour(skin::inkFaint);
    g.setFont(skin::monoFont(9.5f));
    g.drawText("POWER", r.withTop(r.getBottom() - 14.0f), juce::Justification::centred);

    // The jewel goes on LAST, over the rocker's cast shadow — never under it.
    skin::drawJewel(g, jewel, accent_, on_);
}

// ===========================================================================
// ModeSwitch (3 stacked carved segments)
// ===========================================================================
ModeSwitch::ModeSwitch() {}

void ModeSwitch::mouseDown(const juce::MouseEvent& e) {
    auto r = getLocalBounds().withTrimmedBottom(16);
    int segH = r.getHeight() / labels_.size();
    int idx = juce::jlimit(0, labels_.size() - 1, (e.y - r.getY()) / juce::jmax(1, segH));
    if (onSelect) onSelect(idx);
}

void ModeSwitch::paint(juce::Graphics& g) {
    auto full = getLocalBounds().toFloat();
    auto r = full.withTrimmedBottom(16.0f);
    skin::drawWell(g, r, 12.0f);
    int n = labels_.size();
    float segH = r.getHeight() / (float)n;
    for (int i = 0; i < n; ++i) {
        auto seg = juce::Rectangle<float>(r.getX(), r.getY() + segH * i, r.getWidth(), segH)
                       .reduced(3.0f, 2.0f);
        bool on = (i == selected_);
        if (on) {
            g.setGradientFill(juce::ColourGradient(accent_.interpolatedWith(skin::ground,
                                                                            0.45f),
                                                   seg.getTopLeft(),
                                                   accent_.interpolatedWith(juce::Colours::black,
                                                                            0.18f),
                                                   seg.getBottomRight(), false));
            g.fillRoundedRectangle(seg, 7.0f);
            g.setColour(skin::ink);
        } else {
            g.setColour(skin::inkDim);
        }
        g.setFont(skin::monoFont(11.0f));
        g.drawText(labels_[i], seg, juce::Justification::centred);
    }
    g.setColour(skin::inkDim);
    g.setFont(skin::monoFont(9.5f));
    g.drawText("MODE", full.withTop(r.getBottom() + 2.0f), juce::Justification::centred);
}

}  // namespace clipper::native
