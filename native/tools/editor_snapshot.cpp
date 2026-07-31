// Clipper native shell — headless editor SNAPSHOT tool (dev-only, NOT shipped).
//
// Guarded behind the CMake option CLIPPER_BUILD_SNAPSHOT_TOOL (default OFF), so it
// never links into a release plugin/standalone build. It instantiates the real
// ClipperAudioProcessor + ClipperAudioProcessorEditor and writes
// Component::createComponentSnapshot PNGs of:
//
//   * one shot per AMP VOICE (the visual-pass deliverable),
//   * the NATIVE PARITY scenes: the default board, a full board with every pedal
//     type (cables + lit LEDs), the same board after a reorder, a bypassed pedal,
//     and the amp faces at the smallest and largest window sizes (the overlap /
//     occlusion audit), and
//   * the SCROLLING BOARD scenes (native_scroll_*): a six-pedal board at both
//     scroll extremes and mid-scroll, the rail, the minimum window, and the GOLD
//     'Myth' plate face lit and bypassed.
//
// Run headless under xvfb:
//   xvfb-run -a ./clipper_editor_snap <out-dir>
//
// It sets a few parameters purely so the screenshots read richly; nothing here
// affects the shipped plugin.

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

#include "PluginEditor.h"
#include "PluginProcessor.h"

