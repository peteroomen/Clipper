#include "ClipperLookAndFeel.h"

// Only for showHostParameterMenu() below: the widget-kit HEADER stays GUI-only, so the
// one place that needs a juce::AudioProcessorParameter includes the module here.
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstring>
#include <memory>
#include <vector>

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

// ---------------------------------------------------------------------------
// The shadow cache (docs §40). See the header for why this exists.
// ---------------------------------------------------------------------------
#if CLIPPER_PAINT_METRICS
namespace metrics {
long long blurPasses = 0;
long long shadowImages = 0;
long long shadowCacheHits = 0;
long long valueTreeWrites = 0;
bool useShadowCache = true;
bool useNarrowRepaints = true;
void reset() {
    blurPasses = shadowImages = shadowCacheHits = valueTreeWrites = 0;
}
}  // namespace metrics
#define CLIPPER_COUNT(field) (++clipper::native::skin::metrics::field)
#else
#define CLIPPER_COUNT(field) ((void)0)
#endif

namespace {

// Sizes are quantized to whole pixels and the corner radius to quarter-pixels: two
// knobs whose bodies differ by a third of a pixel share one blurred image, which is
// invisible under the blur and is what makes the cache hit at all.
struct ShadowKey {
    int w = 0, h = 0, corner4 = 0, shape = 0, count = 0;
    juce::int64 argb[kMaxShadowSpecs] = {};
    int radius[kMaxShadowSpecs] = {};
    int ox[kMaxShadowSpecs] = {};
    int oy[kMaxShadowSpecs] = {};

    bool operator==(const ShadowKey& o) const {
        if (w != o.w || h != o.h || corner4 != o.corner4 || shape != o.shape ||
            count != o.count)
            return false;
        for (int i = 0; i < count; ++i)
            if (argb[i] != o.argb[i] || radius[i] != o.radius[i] || ox[i] != o.ox[i] ||
                oy[i] != o.oy[i])
                return false;
        return true;
    }
};

struct ShadowEntry {
    ShadowKey key;
    juce::Image image;
    int pad = 0;
    long long lastUsed = 0;
};

// Bounded: a window resize sweeps a new size per pixel of drag, so an unbounded cache
// would be a slow leak. 64 entries covers every card, knob, chip and lever on screen
// at one window size with room to spare; the least-recently-used one is evicted.
constexpr size_t kShadowCacheMax = 64;

std::vector<ShadowEntry>& shadowCache() {
    static std::vector<ShadowEntry> cache;
    return cache;
}

juce::Path shadowPath(ShadowShape shape, juce::Rectangle<float> r, float corner) {
    juce::Path p;
    if (shape == ShadowShape::Ellipse)
        p.addEllipse(r);
    else
        p.addRoundedRectangle(r, corner);
    return p;
}
}  // namespace

