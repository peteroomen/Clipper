// Tiny tactile UI sounds — knob ticks and footswitch thunks — ported verbatim
// in spirit from the approved design artifact. These run on their OWN
// lightweight AudioContext, created lazily on first interaction (a user
// gesture, so it never trips the "AudioContext was not allowed to start"
// warning). This context is completely separate from the audio-processing
// graph in audio.ts — it must NEVER be injected into the pedal signal path.

let ac: AudioContext | null = null;

function ctx(): AudioContext | null {
  if (!ac) {
    try {
      ac = new (window.AudioContext ||
        (window as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext)();
    } catch {
      ac = null;
    }
  }
  if (ac && ac.state === 'suspended') void ac.resume();
  return ac;
}

// A very quiet high-frequency click as a knob crosses a detent.
export function tick(): void {
  const a = ctx();
  if (!a) return;
  const t = a.currentTime;
  const o = a.createOscillator();
  const g = a.createGain();
  o.type = 'square';
  o.frequency.value = 2100;
  g.gain.setValueAtTime(0.015, t);
  g.gain.exponentialRampToValueAtTime(0.0001, t + 0.015);
  o.connect(g).connect(a.destination);
  o.start(t);
  o.stop(t + 0.02);
}

// A soft low thunk for the footswitch press (down) / release (up).
export function thunk(down: boolean): void {
  const a = ctx();
  if (!a) return;
  const t = a.currentTime;
  const o = a.createOscillator();
  const g = a.createGain();
  o.type = 'sine';
  o.frequency.setValueAtTime(down ? 95 : 120, t);
  o.frequency.exponentialRampToValueAtTime(45, t + 0.06);
  g.gain.setValueAtTime(0.12, t);
  g.gain.exponentialRampToValueAtTime(0.0001, t + 0.09);
  o.connect(g).connect(a.destination);
  o.start(t);
  o.stop(t + 0.1);
}
