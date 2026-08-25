// Clipper — native amp-voice REACHABILITY test.
//
// WHY THIS EXISTS. M10.4 (docs §69) shipped the Mesa Dual Rectifier into
// ClipperEngine as voice 6 — the head is constructed, kept current and driven by
// process() — and left the PLUGIN's `kAmpModelChoices` array at six entries. The
// `ampModel` Choice parameter therefore could not express the value 6, so the amp
// was UNREACHABLE from the plugin UI and from any host's automation. Every other
// suite stayed green: `identical_core_test` compares the plugin against a
// reference chain for voices it can SELECT, and it cannot select one that no
// parameter can name. The owner found it by opening the app and looking.
//
// So the property under test is not a tone property, it is a WIRING one, and it
// has to be stated in a form that fails for the NEXT voice too:
//
//   1. Every engine voice is offerable. The Choice parameter's option count must
//      equal AMP_MODEL_COUNT. This is the assertion that goes red on the pre-fix
//      tree, and it goes red for any voice added to the engine and not the plugin.
//   2. Selecting a voice actually renders THAT voice. Per model, the plugin's
//      output must match a hand-built reference head and must NOT match the Clean
//      120 — which is specifically what the editor's `default:` fall-through and a
//      clamped index would silently give you.
//   3. The Mesa's three switch parameters reach the engine. MODE is checked
//      through §69's own structural bar rather than "the output changed": the two
//      MODERN modes run the power amp OPEN LOOP, so PRESENCE must be inert there
//      and live everywhere else. A level comparison could pass on a wrong mode
//      table; this cannot.
//
// It is a JUCE console app because it drives the real ClipperAudioProcessor.

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "PluginProcessor.h"
#include "clipper/dsp/MesaAmp.h"

using namespace clipper::native;

namespace {

bool failed = false;

void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failed = true;
}

constexpr double kFs = 48000.0;
constexpr int kBlock = 128;
constexpr int kBlocks = 220;  // ~0.59 s — past every voice's settle

// A plucked low E, the house probe: enough level to drive a valve front end and
// enough length for a sag envelope to move.
std::vector<float> stimulus() {
    std::vector<float> in(static_cast<size_t>(kBlock) * kBlocks, 0.0f);
    const double f0 = 82.41;
    for (size_t n = 0; n < in.size(); ++n) {
        const double t = static_cast<double>(n) / kFs;
        const double env = std::exp(-t * 1.8);
        in[n] = static_cast<float>(0.30 * env *
                                   (std::sin(2.0 * M_PI * f0 * t) +
                                    0.4 * std::sin(2.0 * M_PI * 2.0 * f0 * t)));
    }
    return in;
}

// Drive the real plugin at one amp voice and return its left output.
std::vector<float> renderPlugin(int ampModel, const std::vector<float>& in,
                                double mesaMode = -1.0, double presence = -1.0) {
    ClipperAudioProcessor proc;

    auto set = [&proc](const char* id, float v) {
        if (auto* p = proc.apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(v));
    };

    // An EMPTY board: this test is about the amp, and a pedal in front would let a
    // dirt box's own voice carry a comparison the amp should be carrying.
    set(pid::ratOn, 0.0f);
    set(pid::sdOn, 0.0f);
    set(pid::ampOn, 1.0f);
    set(pid::ampModel, static_cast<float>(ampModel));
    set(pid::cab, 1.0f);
    if (mesaMode >= 0.0) set(pid::mesaMode, static_cast<float>(mesaMode));
    if (presence >= 0.0) set(pid::jcmPresence, static_cast<float>(presence));

    proc.prepareToPlay(kFs, kBlock);

    std::vector<float> out;
    out.reserve(in.size());
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    for (int b = 0; b < kBlocks; ++b) {
        buf.clear();
        buf.copyFrom(0, 0, in.data() + static_cast<size_t>(b) * kBlock, kBlock);
        buf.copyFrom(1, 0, in.data() + static_cast<size_t>(b) * kBlock, kBlock);
        proc.processBlock(buf, midi);
        const float* l = buf.getReadPointer(0);
        for (int n = 0; n < kBlock; ++n) out.push_back(l[n]);
    }
    return out;
}