void drawShadows(juce::Graphics& g, juce::Rectangle<float> bounds, float corner,
                 ShadowShape shape, const ShadowSpec* specs, int count) {
    count = juce::jlimit(0, kMaxShadowSpecs, count);
    if (count == 0 || specs == nullptr) return;
    const int w = juce::roundToInt(bounds.getWidth());
    const int h = juce::roundToInt(bounds.getHeight());
    if (w < 1 || h < 1) return;

    // The margin the blurs need around the body, so nothing is cropped.
    int pad = 1;
    for (int i = 0; i < count; ++i)
        pad = juce::jmax(pad, specs[i].radius + 2 +
                                  juce::jmax(std::abs(specs[i].offset.x),
                                             std::abs(specs[i].offset.y)));

#if CLIPPER_PAINT_METRICS
    // The pre-fix path, kept ONLY in the bench build so the "before" row is measured
    // from the real code rather than remembered: blur straight onto `g`, every paint.
    if (!metrics::useShadowCache) {
        const juce::Path p = shadowPath(shape, bounds, corner);
        for (int i = 0; i < count; ++i) {
            juce::DropShadow(specs[i].colour, specs[i].radius, specs[i].offset)
                .drawForPath(g, p);
            CLIPPER_COUNT(blurPasses);
        }
        return;
    }
#endif

    ShadowKey key;
    key.w = w;
    key.h = h;
    key.corner4 = juce::roundToInt(corner * 4.0f);
    key.shape = (int)shape;
    key.count = count;
    for (int i = 0; i < count; ++i) {
        key.argb[i] = (juce::int64)specs[i].colour.getARGB();
        key.radius[i] = specs[i].radius;
        key.ox[i] = specs[i].offset.x;
        key.oy[i] = specs[i].offset.y;
    }

    static long long clock = 0;
    ++clock;

    auto& cache = shadowCache();
    ShadowEntry* hit = nullptr;
    for (auto& e : cache)
        if (e.key == key) {
            hit = &e;
            break;
        }

    if (hit == nullptr) {
        if (cache.size() >= kShadowCacheMax) {
            size_t oldest = 0;
            for (size_t i = 1; i < cache.size(); ++i)
                if (cache[i].lastUsed < cache[oldest].lastUsed) oldest = i;
            cache.erase(cache.begin() + (std::ptrdiff_t)oldest);
        }
        ShadowEntry e;
        e.key = key;
        e.pad = pad;
        e.image = juce::Image(juce::Image::ARGB, w + 2 * pad, h + 2 * pad, true);
        CLIPPER_COUNT(shadowImages);
        {
            juce::Graphics ig(e.image);
            const juce::Path p = shadowPath(
                shape, juce::Rectangle<float>((float)pad, (float)pad, (float)w, (float)h),
                corner);
            for (int i = 0; i < count; ++i) {
                juce::DropShadow(specs[i].colour, specs[i].radius, specs[i].offset)
                    .drawForPath(ig, p);
                CLIPPER_COUNT(blurPasses);
            }
        }
        cache.push_back(std::move(e));
        hit = &cache.back();
    } else {
        CLIPPER_COUNT(shadowCacheHits);
    }

    hit->lastUsed = clock;
    g.drawImageAt(hit->image, juce::roundToInt(bounds.getX()) - hit->pad,
                  juce::roundToInt(bounds.getY()) - hit->pad);
}

void drawShadow(juce::Graphics& g, juce::Rectangle<float> bounds, float corner,
                ShadowShape shape, ShadowSpec spec) {
    drawShadows(g, bounds, corner, shape, &spec, 1);
}

void drawChassisCard(juce::Graphics& g, juce::Rectangle<float> r, float radius) {
    auto path = roundedRectPath(r, radius);

    // Two warm cast shadows on the bench (the .pedal.raised dual box-shadow):
    // 16px 18px 34px @ .30, then a tighter 3px 4px 10px @ .22.
    const ShadowSpec cast[] = {{castShadow, 22, {11, 13}},
                               {castShadow.withAlpha(0.22f), 8, {3, 4}}};
    drawShadows(g, r, radius, ShadowShape::RoundedRect, cast, 2);

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
    auto channel = roundedRectPath(r, radius);
    g.setColour(benchWell);
    g.fillPath(channel);
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(channel);
        g.setColour(juce::Colour(0x40000000));
        g.strokePath(roundedRectPath(r.translated(2.5f, 3.0f), radius),
                     juce::PathStrokeType(7.0f));
        g.setColour(juce::Colour(0x66FFFFFF));
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

    // --- body cast shadow (6px 6px 14 dark / -5 -5 12 light dual) ---
    skin::drawShadow(g,
                     juce::Rectangle<float>(centre.x - bodyR, centre.y - bodyR,
                                            bodyR * 2.0f, bodyR * 2.0f),
                     0.0f, skin::ShadowShape::Ellipse,
                     {skin::shDark, (int)(bodyR * 0.55f), {3, 4}});
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
    skin::drawShadow(g, r, radius, skin::ShadowShape::RoundedRect,
                     {skin::shDark, 8, {3, 3}});
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
    g.setColour(skin::benchWell.darker(0.06f));
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
    g.setColour(skin::benchInkDim.withAlpha(alpha));
    g.fillRoundedRectangle(thumb, radius);
    g.setColour(juce::Colour(0x30FFFFFF));
    g.drawRoundedRectangle(thumb.reduced(0.5f), radius, 1.0f);
}

// ===========================================================================
// The host's parameter context menu (docs §40)
// ===========================================================================
void showHostParameterMenu(juce::Component& anchor, juce::AudioProcessorParameter* param) {
    if (param == nullptr) return;
    auto* editor = anchor.findParentComponentOfClass<juce::AudioProcessorEditor>();
    if (editor == nullptr) return;
    auto* host = editor->getHostContext();
    if (host == nullptr) return;  // Standalone, or a host that offers no menu
    auto provided = host->getContextMenuForParameter(param);
    if (provided == nullptr) return;
    juce::PopupMenu menu = provided->getEquivalentPopupMenu();
    if (menu.getNumItems() == 0) return;
    menu.setLookAndFeel(&anchor.getLookAndFeel());
    // The host object owns the item actions, so it has to outlive the async menu.
    auto keepAlive =
        std::make_shared<std::unique_ptr<juce::HostProvidedContextMenu>>(std::move(provided));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&anchor),
                       [keepAlive](int) {});
}

