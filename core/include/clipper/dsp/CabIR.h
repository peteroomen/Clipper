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

// Generate the Orange 4x12 cab IR (M10.3, docs §57): the same sealed 4x12 family
// as the Brit, voiced the other way — a LOWER low cut (bigger, woolier bottom)
// and a pronounced UPPER-MID bark around 1.2 kHz instead of the greenback's
// 200 Hz chunk and early top rolloff. Pairs with the OR120. Same deterministic
// modal recipe and M6.6 peak normalization as the other two.
std::vector<float> generateOrange4x12IR(double sampleRate);

// Peak-normalize an arbitrary IR (e.g. a user-uploaded cab) to UNITY SPECTRAL
// PEAK (max |H(f)| == 1 over the audio band) IN PLACE. The engine NEVER trusts a
// file's level (M6.6): a cab may color the tone but must never boost any band
// past the level entering it, or it pushes the output limiter and fizzes. Call
// this on any IR before handing it to a CabConvolver.
// M10.10: a small TWEED 1x8 open-back combo — the Fender Champ's own box, and the
// smallest speaker in this lineup by a wide margin. Synthesised in the §15 modal
// house style like every other cab here (never a captured third-party IR).
//
// It is voiced by what an 8" driver in a ~13x16x8" open-back pine box PHYSICALLY
// cannot do, not by taste: (a) almost no bottom — the -6 dB corner sits far above
// every 12" cab here, and an open back gives a dipole cancellation rather than the
// port reinforcement a closed 4x12 has; (b) box modes an OCTAVE UP on the 4x12s,
// because the longest internal dimension is ~16" rather than ~30"; (c) cone
// breakup HIGHER and harder, because an 8" cone is far lighter than a 12".
//
// MEASURED against the three cabs that existed before it (-6 dB corner relative to
// each cab's own spectral peak; the two tilt columns re 400 Hz):
//
//     clean212    104.7 Hz    1.9 kHz +2.45 dB    5 kHz -11.01 dB
//     brit412      75.0 Hz            -0.04         -19.06
//     orange412   103.2 Hz            +1.24         -13.98
//     tweed8      198.3 Hz            +3.04          -6.02
//
// i.e. 1.9-2.6x the low corner of any 12" cab, the most upper-mid-forward of the
// four, and the one that keeps the most top. All three are consequences of the
// driver and the box, and clipper_champ_tests asserts the ordering so the voicing
// cannot drift silently.
std::vector<float> generateTweed1x8IR(double sampleRate);

void peakNormalizeIR(std::vector<float>& ir, double sampleRate);

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_CAB_IR_H