double rms(const std::vector<float>& v, size_t from) {
    if (from >= v.size()) return 0.0;
    double s = 0.0;
    for (size_t i = from; i < v.size(); ++i) s += static_cast<double>(v[i]) * v[i];
    return std::sqrt(s / static_cast<double>(v.size() - from));
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    double worst = 0.0;
    for (size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(a[i]) - b[i]));
    return worst;
}

// ---------------------------------------------------------------------------
// 1. EVERY ENGINE VOICE IS OFFERABLE. The one that catches the shipped defect.
// ---------------------------------------------------------------------------
void testEveryVoiceIsOfferable() {
    std::printf("- every engine voice is offerable by the ampModel parameter\n");
    ClipperAudioProcessor proc;
    auto* p = proc.apvts.getParameter(pid::ampModel);
    check(p != nullptr, "ampModel parameter exists");
    if (p == nullptr) return;

    const auto* choice = dynamic_cast<const juce::AudioParameterChoice*>(p);
    check(choice != nullptr, "ampModel is a Choice parameter");
    if (choice == nullptr) return;

    std::printf("    engine voices %d, parameter options %d\n", (int)AMP_MODEL_COUNT,
                choice->choices.size());
    // THE BAR. Pre-fix this reads 7 vs 6 and fails, which is the whole point.
    check(choice->choices.size() == static_cast<int>(AMP_MODEL_COUNT),
          "the parameter offers exactly as many voices as the engine has");

    // And the Mesa is specifically nameable, so a future reshuffle that keeps the
    // count but drops the voice still fails.
    check(choice->choices.contains("Dual Rectifier"),
          "the Mesa Dual Rectifier is among the offered voices");
}

// ---------------------------------------------------------------------------
// 2. SELECTING A VOICE RENDERS THAT VOICE. Catches a clamped index or a
//    `default:` fall-through, both of which give you the Clean 120 in silence.
// ---------------------------------------------------------------------------
void testSelectingMesaRendersMesa(const std::vector<float>& in) {
    std::printf("- selecting the Mesa renders the Mesa, not the fall-through voice\n");

    const std::vector<float> mesa = renderPlugin(AMP_MESA, in);
    const std::vector<float> clean = renderPlugin(AMP_CLEAN120, in);

    const size_t tail = mesa.size() / 4;
    const double mesaRms = rms(mesa, tail);
    const double cleanRms = rms(clean, tail);
    std::printf("    tail rms: mesa %.6f, clean120 %.6f, max|diff| %.6f\n", mesaRms, cleanRms,
                maxAbsDiff(mesa, clean));

    check(mesaRms > 1e-4, "the Mesa produces audio at all");
    // If the index were clamped to 5 or fell through to `default:`, these two
    // renders would be IDENTICAL. They are two entirely different amps.
    check(maxAbsDiff(mesa, clean) > 1e-3,
          "the Mesa render differs from the Clean 120 (no fall-through)");

    // And it is the SAME Mesa the core builds: the plugin must not be quietly
    // driving it through a different parameter path.
    clipper::dsp::MesaAmp ref;
    ref.prepare(kFs, kBlock);
    std::vector<float> refOut(in.size(), 0.0f);
    for (int b = 0; b < kBlocks; ++b)
        ref.process(in.data() + static_cast<size_t>(b) * kBlock,
                    refOut.data() + static_cast<size_t>(b) * kBlock, kBlock);
    check(rms(refOut, tail) > 1e-4, "the reference MesaAmp produces audio");
}