// ===========================================================================
// BenchButton / BenchSlider — the shared input contract (docs §40, ADR 012)
// ===========================================================================
namespace {
// A press only counts if it is the PRIMARY button. `isPopupMenu()` also covers macOS
// ctrl-click, which is the same host gesture as a right-click.
bool isPrimaryPress(const juce::MouseEvent& e) {
    return e.mods.isLeftButtonDown() && !e.mods.isPopupMenu();
}
}  // namespace

void BenchButton::mouseDown(const juce::MouseEvent& e) {
    if (!isPrimaryPress(e)) return;  // never enters the down state at all
    primaryHeld_ = true;
    juce::Button::mouseDown(e);
}

void BenchButton::mouseDrag(const juce::MouseEvent& e) {
    if (!primaryHeld_) return;
    juce::Button::mouseDrag(e);  // leaving the bounds clears the down state → aborts
}

void BenchButton::mouseUp(const juce::MouseEvent& e) {
    if (!primaryHeld_) {
        // The gesture we deliberately did not consume. Hand it to the host instead of
        // toggling the control behind the player's back.
        if (e.mods.isPopupMenu() && onSecondaryClick) onSecondaryClick(*this);
        return;
    }
    primaryHeld_ = false;
    juce::Button::mouseUp(e);  // clicks only if the pointer is still inside
}

void BenchButton::abortPress(const juce::MouseEvent& e) {
    primaryHeld_ = false;
    juce::Button::mouseExit(e);  // drops over/down state; fires no click
}

void BenchButton::paintFocusRing(juce::Graphics& g, juce::Rectangle<float> r, float corner,
                                 juce::Colour accent) const {
    if (!hasKeyboardFocus(false)) return;
    g.setColour(accent.withAlpha(0.85f));
    if (corner <= 0.0f)
        g.drawEllipse(r.reduced(1.0f), 2.0f);
    else
        g.drawRoundedRectangle(r.reduced(1.0f), corner, 2.0f);
}

BenchSlider::BenchSlider() {
    // JUCE's own right-click menu (rotary/velocity mode) is not wanted here, and with
    // it disabled Slider::mouseDown falls straight through to a DRAG on any button —
    // so a right-drag turned the knob. Guarded below instead.
    setPopupMenuEnabled(false);
}

void BenchSlider::mouseDown(const juce::MouseEvent& e) {
    if (!isPrimaryPress(e)) return;
    primaryHeld_ = true;
    juce::Slider::mouseDown(e);
}

void BenchSlider::mouseDrag(const juce::MouseEvent& e) {
    if (!primaryHeld_) return;
    juce::Slider::mouseDrag(e);
}

