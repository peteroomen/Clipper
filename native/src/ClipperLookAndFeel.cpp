#include "ClipperLookAndFeel.h"
#include "UiSound.h"

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

// ---------------------------------------------------------------------------
// THEME state + the two token roots (tokens.css, verbatim)
// ---------------------------------------------------------------------------
namespace {
ThemeMode gThemeMode = ThemeMode::Auto;
bool gSystemDark = false;

juce::PropertiesFile::Options themeStoreOptions() {
    juce::PropertiesFile::Options o;
    o.applicationName = "Clipper";
    o.filenameSuffix = "settings";
#if JUCE_LINUX || JUCE_BSD
    // JUCE's plain folderName would put this at ~/Clipper/; XDG says ~/.config.
    o.folderName = ".config/Clipper";
#else
    o.folderName = "Clipper";
#endif
    o.osxLibrarySubFolder = "Application Support";
    o.commonToAllUsers = false;
    return o;
}
constexpr const char* kThemeKey = "themeMode";
}  // namespace

void setThemeMode(ThemeMode m) { gThemeMode = m; }
ThemeMode themeMode() { return gThemeMode; }
void setSystemDark(bool osIsDark) { gSystemDark = osIsDark; }
bool isDark() {
    return gThemeMode == ThemeMode::Dark || (gThemeMode == ThemeMode::Auto && gSystemDark);
}

juce::String themeModeName(ThemeMode m) {
    switch (m) {
        case ThemeMode::Light: return "light";
        case ThemeMode::Dark:  return "dark";
        default:               return "auto";
    }
}

// The store is opened and closed per call rather than kept alive in a global: a
// juce::PropertiesFile is a Timer + ChangeBroadcaster, and one outliving the JUCE
// message manager (a plugin unloading, the snapshot tool exiting) is a shutdown
// assert waiting to happen. Reading a ~50-byte file twice a session costs nothing.
ThemeMode loadThemeMode() {
    const auto opts = themeStoreOptions();
    juce::PropertiesFile store(opts.getDefaultFile(), opts);
    const juce::String v = store.getValue(kThemeKey, "auto");
    if (v == "light") return ThemeMode::Light;
    if (v == "dark") return ThemeMode::Dark;
    return ThemeMode::Auto;
}

void saveThemeMode(ThemeMode m) {
    const auto opts = themeStoreOptions();
    juce::PropertiesFile store(opts.getDefaultFile(), opts);
    store.setValue(kThemeKey, themeModeName(m));
    store.saveIfNeeded();
}

// --- the BENCH, per theme (tokens.css :root / prefers-color-scheme: dark) ----
juce::Colour ground() { return isDark() ? juce::Colour(0xff26292E) : juce::Colour(0xffE5E3DE); }
juce::Colour groundDeep() {
    return isDark() ? juce::Colour(0xff202329) : juce::Colour(0xffDBD8D2);
}
juce::Colour benchWell() {
    return isDark() ? juce::Colour(0xff1D2025) : juce::Colour(0xffD6D3CC);
}
juce::Colour benchInk() { return isDark() ? juce::Colour(0xffE8E9EB) : juce::Colour(0xff2B2C2E); }
juce::Colour benchInkDim() {
    return isDark() ? juce::Colour(0xff969BA3) : juce::Colour(0xff71736F);
}
juce::Colour benchFaint() {
    return isDark() ? juce::Colour(0xff6A6F77) : juce::Colour(0xff9B9D98);
}
juce::Colour castShadow() {
    // Light: the warm .pedal.raised cast, rgba(54,50,44,.30). Dark: the base
    // .raised --sh-dark rgba(9,11,14,.85) — the warm brown is invisible on charcoal
    // and the dark theme's own recipe is the near-black one.
    return isDark() ? juce::Colour(0xd9090B0E) : juce::Colour(0x4D36322C);
}
juce::Colour cable() { return isDark() ? juce::Colour(0xff14161A) : juce::Colour(0xff35383E); }
juce::Colour cableHi() { return isDark() ? juce::Colour(0x1CFFFFFF) : juce::Colour(0x80FFFFFF); }
juce::Colour cablePlug() {
    return isDark() ? juce::Colour(0xff474C54) : juce::Colour(0xffC4C0B8);
}

juce::Colour accent(AccentId id) {
    const bool d = isDark();
    switch (id) {
        case AccentId::Rat:    return d ? juce::Colour(0xffFF4A34) : juce::Colour(0xffF03B24);
        case AccentId::Sd:     return d ? juce::Colour(0xffFFC94D) : juce::Colour(0xffB58900);
        case AccentId::Ts:     return d ? juce::Colour(0xff2FD07A) : juce::Colour(0xff1E9E5A);
        case AccentId::Muff:   return d ? juce::Colour(0xffB98CEE) : juce::Colour(0xff7A3FBF);
        case AccentId::Phaser: return d ? juce::Colour(0xffFF8C3A) : juce::Colour(0xffC4611A);
        // Gold takes the DARK token in BOTH themes — see the header: it only ever
        // paints on the pinned-dark chassis, where #8F6A22 measures ~2.8:1.
        case AccentId::Gold:   return juce::Colour(0xffD9B36B);
        // Wah "Weeper" (docs §58) — TEAL, --accent-wah. Light #17868F / dark
        // #3ED0DD, verbatim from tokens.css. Deliberately the furthest hue from
        // the six dirt/mod accents: this is the first pedal of a new family and
        // the board should read that at a glance.
        case AccentId::Wah:    return d ? juce::Colour(0xff3ED0DD) : juce::Colour(0xff17868F);
        case AccentId::Jcm:    return d ? juce::Colour(0xffE8B84B) : juce::Colour(0xffA87A18);
        case AccentId::Twin:   return d ? juce::Colour(0xff7FB4E8) : juce::Colour(0xff4E7BA8);
        case AccentId::Ac30:   return d ? juce::Colour(0xffE08A4A) : juce::Colour(0xffB4612C);
        case AccentId::Clean:  return d ? juce::Colour(0xffFF4A34) : juce::Colour(0xffF03B24);
    }
    return d ? juce::Colour(0xffFF4A34) : juce::Colour(0xffF03B24);
}

