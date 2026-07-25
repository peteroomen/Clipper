// Clipper native shell — the editor PAINT + INPUT + LAYOUT bench (dev-only, NOT shipped).
//
// Guarded behind the CMake option CLIPPER_BUILD_PAINT_BENCH (default OFF), which is
// also what compiles the CLIPPER_PAINT_METRICS counters into the editor at all — so a
// release plugin/standalone/VST3 build carries none of this.
//
// It exists because the audit's UI findings are all quantitative and were, until this
// slice, estimates ("on the order of a thousand offscreen image allocations and blurs
// per second"). Everything below runs the REAL editor against a REAL desktop peer, so
// JUCE's own dirty-region plumbing decides what repaints — a synthetic
// paintEntireComponent would measure the opposite of the thing being fixed.
//
// Run headless under xvfb:
//   xvfb-run -a ./clipper_paint_bench
//
// Sections:
//   A  SUSTAINED EDGE-DRAG SCROLL — the drag-to-edge auto-scroll pump's 42 Hz, with the
//      shadow cache and the narrowed repaints switched off (the audit's "before") and on
//      (this slice's "after"). Reports DropShadow blur passes and offscreen shadow-image
//      allocations per second.
//   B  DRAG-REORDER — one grip drag sweeping a six-pedal board end to end, crossing
//      every card. Reports ValueTree writes and blur passes for the whole gesture.
//   C  INPUT SAFETY — a press that is never released, a press dragged off the control,
//      and a right-click, on Power / Bright / Cab / a footswitch / a mode segment: none
//      may move the bound parameter. A plain click must, so the test can fail.
//   D  ACCESSIBLE NAMES — every knob, button and radio segment must carry the text its
//      accessibility handler will report, and the toggles must be checkable.
//   E  LAYOUT AUDIT + MINIMUM WIDTH — sweeps the editor width down and reports the
//      smallest one at which no control escapes its chassis and no two overlap.
//
// Exit code 0 iff C, D and E all pass. A and B only measure.

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "PedalCard.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