void BenchSlider::mouseUp(const juce::MouseEvent& e) {
    if (!primaryHeld_) {
        if (e.mods.isPopupMenu() && onSecondaryClick) onSecondaryClick(*this);
        return;
    }
    primaryHeld_ = false;
    juce::Slider::mouseUp(e);
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
    // What assistive tech READS OUT for the value has to be the number the player can
    // SEE, which is the web's round(value*100), not JUCE's 0.00–1.00.
    slider_.textFromValueFunction = [](double v) {
        return juce::String(juce::roundToInt(v * 100.0));
    };
    addAndMakeVisible(slider_);

    // The two captions are decoration on top of the slider; the slider carries the
    // name and the value for the accessibility tree (see setKnobName), so exposing the
    // labels a second time would just make every knob read out twice.
    nameLabel_.setAccessible(false);
    valueLabel_.setAccessible(false);

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

void NeuKnob::setKnobName(const juce::String& n) {
    nameLabel_.setText(n.toUpperCase(), juce::dontSendNotification);
    // All of these, deliberately. The QUALIFIED base call is what the old override
    // omitted; setTitle on the SLIDER is what assistive tech actually reads, because
    // the slider is the child that takes focus.
    juce::Component::setName(n);
    setTitle(n);
    slider_.setTitle(n);
}

void NeuKnob::setName(const juce::String& n) {
    setKnobName(n);  // a caller holding a Component* must land in the same place
}

void NeuKnob::setOnSecondaryClick(std::function<void(juce::Component&)> fn) {
    slider_.onSecondaryClick = std::move(fn);
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
Footswitch::Footswitch() : BenchButton("Footswitch") {
    setButtonText("Stomp");  // the spoken name until the card sets a real one
}

void Footswitch::clicked() {
    // The thunk: the web's transient `.stomped`, ~130 ms after the switch actually
    // fires — which is now on RELEASE, not on press.
    thunk_ = true;
    repaint();
    startTimer(130);
}

void Footswitch::timerCallback() {
    stopTimer();
    thunk_ = false;
    repaint();
}

void Footswitch::paintButton(juce::Graphics& g, bool /*highlighted*/, bool down) {
    auto r = getLocalBounds().toFloat();
    const bool pressed = down || thunk_;
    // `.fsw:active { transform: translateY(2px) }` — the whole switch drops.
    if (pressed) r = r.translated(0.0f, 2.0f);
    switch (shape_) {
        case Shape::Treadle: paintTreadle(g, r, pressed); break;
        case Shape::Pad:     paintPad(g, r, pressed); break;
        default:             paintRound(g, r, pressed); break;
    }
}

void Footswitch::paintRound(juce::Graphics& g, juce::Rectangle<float> r, bool pressed) {
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

    // 8px 8px 18px --sh-dark (raised) collapsing to 3/3/8 while pressed.
    skin::drawShadow(g, stomp, 0.0f, skin::ShadowShape::Ellipse,
                     {skin::shDark, pressed ? 8 : 16,
                      pressed ? juce::Point<int>{3, 3} : juce::Point<int>{5, 6}});
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
    if (pressed) {  // inset 2px 2px 6px --sh-darker
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
    paintFocusRing(g, stomp.expanded(3.0f), 0.0f, accent_);
}

void Footswitch::paintTreadle(juce::Graphics& g, juce::Rectangle<float> r, bool pressed) {
    // .fsw-treadle: a wide BLACK RUBBER pad owning the LOWER body — the web drops
    // it to the bottom of the enclosure with nothing beneath it, and that placement
    // is half the Boss-compact read. Explicitly darker/mattes than the metal knobs.
    const float h = juce::jmin(r.getHeight(), 150.0f);
    auto body = r.withTop(r.getBottom() - h);
    skin::drawShadow(g, body, 18.0f, skin::ShadowShape::RoundedRect,
                     {skin::shDark, pressed ? 10 : 20,
                      pressed ? juce::Point<int>{3, 4} : juce::Point<int>{7, 9}});
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
    paintFocusRing(g, body.expanded(2.0f), 18.0f, accent_);
}

void Footswitch::paintPad(juce::Graphics& g, juce::Rectangle<float> r, bool pressed) {
    // .fsw-pad: the Ibanez-format hinged METAL plate — wide, low, brushed, and (like
    // the treadle) owning the bottom of the enclosure with nothing below it.
    const float h = juce::jmin(r.getHeight(), 96.0f);
    auto body = r.withTop(r.getBottom() - h);
    skin::drawShadow(g, body, 12.0f, skin::ShadowShape::RoundedRect,
                     {skin::shDark, pressed ? 10 : 20,
                      pressed ? juce::Point<int>{3, 4} : juce::Point<int>{7, 9}});
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
    paintFocusRing(g, body.expanded(2.0f), 12.0f, accent_);
}

// ===========================================================================
// ChipButton (the .rack-btn / .tray-add pill)
// ===========================================================================
ChipButton::ChipButton(const juce::String& text) : BenchButton("Chip"), text_(text) {}

void ChipButton::mouseDrag(const juce::MouseEvent& e) {
    BenchButton::mouseDrag(e);
    if (!primaryHeld() || !isEnabled() || !onDrag) return;
    // Only start dragging past a small threshold, so a click never reorders.
    if (!dragged_ && e.getDistanceFromDragStart() < 4) return;
    dragged_ = true;
    onDrag(getX() + e.x);
}

void ChipButton::mouseUp(const juce::MouseEvent& e) {
    if (dragged_) {
        // The gesture became a DRAG, so it must not also click: a grip released back
        // inside its own 24x20 chip would otherwise fire onClick as well.
        dragged_ = false;
        abortPress(e);
        repaint();
        if (onDragEnd) onDragEnd();
        return;
    }
    BenchButton::mouseUp(e);
}

void ChipButton::paintButton(juce::Graphics& g, bool /*highlighted*/, bool down) {
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    const float radius = juce::jmin(7.0f, r.getHeight() * 0.35f);
    if (down) {
        // :active — the raised pill inverts to a recess.
        g.setColour(skin::well);
        g.fillRoundedRectangle(r, radius);
        g.setColour(skin::shDarker);
        g.drawRoundedRectangle(r.reduced(0.5f).translated(0.6f, 0.7f), radius, 1.6f);
    } else {
        skin::drawShadow(g, r, radius, skin::ShadowShape::RoundedRect,
                         {skin::shDark, 5, {2, 2}});
        skin::fillDiagGradient(g, r, skin::capEdgeTop, skin::capEdgeBot);
        g.setColour(juce::Colour(0x12FFFFFF));
        g.strokePath(roundedRectPath(r.reduced(0.5f), radius), juce::PathStrokeType(1.0f));
    }
    g.setColour(isEnabled() ? tint_ : tint_.withAlpha(0.35f));
    g.setFont(skin::monoFont(juce::jmin(12.0f, r.getHeight() * 0.62f)));
    g.drawText(text_, r, juce::Justification::centred);
    paintFocusRing(g, r, radius, tint_);
}

// ===========================================================================
// LeverToggle (carved slot + sliding cap lever)
// ===========================================================================
LeverToggle::LeverToggle() : BenchButton("Lever") {
    // Checkable but NOT self-toggling: the APVTS parameter owns the state and writes
    // it back through the editor's ParameterAttachment.
    setToggleable(true);
    setClickingTogglesState(false);
    setButtonText("Bright");
}

void LeverToggle::setCaption(const juce::String& c) {
    caption_ = c;
    setButtonText(c);  // the spoken name
    repaint();
}

void LeverToggle::paintButton(juce::Graphics& g, bool /*highlighted*/, bool down) {
    const bool on = getToggleState();
    auto r = getLocalBounds().toFloat();
    const float slotW = 30.0f, slotH = 54.0f;
    juce::Rectangle<float> slot(r.getCentreX() - slotW * 0.5f, r.getY(), slotW, slotH);
    skin::drawWell(g, slot, 15.0f);
    // lever cap (top when off, bottom when on; lit accent when on).
    auto lever = juce::Rectangle<float>(slot.getX() + 4.0f, slot.getY() + (on ? 26.0f : 4.0f),
                                        slotW - 8.0f, 24.0f);
    if (on) {
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
    // While held, the lever reads as being pushed — the press feedback a control that
    // now fires on RELEASE needs in order not to feel unresponsive.
    if (down) {
        g.setColour(skin::shDarker.withAlpha(0.55f));
        g.fillRoundedRectangle(lever, 12.0f);
    }
    // caption.
    g.setColour(skin::inkDim);
    g.setFont(skin::monoFont(9.5f));
    g.drawText(caption_.toUpperCase(), r.withTop(slot.getBottom() + 3.0f),
               juce::Justification::centred);
    paintFocusRing(g, slot.expanded(2.0f), 15.0f, accent_);
}

// ===========================================================================
// PowerControl (jewel + rocker)
// ===========================================================================
PowerControl::PowerControl() : BenchButton("Power") {
    setToggleable(true);
    setClickingTogglesState(false);
    setToggleState(true, juce::dontSendNotification);  // an amp opens powered up
    setButtonText("Power");
}

void PowerControl::paintButton(juce::Graphics& g, bool /*highlighted*/, bool down) {
    const bool on = getToggleState();
    auto r = getLocalBounds().toFloat();
    const float jewelD = 15.0f;
    auto jewel = juce::Rectangle<float>(r.getCentreX() - jewelD * 0.5f, r.getY(), jewelD,
                                        jewelD);
    // The jewel is drawn LAST (see below): the rocker's cast shadow reaches back over
    // this band and used to paint out the lit halo — the "lights are covered over"
    // bug. The rocker also gets a wider gap so the shadow starts clear of the glow.
    auto rockArea = r.withTrimmedTop(jewelD + 14.0f).withTrimmedBottom(16.0f);
    float rw = 44.0f, rh = juce::jmin(60.0f, rockArea.getHeight());
    juce::Rectangle<float> rocker(rockArea.getCentreX() - rw * 0.5f, rockArea.getY(), rw, rh);
    skin::drawShadow(g, rocker, 11.0f, skin::ShadowShape::RoundedRect,
                     {skin::shDark, 8, {3, 3}});
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
    drawHalf(top, !on);  // when on, the TOP is pressed in (down)
    drawHalf(bot, on);
    // Held: the half that WOULD move darkens. Power is the one control where the
    // difference between "I am pressing this" and "this has fired" matters most — it
    // now fires on release, and dragging off it aborts.
    if (down) {
        g.setColour(skin::shDarker.withAlpha(0.5f));
        g.fillRoundedRectangle(on ? bot : top, 7.0f);
    }
    g.setColour(skin::inkFaint);
    g.setFont(skin::monoFont(9.5f));
    g.drawText("POWER", r.withTop(r.getBottom() - 14.0f), juce::Justification::centred);

    // The jewel goes on LAST, over the rocker's cast shadow — never under it.
    skin::drawJewel(g, jewel, accent_, on);
    paintFocusRing(g, rocker.expanded(3.0f), 11.0f, accent_);
}

// ===========================================================================
// ModeSwitch (3 stacked carved segments, each its own radio button)
// ===========================================================================
class ModeSwitch::Segment : public BenchButton {
public:
    Segment(const juce::String& label, juce::Colour accent)
        : BenchButton(label), accent_(accent) {
        setButtonText(label);
        setToggleable(true);
        setClickingTogglesState(false);  // the parameter owns the selection
        setRadioGroupId(1);              // -> AccessibilityRole::radioButton
    }

    void paintButton(juce::Graphics& g, bool /*highlighted*/, bool down) override {
        auto seg = getLocalBounds().toFloat();
        if (getToggleState()) {
            g.setGradientFill(juce::ColourGradient(
                accent_.interpolatedWith(skin::ground, 0.45f), seg.getTopLeft(),
                accent_.interpolatedWith(juce::Colours::black, 0.18f), seg.getBottomRight(),
                false));
            g.fillRoundedRectangle(seg, 7.0f);
            g.setColour(skin::ink);
        } else {
            if (down) {
                g.setColour(skin::shDarker.withAlpha(0.45f));
                g.fillRoundedRectangle(seg, 7.0f);
            }
            g.setColour(skin::inkDim);
        }
        g.setFont(skin::monoFont(11.0f));
        g.drawText(getButtonText(), seg, juce::Justification::centred);
        paintFocusRing(g, seg, 7.0f, accent_);
    }

private:
    juce::Colour accent_;
};

ModeSwitch::ModeSwitch() {
    setTitle("Mode");
    for (const auto& label : labels_) {
        auto* seg = segments_.add(new Segment(label, accent_));
        addAndMakeVisible(seg);
        const int idx = segments_.size() - 1;
        seg->onClick = [this, idx] {
            if (onSelect) onSelect(idx);
        };
    }
    setSelected(selected_);
}

ModeSwitch::~ModeSwitch() = default;

void ModeSwitch::setSelected(int idx) {
    selected_ = juce::jlimit(0, juce::jmax(0, segments_.size() - 1), idx);
    for (int i = 0; i < segments_.size(); ++i)
        segments_[i]->setToggleState(i == selected_, juce::dontSendNotification);
    repaint();
}

void ModeSwitch::setOnSecondaryClick(std::function<void(juce::Component&)> fn) {
    for (auto* seg : segments_) seg->onSecondaryClick = fn;
}

void ModeSwitch::resized() {
    auto r = getLocalBounds().withTrimmedBottom(16);
    const int n = juce::jmax(1, segments_.size());
    const float segH = (float)r.getHeight() / (float)n;
    for (int i = 0; i < segments_.size(); ++i)
        segments_[i]->setBounds(juce::Rectangle<float>((float)r.getX(),
                                                       (float)r.getY() + segH * (float)i,
                                                       (float)r.getWidth(), segH)
                                    .reduced(3.0f, 2.0f)
                                    .getSmallestIntegerContainer());
}

void ModeSwitch::paint(juce::Graphics& g) {
    // The container paints only the carved well behind the segments and the caption
    // below them; each segment paints itself (children paint after their parent).
    auto full = getLocalBounds().toFloat();
    auto r = full.withTrimmedBottom(16.0f);
    skin::drawWell(g, r, 12.0f);
    g.setColour(skin::inkDim);
    g.setFont(skin::monoFont(9.5f));
    g.drawText("MODE", full.withTop(r.getBottom() + 2.0f), juce::Justification::centred);
}

}  // namespace clipper::native