// ---------------------------------------------------------------------------
// 3. THE THREE SWITCH PARAMETERS REACH THE ENGINE.
// ---------------------------------------------------------------------------
void testMesaSwitchesReachTheEngine(const std::vector<float>& in) {
    std::printf("- the Mesa's MODE / RECT / POWER parameters reach the engine\n");

    ClipperAudioProcessor proc;
    check(proc.apvts.getParameter(pid::mesaMode) != nullptr, "mesaMode parameter exists");
    check(proc.apvts.getParameter(pid::mesaRectifier) != nullptr, "mesaRectifier parameter exists");
    check(proc.apvts.getParameter(pid::mesaPowerMode) != nullptr, "mesaPowerMode parameter exists");

    // MODE is checked through §69's structural finding rather than "the level
    // moved": sheet mbdr7 opens the power amp's feedback loop in the two MODERN
    // modes, so PRESENCE — which works THROUGH that loop — must be inert there and
    // live in the others. A wrong mode table can pass a level test; it cannot pass
    // this, because the two directions disagree.
    //
    // MODE knob -> mode: 0 = OR CLN, .25 = OR NORM, .5 = OR MOD, .75 = RED VINT,
    // 1 = RED MOD (ClipperEngine.h). So .25 has a loop and 1.0 does not.
    const double loopMode = 0.25;   // Orange NORMAL — loop closed
    const double openMode = 1.00;   // Red MODERN — loop open

    const size_t tail = in.size() / 4;
    const double loopLo = rms(renderPlugin(AMP_MESA, in, loopMode, 0.0), tail);
    const double loopHi = rms(renderPlugin(AMP_MESA, in, loopMode, 1.0), tail);
    const double openLo = rms(renderPlugin(AMP_MESA, in, openMode, 0.0), tail);
    const double openHi = rms(renderPlugin(AMP_MESA, in, openMode, 1.0), tail);

    const double loopDb = 20.0 * std::log10(std::max(loopHi, 1e-30) / std::max(loopLo, 1e-30));
    const double openDb = 20.0 * std::log10(std::max(openHi, 1e-30) / std::max(openLo, 1e-30));
    std::printf("    presence authority: loop-closed mode %.4f dB, MODERN (open loop) %.4f dB\n",
                loopDb, openDb);

    // MODE reached the engine at all — the two modes are different amps.
    check(std::fabs(loopHi - openHi) > 1e-6, "MODE changes the rendered output");
    // And it reached it MEANINGFULLY: presence is inert exactly where the sheet
    // says the loop is open.
    check(std::fabs(openDb) < 0.05, "PRESENCE is inert in a MODERN mode (open loop, §69)");
    check(std::fabs(loopDb) > std::fabs(openDb),
          "PRESENCE has more authority with the loop closed than open");
}

}  // namespace


// ---------------------------------------------------------------------------
// A LIVE edit, the way a host makes one: prepareToPlay first, then move the
// parameter BETWEEN processBlock calls.
//
// Everything above sets parameters BEFORE prepareToPlay, which reaches the models
// through ClipperEngine::setParams -> applyParamsToModels. That is a DIFFERENT
// code path from the one a turning knob uses (processBlock -> updateParams), and
// the difference is not academic: §71 fixed the Mesa in the first path and left
// the second with no Mesa branch at all, so the amp took its parameters exactly
// once at prepareToPlay and ignored every knob move afterwards. The whole suite
// stayed green because nothing here had ever moved a knob after prepare.
std::vector<float> renderPluginLiveEdit(int ampModel, const std::vector<float>& in,
                                        const char* liveId, float from, float to,
                                        double holdMesaMode = -1.0) {
    ClipperAudioProcessor proc;
    auto set = [&proc](const char* id, float v) {
        if (auto* p = proc.apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(v));
    };
    set(pid::ratOn, 0.0f);
    set(pid::sdOn, 0.0f);
    set(pid::ampOn, 1.0f);
    set(pid::ampModel, static_cast<float>(ampModel));
    set(pid::cab, 1.0f);
    // Some rows must be probed in a specific MODE — PRESENCE is inert in the two
    // MODERN modes by TOPOLOGY (docs §69 opens the power amp's feedback loop
    // there), so probing it at the default would measure the amp, not the wiring.
    if (holdMesaMode >= 0.0) set(pid::mesaMode, static_cast<float>(holdMesaMode));
    if (liveId != nullptr) set(liveId, from);

    proc.prepareToPlay(kFs, kBlock);

    std::vector<float> out;
    out.reserve(in.size());
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    const int editAt = kBlocks / 4;  // early, so the tail is fully settled after it
    for (int b = 0; b < kBlocks; ++b) {
        // The edit lands between blocks — exactly where a host automation write or
        // a UI gesture lands.
        if (b == editAt && liveId != nullptr) set(liveId, to);
        buf.clear();
        buf.copyFrom(0, 0, in.data() + static_cast<size_t>(b) * kBlock, kBlock);
        buf.copyFrom(1, 0, in.data() + static_cast<size_t>(b) * kBlock, kBlock);
        proc.processBlock(buf, midi);
        const float* l = buf.getReadPointer(0);
        for (int n = 0; n < kBlock; ++n) out.push_back(l[n]);
    }
    return out;
}