using namespace clipper::native;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void pump(int ms) {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

double nowSeconds() {
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

void setParam(juce::AudioProcessorValueTreeState& apvts, const char* id, float denorm) {
    if (auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(denorm));
}

float paramValue(juce::AudioProcessorValueTreeState& apvts, const char* id) {
    auto* p = apvts.getParameter(id);
    return p != nullptr ? p->getValue() : -1.0f;
}

// ---------------------------------------------------------------------------
// Component-tree walking. The editor's members are private, so the bench finds its
// widgets the way an accessibility client would: by walking and type-testing.
// ---------------------------------------------------------------------------
template <typename T>
void collect(juce::Component& root, std::vector<T*>& out) {
    for (int i = 0; i < root.getNumChildComponents(); ++i) {
        auto* c = root.getChildComponent(i);
        if (c == nullptr) continue;
        if (auto* t = dynamic_cast<T*>(c)) out.push_back(t);
        collect(*c, out);
    }
}

template <typename T>
std::vector<T*> findAll(juce::Component& root) {
    std::vector<T*> out;
    collect(root, out);
    return out;
}

// ---------------------------------------------------------------------------
// Section A — the sustained edge-drag scroll
// ---------------------------------------------------------------------------
struct ScrollRun {
    double seconds = 0.0;
    long long blurs = 0, images = 0, hits = 0;
    int ticks = 0;
};

// The drag-to-edge pump ticks every kAutoScrollMs (24 ms ≈ 42 Hz) and each tick calls
// setViewPosition, which is exactly what the shipped auto-scroll does. Driving it this
// way rather than through a held pointer is what makes it SUSTAINED: a real held drag
// stops as soon as the board hits its end.
ScrollRun measureScroll(ClipperAudioProcessorEditor& ed, bool cache, bool narrow,
                        int ticks) {
    skin::metrics::useShadowCache = cache;
    skin::metrics::useNarrowRepaints = narrow;

    // Warm up first, so a cold cache is not charged to the steady-state rate (and so the
    // "before" row is not charged for the first paint of every surface either).
    for (int i = 0; i < 12; ++i) {
        ed.setBoardScroll((i % 2) == 0 ? 0.15 : 0.85);
        pump(24);
    }

    skin::metrics::reset();
    ScrollRun r;
    const double t0 = nowSeconds();
    for (int i = 0; i < ticks; ++i) {
        // Oscillate across the overflow at the pump's step, so it never parks against an
        // end and stops being a scroll.
        const double phase = (double)(i % 40) / 40.0;
        ed.setBoardScroll(phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0);
        pump(24);
    }
    r.seconds = nowSeconds() - t0;
    r.ticks = ticks;
    r.blurs = skin::metrics::blurPasses;
    r.images = skin::metrics::shadowImages;
    r.hits = skin::metrics::shadowCacheHits;
    skin::metrics::useShadowCache = true;
    skin::metrics::useNarrowRepaints = true;
    return r;
}

void reportScroll(const char* label, const ScrollRun& r) {
    const double perSec = r.seconds > 0.0 ? (double)r.blurs / r.seconds : 0.0;
    const double imgPerSec = r.seconds > 0.0 ? (double)r.images / r.seconds : 0.0;
    std::printf("  %-34s %7lld blurs  %7lld imgs  %8lld hits   %8.1f blurs/s  %7.1f imgs/s"
                "   (%d ticks in %.2f s)\n",
                label, r.blurs, r.images, r.hits, perSec, imgPerSec, r.ticks, r.seconds);
}

// ---------------------------------------------------------------------------
// Section B — one grip drag across the whole board
// ---------------------------------------------------------------------------
struct DragRun {
    long long writes = 0, blurs = 0, images = 0;
    int steps = 0;
};

DragRun measureDrag(ClipperAudioProcessorEditor& ed, juce::Component& content, bool cache,
                    bool narrow) {
    skin::metrics::useShadowCache = cache;
    skin::metrics::useNarrowRepaints = narrow;
    ed.setBoardScroll(0.0);
    pump(40);
    skin::metrics::reset();

    DragRun r;
    // A mouse delivers roughly 60–125 mouseDrag events a second, i.e. a step of a few
    // px at a normal drag speed. 8 px per step across the content is that.
    const int span = content.getWidth();
    for (int x = 20; x < span - 20; x += 8) {
        ed.dragCardTo(0, x);
        pump(2);
        ++r.steps;
    }
    ed.endCardDrag();
    pump(40);
    r.writes = skin::metrics::valueTreeWrites;
    r.blurs = skin::metrics::blurPasses;
    r.images = skin::metrics::shadowImages;
    skin::metrics::useShadowCache = true;
    skin::metrics::useNarrowRepaints = true;
    return r;
}

// ---------------------------------------------------------------------------
// Section C — input safety
// ---------------------------------------------------------------------------
juce::MouseEvent makeEvent(juce::Component& c, juce::Point<float> local,
                           juce::ModifierKeys mods) {
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), local, mods,
                            juce::MouseInputSource::defaultPressure,
                            juce::MouseInputSource::defaultOrientation,
                            juce::MouseInputSource::defaultRotation,
                            juce::MouseInputSource::defaultTiltX,
                            juce::MouseInputSource::defaultTiltY, &c, &c,
                            juce::Time::getCurrentTime(), local,
                            juce::Time::getCurrentTime(), 1, false);
}

// Warp the REAL pointer, because juce::Button decides "is the pointer still over me?"
// from Component::isMouseOver() for a mouse source — NOT from the event position. A
// synthetic drag alone therefore cannot express "the pointer left the control", and a
// test built on synthetic positions would pass whatever the widget did. Returns whether
// the warp actually took effect, so a platform where it does not is reported rather
// than silently turning this section into a no-op.
bool pointerOver(juce::Component& c) {
    const auto centre = c.localPointToGlobal(c.getLocalBounds().getCentre());
    juce::Desktop::setMousePosition(centre);
    pump(60);
    return c.isMouseOver(true);
}

bool pointerAwayFrom(juce::Component& c, juce::Component& editor) {
    juce::Desktop::setMousePosition(
        editor.localPointToGlobal(juce::Point<int>(editor.getWidth() - 3, 3)));
    pump(60);
    return !c.isMouseOver(true);
}

const juce::ModifierKeys kLeft{juce::ModifierKeys::leftButtonModifier};
const juce::ModifierKeys kRight{juce::ModifierKeys::rightButtonModifier};

struct InputCase {
    const char* label;
    juce::Component* widget;
    const char* paramId;
};

