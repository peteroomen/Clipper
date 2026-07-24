// Clipper native shell — headless editor SNAPSHOT tool (dev-only, NOT shipped).
//
// Guarded behind the CMake option CLIPPER_BUILD_SNAPSHOT_TOOL (default OFF), so it
// never links into a release plugin/standalone build. It instantiates the real
// ClipperAudioProcessor + ClipperAudioProcessorEditor and writes
// Component::createComponentSnapshot PNGs of:
//
//   * one shot per AMP VOICE (the visual-pass deliverable), and
//   * the NATIVE PARITY scenes: the default board, a full board with every pedal
//     type (cables + lit LEDs), the same board after a reorder, a bypassed pedal,
//     and the amp faces at the smallest and largest window sizes (the overlap /
//     occlusion audit).
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

    // The SMALLEST window with a crowded board — the squeeze audit.
    editor->setSize(1040, 560);
    shoot(*editor, outDir, "native_parity_full_board_small.png",
          "all five pedals, smallest window that fits the board");

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

    editor = nullptr;
    return failures == 0 ? 0 : 1;
}