namespace {
void setParam(juce::AudioProcessorValueTreeState& apvts, const char* id, float denorm) {
    if (auto* p = apvts.getParameter(id))
        p->setValueNotifyingHost(p->convertTo0to1(denorm));
}

bool writeSnapshot(clipper::native::ClipperAudioProcessorEditor& ed, const juce::File& out) {
    juce::Image img = ed.createComponentSnapshot(ed.getLocalBounds(), true, 2.0f);
    if (!img.isValid()) return false;
    out.deleteFile();
    juce::FileOutputStream os(out);
    if (!os.openedOk()) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(img, os);
}

int failures = 0;

void shoot(clipper::native::ClipperAudioProcessorEditor& ed, const juce::File& dir,
           const char* file, const char* label) {
    ed.refreshFromState();  // synchronous board/face rebuild + relayout
    ed.resized();
    juce::File out = dir.getChildFile(file);
    if (writeSnapshot(ed, out)) {
        std::printf("wrote %s  (%s)\n", out.getFullPathName().toRawUTF8(), label);
    } else {
        std::printf("FAILED to write %s\n", out.getFullPathName().toRawUTF8());
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::File outDir = (argc > 1) ? juce::File::getCurrentWorkingDirectory().getChildFile(
                                         juce::String(argv[1]))
                                   : juce::File::getCurrentWorkingDirectory();
    outDir.createDirectory();

    using namespace clipper::native;
    ClipperAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    // A richer default scene for the shots (does not touch the shipped plugin).
    setParam(proc.apvts, pid::reverb, 0.30f);

    auto editor = std::make_unique<ClipperAudioProcessorEditor>(proc);

    // ---- 1. NATIVE PARITY scenes ---------------------------------------------
    // The shipped default board: one RAT, guitar straight into the amp.
    proc.setChainOrder(ClipperAudioProcessor::defaultChain());
    setParam(proc.apvts, pid::ampModel, 0.0f);
    shoot(*editor, outDir, "native_parity_default_board.png",
          "default board - one RAT, cables, LED lit");

    // A FULL board: four pedal types in chain order, all engaged. This is the shot
    // that proves add/reorder, the cables, and the LEDs together.
    proc.setChainOrder({PEDAL_RAT, PEDAL_MUFF, PEDAL_TS, PEDAL_PHASER});
    for (const char* id : {pid::ratOn, pid::muffOn, pid::tsOn, pid::phaserOn})
        setParam(proc.apvts, id, 1.0f);
    editor->setSize(1560, 660);
    shoot(*editor, outDir, "native_parity_four_pedals.png",
          "4-pedal board - cables + LEDs lit");

    // The same four, REORDERED (the phaser moved to the front of the chain) — the
    // parity behaviour the fixed chain could never do.
    proc.setChainOrder({PEDAL_PHASER, PEDAL_RAT, PEDAL_MUFF, PEDAL_TS});
    shoot(*editor, outDir, "native_parity_reordered.png",
          "same four, phaser moved to the front");

    // A BYPASSED pedal: its LED goes dark and its knobs dim while the rest stay
    // lit — the engaged state must read at a glance.
    setParam(proc.apvts, pid::muffOn, 0.0f);
    shoot(*editor, outDir, "native_parity_bypassed_pedal.png",
          "Pi bypassed - LED dark, knobs dimmed");
    setParam(proc.apvts, pid::muffOn, 1.0f);

    // All five types at once, at the widest layout (the board's worst case).
    proc.setChainOrder({PEDAL_RAT, PEDAL_SD, PEDAL_TS, PEDAL_MUFF, PEDAL_PHASER});
    setParam(proc.apvts, pid::sdOn, 1.0f);
    editor->setSize(2000, 700);
    shoot(*editor, outDir, "native_parity_full_board_large.png",
          "all five pedals, large window");

    // The SMALLEST window with a crowded board. This used to be the SQUEEZE audit —
    // the window's minimum grew until the board fitted. It is now the SCROLL audit:
    // the window stays at 1040 and the board runs off the edge.
    editor->setSize(1040, 560);
    shoot(*editor, outDir, "native_parity_full_board_small.png",
          "all five pedals, minimum window - the board scrolls instead");

    // ---- 1b. the SCROLLING BOARD + the RAIL + the GOLD pedal ------------------
    // The board no longer grows the window; it scrolls inside a viewport while the
    // INPUT card and the AMP face stay pinned outside it. These shots are the proof:
    // overflow, the rail the pedals stand on, and the boundary cables at both scroll
    // extremes. `shootScrolled` parks the board at a proportion of its overflow.
    auto shootScrolled = [&](const char* file, const char* label, double proportion) {
        editor->refreshFromState();
        editor->resized();
        editor->setBoardScroll(proportion);
        shoot(*editor, outDir, file, label);
    };

    // The shipped default board, now sitting on the rail with room to spare.
    proc.setChainOrder(ClipperAudioProcessor::defaultChain());
    setParam(proc.apvts, pid::ampModel, 0.0f);
    editor->setSize(1360, 640);
    shootScrolled("native_scroll_default_board.png",
                  "default board - one RAT on the rail, board fits (no scrollbar)", 0.0);

    // ALL SIX types at once — the board the old grow-the-window build would have
    // demanded ~1622 px of desk for. Photographed mid-scroll, so the rail runs off
    // both edges and both boundary cables terminate at their grommets.
    const std::vector<int> full = {PEDAL_RAT,  PEDAL_SD,     PEDAL_TS,
                                   PEDAL_MUFF, PEDAL_PHASER, PEDAL_GOLD};
    proc.setChainOrder(full);
    for (const char* id : {pid::ratOn, pid::sdOn, pid::tsOn, pid::muffOn, pid::phaserOn,
                           pid::goldOn})
        setParam(proc.apvts, id, 1.0f);
    shootScrolled("native_scroll_six_pedals_midscroll.png",
                  "6-pedal board mid-scroll - rail overflows, edge veils on both sides",
                  0.5);
    shootScrolled("native_scroll_six_pedals_start.png",
                  "the same six, scrolled hard left - input cable reaches the first pedal",
                  0.0);
    shootScrolled("native_scroll_six_pedals_end.png",
                  "the same six, scrolled hard right - last pedal meets the amp cable",
                  1.0);
    if (editor->boardOverflow() <= 0) {
        std::printf("FAILED: the six-pedal board did not overflow the viewport\n");
        ++failures;
    }

    // The SMALLEST window with that same six-pedal board: the fixed INPUT card and
    // the full AMP face still stand outside a board that is mostly off screen.
    editor->setSize(1040, 560);
    shootScrolled("native_scroll_small_window.png",
                  "minimum window - fixed input + amp, board scrolled", 0.45);

    // The GOLD pedal's face, close enough to judge: the engraved nameplate, the
    // hairline gold rules, the round stomp. Alone on the board so nothing crowds it.
    proc.setChainOrder({PEDAL_GOLD});
    editor->setSize(1120, 700);
    shootScrolled("native_scroll_gold_face.png",
                  "the GOLD 'Myth' plate face - engraved nameplate, gold accent", 0.0);

    // The gold box BYPASSED: the LED goes dark, the knobs dim, and the engraving
    // fades with them — the plate has to read as off, not merely unlit.
    setParam(proc.apvts, pid::goldOn, 0.0f);
    shootScrolled("native_scroll_gold_bypassed.png",
                  "Myth bypassed - engraving and LED both go quiet", 0.0);
    setParam(proc.apvts, pid::goldOn, 1.0f);

    // ---- 2. the amp voices (the original visual-pass deliverable) -------------
    struct Voice {
        int index;
        const char* file;
        const char* label;
    };
    const Voice voices[] = {{0, "clipper_native_clean120.png", "Clean 120"},
                            {1, "clipper_native_eight_hundred.png", "Eight Hundred"},
                            {2, "clipper_native_twin.png", "Twin Sixty-Five"},
                            {3, "clipper_native_thirty.png", "Thirty"}};

    proc.setChainOrder({PEDAL_RAT, PEDAL_SD});
    editor->setSize(1360, 640);
    for (const auto& v : voices) {
        setParam(proc.apvts, pid::ampModel, (float)v.index);
        shoot(*editor, outDir, v.file, v.label);
    }

    // ---- 3. the amp-face audit (the chorus-row overlap fix) ------------------
    setParam(proc.apvts, pid::ampModel, 0.0f);
    setParam(proc.apvts, pid::chorusMode, 1.0f);
    shoot(*editor, outDir, "native_parity_clean120_chorus.png",
          "Clean 120 chorus row - divider clear of the knobs");
    editor->setSize(1040, 560);
    shoot(*editor, outDir, "native_parity_clean120_chorus_small.png",
          "Clean 120 chorus row, minimum window");
    editor->setSize(2000, 1000);
    shoot(*editor, outDir, "native_parity_clean120_chorus_large.png",
          "Clean 120 chorus row, maximum window");

    // The Twin's tremolo row shares the same divider code path — audit it too.
    setParam(proc.apvts, pid::ampModel, 2.0f);
    editor->setSize(1360, 640);
    shoot(*editor, outDir, "native_parity_twin_tremolo.png",
          "Twin tremolo row - same divider band");

    // ---- 4. BOTH THEMES x every amp voice (visual pass 3) --------------------
    // The editor ships the web's two token contexts. The mode is set with
    // persist=false so photographing them never rewrites the user's own setting.
    // The board carries two pedals so the pinned-dark chassis (which must NOT move
    // between themes) and its cast shadow on the rail are in every frame.
    {
        proc.setChainOrder({PEDAL_RAT, PEDAL_SD});
        editor->setSize(1360, 640);
        setParam(proc.apvts, pid::chorusMode, 1.0f);
        struct ThemeShot {
            clipper::native::skin::ThemeMode mode;
            const char* tag;
        };
        const ThemeShot themes[] = {{clipper::native::skin::ThemeMode::Light, "light"},
                                    {clipper::native::skin::ThemeMode::Dark, "dark"}};
        for (const auto& t : themes) {
            editor->setThemeMode(t.mode, /*persist=*/false);
            for (const auto& v : voices) {
                setParam(proc.apvts, pid::ampModel, (float)v.index);
                const juce::String file =
                    juce::String("native_theme_") + t.tag + "_" +
                    juce::String(v.file).replace("clipper_native_", "");
                shoot(*editor, outDir, file.toRawUTF8(),
                      (juce::String(v.label) + " - " + t.tag + " theme").toRawUTF8());
            }
        }
        // The full six-pedal board in each theme: the rail, the edge veils, the
        // cables and the pedal cast shadows all have to survive the flip.
        proc.setChainOrder(full);
        setParam(proc.apvts, pid::ampModel, 1.0f);
        for (const auto& t : themes) {
            editor->setThemeMode(t.mode, /*persist=*/false);
            editor->refreshFromState();
            editor->resized();
            editor->setBoardScroll(0.5);
            shoot(*editor, outDir,
                  (juce::String("native_theme_") + t.tag + "_board.png").toRawUTF8(),
                  (juce::String("6-pedal board mid-scroll - ") + t.tag + " theme")
                      .toRawUTF8());
        }
        editor->setThemeMode(clipper::native::skin::ThemeMode::Light, /*persist=*/false);
    }

    editor = nullptr;
    return failures == 0 ? 0 : 1;
}