void runInputCase(ClipperAudioProcessorEditor& ed, juce::AudioProcessorValueTreeState& apvts,
                  const InputCase& c, bool warpWorks) {
    if (c.widget == nullptr) {
        check(false, "widget was found in the editor tree");
        return;
    }
    auto& w = *c.widget;
    const juce::Point<float> inside = w.getLocalBounds().getCentre().toFloat();

    // 1. A PRESS ALONE must not fire. This is the whole audit finding, and it is
    //    position-independent, so it is exact even headless.
    const float before = paramValue(apvts, c.paramId);
    if (warpWorks) pointerOver(w);
    w.mouseDown(makeEvent(w, inside, kLeft));
    pump(10);
    const float afterPress = paramValue(apvts, c.paramId);
    std::printf("    %s: press-only %.3f -> %.3f\n", c.label, before, afterPress);
    check(std::abs(afterPress - before) < 1.0e-6f,
          "a press alone does not move the parameter (it fires on release)");

    // 2. Now DRAG OFF and release. With the real pointer moved away, JUCE's own
    //    wasOver bookkeeping is genuinely false, so this is the real abort path.
    bool away = true;
    if (warpWorks) away = pointerAwayFrom(w, ed);
    const juce::Point<float> outside{-400.0f, -400.0f};
    w.mouseDrag(makeEvent(w, outside, kLeft));
    w.mouseUp(makeEvent(w, outside, kLeft));
    pump(10);
    const float afterAbort = paramValue(apvts, c.paramId);
    std::printf("    %s: press+drag-off+release %.3f -> %.3f  (pointer really off: %s)\n",
                c.label, before, afterAbort, away ? "yes" : "NO");
    check(std::abs(afterAbort - before) < 1.0e-6f,
          "a press dragged off the control aborts (no toggle)");

    // 3. A RIGHT-CLICK must not fire — this is the gesture that opens the host's
    //    automation menu, and it used to toggle the control.
    if (warpWorks) pointerOver(w);
    w.mouseDown(makeEvent(w, inside, kRight));
    pump(10);
    w.mouseUp(makeEvent(w, inside, kRight));
    pump(10);
    const float afterRight = paramValue(apvts, c.paramId);
    std::printf("    %s: right-click %.3f -> %.3f\n", c.label, before, afterRight);
    check(std::abs(afterRight - before) < 1.0e-6f, "a right-click does not toggle it");

    // 4. SENSITIVITY: a plain left click, released inside, MUST move the parameter.
    //    Without this the three checks above could be passing on a dead control.
    if (warpWorks) pointerOver(w);
    w.mouseDown(makeEvent(w, inside, kLeft));
    pump(10);
    w.mouseUp(makeEvent(w, inside, kLeft));
    pump(20);
    const float afterClick = paramValue(apvts, c.paramId);
    std::printf("    %s: left click %.3f -> %.3f\n", c.label, before, afterClick);
    check(std::abs(afterClick - before) > 1.0e-6f,
          "a left click released inside DOES move it (the checks above can fail)");

    // Put it back where it started so the next case is independent.
    if (auto* p = apvts.getParameter(c.paramId)) p->setValueNotifyingHost(before);
    pump(10);
}