const Scheme& darkIsland() {
    // The pedal chassis pinning — exactly the constants above, gathered.
    static const Scheme s{capTop,     capBot,   capEdgeTop, capEdgeBot, well,
                          panelTop,   panelBot, ink,        inkDim,     inkFaint,
                          shDark,     shDarker, shLight,    arcTrack};
    return s;
}

const Scheme& lightBench() {
    // tokens.css :root LIGHT values, verbatim — the context the web amp's
    // controls actually resolve in (the dark pinning is .pedal-scoped).
    static const Scheme s{
        juce::Colour(0xffEFEDE8), juce::Colour(0xffD9D6CF),   // --cap 145deg
        juce::Colour(0xffDDDAD3), juce::Colour(0xffEFEDE8),   // --cap-edge 145deg
        juce::Colour(0xffD6D3CC),                             // --well
        juce::Colour(0xffEAE8E3), juce::Colour(0xffDFDCD6),   // --panel-grad 160deg
        juce::Colour(0xff2B2C2E), juce::Colour(0xff71736F),   // --ink / --ink-dim
        juce::Colour(0xff9B9D98),                             // --ink-faint
        juce::Colour(0xd9A39D92),  // --sh-dark   rgba(163,157,146,.85)
        juce::Colour(0x8c8C867A),  // --sh-darker rgba(140,134,122,.55)
        juce::Colour(0xf2FFFFFF),  // --sh-light  rgba(255,255,255,.95)
        juce::Colour(0x14000000)   // --arc-track rgba(0,0,0,.08)
    };
    return s;
}

const Scheme& benchScheme() {
    // The dark root's control tokens ARE the .pedal pinning, byte for byte — which
    // is why a dark-theme amp panel needs no third scheme and why the pedals do not
    // move between themes.
    return isDark() ? darkIsland() : lightBench();
}

void fillDiagGradient(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour from,
                      juce::Colour to) {
    // ~150deg: from top-left toward bottom-right.
    juce::ColourGradient grad(from, r.getTopLeft(), to, r.getBottomRight(), false);
    g.setGradientFill(grad);
    g.fillRect(r);
}

void drawIslandCastShadow(juce::Graphics& g, juce::Rectangle<float> r, float radius) {
    auto path = roundedRectPath(r, radius);
    // The .pedal.raised dual cast: 16px 18px 34px @ .30 + a tighter 3px 4px 10px at
    // ~.73 of it. In dark theme castShadow() is the near-black --sh-dark, so the
    // ratio carries over and the recipe stays one recipe.
    const juce::Colour cs = castShadow();
    juce::DropShadow(cs, 22, {11, 13}).drawForPath(g, path);
    juce::DropShadow(cs.withMultipliedAlpha(0.73f), 8, {3, 4}).drawForPath(g, path);
}

void drawChassisBody(juce::Graphics& g, juce::Rectangle<float> r, float radius) {
    auto path = roundedRectPath(r, radius);

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

void drawChassisCard(juce::Graphics& g, juce::Rectangle<float> r, float radius) {
    // Cast + body, for a card the EDITOR paints (the input card, the amp): those
    // are not child components, so their shadow has the bench to fall on.
    drawIslandCastShadow(g, r, radius);
    drawChassisBody(g, r, radius);
}

void drawBenchCard(juce::Graphics& g, juce::Rectangle<float> r, float radius) {
    auto path = roundedRectPath(r, radius);
    // The web .raised dual box-shadow: 10px 10px 24px sh-dark + -10 -10 22 sh-light.
    const Scheme& s = benchScheme();
    juce::DropShadow(s.shDark.withMultipliedAlpha(0.75f), 16, {7, 7}).drawForPath(g, path);
    juce::DropShadow(s.shLight.withMultipliedAlpha(0.85f), 15, {-7, -7}).drawForPath(g, path);
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(path);
        fillDiagGradient(g, r, s.panelTop, s.panelBot);
    }
    // inset 0 1px 0 sh-light — the top rim catching the bench light.
    g.setColour(s.shLight.withMultipliedAlpha(0.8f));
    g.drawLine(r.getX() + radius, r.getY() + 1.0f, r.getRight() - radius, r.getY() + 1.0f,
               1.2f);
    // A hairline darker edge so the light card separates from the light bench.
    g.setColour(s.shDarker.withMultipliedAlpha(0.55f));
    g.strokePath(roundedRectPath(r.reduced(0.5f), radius), juce::PathStrokeType(1.0f));
}

void drawWell(juce::Graphics& g, juce::Rectangle<float> r, float radius, const Scheme& s) {
    auto path = roundedRectPath(r, radius);
    g.setColour(s.well);
    g.fillPath(path);
    // inset dark top-left, inset light bottom-right (the carved recess).
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(path);
        g.setColour(s.shDarker);
        g.strokePath(roundedRectPath(r.translated(1.6f, 1.8f), radius),
                     juce::PathStrokeType(3.0f));
        g.setColour(s.shLight);
        g.strokePath(roundedRectPath(r.translated(-1.4f, -1.4f), radius),
                     juce::PathStrokeType(2.0f));
    }
}

float glowSpread(float jewelDiameter) {
    // The wide glow layer (box-shadow 0 0 16px) reaches ~13 px of visible halo past
    // the web's 16 px jewel; scale with the diameter so a small LED keeps its
    // proportions.
    return jewelDiameter * 0.8f;
}

