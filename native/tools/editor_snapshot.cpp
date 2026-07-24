// Clipper native shell — headless editor SNAPSHOT tool (dev-only, NOT shipped).
//
// Guarded behind the CMake option CLIPPER_BUILD_SNAPSHOT_TOOL (default OFF), so it
// never links into a release plugin/standalone build. It instantiates the real
// ClipperAudioProcessor + ClipperAudioProcessorEditor, drives the amp-voice choice
// through all four voices, and writes a Component::createComponentSnapshot PNG per
// voice — the deliverable proving the neumorphic editor + the amp-face switching.
//
// Run headless under xvfb:
//   xvfb-run -a ./clipper_editor_snap <out-dir>
// Emits: clipper_native_clean120.png / _eight_hundred.png / _twin.png / _thirty.png
//
// It sets a couple of parameters (SD-1 engaged, a touch of reverb) purely so the
// screenshots read richly; nothing here affects the shipped plugin.

#include <juce_gui_basics/juce_gui_basics.h>

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
}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::File outDir = (argc > 1) ? juce::File::getCurrentWorkingDirectory().getChildFile(
                                         juce::String(argv[1]))
                                   : juce::File::getCurrentWorkingDirectory();
    outDir.createDirectory();

    clipper::native::ClipperAudioProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    // A richer default scene for the shots (does not touch the shipped plugin).
    setParam(proc.apvts, clipper::native::pid::sdOn, 1.0f);
    setParam(proc.apvts, clipper::native::pid::reverb, 0.30f);

    auto editor = std::make_unique<clipper::native::ClipperAudioProcessorEditor>(proc);
    // (uses the editor's default size from its constructor)

    struct Voice {
        int index;
        const char* file;
        const char* label;
    };
    const Voice voices[] = {{0, "clipper_native_clean120.png", "Clean 120"},
                            {1, "clipper_native_eight_hundred.png", "Eight Hundred"},
                            {2, "clipper_native_twin.png", "Twin Sixty-Five"},
                            {3, "clipper_native_thirty.png", "Thirty"}};

    int failures = 0;
    for (const auto& v : voices) {
        setParam(proc.apvts, clipper::native::pid::ampModel, (float)v.index);
        editor->refreshFromState();  // synchronous face rebuild + relayout

        juce::File out = outDir.getChildFile(v.file);
        if (writeSnapshot(*editor, out)) {
            std::printf("wrote %s  (%s)\n", out.getFullPathName().toRawUTF8(), v.label);
        } else {
            std::printf("FAILED to write %s\n", out.getFullPathName().toRawUTF8());
            ++failures;
        }
    }

    editor = nullptr;
    return failures == 0 ? 0 : 1;
}
