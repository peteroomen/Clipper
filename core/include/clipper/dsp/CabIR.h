// Clipper — portable DSP core (M5).
//
// Procedural default cab impulse response: a plausible closed-back 2x12 guitar
// cabinet, generated deterministically (seeded — no Date/random_device) so tests
// can rely on it byte-for-byte. This is a documented PLACEHOLDER until real
// user-uploadable IRs land; see CabIR.cpp for the generator recipe and the
// measured frequency response.
//
// Platform-free C++17.

#ifndef CLIPPER_DSP_CAB_IR_H
#define CLIPPER_DSP_CAB_IR_H

#include <vector>

namespace clipper::dsp {

// Generate the default 2x12 cab IR at the given sample rate (48 kHz nominal).
// Deterministic: identical output for identical sampleRate every call.
std::vector<float> generateDefaultCab2x12IR(double sampleRate);

// Generate the Brit 4x12 cab IR (a closed-back Marshall-style greenback voicing:
// thicker low-mids, darker top; pairs with the JCM800). Same deterministic modal
// recipe and M6.6 peak normalization as the 2x12.
std::vector<float> generateBrit4x12IR(double sampleRate);

// Peak-normalize an arbitrary IR (e.g. a user-uploaded cab) to UNITY SPECTRAL
// PEAK (max |H(f)| == 1 over the audio band) IN PLACE. The engine NEVER trusts a
// file's level (M6.6): a cab may color the tone but must never boost any band
// past the level entering it, or it pushes the output limiter and fizzes. Call
// this on any IR before handing it to a CabConvolver.
void peakNormalizeIR(std::vector<float>& ir, double sampleRate);

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_CAB_IR_H