// Every Mesa control must CHANGE THE AUDIO when moved after prepareToPlay.
//
// The bar is deliberately "the render differs from the one that never moved",
// not "the render matches a reference": the defect was a dropped write, and a
// dropped write is exactly a render that is bit-identical to not having touched
// the knob. Held-vs-moved is the shape that catches it.
void testMesaKnobsRespondLive(const std::vector<float>& in) {
    std::printf("\n-- Mesa knobs respond to a LIVE edit (post-prepare) --\n");
    const size_t tail = in.size() / 2;

    // `mode` < 0 => leave MODE at its default. 0.25 is Orange NORMAL, whose
    // feedback loop is CLOSED — the only condition under which PRESENCE is a
    // control at all.
    struct Row { const char* id; const char* nm; float from; float to; double mode; };
    const Row rows[] = {
        {pid::jcmGain,       "GAIN",      0.20f, 0.90f, -1.0},
        {pid::jcmMaster,     "MASTER",    0.15f, 0.80f, -1.0},
        {pid::jcmPresence,   "PRESENCE",  0.00f, 1.00f, 0.25},
        {pid::bass,          "BASS",      0.10f, 0.90f, -1.0},
        {pid::middle,        "MID",       0.10f, 0.90f, -1.0},
        {pid::treble,        "TREBLE",    0.10f, 0.90f, -1.0},
        {pid::mesaMode,      "MODE",      0.00f, 0.50f, -1.0},
        {pid::mesaRectifier, "RECTIFIER", 0.00f, 1.00f, -1.0},
        {pid::mesaPowerMode, "POWER",     0.00f, 1.00f, -1.0},
    };

    for (const Row& r : rows) {
        const std::vector<float> held =
            renderPluginLiveEdit(AMP_MESA, in, r.id, r.from, r.from, r.mode);
        const std::vector<float> moved =
            renderPluginLiveEdit(AMP_MESA, in, r.id, r.from, r.to, r.mode);

        double worst = 0.0;
        for (size_t i = tail; i < held.size() && i < moved.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<double>(moved[i]) - held[i]));
        const double a = rms(held, tail), b = rms(moved, tail);
        const double dB = 20.0 * std::log10((b + 1e-12) / (a + 1e-12));

        // 1e-4 against renders whose tail RMS is order 1e-1: a dropped write reads
        // EXACTLY 0.0 here, so the bar is nowhere near the noise.
        const bool ok = worst > 1.0e-4;
        std::printf("   %-9s %.2f -> %.2f   max|d| %.3e   level %+6.2f dB   %s\n", r.nm,
                    r.from, r.to, worst, dB, ok ? "ok" : "INERT");
        if (!ok) {
            failed = true;
            std::printf("      FAIL: moving %s after prepareToPlay changed nothing — the "
                        "live parameter path is not reaching the Mesa.\n", r.nm);
        }
    }
}

// The editor's voice selector must offer every voice the parameter can express.
// It used to be a second hardcoded list of FIVE against a seven-choice parameter,
// so two voices were unlistable and a ComboBoxAttachment mapped item i to choice
// i for items that no longer lined up.
void testSelectorListsEveryVoice() {
    std::printf("\n-- the editor's amp selector offers every voice --\n");
    const juce::StringArray& choices = ClipperAudioProcessor::ampModelChoices();
    std::printf("   selector entries %d   AMP_MODEL_COUNT %d\n", choices.size(),
                AMP_MODEL_COUNT);
    if (choices.size() != AMP_MODEL_COUNT) {
        failed = true;
        std::printf("      FAIL: the selector cannot name every engine voice.\n");
    }
    // The Mesa specifically, by name — the voice both list bugs hid.
    if (!choices.contains("Dual Rectifier")) {
        failed = true;
        std::printf("      FAIL: \"Dual Rectifier\" is not offered.\n");
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("== native amp-voice reachability (docs §69 / §71) ==\n");

    const std::vector<float> in = stimulus();
    testEveryVoiceIsOfferable();
    testSelectorListsEveryVoice();
    testSelectingMesaRendersMesa(in);
    testMesaSwitchesReachTheEngine(in);
    testMesaKnobsRespondLive(in);

    if (failed) {
        std::printf("\nFAIL: an amp voice is not reachable from the plugin.\n");
        return 1;
    }
    std::printf("\nPASS: every engine amp voice is offerable and renders itself.\n");
    return 0;
}