// ---------------------------------------------------------------------------
// Section D — accessible names + roles
// ---------------------------------------------------------------------------
// Deliberately checks the INPUTS to JUCE's accessibility handlers rather than a live
// platform a11y client: detail::ButtonAccessibilityHandler reports getTitle() and falls
// back to getButtonText(), and SliderAccessibilityHandler reports getTitle(); a headless
// X server has no AT-SPI bus to ask. This is the same information, without the bus.
void auditNames(ClipperAudioProcessorEditor& ed) {
    int knobs = 0, anonymousKnobs = 0;
    for (auto* k : findAll<NeuKnob>(ed)) {
        if (!k->isVisible()) continue;
        ++knobs;
        if (k->slider().getTitle().isEmpty()) {
            ++anonymousKnobs;
            std::printf("    anonymous knob at %s\n",
                        k->getBounds().toString().toRawUTF8());
        }
    }
    std::printf("    visible knobs: %d, anonymous: %d\n", knobs, anonymousKnobs);
    check(knobs >= 6, "the walk actually found the knobs");
    check(anonymousKnobs == 0, "every visible knob carries an accessible name");

    int buttons = 0, anonymousButtons = 0, unfocusable = 0;
    for (auto* b : findAll<juce::Button>(ed)) {
        if (!b->isVisible()) continue;
        ++buttons;
        if (b->getTitle().isEmpty() && b->getButtonText().isEmpty()) {
            ++anonymousButtons;
            std::printf("    anonymous button '%s'\n", b->getName().toRawUTF8());
        }
        if (!b->getWantsKeyboardFocus()) ++unfocusable;
    }
    std::printf("    visible buttons: %d, anonymous: %d, not keyboard-focusable: %d\n",
                buttons, anonymousButtons, unfocusable);
    check(buttons >= 8, "the walk actually found the buttons");
    check(anonymousButtons == 0, "every visible button carries an accessible name");
    check(unfocusable == 0, "every visible button is reachable from the keyboard");

    // The two amp toggles and the three mode segments must read as CHECKABLE, and the
    // segments as a radio group — that is the difference between "a button" and "a
    // switch that is currently on".
    auto levers = findAll<LeverToggle>(ed);
    auto powers = findAll<PowerControl>(ed);
    bool checkable = !levers.empty() && !powers.empty();
    for (auto* l : levers) checkable = checkable && l->isToggleable();
    for (auto* p : powers) checkable = checkable && p->isToggleable();
    check(checkable, "Bright/Cab/Power report as checkable toggles");

    auto modes = findAll<ModeSwitch>(ed);
    int radioSegments = 0;
    for (auto* m : modes)
        for (auto* b : findAll<juce::Button>(*m))
            if (b->getRadioGroupId() != 0 && b->isToggleable()) ++radioSegments;
    std::printf("    mode switches: %d, radio segments: %d\n", (int)modes.size(),
                radioSegments);
    check(modes.empty() || radioSegments == 3,
          "each mode position is its own radio button");
}

// ---------------------------------------------------------------------------
// Section E — the layout audit, and the minimum width it permits
// ---------------------------------------------------------------------------
// The two chain "cards" are PAINTED rectangles, not components, so a generic
// parent-bounds walk cannot see a knob that has escaped one — which is exactly the way
// a too-narrow window breaks. Hence the two bounds accessors on the editor.
std::vector<juce::String> auditLayout(ClipperAudioProcessorEditor& ed) {
    std::vector<juce::String> issues;
    const auto amp = ed.ampCardBounds();
    const auto input = ed.inputCardBounds();

    struct Item {
        juce::Component* c;
        juce::String name;
    };
    std::vector<Item> ampControls;
    for (auto* k : findAll<NeuKnob>(ed)) {
        if (!k->isVisible()) continue;
        // A pedal card's knobs live inside the card, not on the bench.
        if (k->findParentComponentOfClass<PedalCard>() != nullptr) continue;
        ampControls.push_back({k, "knob " + k->slider().getTitle()});
    }
    for (auto* l : findAll<LeverToggle>(ed))
        if (l->isVisible()) ampControls.push_back({l, "lever " + l->getButtonText()});
    for (auto* p : findAll<PowerControl>(ed))
        if (p->isVisible()) ampControls.push_back({p, "power"});
    for (auto* m : findAll<ModeSwitch>(ed))
        if (m->isVisible()) ampControls.push_back({m, "mode switch"});

    // 1. Every bench-level control belongs to one of the two painted chassis, with a
    //    little margin for the chassis rim.
    for (const auto& it : ampControls) {
        const auto b = it.c->getBounds();
        const bool inAmp = amp.reduced(6, 6).contains(b);
        const bool inInput = input.reduced(4, 4).contains(b);
        if (!inAmp && !inInput)
            issues.push_back(it.name + " " + b.toString() + " escapes its chassis (amp " +
                             amp.toString() + ", input " + input.toString() + ")");
    }
    // 2. No two of them overlap.
    for (size_t i = 0; i < ampControls.size(); ++i)
        for (size_t j = i + 1; j < ampControls.size(); ++j)
            if (ampControls[i].c->getBounds().intersects(ampControls[j].c->getBounds()))
                issues.push_back(ampControls[i].name + " overlaps " + ampControls[j].name);

    // 3. The top-bar pills stay on the bench and clear of each other.
    auto combos = findAll<juce::ComboBox>(ed);
    for (size_t i = 0; i < combos.size(); ++i) {
        if (!combos[i]->isVisible()) continue;
        if (!ed.getLocalBounds().contains(combos[i]->getBounds()))
            issues.push_back("a top-bar pill " + combos[i]->getBounds().toString() +
                             " leaves the editor");
        for (size_t j = i + 1; j < combos.size(); ++j)
            if (combos[i]->getBounds().intersects(combos[j]->getBounds()))
                issues.push_back("the top-bar pills overlap");
    }

    // 4. Every pedal card's own controls stay inside that card.
    for (auto* card : findAll<PedalCard>(ed)) {
        if (!card->isVisible()) continue;
        for (int i = 0; i < card->getNumChildComponents(); ++i) {
            auto* ch = card->getChildComponent(i);
            if (ch == nullptr || !ch->isVisible()) continue;
            if (!card->getLocalBounds().contains(ch->getBounds()))
                issues.push_back("a control leaves pedal card " +
                                 card->getLocalBounds().toString() + ": " +
                                 ch->getBounds().toString());
        }
    }

    // 5. The board viewport must be able to show the WIDEST card (the GOLD plate face is
    //    236 px) plus the rail padding, or the scrolling board cannot be used.
    juce::Component* viewport = nullptr;
    for (auto* v : findAll<juce::Viewport>(ed)) viewport = v;
    if (viewport == nullptr)
        issues.push_back("no board viewport found");
    else if (viewport->getWidth() < 236 + 40)
        issues.push_back("board viewport is only " +
                         juce::String(viewport->getWidth()) +
                         " px — narrower than one GOLD card + rail padding (276)");
    return issues;
}

