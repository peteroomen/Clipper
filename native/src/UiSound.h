// Tiny tactile UI sounds — the native port of web/src/ui-sound.ts (knob ticks and
// footswitch thunks), synthesis constants carried over verbatim:
//
//   tick()      a very quiet 2100 Hz square, gain 0.015 → 0.0001 over 15 ms
//   thunk(down) a soft sine drop, 95 Hz (down) / 120 Hz (up) → 45 Hz over 60 ms,
//               gain 0.12 → 0.0001 over 90 ms
//
// Like the web (which runs these on their OWN AudioContext), this engine is
// completely separate from the processing graph: it opens its own output-only
// juce::AudioDeviceManager, lazily, on the first sound. It must NEVER be wired
// into the pedal signal path.
//
// STANDALONE ONLY. A plugin must not open a second audio device inside a DAW
// (exclusive-mode drivers, added device churn), so in any hosted wrapper both
// calls are silent no-ops. If the device fails to open (no audio hardware — e.g.
// a headless container), the engine disables itself quietly.

#ifndef CLIPPER_NATIVE_UI_SOUND_H
#define CLIPPER_NATIVE_UI_SOUND_H

namespace clipper::native::uisound {

// The web's knob detent width: a tick fires when the value has moved 0.04 from
// the last ticked value (ui-sound.ts via Knob.tsx TICK_DETENT).
inline constexpr double kTickDetent = 0.04;

void tick();
void thunk(bool down);

}  // namespace clipper::native::uisound

#endif  // CLIPPER_NATIVE_UI_SOUND_H
