// Clipper native shell — user IR file loading (the cab/IR picker's message-thread
// half).
//
// This is the native sibling of web/src/cab.ts, and it follows that file's
// conventions deliberately rather than inventing its own, because the two
// front-ends must voice a given .wav the same way:
//
//   * MONO-ISE by AVERAGING every channel (web monoize()) — not "take channel 0".
//   * RESAMPLE to the engine rate (the web uses an OfflineAudioContext; native uses
//     juce::LagrangeInterpolator, the best interpolator JUCE ships without pulling
//     in a resampling source).
//   * CAP at 4096 samples with a 128-sample raised-cosine fade on the tail, so a
//     truncation cannot add a click / HF spray (web MAX_IR_SAMPLES / TRUNCATE_FADE).
//   * DO NOT normalize here. The engine peak-normalizes (M6.6, ClipperEngine::
//     loadCurrentCabIntoPair) exactly where the C ABI does — the engine never
//     trusts a file's level.
//
// Everything here does file I/O and allocates: MESSAGE THREAD ONLY.

#ifndef CLIPPER_NATIVE_CAB_IR_FILE_H
#define CLIPPER_NATIVE_CAB_IR_FILE_H

#include <juce_audio_formats/juce_audio_formats.h>

#include <vector>

namespace clipper::native {

// The convolver partitions at 128 and handles longer IRs fine, but a guitar cab IR
// is short; 4096 samples (~85 ms @48k) is plenty and bounds CPU/memory. Same value
// as web/src/cab.ts MAX_IR_SAMPLES.
constexpr int kMaxIrSamples = 4096;
// Raised-cosine fade applied over the last samples when we truncate.
constexpr int kIrTruncateFade = 128;
// A hard bound on how much of a file we will read before resampling. Four seconds
// is absurdly long for a cab IR and keeps a mis-picked 20-minute wav from costing
// anything.
constexpr double kMaxIrSourceSeconds = 4.0;

struct CabIrFileResult {
    bool ok = false;
    juce::String error;            // human-readable; empty on success
    juce::String label;            // file name, no extension, <= 40 chars
    std::vector<float> samples;    // MONO, at `sampleRate`, capped + faded
    double sampleRate = 0.0;       // == the engine rate that was requested
    int originalLength = 0;        // resampled length BEFORE the 4096 cap
    bool truncated = false;        // the source was longer than the cap
};

// Decode + process a user IR file into engine-rate mono samples. Never throws;
// every failure comes back as ok == false with a message fit to show a player.
CabIrFileResult loadCabIrFile(const juce::File& file, double engineRate);

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_CAB_IR_FILE_H