// Every voice, with a full board — the worst case for the amp face AND for the board.
bool layoutOkAt(ClipperAudioProcessor& proc, ClipperAudioProcessorEditor& ed, int w, int h,
                bool verbose) {
    bool ok = true;
    for (int voice = 0; voice < 4; ++voice) {
        setParam(proc.apvts, pid::ampModel, (float)voice);
        ed.setSize(w, h);
        ed.refreshFromState();
        ed.resized();
        pump(10);
        const auto issues = auditLayout(ed);
        if (!issues.empty()) {
            ok = false;
            if (verbose)
                for (const auto& i : issues)
                    std::printf("    %dx%d voice %d: %s\n", w, h, voice, i.toRawUTF8());
        }
    }
    return ok;
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    ClipperAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);
    setParam(proc.apvts, pid::reverb, 0.30f);

    auto editor = std::make_unique<ClipperAudioProcessorEditor>(proc);
    editor->setSize(1360, 640);
    editor->setVisible(true);
    // A REAL desktop peer, so repaint() goes through JUCE's real dirty-region plumbing.
    editor->addToDesktop(0);
    editor->toFront(true);
    pump(400);

    // A six-pedal board: the audit's own worst case, and the only one that overflows.
    const std::vector<int> full = {PEDAL_RAT,  PEDAL_SD,     PEDAL_TS,
                                   PEDAL_MUFF, PEDAL_PHASER, PEDAL_GOLD};
    proc.setChainOrder(full);
    for (const char* id : {pid::ratOn, pid::sdOn, pid::tsOn, pid::muffOn, pid::phaserOn,
                           pid::goldOn})
        setParam(proc.apvts, id, 1.0f);
    setParam(proc.apvts, pid::ampModel, 0.0f);  // Clean 120: has the mode switch
    editor->refreshFromState();
    editor->resized();
    pump(300);

    std::printf("=== Clipper editor paint / input / layout bench ===\n");
    std::printf("editor %dx%d, board of %d pedals, overflow %d px, peer %s\n",
                editor->getWidth(), editor->getHeight(), (int)full.size(),
                editor->boardOverflow(),
                editor->getPeer() != nullptr ? "yes" : "NO (results are not meaningful)");
    if (editor->boardOverflow() <= 0) {
        std::printf("FAIL: the six-pedal board does not overflow — nothing to scroll\n");
        ++failures;
    }

    // ---- A. sustained edge-drag scroll -------------------------------------
    std::printf("\n--- A. sustained edge-drag scroll (the auto-scroll pump's 42 Hz) ---\n");
    const ScrollRun before = measureScroll(*editor, /*cache=*/false, /*narrow=*/false, 84);
    const ScrollRun cacheOnly = measureScroll(*editor, true, false, 84);
    const ScrollRun after = measureScroll(*editor, true, true, 84);
    reportScroll("before (audit)", before);
    reportScroll("+ shadow cache", cacheOnly);
    reportScroll("+ narrowed repaints (after)", after);

    // ---- B. one drag-reorder across the board ------------------------------
    std::printf("\n--- B. one grip drag across a six-pedal board ---\n");
    juce::Component* content = nullptr;
    for (auto* v : findAll<juce::Viewport>(*editor))
        content = v->getViewedComponent();
    if (content == nullptr) {
        std::printf("FAIL: no board content found\n");
        ++failures;
    } else {
        const DragRun dBefore = measureDrag(*editor, *content, false, false);
        proc.setChainOrder(full);
        editor->refreshFromState();
        pump(60);
        const DragRun dAfter = measureDrag(*editor, *content, true, true);
        std::printf("  before (audit) : %lld ValueTree writes, %lld blurs, %lld imgs, "
                    "%d drag steps\n",
                    dBefore.writes, dBefore.blurs, dBefore.images, dBefore.steps);
        std::printf("  after          : %lld ValueTree writes, %lld blurs, %lld imgs, "
                    "%d drag steps\n",
                    dAfter.writes, dAfter.blurs, dAfter.images, dAfter.steps);
        check(dAfter.writes <= 1, "a whole drag-reorder writes the ValueTree at most once");
    }

    // ---- C. input safety ---------------------------------------------------
    std::printf("\n--- C. input safety (press / drag-off / right-click) ---\n");
    proc.setChainOrder(full);
    setParam(proc.apvts, pid::ampModel, 0.0f);
    editor->refreshFromState();
    editor->resized();
    pump(200);

    juce::Component* powerW = nullptr;
    for (auto* p : findAll<PowerControl>(*editor)) powerW = p;
    LeverToggle* brightW = nullptr;
    LeverToggle* cabW = nullptr;
    for (auto* l : findAll<LeverToggle>(*editor)) {
        if (l->getButtonText() == "Bright") brightW = l;
        if (l->getButtonText() == "Cab") cabW = l;
    }
    Footswitch* stompW = nullptr;
    for (auto* card : findAll<PedalCard>(*editor)) {
        if (card->type() != PEDAL_RAT) continue;
        for (auto* f : findAll<Footswitch>(*card)) stompW = f;
    }
    juce::Button* modeSeg = nullptr;
    for (auto* m : findAll<ModeSwitch>(*editor)) {
        auto segs = findAll<juce::Button>(*m);
        if (segs.size() >= 2) modeSeg = segs[1];  // "Chorus", so a click is a real change
    }

    // Can the real pointer be warped on this platform? If not, say so — do not let the
    // section quietly become a no-op (see pointerOver).
    bool warpWorks = powerW != nullptr && pointerOver(*powerW);
    std::printf("  real-pointer warp usable: %s%s\n", warpWorks ? "yes" : "no",
                warpWorks ? "" : "  (drag-off is then asserted on OUTCOME only)");

    const InputCase cases[] = {{"Power", powerW, pid::ampOn},
                               {"Bright", brightW, pid::bright},
                               {"Cab", cabW, pid::cab},
                               {"RAT stomp", stompW, pid::ratOn},
                               {"Mode segment", modeSeg, pid::chorusMode}};
    for (const auto& c : cases) runInputCase(*editor, proc.apvts, c, warpWorks);

    // ---- D. accessible names ----------------------------------------------
    std::printf("\n--- D. accessible names + roles ---\n");
    auditNames(*editor);

    // ---- E. layout audit + minimum width ----------------------------------
    std::printf("\n--- E. layout audit, and the minimum width it permits ---\n");
    const int h = 560;
    std::printf("  at the CURRENT minimum 1040x%d:\n", h);
    const bool okAtOld = layoutOkAt(proc, *editor, 1040, h, true);
    check(okAtOld, "the shipped minimum width passes the audit");

    int smallest = 1040;
    for (int w = 1040; w >= 700; w -= 4) {
        if (!layoutOkAt(proc, *editor, w, h, false)) break;
        smallest = w;
    }
    std::printf("  smallest width that passes for all four voices: %d px\n", smallest);
    std::printf("  first failing width (%d px) reported:\n", smallest - 4);
    layoutOkAt(proc, *editor, smallest - 4, h, true);

    // And the same sweep for the HEIGHT, since a short window starves the amp rows.
    int smallestH = 560;
    for (int hh = 560; hh >= 380; hh -= 4) {
        if (!layoutOkAt(proc, *editor, smallest, hh, false)) break;
        smallestH = hh;
    }
    std::printf("  smallest height that passes at %d px wide: %d px\n", smallest,
                smallestH);

    editor->removeFromDesktop();
    editor = nullptr;

    std::printf("\n%s: %d check(s) failed\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