void drawJewel(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent, bool on,
               const Scheme& s) {
    auto c = r.getCentre();
    float rad = juce::jmin(r.getWidth(), r.getHeight()) * 0.5f;
    if (on) {
        // The web glow: box-shadow 0 0 16px accent-glow + 0 0 5px accent-glow,
        // accent-glow ≈ accent at .5 alpha. Two SMOOTH radial gradients — the old
        // four stacked discs compounded into visibly banded steps.
        const juce::Colour glow = accent.withAlpha(0.5f);
        auto layer = [&](float spread, float alphaScale) {
            const float R = rad + spread;
            // Soft from the jewel's EDGE outward — a blur halo, not a plateau.
            juce::ColourGradient grad(glow.withMultipliedAlpha(alphaScale), c.x, c.y,
                                      glow.withAlpha(0.0f), c.x + R, c.y, true);
            grad.addColour(juce::jlimit(0.05, 0.95, (double)(rad / R) * 0.9),
                           glow.withMultipliedAlpha(alphaScale * 0.55f));
            g.setGradientFill(grad);
            g.fillEllipse(c.x - R, c.y - R, R * 2.0f, R * 2.0f);
        };
        layer(glowSpread(rad * 2.0f), 0.75f);  // 0 0 16px
        layer(rad * 0.62f, 0.8f);              // 0 0 5px
        // The lit jewel — the web's radial-gradient(circle at 35% 30%, <hot>,
        // accent 55%, <deep rim>): a near-white hot spot up-left, the accent
        // mid-body, a deep rim.
        juce::ColourGradient body(accent.interpolatedWith(juce::Colours::white, 0.78f),
                                  c.x - rad * 0.30f, c.y - rad * 0.40f,
                                  accent.darker(1.1f), c.x + rad * 0.9f,
                                  c.y + rad * 0.9f, true);
        body.addColour(0.55, accent);
        g.setGradientFill(body);
        g.fillEllipse(c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
    } else {
        // Recessed dark dot (color-mix accent 18% into the well) + the web's
        // inset 2px 2px 4px sh-darker.
        g.setColour(s.well.interpolatedWith(accent, 0.18f));
        g.fillEllipse(c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
        {
            juce::Graphics::ScopedSaveState ss(g);
            juce::Path clip;
            clip.addEllipse(c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
            g.reduceClipRegion(clip);
            g.setColour(s.shDarker);
            g.drawEllipse(c.x - rad + 0.6f, c.y - rad + 0.8f, rad * 2.0f - 0.6f,
                          rad * 2.0f - 0.6f, 1.6f);
        }
        g.setColour(s.shDarker);
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
    // The bore (.jack::after) — a darker inner disc. Chassis-toned, not bench-toned:
    // a jack is hardware and reads the same in both themes.
    auto bore = body.reduced(diameter * 0.25f);
    g.setColour(well.darker(0.6f));
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
        g.setColour(castShadow().withMultipliedAlpha(0.6f));
        g.strokePath(shadow, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }
    // .cable-body — the rubber tube.
    g.setColour(cable());
    g.strokePath(p, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved,
                                         juce::PathStrokeType::rounded));
    // .cable-hi — the specular edge hugging the top of the tube (translateY -1.4).
    {
        juce::Path hi = p;
        hi.applyTransform(juce::AffineTransform::translation(0.0f, -1.4f));
        g.setColour(cableHi().withMultipliedAlpha(0.44f));
        g.strokePath(hi, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    }
    // .cable-plug — the plug disc at each end.
    g.setColour(cablePlug());
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

juce::Font serifFont(float height) {
    return juce::Font(juce::FontOptions(juce::Font::getDefaultSerifFontName(), height,
                                        juce::Font::plain));
}

void drawTracked(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                 float tracking) {
    if (text.isEmpty()) return;
    const juce::Font f = g.getCurrentFont();
    const int n = text.length();

    // Measure once with the tracking folded in, then shrink the tracking (never the
    // font — the engraving reads by its wide spacing) if the run would overhang.
    auto runWidth = [&](float track) {
        float w = 0.0f;
        for (int i = 0; i < n; ++i)
            w += juce::GlyphArrangement::getStringWidth(f, juce::String::charToString(text[i]));
        return w + track * static_cast<float>(juce::jmax(0, n - 1));
    };
    float track = tracking;
    if (runWidth(track) > area.getWidth() && n > 1)
        track = juce::jmax(0.0f, track - (runWidth(track) - area.getWidth()) /
                                             static_cast<float>(n - 1));

    float x = area.getX() + (area.getWidth() - runWidth(track)) * 0.5f;
    for (int i = 0; i < n; ++i) {
        const juce::String ch = juce::String::charToString(text[i]);
        const float w = juce::GlyphArrangement::getStringWidth(f, ch);
        g.drawText(ch, juce::Rectangle<float>(x, area.getY(), w, area.getHeight()),
                   juce::Justification::centred, false);
        x += w + track;
    }
}

void drawNamePlate(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour accent,
                   const juce::String& text, bool engaged) {
    if (r.getWidth() < 8.0f || r.getHeight() < 8.0f) return;
    const float radius = 6.0f;  // .name-plate border-radius
    auto path = roundedRectPath(r, radius);

    // The milled band: linear-gradient(160deg,#23262b,#1b1e22) — a shade below the
    // chassis, so it reads as material removed rather than a panel stuck on.
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(path);
        fillDiagGradient(g, r, juce::Colour(0xff23262B), juce::Colour(0xff1B1E22));
        // inset 2px 3px 6px sh-darker / inset -1px -1px 3px sh-light — the recess.
        g.setColour(shDarker);
        g.strokePath(roundedRectPath(r.translated(2.0f, 3.0f), radius),
                     juce::PathStrokeType(4.0f));
        g.setColour(shLight);
        g.strokePath(roundedRectPath(r.translated(-1.2f, -1.2f), radius),
                     juce::PathStrokeType(2.0f));
    }

    // The hairline gold rules along the plate's top and bottom lips (border-top 55%,
    // border-bottom 40%) — the only place the accent touches a large surface.
    g.setColour(accent.withMultipliedAlpha(engaged ? 0.55f : 0.34f));
    g.drawLine(r.getX() + radius, r.getY() + 0.5f, r.getRight() - radius, r.getY() + 0.5f, 1.0f);
    g.setColour(accent.withMultipliedAlpha(engaged ? 0.40f : 0.24f));
    g.drawLine(r.getX() + radius, r.getBottom() - 0.5f, r.getRight() - radius,
               r.getBottom() - 0.5f, 1.0f);

    // ENGRAVED lettering: cut in from above (a dark line along the top of each
    // stroke), catching light on the lower lip — the inverse of an emboss.
    auto textArea = r.reduced(10.0f, 0.0f);
    const float h = juce::jlimit(13.0f, 26.0f, r.getHeight() * 0.60f);
    g.setFont(serifFont(h));
    const juce::String mark = text.toUpperCase();
    const float track = juce::jmax(2.0f, h * 0.42f);  // .42em tracking

    g.setColour(juce::Colour(0xBF000000));
    drawTracked(g, mark, textArea.translated(0.0f, -1.0f), track);
    g.setColour(juce::Colour(0x12FFFFFF));
    drawTracked(g, mark, textArea.translated(0.0f, 1.0f), track);
    // color-mix(accent 78%, ink) — gold, but pulled toward the panel ink so it reads
    // as metal catching light rather than printed paint.
    g.setColour(accent.interpolatedWith(ink, 0.22f).withMultipliedAlpha(engaged ? 1.0f : 0.55f));
    drawTracked(g, mark, textArea, track);
}

void drawBoardRail(juce::Graphics& g, juce::Rectangle<float> r) {
    if (r.getWidth() < 4.0f || r.getHeight() < 4.0f) return;

    // 1. The CHANNEL milled into the bench — board.css's .board-source recess
    //    (inset 4px 4px 10px sh-darker, inset -3px -3px 8px sh-light), scaled up to
    //    a whole plank. Bench-toned, so it belongs to the porcelain rather than
    //    competing with the dark enclosures standing in it.
    const float radius = 14.0f;
    const Scheme& bs = benchScheme();
    auto channel = roundedRectPath(r, radius);
    g.setColour(benchWell());
    g.fillPath(channel);
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(channel);
        // The recess pair, in the BENCH scheme — in dark theme --sh-light is
        // rgba(255,255,255,.055), so the milled highlight goes from porcelain-bright
        // to the faint sheen a dark surface actually catches.
        g.setColour(bs.shDarker.withMultipliedAlpha(isDark() ? 1.0f : 0.75f));
        g.strokePath(roundedRectPath(r.translated(2.5f, 3.0f), radius),
                     juce::PathStrokeType(7.0f));
        g.setColour(bs.shLight);
        g.strokePath(roundedRectPath(r.translated(-2.0f, -2.0f), radius),
                     juce::PathStrokeType(5.0f));
    }

    // 2. The ribbed rubber MAT lying in the channel. Warm dark grey — a different
    //    family from the cool charcoal chassis, so a pedal never dissolves into it.
    auto mat = r.reduced(7.0f, 6.0f);
    if (mat.getWidth() < 4.0f || mat.getHeight() < 4.0f) return;
    auto matPath = roundedRectPath(mat, 9.0f);
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(matPath);
        // A different family from the cool charcoal chassis in both themes; the dark
        // theme's mat is a shade deeper so a pedal never dissolves into it.
        if (isDark())
            fillDiagGradient(g, mat, juce::Colour(0xff3A3830), juce::Colour(0xff2A2824));
        else
            fillDiagGradient(g, mat, juce::Colour(0xff4A473F), juce::Colour(0xff35332E));

        // Drawn ribs, not a texture bitmap: a light edge and a dark valley per rib,
        // the way moulded rubber matting actually catches a workbench lamp.
        const float pitch = 11.0f;
        for (float x = mat.getX() + pitch * 0.5f; x < mat.getRight(); x += pitch) {
            g.setColour(juce::Colour(0x14FFFFFF));
            g.drawLine(x, mat.getY() + 2.0f, x, mat.getBottom() - 2.0f, 1.6f);
            g.setColour(juce::Colour(0x33000000));
            g.drawLine(x + 1.8f, mat.getY() + 2.0f, x + 1.8f, mat.getBottom() - 2.0f, 1.4f);
        }
        // The mat sits IN the channel: shadow along its top lip, light along the base.
        g.setColour(juce::Colour(0x59000000));
        g.strokePath(roundedRectPath(mat.translated(0.0f, 2.4f), 9.0f),
                     juce::PathStrokeType(4.0f));
        g.setColour(juce::Colour(0x14FFFFFF));
        g.drawLine(mat.getX() + 9.0f, mat.getBottom() - 1.0f, mat.getRight() - 9.0f,
                   mat.getBottom() - 1.0f, 1.0f);
    }
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
    // Which token context this dial resolves in (NeuKnob::setScheme). Web parity:
    // amp knobs are LIGHT porcelain, pedal knobs the pinned-dark chassis.
    const skin::Scheme& sc = s.getProperties().getWithDefault("clipperLightScheme", false)
                                 ? skin::lightBench()
                                 : skin::darkIsland();

    // --- body cast shadows: the web's DUAL 6px 6px 14 sh-dark + -5 -5 12 sh-light.
    // The light counter-shadow is what makes the body read as RAISED — it was
    // missing entirely before this pass (visible on the porcelain amp knobs,
    // subtle-but-present on the dark chassis, exactly like the CSS).
    {
        juce::Path bp;
        bp.addEllipse(centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
        juce::DropShadow(sc.shDark, (int)(bodyR * 0.55f), {3, 4}).drawForPath(g, bp);
        juce::DropShadow(sc.shLight, (int)(bodyR * 0.48f), {-3, -3}).drawForPath(g, bp);
    }
    // --- body (--cap-edge 145deg) ---
    {
        juce::Rectangle<float> br(centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f,
                                  bodyR * 2.0f);
        g.setGradientFill(juce::ColourGradient(sc.capEdgeTop, br.getTopLeft(),
                                               sc.capEdgeBot, br.getBottomRight(),
                                               false));
        g.fillEllipse(br);
    }
    // --- knurled skirt (repeating-conic 2.5deg/9deg dark ticks, opacity .35) ---
    {
        g.setColour(sc.shDark.withMultipliedAlpha(0.35f));
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
    // --- cap dome (--cap 145deg) — the web cap has BOTH inset rims AND its own
    // cast shadow onto the skirt (2px 3px 6px sh-darker), which is the depth cue
    // that separates cap from knurl.
    const float capR = bodyR * 0.80f;
    {
        juce::Rectangle<float> cr(centre.x - capR, centre.y - capR, capR * 2.0f,
                                  capR * 2.0f);
        {
            juce::Path cp;
            cp.addEllipse(cr);
            juce::DropShadow(sc.shDarker, 5, {2, 3}).drawForPath(g, cp);
        }
        g.setGradientFill(juce::ColourGradient(sc.capTop, cr.getTopLeft(), sc.capBot,
                                               cr.getBottomRight(), false));
        g.fillEllipse(cr);
        g.setColour(sc.shLight.withMultipliedAlpha(0.35f));  // inset light TL rim
        g.drawEllipse(cr.reduced(0.6f).translated(-0.4f, -0.4f), 1.1f);
        g.setColour(sc.shDarker);  // inset dark bottom-right
        g.drawEllipse(cr.reduced(0.6f).translated(0.6f, 0.8f), 1.0f);
    }
    // --- ink pointer ---
    {
        juce::Path ptr;
        float pw = juce::jmax(2.4f, bodyR * 0.10f);
        ptr.addRoundedRectangle(-pw * 0.5f, -capR * 0.92f, pw, capR * 0.52f, pw * 0.5f);
        g.setColour(sc.ink.withAlpha(dim ? 0.4f : 0.85f));
        g.fillPath(ptr, juce::AffineTransform::rotation(angle).translated(centre));
    }
    // --- the floating value ARC (track + accent fill) ---
    {
        const float thick = juce::jmax(2.6f, side * 0.055f);
        juce::Path track;
        track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, kArcStart, kArcEnd, true);
        g.setColour(sc.arcTrack);
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

void ClipperLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y,
                                       int width, int height, bool isScrollbarVertical,
                                       int thumbStartPosition, int thumbSize,
                                       bool isMouseOver, bool isMouseDown) {
    auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float thick = isScrollbarVertical ? area.getWidth() : area.getHeight();
    const float radius = thick * 0.5f;

    // The TRACK: a shallow groove scratched into the bench, not a filled gutter —
    // it must not read as a second rail under the real one.
    auto track = isScrollbarVertical ? area.reduced(thick * 0.30f, 2.0f)
                                     : area.reduced(2.0f, thick * 0.30f);
    g.setColour(skin::benchWell().darker(0.06f));
    g.fillRoundedRectangle(track, track.getHeight() * 0.5f);

    if (thumbSize <= 0) return;  // content fits — nothing to grab

    // The THUMB: a soft capsule in the bench's own ink, brightening on hover/drag.
    auto thumb = isScrollbarVertical
                     ? juce::Rectangle<float>(area.getX(), (float)thumbStartPosition,
                                              area.getWidth(), (float)thumbSize)
                     : juce::Rectangle<float>((float)thumbStartPosition, area.getY(),
                                              (float)thumbSize, area.getHeight());
    thumb = isScrollbarVertical ? thumb.reduced(thick * 0.22f, 1.0f)
                                : thumb.reduced(1.0f, thick * 0.22f);
    const float alpha = isMouseDown ? 0.92f : (isMouseOver ? 0.78f : 0.58f);
    g.setColour(skin::benchInkDim().withAlpha(alpha));
    g.fillRoundedRectangle(thumb, radius);
    g.setColour(skin::benchScheme().shLight.withMultipliedAlpha(0.35f));
    g.drawRoundedRectangle(thumb.reduced(0.5f), radius, 1.0f);
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

    slider_.onValueChange = [this] {
        refreshReadout();
        // The web's knob detent tick (Knob.tsx TICK_DETENT): a quiet click each
        // time the value moves 0.04 from the last ticked value — but only while
        // the USER is turning it (a host/preset update must stay silent).
        const double v = slider_.getValue();
        if (slider_.isMouseButtonDown() &&
            std::abs(v - lastTickValue_) >= uisound::kTickDetent) {
            lastTickValue_ = v;
            uisound::tick();
        }
    };
    scheme_ = &skin::darkIsland();
    refreshReadout();
    lastTickValue_ = slider_.getValue();
}

void NeuKnob::setScheme(const skin::Scheme& s) {
    scheme_ = &s;
    slider_.getProperties().set("clipperLightScheme", &s == &skin::lightBench());
    nameLabel_.setColour(juce::Label::textColourId,
                         dimmed_ ? s.inkDim.withAlpha(0.5f) : s.inkDim);
    repaint();
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
    const skin::Scheme& sc = scheme_ ? *scheme_ : skin::darkIsland();
    valueLabel_.setColour(juce::Label::textColourId,
                          d ? accent_.withAlpha(0.45f) : accent_);
    nameLabel_.setColour(juce::Label::textColourId,
                         d ? sc.inkDim.withAlpha(0.5f) : sc.inkDim);
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
    uisound::thunk(true);  // the web's onPointerDown thunk
    if (onClick) onClick();
}

void Footswitch::mouseUp(const juce::MouseEvent&) {
    uisound::thunk(false);  // the release half of the web's stomp sound
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
    // 8px 8px 18px sh-dark + -7 -7 16 sh-light (raised) collapsing to 3/3/8 + -3/-3/7
    // while pressed — the web .fsw dual shadow, light counter included.
    juce::DropShadow(skin::shDark, pressed_ ? 8 : 16, pressed_ ? juce::Point<int>{3, 3}
                                                               : juce::Point<int>{5, 6})
        .drawForPath(g, sp);
    juce::DropShadow(skin::shLight, pressed_ ? 7 : 14, pressed_ ? juce::Point<int>{-3, -3}
                                                                : juce::Point<int>{-5, -5})
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
    uisound::thunk(true);
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
    uisound::thunk(false);
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
// LeverToggle (carved slot + sliding cap lever) — the web .toggle/.t-slot/.t-lever
// recipe verbatim, in the LIGHT scheme (amp.css resolves light), with the 160 ms
// overshoot slide (transition: top .16s cubic-bezier(.34,1.56,.64,1)).
// ===========================================================================
namespace {
// easeOutBack ≈ cubic-bezier(.34,1.56,.64,1): overshoots ~10 % then settles.
float easeOutBack(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}
constexpr int kLeverAnimMs = 160;
// .t-slot 30x54 radius 15; .t-lever inset 4 px each side, 24 px tall, top 4 (off)
// → 26 (on). The lever's cast shadow is 2px 3px 6px, so it needs ~8 px of
// head-room on the right and below and ~4 px above/left.
constexpr float kSlotW = 30.0f;
constexpr float kSlotH = 54.0f;
constexpr float kLeverInset = 4.0f;
constexpr float kLeverH = 24.0f;
constexpr float kLeverShadowPad = 8.0f;
}  // namespace

LeverToggle::LeverToggle() {}

int LeverToggle::preferredWidth() {
    return (int)std::ceil(kSlotW + 2.0f * kLeverShadowPad);
}

void LeverToggle::setOn(bool o) {
    if (!everSet_ || on_ == o) {
        // First sync from the APVTS (or a no-op): SNAP — a window opening on a
        // saved session must not play the slide.
        everSet_ = true;
        on_ = o;
        leverPos_ = o ? 1.0f : 0.0f;
        repaint();
        return;
    }
    on_ = o;
    animFrom_ = leverPos_;
    animStartMs_ = juce::Time::getMillisecondCounterHiRes();
    animating_ = true;
    startTimerHz(60);
}

void LeverToggle::timerCallback() {
    const float t = juce::jlimit(
        0.0f, 1.0f,
        (float)((juce::Time::getMillisecondCounterHiRes() - animStartMs_) / kLeverAnimMs));
    const float target = on_ ? 1.0f : 0.0f;
    // CLAMPED (visual pass 3). easeOutBack peaks at 1.0987, which put the lever
    // 2.17 px past the slot's 4 px inset — out of the slot at whichever end it was
    // travelling to. The spring's timing is what the web's cubic-bezier gives you;
    // the overshoot past a physical stop is not something a real lever does.
    leverPos_ = juce::jlimit(0.0f, 1.0f, animFrom_ + (target - animFrom_) * easeOutBack(t));
    if (t >= 1.0f) {
        leverPos_ = target;
        animating_ = false;
        stopTimer();
    }
    repaint();
}

void LeverToggle::mouseDown(const juce::MouseEvent&) {
    uisound::thunk(true);
    if (onClick) onClick();
}

void LeverToggle::mouseUp(const juce::MouseEvent&) {
    uisound::thunk(false);
}

void LeverToggle::paint(juce::Graphics& g) {
    const skin::Scheme& sc = skin::benchScheme();
    auto r = getLocalBounds().toFloat();
    // .t-slot: 30x54, radius 15, a carved well (inset 4/4/9 darker + -3/-3/7 light).
    juce::Rectangle<float> slot(r.getCentreX() - kSlotW * 0.5f, r.getY(), kSlotW, kSlotH);
    skin::drawWell(g, slot, 15.0f, sc);
    // .t-lever: inset 4 px each side, 24 px tall, top 4 (off) → 26 (on), animated.
    // The travel is exactly (slotH - 2*inset - leverH) = 22 px, so the settled lit
    // position IS the web's top:26px with the 4 px bottom inset preserved.
    const float travel = kSlotH - 2.0f * kLeverInset - kLeverH;
    const float leverY = slot.getY() + kLeverInset + leverPos_ * travel;
    auto lever = juce::Rectangle<float>(slot.getX() + kLeverInset, leverY,
                                        kSlotW - 2.0f * kLeverInset, kLeverH);
    {
        juce::Path lp = roundedRectPath(lever, 12.0f);
        juce::DropShadow(sc.shDark, 5, {2, 3}).drawForPath(g, lp);
    }
    // The lit gradient only once the lever has crossed halfway, like the web's
    // class flip (the colour switches while the slide is in flight).
    const bool lit = leverPos_ > 0.5f;
    if (lit) {
        g.setGradientFill(juce::ColourGradient(
            accent_.interpolatedWith(skin::ground(), 0.40f), lever.getTopLeft(),
            accent_.interpolatedWith(juce::Colours::black, 0.15f), lever.getBottomRight(),
            false));
    } else {
        g.setGradientFill(juce::ColourGradient(sc.capTop, lever.getTopLeft(), sc.capBot,
                                               lever.getBottomRight(), false));
    }
    g.fillRoundedRectangle(lever, 12.0f);
    // inset 0 1px 1px sh-light. On the UNLIT cap this is the web's rim. On the LIT
    // lever it is not: --sh-light is rgba(255,255,255,.95) in light theme, and a
    // 95 %-white hairline across a saturated accent reads as a white LINE painted on
    // the orange (the owner's report), not as light catching a cap edge. The lit
    // lever takes a rim of its own accent, brightened — the same cue, in the right
    // material.
    g.setColour(lit ? accent_.brighter(0.55f).withAlpha(0.35f)
                    : sc.shLight.withMultipliedAlpha(0.7f));
    g.drawLine(lever.getX() + 6.0f, lever.getY() + 1.0f, lever.getRight() - 6.0f,
               lever.getY() + 1.0f, 1.0f);
    // caption (.toggle .k-name — ink-dim, mono small caps).
    g.setColour(sc.inkDim);
    g.setFont(skin::monoFont(9.5f));
    g.drawText(caption_.toUpperCase(), r.withTop(slot.getBottom() + 5.0f),
               juce::Justification::centredTop);
}

// ===========================================================================
// PowerControl (jewel + rocker) — the web .power/.jewel/.rocker recipe verbatim,
// LIGHT scheme. The jewel band reserves glowSpread() head-room on every side so
// the lit halo renders ROUND instead of clipped square at the component edge
// (the 2026-07-31 "draw order" report), and the rocker gets its full 46x64.
// ===========================================================================
namespace {
constexpr float kJewelD = 16.0f;       // .jewel 16px
constexpr float kRockerW = 46.0f;      // .rocker 46x64, radius 12
constexpr float kRockerH = 64.0f;
constexpr float kPowerCaptionH = 16.0f;
constexpr float kPowerGap = 9.0f;      // .power gap: 9px
// The rocker's own cast shadow is 5px 5px 12px: it reaches ~12 px past the body on
// the right and below, ~7 px on the left and above. Take the larger for both sides
// so the widget is symmetric and nothing is sliced at the bounds.
constexpr float kRockerShadowPad = 12.0f;
}  // namespace

PowerControl::PowerControl() {}

int PowerControl::preferredHeight() {
    return (int)std::ceil(skin::glowSpread(kJewelD) + kJewelD + kPowerGap + kRockerH +
                          4.0f + kPowerCaptionH);
}

int PowerControl::preferredWidth() {
    // The rocker + its shadow head-room. That also covers the jewel's halo, which
    // reaches jewelR + glowSpread = 8 + 12.8 = 20.8 px from the centre against the
    // (46 + 24)/2 = 35 px this gives it.
    return (int)std::ceil(kRockerW + 2.0f * kRockerShadowPad);
}

void PowerControl::mouseDown(const juce::MouseEvent&) {
    uisound::thunk(true);
    if (onClick) onClick();
}

void PowerControl::mouseUp(const juce::MouseEvent&) {
    uisound::thunk(false);
}

void PowerControl::paint(juce::Graphics& g) {
    const skin::Scheme& sc = skin::benchScheme();
    auto r = getLocalBounds().toFloat();
    // The jewel band: glow head-room ABOVE the jewel; the sides are covered by
    // preferredWidth() reserving the rocker's shadow pad, which is wider than the
    // halo's own reach.
    const float glowPad = skin::glowSpread(kJewelD);
    auto jewel = juce::Rectangle<float>(r.getCentreX() - kJewelD * 0.5f,
                                        r.getY() + glowPad, kJewelD, kJewelD);

    const float rockerY = jewel.getBottom() + kPowerGap;
    juce::Rectangle<float> rocker(r.getCentreX() - kRockerW * 0.5f, rockerY, kRockerW,
                                  juce::jmin(kRockerH, r.getBottom() - kPowerCaptionH -
                                                           4.0f - rockerY));
    juce::Path rp = roundedRectPath(rocker, 12.0f);
    // .rocker dual shadow: 5px 5px 12px sh-dark + -4 -4 10 sh-light.
    juce::DropShadow(sc.shDark, 9, {3, 3}).drawForPath(g, rp);
    juce::DropShadow(sc.shLight, 8, {-3, -3}).drawForPath(g, rp);
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(rp);
        skin::fillDiagGradient(g, rocker, sc.capEdgeTop, sc.capEdgeBot);
    }
    // Two rocker halves (::before top / ::after bottom): left/right 6, height 24,
    // radius 8. The RAISED half is --cap with an inset light-top/dark-bottom pair;
    // the PRESSED half is --well with an inset 3/3/6 darker. When on, the TOP is
    // pressed in.
    const float halfH = juce::jmin(24.0f, rocker.getHeight() * 0.5f - 8.0f);
    auto top = juce::Rectangle<float>(rocker.getX() + 6.0f, rocker.getY() + 6.0f,
                                      rocker.getWidth() - 12.0f, halfH);
    auto bot = juce::Rectangle<float>(rocker.getX() + 6.0f,
                                      rocker.getBottom() - 6.0f - halfH,
                                      rocker.getWidth() - 12.0f, halfH);
    auto drawHalf = [&](juce::Rectangle<float> h, bool raised) {
        if (raised) {
            g.setGradientFill(juce::ColourGradient(sc.capTop, h.getTopLeft(), sc.capBot,
                                                   h.getBottomRight(), false));
            g.fillRoundedRectangle(h, 8.0f);
            g.setColour(sc.shLight.withMultipliedAlpha(0.65f));  // inset 1px 2px 3px light
            g.drawLine(h.getX() + 5.0f, h.getY() + 1.2f, h.getRight() - 5.0f,
                       h.getY() + 1.2f, 1.2f);
            g.setColour(sc.shDarker);  // inset -1 -2 4 darker (bottom lip)
            g.drawLine(h.getX() + 5.0f, h.getBottom() - 1.0f, h.getRight() - 5.0f,
                       h.getBottom() - 1.0f, 1.0f);
        } else {
            g.setColour(sc.well);
            g.fillRoundedRectangle(h, 8.0f);
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(roundedRectPath(h, 8.0f));
            g.setColour(sc.shDarker);  // inset 3px 3px 6px darker
            g.strokePath(roundedRectPath(h.translated(1.4f, 1.6f), 8.0f),
                         juce::PathStrokeType(2.6f));
        }
    };
    drawHalf(top, !on_);
    drawHalf(bot, on_);

    g.setColour(sc.inkFaint);
    g.setFont(skin::monoFont(9.5f));
    g.drawText("POWER", r.withTop(r.getBottom() - kPowerCaptionH),
               juce::Justification::centredTop);

    // The jewel goes on LAST, over both rocker shadows — never under them.
    skin::drawJewel(g, jewel, accent_, on_, sc);
}

// ===========================================================================
// ModeSwitch — the web .mode-switch, rebuilt to its geometry (visual pass 3).
//
// The CSS: every .mode-opt is the same 78 px-wide box; `+ .mode-opt` pulls each one
// up by 1 px so the stack is flush with a 1 px OVERLAP; only nth-of-type(1)/(last)
// round their outer pair (12 px); the inactive ones are carved wells (inset 3/3/7
// darker + -3/-3/6 light) and the active one is RAISED and lit (accent gradient +
// 2px 3px 6px cast + inset 0 1px 1px light) at the SAME footprint.
//
// Two things made the old translation read as a wider, sharp-cornered red block:
// the inset strokes were FULL rounded rects rather than the segment's own
// corner-specific path (so every well drew a capsule outline inside itself, and the
// active middle segment — which has no rounded corners at all — was the only one
// without that inner capsule), and the stack ran edge to edge in the component, so
// the active segment's cast shadow was clipped on both sides and piled up as extra
// visual width. The stack now keeps kModeShadowPad of head-room, and the active
// segment is drawn LAST so its shadow falls on its neighbours instead of under them.
// ===========================================================================
namespace {
constexpr float kModeCaptionH = 16.0f;
constexpr float kModeShadowPad = 8.0f;  // head-room for the 2px 3px 6px active cast
constexpr float kModeSegW = 78.0f;      // .mode-switch --seg-w
constexpr float kModeRadius = 12.0f;
constexpr float kModeOverlap = 1.0f;    // .mode-opt + .mode-opt { margin-top: -1px }
}  // namespace

ModeSwitch::ModeSwitch() {}

int ModeSwitch::preferredWidth() {
    return (int)std::ceil(kModeSegW + 2.0f * kModeShadowPad);
}

void ModeSwitch::mouseDown(const juce::MouseEvent& e) {
    // Same geometry as paint(): the caption band is trimmed, the stack is inset.
    auto r = getLocalBounds().toFloat().withTrimmedBottom(kModeCaptionH);
    const float segH = r.getHeight() / (float)juce::jmax(1, labels_.size());
    const int idx = juce::jlimit(0, labels_.size() - 1,
                                 (int)std::floor(((float)e.y - r.getY()) /
                                                 juce::jmax(1.0f, segH)));
    uisound::thunk(true);
    if (onSelect) onSelect(idx);
}

void ModeSwitch::mouseUp(const juce::MouseEvent&) {
    uisound::thunk(false);
}

void ModeSwitch::paint(juce::Graphics& g) {
    const skin::Scheme& sc = skin::benchScheme();
    auto full = getLocalBounds().toFloat();
    auto r = full.withTrimmedBottom(kModeCaptionH).reduced(kModeShadowPad, 0.0f);
    const int n = juce::jmax(1, labels_.size());
    const float segH = r.getHeight() / (float)n;

    // One footprint for every segment, differing ONLY in which outer corners round.
    auto segBounds = [&](int i) {
        return juce::Rectangle<float>(r.getX(), r.getY() + segH * (float)i, r.getWidth(),
                                      segH + (i < n - 1 ? kModeOverlap : 0.0f));
    };
    auto segPath = [&](int i) {
        auto s = segBounds(i);
        juce::Path p;
        p.addRoundedRectangle(s.getX(), s.getY(), s.getWidth(), s.getHeight(), kModeRadius,
                              kModeRadius, i == 0, i == 0, i == n - 1, i == n - 1);
        return p;
    };
    auto caption = [&](int i, bool on) {
        g.setColour(on ? sc.ink : sc.inkDim);
        g.setFont(skin::monoFont(11.0f));
        g.drawText(labels_[i], segBounds(i).withHeight(segH), juce::Justification::centred);
    };

    // The carved neighbours first…
    for (int i = 0; i < n; ++i) {
        if (i == selected_) continue;
        const juce::Path sp = segPath(i);
        g.setColour(sc.well);
        g.fillPath(sp);
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(sp);
            // inset 3px 3px 7px sh-darker / inset -3px -3px 6px sh-light — as the
            // SEGMENT's own path, so an interior edge stays a straight seam.
            juce::Path in = sp;
            in.applyTransform(juce::AffineTransform::translation(1.6f, 1.8f));
            g.setColour(sc.shDarker);
            g.strokePath(in, juce::PathStrokeType(3.0f));
            juce::Path out = sp;
            out.applyTransform(juce::AffineTransform::translation(-1.4f, -1.4f));
            g.setColour(sc.shLight.withMultipliedAlpha(0.8f));
            g.strokePath(out, juce::PathStrokeType(2.2f));
        }
        caption(i, false);
    }

    // …then the ACTIVE one, last, so its cast shadow lands on them (it is the raised
    // element) — at exactly the same footprint.
    if (selected_ >= 0 && selected_ < n) {
        const juce::Path sp = segPath(selected_);
        auto seg = segBounds(selected_);
        juce::DropShadow(sc.shDark, 6, {2, 3}).drawForPath(g, sp);
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(sp);
            g.setGradientFill(juce::ColourGradient(
                accent_.interpolatedWith(skin::ground(), 0.45f), seg.getTopLeft(),
                accent_.interpolatedWith(juce::Colours::black, 0.18f), seg.getBottomRight(),
                false));
            g.fillPath(sp);
            // inset 0 1px 1px sh-light — as on the lever, a 95 %-white hairline on a
            // saturated accent reads as a painted line, so the lit segment gets its
            // own accent brightened instead.
            g.setColour(accent_.brighter(0.55f).withAlpha(0.32f));
            g.drawLine(seg.getX() + 8.0f, seg.getY() + 1.0f, seg.getRight() - 8.0f,
                       seg.getY() + 1.0f, 1.0f);
        }
        caption(selected_, true);
    }

    g.setColour(sc.inkDim);
    g.setFont(skin::monoFont(9.5f));
    g.drawText("MODE", full.withTop(r.getBottom() + 4.0f), juce::Justification::centredTop);
}

}  // namespace clipper::native
