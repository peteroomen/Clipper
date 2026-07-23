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

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_CAB_IR_H
