// Clipper native shell — the CAB / IR PICKER state test.
//
// The picker's audio behaviour (declicked swap, no allocation on the render path,
// latency unmoved) is proved by clipper_chain_edit_test. This test is about the
// OTHER half — the half a player notices the morning after the session:
//
//   1. ROUND TRIP — a built-in choice and a custom IR's FILE PATH both survive
//      getStateInformation -> setStateInformation into a FRESH processor, which is
//      exactly what a DAW does when it saves and reopens a project.
//   2. THE PATH IS NOT A PARAMETER — it lives in the APVTS state tree (a `cab`
//      child node), never in the parameter list, so no host tries to automate a
//      file path and no automation lane can corrupt one.
//   3. MISSING-FILE FALLBACK — a session that references an IR which is no longer
//      on disk (a different machine, an unmounted drive, a renamed file) falls back
//      to the Clean 2x12 and SAYS SO, rather than opening silent or crashing. This
//      is the web's convention (App.tsx), ported.
//   4. A BAD FILE IS REFUSED and the previous cab keeps playing.
//   5. LATENCY IS CAB-INDEPENDENT — the partition stays 128 for every choice, so
//      switching cabs never asks the host to re-align the track.
//
// It is a JUCE console app because it drives the real ClipperAudioProcessor.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>

#include "PluginProcessor.h"

using namespace clipper::native;

namespace {

bool failed = false;

void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failed = true;
}

// Write a short synthetic cab IR so the test never depends on a fixture living on
// disk: a decaying comb, mono, 48 kHz, 512 frames. Audibly a cab it is not; a valid
// .wav the loader must accept it is.
bool writeTestIr(const juce::File& f, double rate, int frames, int channels) {
    f.deleteFile();
    juce::AudioBuffer<float> buf(channels, frames);
    buf.clear();
    for (int c = 0; c < channels; ++c) {
        float* d = buf.getWritePointer(c);
        d[0] = 1.0f;
        if (frames > 64) d[64] = -0.5f;
        if (frames > 191) d[191] = 0.25f;
    }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
    if (os == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(os.get(), rate, (unsigned)channels, 16, {}, 0));
    if (w == nullptr) return false;
    os.release();  // the writer owns the stream now
    return w->writeFromAudioSampleBuffer(buf, 0, frames);
}

// Does the saved state carry the path as a TREE PROPERTY and NOT as a parameter?
bool stateHasPathOutsideParameters(const juce::MemoryBlock& blob, const juce::String& path) {
    std::unique_ptr<juce::XmlElement> xml(
        juce::AudioProcessor::getXmlFromBinary(blob.getData(), (int)blob.getSize()));
    if (xml == nullptr) return false;
    bool foundInCabNode = false;
    for (auto* child : xml->getChildIterator()) {
        if (child->hasTagName("cab") && child->getStringAttribute("customIr") == path)
            foundInCabNode = true;
        // Every APVTS parameter is a <PARAM id="..." value="..."/> child; a path can
        // never legitimately appear in one.
        if (child->hasTagName("PARAM") && child->getStringAttribute("value") == path)
            return false;
    }
    return foundInCabNode;
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;  // the message manager the APVTS wants
    std::printf("=== Clipper cab/IR picker state test ===\n");

    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("clipper_cab_state_test");
    dir.createDirectory();
    const juce::File irFile = dir.getChildFile("Test Cab 1x12.wav");
    const juce::File stereoIr = dir.getChildFile("stereo-ir.wav");
    const juce::File badFile = dir.getChildFile("not-audio.wav");

    std::printf("\n--- fixtures ---\n");
    check(writeTestIr(irFile, 48000.0, 512, 1), "wrote a mono 48 k test IR");
    check(writeTestIr(stereoIr, 44100.0, 700, 2), "wrote a stereo 44.1 k test IR");
    badFile.replaceWithText("this is not a wav file");

    // ---- 1. a BUILT-IN choice round-trips ----------------------------------
    std::printf("\n--- built-in choice round trip ---\n");
    juce::MemoryBlock blob;
    {
        ClipperAudioProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        check(proc.cabChoice() == CAB_CLEAN212, "a fresh instance opens on the Clean 2x12");
        proc.setCabChoice(CAB_BRIT412);
        check(proc.cabChoice() == CAB_BRIT412, "the Brit 4x12 was selected");
        proc.getStateInformation(blob);
    }
    {
        ClipperAudioProcessor proc;
        proc.setStateInformation(blob.getData(), (int)blob.getSize());
        proc.prepareToPlay(48000.0, 128);
        check(proc.cabChoice() == CAB_BRIT412, "the Brit 4x12 survived save + reload");
        check(proc.cabNote().isEmpty(), "a built-in cab has nothing to report");
    }

    // ---- 2. a CUSTOM IR's PATH round-trips (as a property, not a parameter) --
    std::printf("\n--- custom IR round trip ---\n");
    juce::MemoryBlock customBlob;
    int latencyWithCustom = -1;
    {
        ClipperAudioProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        const int latClean = proc.getLatencySamples();
        check(proc.loadCustomIrFile(irFile), "the mono IR loaded");
        check(proc.cabChoice() == CAB_CUSTOM, "loading an IR selects the Custom cab");
        check(proc.customIrPath() == irFile.getFullPathName(), "the path was stored");
        check(proc.cabLabel() == "Test Cab 1x12", "the chip label is the file name");
        latencyWithCustom = proc.getLatencySamples();
        check(latencyWithCustom == latClean,
              "a custom IR does not move the reported latency");
        proc.getStateInformation(customBlob);
        check(stateHasPathOutsideParameters(customBlob, irFile.getFullPathName()),
              "the path is a state-tree property and NOT an automatable parameter");
    }
    {
        ClipperAudioProcessor proc;
        proc.setStateInformation(customBlob.getData(), (int)customBlob.getSize());
        proc.prepareToPlay(48000.0, 128);
        check(proc.cabChoice() == CAB_CUSTOM, "the Custom cab survived save + reload");
        check(proc.customIrPath() == irFile.getFullPathName(), "so did the path");
        check(proc.customIrLabel() == "Test Cab 1x12", "and the IR was re-read from disk");
        check(proc.cabNote().isEmpty(), "a found IR reports nothing");
        check(proc.getLatencySamples() == latencyWithCustom, "latency is unchanged");
    }

    // A STEREO 44.1 k source: mono-ised by averaging and resampled to the engine
    // rate (web/src/cab.ts conventions), not rejected.
    {
        ClipperAudioProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        check(proc.loadCustomIrFile(stereoIr),
              "a stereo 44.1 k IR loads (mono-ised + resampled)");
        check(proc.cabLabel() == "stereo-ir", "its label is the file name");
    }

    // ---- 3. MISSING FILE on reload -> fall back to the Clean 2x12 ------------
    std::printf("\n--- missing-IR fallback ---\n");
    {
        irFile.deleteFile();  // the drive is gone / the file was renamed
        ClipperAudioProcessor proc;
        proc.setStateInformation(customBlob.getData(), (int)customBlob.getSize());
        proc.prepareToPlay(48000.0, 128);
        check(proc.cabChoice() == CAB_CLEAN212,
              "a session whose IR is missing falls back to the Clean 2x12");
        check(proc.cabNote().isNotEmpty(), "...and says so, rather than failing silently");
        std::printf("  note: \"%s\"\n", proc.cabNote().toRawUTF8());
        check(proc.customIrLabel().isEmpty(), "the stale IR label is cleared");
        // Deliberate difference from the web, which clears its label AND its
        // reference: the PATH is kept, so re-mounting the drive and re-selecting
        // Custom is one click rather than a fresh file hunt.
        check(proc.customIrPath().isNotEmpty(),
              "the path is kept, so the session can be repaired by re-selecting Custom");

        // ...and re-selecting Custom once the file is back really does repair it.
        // This is the RETRY path: the request looks identical to the one that just
        // failed, so it only works because a user action is applied unconditionally
        // (applyCabFromState's `force`) rather than deduplicated.
        check(writeTestIr(irFile, 48000.0, 512, 1), "the IR file is back on disk");
        proc.setCabChoice(CAB_CUSTOM);
        check(proc.cabChoice() == CAB_CUSTOM, "re-selecting Custom takes this time");
        check(proc.customIrLabel() == "Test Cab 1x12", "the IR was re-read");
        check(proc.cabNote().isEmpty(), "and the fallback note is cleared");
        irFile.deleteFile();
    }

    // ---- 4. an UNREADABLE file is refused, the current cab keeps playing -----
    std::printf("\n--- bad file ---\n");
    {
        ClipperAudioProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        proc.setCabChoice(CAB_BRIT412);
        check(!proc.loadCustomIrFile(badFile), "a non-audio file is refused");
        check(proc.cabChoice() == CAB_BRIT412, "the previous cab is still selected");
        check(proc.cabNote().isNotEmpty(), "the reason is shown");
        std::printf("  note: \"%s\"\n", proc.cabNote().toRawUTF8());
        check(!proc.loadCustomIrFile(dir.getChildFile("does-not-exist.wav")),
              "a missing file is refused");
        check(proc.cabChoice() == CAB_BRIT412, "and still does not disturb the rig");
    }

    // ---- 5. LATENCY is the same for every cab -------------------------------
    std::printf("\n--- latency across every cab ---\n");
    {
        check(writeTestIr(irFile, 48000.0, 512, 1), "re-wrote the test IR");
        ClipperAudioProcessor proc;
        proc.prepareToPlay(48000.0, 128);
        const int a = proc.getLatencySamples();
        proc.setCabChoice(CAB_BRIT412);
        const int b = proc.getLatencySamples();
        proc.loadCustomIrFile(irFile);
        const int c = proc.getLatencySamples();
        std::printf("  clean212 %d, brit412 %d, custom %d\n", a, b, c);
        check(a == b && b == c, "the cab partition stays 128 for every choice");
    }

    dir.deleteRecursively();

    if (failed) {
        std::printf("\nFAIL: the cab picker's state does not round-trip correctly.\n");
        return 1;
    }
    std::printf("\nPASS: cab choice + custom IR path round-trip, and a missing IR "
                "falls back cleanly.\n");
    return 0;
}
