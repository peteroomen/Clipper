// The assistant's hands (M6). A small, typed tool surface the coach calls to
// change the rig. Each tool operates on the SAME rig-state setters the App's
// knobs/switches use (threaded in as a RigController), so when the AI acts the
// knobs visibly move and the change flows to the worklet + localStorage.
//
// Tools (exactly these):
//   set_param   {unit: pedal|amp, param, value 0..1}  — clamps
//   set_engaged {unit: pedal|amp, engaged: bool}
//   set_switch  {name: bright|cab, on: bool}

import type { RigState } from '../rig';
import type { TunerReading } from '../tuner';

export type Unit = 'pedal' | 'amp' | 'input';
export type SwitchName = 'bright' | 'cab' | 'chorus' | 'vibrato' | 'tremolo';

// The seam between the assistant and the live rig. App implements this over its
// existing setters; the tool executor only ever touches the rig through here.
// M6.4: pedal ops address a pedal INSTANCE by its 0-based chain index, and the
// chain can be edited (add/remove/move).
export interface RigController {
  getRig: () => RigState;
  // Current post-trim input peak in dBFS (null when not running), for the coach
  // to help calibrate the input trim.
  getPeakDbFs?: () => number | null;
  // Latest tuner reading (M7), null when no tuner is engaged / no clear pitch, so
  // the coach can say "you're a few cents flat on the G".
  getTunerReading?: () => TunerReading | null;
  // Apply a normalized param; returns the clamped value actually applied.
  // pedalIndex targets a specific pedal instance (default 0) when unit==='pedal'.
  setParam: (unit: Unit, param: string, value: number, pedalIndex?: number) => number;
  setEngaged: (unit: Unit, engaged: boolean, pedalIndex?: number) => void;
  setSwitch: (name: SwitchName, on: boolean) => void;
  // Cab expansion: switch between the BUILT-IN cabs
  // ('clean212' | 'brit412' | 'orange412').
  // Never selects 'custom' (that needs a user file upload).
  setCab: (cab: 'clean212' | 'brit412' | 'orange412') => void;
  // M9.4/M10.1/v1.1: swap the amp voice ('clean120' | 'jcm800' | 'twin' | 'ac30').
  setAmp: (
    type: 'clean120' | 'jcm800' | 'twin' | 'ac30' | 'orange' | 'rockerverb'
  ) => void;
  // Chain edits (M6.4). addPedal returns the new instance's chain index.
  addPedal: (type: string, position?: number) => number;
  removePedal: (index: number) => void;
  movePedal: (from: number, to: number) => void;
}

// JSON-schema tool definitions sent to the API (name/description/input_schema).
// Kept deterministic (stable order, additionalProperties:false) so they cache.
export const TOOLS = [
  {
    name: 'set_param',
    description:
      "Set a continuous knob on the rig to an absolute normalized value (0..1). " +
      "RAT pedal params: 'dist' (0=clean, 1=max saturation), 'filter' (0=bright, " +
      "1=dark — the RAT filter is a post-clipping low-pass, so clockwise/higher " +
      "tames fizz without changing how hard it clips), 'level' (output volume). " +
      "SD-1 pedal params: 'drive' (0..1 overdrive amount), 'tone' (0=dark .. 1=bright, " +
      "0.5=flat — a treble tilt), 'level'. (For an SD-1 you may use 'dist' as an " +
      "alias for 'drive' and 'filter' for 'tone' — same slots.) " +
      "Phaser pedal param: 'speed' (its ONE knob — the LFO sweep rate; low = a " +
      "slow tape-warble, high = a fast Leslie-ish swirl). " +
      "Amp params: 'volume', 'bass', 'middle', 'treble' (tone controls are flat " +
      "at 0.5), plus the chorus/vibrato modulation 'speed' (LFO rate ~0.15-8 Hz) " +
      "and 'depth' (sweep amount / how deep the pitch wobble is) — these only do " +
      "something when the chorus or vibrato mode is on (set that with set_switch). " +
      "'reverb' is the JC-120 spring reverb MIX (0 = fully dry, 1 = drenched); the " +
      "decay is fixed (~1.5 s, spring-flavored) so this one knob is just how much " +
      "wet you blend in. Keep it low (10-30) for clarity and note definition, " +
      "higher for ballads/ambient washes. " +
      "JCM800 amp params (only when the JCM is the active amp — see set_amp): " +
      "'gain' (preamp drive — how much dirt/saturation the front end makes), " +
      "'master' (the power-amp drive — pushes the phase inverter / EL34s into " +
      "power-tube compression and grit; the loud, cranked-Marshall character lives " +
      "here, so gain vs master is preamp-dirt vs power-amp-feel), 'presence' (a " +
      "power-amp HIGH-frequency lift via the feedback loop — sharpens attack/edge " +
      "on top; distinct from 'treble', which shapes the preamp tone stack BEFORE " +
      "the distortion, while presence brightens the finished power-amp sound AFTER " +
      "it). The JCM also uses 'bass'/'middle'/'treble' (its Marshall tone stack) but " +
      "IGNORES volume/speed/depth/reverb (a real 2204 has no chorus, reverb, or " +
      "separate volume). " +
      "AC30 'top boost' amp params (only when the AC30 is the active amp — see set_amp): " +
      "it uses 'volume' (which IS its overdrive — crank for class-A grind), 'bass'/" +
      "'treble' (the top-boost tone stack; NO 'middle'), 'reverb', and REUSES the " +
      "'presence' param as its top CUT control — INVERTED, so HIGHER 'presence' = " +
      "DARKER/smoother (tames the top without losing chime). It IGNORES middle/gain/" +
      "master/bright/chorus. " +
      "Orange OR120 amp params (only when the Orange is the active amp — see set_amp): " +
      "it uses 'volume' (which IS the whole amp — there is NO master, so the power " +
      "section is the overdrive and the knob position IS the amount of dirt), 'bass'/" +
      "'treble' (a James/Baxandall stack: NO 'middle', and turning BOTH down leaves the " +
      "mids standing — that is how you get the Orange midrange honk), 'fac' (the " +
      "six-position F.A.C. rotary — 0 is the fattest and each click to the right takes " +
      "away bass AND gain; use it to tighten a woolly low end instead of pulling the " +
      "bass knob down), 'presence' REUSED as its H.F. BOOST (a RESONANT top-end peak " +
      "around 5 kHz at the driver's cathode, not a feedback shelf — the Orange has no " +
      "bright switch, this is it), and 'reverb'. Its panel prints slot 0 as GAIN. It " +
      "IGNORES middle/gain/master/bright/chorus. " +
      "Rockerverb amp params (only when the Rockerverb is the active amp — see " +
      "set_amp): it is the one Orange in this rig with BOTH a 'gain' and a 'master', " +
      "and they are INDEPENDENT — 'gain' sets how dirty (four cascaded valve stages " +
      "behind one ganged pot) and 'master' sets how loud (a real post-tone-stack " +
      "volume). That is the whole point of the amp: at the SAME output level it makes " +
      "about 16x the distortion the OR120 does, so it is the voice to reach for when " +
      "someone wants saturation at a survivable volume. Its 'master' is a LINEAR pot, " +
      "so the useful range is the bottom of the travel — 5-15 is a room level, and it " +
      "stops getting louder above about 20 because the power valves are already flat " +
      "out. It also has 'bass'/'middle'/'treble' (a full Marshall-lineage stack, so " +
      "unlike the OR120 it HAS a mid control) and 'reverb' (a real one — it is in the " +
      "amp's name). It IGNORES volume/presence/fac/bright/chorus. " +
      "Input params: 'trim' — the rig-level INPUT gain BEFORE the pedal " +
      "(0..1 maps to -12..+24 dB, 1/3 = 0 dB). Raise it when the input peak is " +
      "weak (below ~-12 dBFS) so the guitar actually drives the diodes; lower it " +
      "if it's hot (near 0 dBFS). The knob moves visibly and the change is heard " +
      'immediately.',
    input_schema: {
      type: 'object',
      properties: {
        unit: { type: 'string', enum: ['pedal', 'amp', 'input'] },
        param: {
          type: 'string',
          enum: [
            'dist',
            'drive',
            'sustain',
            'filter',
            'tone',
            'level',
            'volume',
            'bass',
            'middle',
            'treble',
            'speed',
            'depth',
            'reverb',
            'gain',
            'presence',
            'master',
            'fac',
            'trim',
          ],
        },
        value: { type: 'number', minimum: 0, maximum: 1 },
        pedal: {
          type: 'integer',
          minimum: 0,
          description:
            "Which pedal INSTANCE (0-based position in the chain) when unit is 'pedal'. " +
            'Defaults to 0 (the first pedal). Ignored for amp/input.',
        },
      },
      required: ['unit', 'param', 'value'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_engaged',
    description:
      'Engage or bypass a unit. For a pedal, engaged=false is true bypass ' +
      '(clean signal, LED dark) — address a specific pedal instance with `pedal`. ' +
      'For the amp, engaged=false powers the amp+cab off (signal passes straight ' +
      'through).',
    input_schema: {
      type: 'object',
      properties: {
        unit: { type: 'string', enum: ['pedal', 'amp'] },
        engaged: { type: 'boolean' },
        pedal: {
          type: 'integer',
          minimum: 0,
          description: "Which pedal instance (0-based) when unit is 'pedal'. Defaults to 0.",
        },
      },
      required: ['unit', 'engaged'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_switch',
    description:
      "Flip an amp switch. 'bright' adds a high-frequency shelf (extra sparkle/" +
      "presence). 'cab' toggles the 2x12 speaker cabinet simulation (off = raw, " +
      "fizzier, no speaker rolloff). 'chorus' and 'vibrato' select the JC-120's " +
      "modulation: turning 'chorus' on gives the lush dry-left / wet-right stereo " +
      "bloom, 'vibrato' on gives a true pitch wobble on both sides; these two are " +
      "mutually exclusive (turning either off returns to no modulation). " +
      "'tremolo' is the Twin's on/off for its optical tremolo (default off; only " +
      "meaningful when amp.type is 'twin'). Use set_param 'speed'/'depth' to " +
      "shape the movement once one is on.",
    input_schema: {
      type: 'object',
      properties: {
        name: { type: 'string', enum: ['bright', 'cab', 'chorus', 'vibrato', 'tremolo'] },
        on: { type: 'boolean' },
      },
      required: ['name', 'on'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_cab',
    description:
      "Choose the speaker CABINET. Two built-ins: 'clean212' — the Clean 2×12, " +
      'the flat, open clean platform (pairs with the JC-120 amp); and ' +
      "'brit412' — a Marshall-style 4×12, thicker in the low-mids and noticeably " +
      'DARKER on top (a greenback-ish voicing), the classic rock/JCM cab. Reach ' +
      "for brit412 when the player wants a thicker, darker, rock voicing, and " +
      'clean212 for the pristine clean platform. And ' +
      "'orange412' — an Orange-style " +
      '4×12: the biggest, woolliest bottom of the three (its -6 dB low corner reaches ' +
      '9 Hz lower than the Brit) with a pronounced UPPER-MID BARK around 1.2 kHz. It is ' +
      "the OR120's own cab and the natural pairing for the Overdrive 120. " +
      'This selects WHICH cab; the ' +
      "'cab' switch (set_switch) still bypasses the cab entirely. You cannot pick " +
      'a user-uploaded custom IR — the player loads that themselves from the amp menu.',
    input_schema: {
      type: 'object',
      properties: {
        cab: { type: 'string', enum: ['clean212', 'brit412', 'orange412'] },
      },
      required: ['cab'],
      additionalProperties: false,
    },
  },
  {
    name: 'set_amp',
    description:
      'Choose the AMP head. Five voices: ' +
      "'clean120' — the JC-120-style solid-state CLEAN platform (linear; all the " +
      'dirt comes from the pedals in front; has the bright switch, stereo chorus/' +
      'vibrato, and spring reverb). ' +
      "'jcm800' — a Marshall JCM800 2204: a real VALVE head with its own preamp " +
      'distortion (4× 12AX7 cascade) and cranked EL34 power-amp grit. It is a MONO ' +
      'head with GAIN + MASTER + a Marshall bass/mid/treble tone stack + PRESENCE + ' +
      'a spring REVERB, and no chorus/bright/volume. The canonical move is an SD-1 ' +
      'boosting a cranked JCM; pairs best with the Brit 4×12 cab. ' +
      "'twin' — a Fender blackface 'Twin-style' combo: the CLEAN-HEADROOM KING. A " +
      'valve amp (2× 12AX7 → Fender tone stack → 6L6 power) that stays glassy and ' +
      'clean until pushed hard, then breaks up late from the power stage. It has ' +
      'VOLUME + a scooped Fender bass/mid/treble + a BRIGHT switch + a period-correct ' +
      'spring REVERB + an optical TREMOLO (the SPEED/INTENSITY knobs — a "vibrato" on ' +
      'the panel but really amplitude tremolo). Reach for the Twin for pristine cleans, ' +
      'shimmer, surf, and the classic reverb-and-tremolo combo; the BRIGHT switch bites ' +
      'at low volume. A real Twin is a 2×12, so it pairs best with the Clean 2×12 cab. ' +
      "'ac30' — a 'top boost' class-A combo: the CHIME/JANGLE voice (bright, glassy, " +
      'harmonically rich). A valve amp whose VOLUME knob IS the overdrive — crank the ' +
      'volume for the class-A grind and shimmer (it is not a clean-only amp and there is ' +
      'no separate gain knob). It has VOLUME + a bass/treble TOP-BOOST stack + a CUT ' +
      'control + a spring REVERB. CUT tames the top WITHOUT losing the chime the way ' +
      'pulling treble down would — and it is INVERTED: higher CUT = darker. No middle/' +
      'gain/master/bright/chorus. The chime-and-jangle of a Rickenbacker into a top-boost ' +
      'combo, from the Beatles through Britpop to Radiohead. ' +
      "'orange' — an early-70s Orange OR120 'Overdrive' head: the MID-FORWARD voice, and " +
      'the counterweight to the JCM800. Same EL34 push-pull power section, but a ' +
      'CATHODYNE phase inverter (stiffer, punchier, fuzzier than a long-tail pair) and a ' +
      'James/Baxandall tone stack that leaves the midrange STANDING where a Marshall ' +
      'stack scoops it — measured, its tone network sits 8.3 dB more mid-forward than the ' +
      "JCM's at noon. There is NO MASTER: the single GAIN knob is the whole amp, clean in " +
      'the bottom third and roaring at the top, so set the dirt with it (it writes the ' +
      "shared 'volume' slot). It has GAIN + BASS/TREBLE (no middle) + the six-position " +
      'F.A.C. rotary (clicks bass and gain away as it climbs) + H.F. BOOST (the presence ' +
      'slot) + REVERB. Reach for it for thick, ' +
      'woolly, midrange-forward British rock and doom/stoner weight — where a JCM cuts, ' +
      'this one shoves. Pairs with the Orange 4×12. ' +
      "'rockerverb' — the MODERN Orange (Rockerverb 100, dirty channel): the same " +
      'maker as the OR120 and the deliberate counterweight to it. FOUR cascaded valve ' +
      'stages behind one ganged GAIN pot, a Marshall-lineage tone stack (so it HAS a ' +
      'MIDDLE, and it is mid-SCOOPED where the OR120 is mid-forward — measured, the ' +
      'two networks sit 6.8 dB apart on the same scale), a real MASTER volume after ' +
      'the tone stack, and an authentic valve-driven spring REVERB. The master is what ' +
      'makes it different to play: GAIN and level are independent, so it can be filthy ' +
      'and quiet — level-matched against the OR120 at the same output it makes about ' +
      '16x the distortion. Reach for it for modern heavy: doom, sludge, stoner and ' +
      'anything that wants saturation without the room volume of a cranked ' +
      'non-master amp. The master is a LINEAR pot so keep it low (5-15). Pairs with ' +
      'the Orange 4×12, the same cab as the OR120. Switching is click-free; the ' +
      'cab and pedals carry over.',
    input_schema: {
      type: 'object',
      properties: {
        type: {
          type: 'string',
          enum: ['clean120', 'jcm800', 'twin', 'ac30', 'orange', 'rockerverb'],
        },
      },
      required: ['type'],
      additionalProperties: false,
    },
  },
  {
    name: 'add_pedal',
    description:
      'Add a pedal to the chain. Signal runs guitar -> pedals in order -> amp, so ' +
      'ORDER matters: a dirt box earlier vs later in the chain hits the amp ' +
      "differently. Types: 'rat' (hard, aggressive, symmetric clipping — the " +
      "scooped, cutting RAT), 'sd1' (a Boss SD-1: soft, warm, ASYMMETRIC " +
      'overdrive with a mid-hump — the classic transparent boost, great in front ' +
"of another dirt or a cranked amp), 'ts' (a TS808 'Screamer' — the GREEN " +
      'box: soft, SYMMETRIC clipping with the same ~720 Hz mid-hump but a smoother, ' +
      'glassier grind and less top-end gain; THE stacking pedal — low drive + high ' +
      'level as a mid-forward clean boost into a pushed amp), ' +
      "'muff' (a big-box FUZZ, the violet 'Pi' — a four-transistor wall of sustain: " +
      'MASSIVE, thick, endlessly-sustaining saturation from cascaded diode clipping, ' +
      'with a signature mid-SCOOP tone control; this is FUZZ, not overdrive, and far ' +
      'more compressed/saturated than the RAT/SD-1/TS — its knobs are SUSTAIN, TONE, ' +
      'VOLUME. Sounds best into a CLEAN amp with headroom, e.g. the Twin), ' +
      "'gold' (the GOLD 'Myth' overdrive — the gold-box legend: it blends a " +
      'full-bandwidth CLEAN signal in parallel with a germanium-clipped one, and its ' +
      'GAIN knob cross-fades between them, so at low gain it is famously TRANSPARENT ' +
      '(a clean boost that keeps your amp and guitar sounding like themselves) and at ' +
      'high gain it is thick but still articulate. Knobs are GAIN, TREBLE, OUTPUT; ' +
      'huge headroom, so it is the classic always-on pedal and the classic way to shove ' +
      'an already-breaking-up amp over the edge), ' +
      "'comp' (the 'Squash' COMPRESSOR — the only DYNAMICS pedal here, not a dirt " +
      'box: an OTA compressor of the classic red-box/Ross school with just two ' +
      'knobs, SUSTAIN and LEVEL. It evens out volume, makes quiet notes bigger and ' +
      'loud ones smaller, and adds long, singing sustain — the country/funk chicken-' +
      'picking pop, and the "every note the same size" clean lead. It is a LIMITER ' +
      'more than a gentle studio compressor: above its threshold the output barely ' +
      'moves. SUSTAIN is NOT a threshold — it sets how much gain sits in front of a ' +
      'FIXED threshold, so turning it up buys more squash, more make-up gain AND ' +
      'more hiss together, and it also squashes the pick attack harder. Put it ' +
      "FIRST in the chain, before the dirt), " +
      "'gate' (the 'Curfew' NOISE GATE — a utility, not a voice: it makes no " +
      'sound of its own, it takes one away. Two knobs, THRESHOLD and DECAY. ' +
      'THRESHOLD really IS a threshold (unlike the compressor\'s SUSTAIN): below ' +
      'it the gate shuts, above it the pedal is UNITY and does nothing at all. ' +
      'DECAY sets how fast the sound fades once it shuts. It is what makes a ' +
      'high-gain rig playable — silence between phrases, and no hiss riding under ' +
      'the palm mutes. Put it AFTER the dirt (or in an effects loop), never ' +
      "before it), " +
      "'phaser' (a script-era 4-stage phaser — the classic swirling/whooshing " +
      'modulation with ONE knob, SPEED: placed AFTER the dirt it gives the vocal ' +
      'EVH swoosh, before the dirt it is subtler; slow = tape-warble, fast = ' +
      "Leslie-ish shimmer), " +
      "'wah' (the 'Weeper' — a Cry-Baby-style WAH and the rig's first FILTER " +
      'pedal: a sharp resonant peak sweeping ~450 Hz to ~2.25 kHz, the vowel/voice ' +
      'effect. Its knobs are POSITION (heel 0 -> toe 100 — the treadle itself, an ' +
      'ordinary parameter you can set or automate), SENSE (0 = a plain manual wah; ' +
      'above 0 an ENVELOPE FOLLOWER sweeps the same filter from your picking, i.e. ' +
      'an auto-wah/envelope filter) and VOICE (how NARROW the peak is — the classic ' +
      '"vocal mod": low = broad and vowel-less, high = sharp and talkative). ' +
      'Placement matters: BEFORE the dirt is the classic funk/rhythm wah, AFTER the ' +
      'dirt is the screaming lead wah), ' +
      "'delay' (the 'Echoman' — a Deluxe-Memory-Man-style BUCKET-BRIGADE ANALOG " +
      'DELAY, and the rig\'s first delay of any kind. Its knobs are DELAY (the echo ' +
      'time, 30 ms to 550 ms, and on a bucket brigade this knob is the CLOCK, so a ' +
      'longer setting also makes the repeats DARKER), FEEDBACK (how many repeats; ' +
      'at the top it self-oscillates into a bounded swirl) and BLEND (wet level; at ' +
      '0 the pedal is bit-exactly your dry signal). The repeats get darker and ' +
      'blurrier every pass, which is the whole point of an analog delay — put it ' +
      'LAST, after the dirt, so it echoes the distorted note rather than ' +
      'distorting the echoes), ' +
      "and 'tuner' (a chromatic tuner — no " +
      'tone, but when stomped ON it MUTES the rig so the player can tune in ' +
      'silence; put it first in the chain by convention), ' +
      "'chorus' (the 'Ensemble' CHORUS — MODULATION, not dirt: the world's first " +
      'chorus pedal, and literally the JC-120 amp\'s own chorus circuit in a floor ' +
      'box. Knobs are RATE, DEPTH and MODE. MODE is a two-position switch: below ' +
      'halfway it is CHORUS (a lush, wide 1970s shimmer — dry and detuned-wet mixed ' +
      'together, which is what makes it sound like two guitars slightly out of tune ' +
      'with each other), at or above halfway it is VIBRATO (the dry signal is gone ' +
      'entirely, so you get real seasick pitch wobble instead of shimmer). The two ' +
      'modes also use different, NON-OVERLAPPING speed ranges — chorus is slow ' +
      '(about 1-3 Hz) and vibrato starts faster than chorus ever gets, so switching ' +
      'mode is a bigger change than moving RATE. Note DEPTH never fully switches ' +
      'the effect off, exactly like the real pedal). Omit `position` to ' +
      'append at the end (just before the amp), or give a 0-based slot to insert.',
    input_schema: {
      type: 'object',
      properties: {
        type: { type: 'string', enum: ['rat', 'sd1', 'ts', 'muff', 'gold', 'comp', 'gate', 'phaser', 'wah', 'chorus', 'delay', 'tuner'] },
        position: { type: 'integer', minimum: 0 },
      },
      required: ['type'],
      additionalProperties: false,
    },
  },
  {
    name: 'remove_pedal',
    description: 'Remove the pedal instance at the given 0-based chain index.',
    input_schema: {
      type: 'object',
      properties: { index: { type: 'integer', minimum: 0 } },
      required: ['index'],
      additionalProperties: false,
    },
  },
  {
    name: 'move_pedal',
    description:
      'Reorder the chain: move the pedal at 0-based index `from` to index `to`. ' +
      'Use this to change how pedals stack (e.g. put a booster before or after ' +
      'the dirt).',
    input_schema: {
      type: 'object',
      properties: {
        from: { type: 'integer', minimum: 0 },
        to: { type: 'integer', minimum: 0 },
      },
      required: ['from', 'to'],
      additionalProperties: false,
    },
  },
] as const;

// Maps the assistant's short param name to the RigState param name the setters
// expect. Pedal 'dist' -> 'distortion'; everything else is 1:1.
const PEDAL_PARAM: Record<string, string> = {
  dist: 'distortion',
  drive: 'distortion', // SD-1 Drive shares the distortion slot (id 0)
  speed: 'distortion', // phaser SPEED is the one knob — also slot 0
  position: 'distortion', // wah POSITION (heel->toe) shares slot 0
  sense: 'filter', // wah SENSITIVITY (0 = manual pedal) shares slot 1
  voice: 'level', // wah VOICE (peak width, the "vocal mod") shares slot 2
  delay: 'distortion', // delay TIME (the BBD clock) shares slot 0
  time: 'distortion', // ... and its natural synonym
  feedback: 'filter', // delay FEEDBACK (repeats) shares slot 1
  repeats: 'filter', // ... and its natural synonym
  blend: 'level', // delay BLEND (wet level) shares slot 2
  mix: 'level', // ... and its natural synonym
  sustain: 'distortion', // Muff SUSTAIN shares the distortion slot (id 0)
  filter: 'filter',
  tone: 'filter', // SD-1/Muff Tone shares the filter slot (id 1)
  level: 'level',
  volume: 'level', // Muff VOLUME shares the level slot (id 2)
};
const AMP_PARAM: Record<string, string> = {
  volume: 'volume',
  bass: 'bass',
  middle: 'middle',
  treble: 'treble',
  speed: 'speed',
  depth: 'depth',
  reverb: 'reverb',
  // M9.4 JCM800 knobs (1:1).
  gain: 'gain',
  presence: 'presence',
  master: 'master',
  // M10.3 Orange OR120 (1:1).
  fac: 'fac',
};
const INPUT_PARAM: Record<string, string> = {
  trim: 'trim',
};

// A short, human-readable label for a param (for the in-flow chips).
const PARAM_LABEL: Record<string, string> = {
  dist: 'Dist',
  drive: 'Drive',
  sustain: 'Sustain',
  filter: 'Filter',
  tone: 'Tone',
  level: 'Level',
  volume: 'Vol',
  bass: 'Bass',
  middle: 'Mid',
  treble: 'Treble',
  speed: 'Speed',
  position: 'Position',
  sense: 'Sense',
  voice: 'Voice',
  depth: 'Depth',
  reverb: 'Reverb',
  gain: 'Gain',
  presence: 'Presence',
  master: 'Master',
  fac: 'F.A.C.',
  trim: 'Trim',
};

// NaN-REJECTING clamp (2026-07-24 audit, finding 1). `typeof NaN === 'number'` is
// true and `Math.min(1, Math.max(0, NaN))` is NaN, so the old form let a NaN
// straight through to the engine, where it latched permanently in recursive state.
// This file declares `minimum: 0, maximum: 1` in the tool JSON schema but the
// schema is not enforced at runtime, so any non-numeric model emission ("max",
// null, {}) arrives here as NaN — this is the boundary that has to catch it.
const clamp01 = (n: number) => {
  const v = typeof n === 'number' && Number.isFinite(n) ? n : 0;
  return Math.min(1, Math.max(0, v));
};
const to100 = (n: number) => Math.round(clamp01(n) * 100);

// Resolve a tool's optional `pedal` (0-based instance index) to a valid chain
// index, clamped to [0, count-1] (0 when the chain is empty).
function pedalIndexOf(input: Record<string, unknown>, count: number): number {
  const raw = input.pedal;
  const i = typeof raw === 'number' && Number.isFinite(raw) ? raw | 0 : 0;
  if (count <= 0) return 0;
  return Math.min(count - 1, Math.max(0, i));
}

export interface ToolExecution {
  // The tool_result content string returned to the model (a short applied JSON).
  content: string;
  // A compact chip label rendered in the chat flow, e.g. "Dist 70 → 55".
  chip: string;
}

// Execute one tool_use locally against the live rig. Reads current state to
// compute the "from" value for the chip, applies via the controller, and
// returns both the model-facing tool_result content and a UI chip label.
export function executeTool(
  controller: RigController,
  name: string,
  input: Record<string, unknown>
): ToolExecution {
  if (name === 'set_param') {
    const unit: Unit =
      input.unit === 'amp' ? 'amp' : input.unit === 'input' ? 'input' : 'pedal';
    const param = String(input.param ?? '');
    const target = clamp01(Number(input.value));
    const rig = controller.getRig();
    const map = unit === 'pedal' ? PEDAL_PARAM : unit === 'amp' ? AMP_PARAM : INPUT_PARAM;
    const rigParam = map[param];
    if (!rigParam) {
      return {
        content: JSON.stringify({ error: `unknown ${unit} param '${param}'` }),
        chip: `? ${param}`,
      };
    }
    const pedalIndex = pedalIndexOf(input, rig.pedals.length);
    let params: Record<string, number>;
    if (unit === 'pedal') {
      const inst = rig.pedals[pedalIndex];
      if (!inst) {
        return {
          content: JSON.stringify({ error: `no pedal at index ${pedalIndex}` }),
          chip: `? pedal ${pedalIndex + 1}`,
        };
      }
      params = inst.params as unknown as Record<string, number>;
    } else if (unit === 'amp') {
      params = rig.amp.params as unknown as Record<string, number>;
    } else {
      params = rig.input as unknown as Record<string, number>;
    }
    const from = to100(params[rigParam] ?? 0);
    const applied = controller.setParam(unit, rigParam, target, pedalIndex);
    const to = to100(applied);
    // Tag the chip with the pedal position when there is more than one pedal.
    const posTag = unit === 'pedal' && rig.pedals.length > 1 ? ` #${pedalIndex + 1}` : '';
    return {
      content: JSON.stringify({ applied: { unit, param, value: applied, pedal: unit === 'pedal' ? pedalIndex : undefined } }),
      chip: `${PARAM_LABEL[param] ?? param}${posTag} ${from} → ${to}`,
    };
  }

  if (name === 'set_engaged') {
    const unit = input.unit === 'amp' ? 'amp' : 'pedal';
    const engaged = Boolean(input.engaged);
    const rig = controller.getRig();
    const pedalIndex = pedalIndexOf(input, rig.pedals.length);
    controller.setEngaged(unit, engaged, pedalIndex);
    const posTag = unit === 'pedal' && rig.pedals.length > 1 ? ` #${pedalIndex + 1}` : '';
    const label = unit === 'pedal' ? `Pedal${posTag}` : 'Amp';
    const state =
      unit === 'pedal' ? (engaged ? 'engaged' : 'bypassed') : engaged ? 'on' : 'off';
    return {
      content: JSON.stringify({ applied: { unit, engaged, pedal: unit === 'pedal' ? pedalIndex : undefined } }),
      chip: `${label} ${state}`,
    };
  }

  if (name === 'add_pedal') {
    const type: 'rat' | 'sd1' | 'ts' | 'muff' | 'gold' | 'comp' | 'gate' | 'phaser' | 'wah' | 'chorus' | 'delay' | 'tuner' =
      input.type === 'tuner' ? 'tuner'
      : input.type === 'sd1' ? 'sd1'
      : input.type === 'ts' ? 'ts'
      : input.type === 'muff' ? 'muff'
      : input.type === 'gold' ? 'gold'
      : input.type === 'comp' ? 'comp'
      : input.type === 'gate' ? 'gate'
      : input.type === 'phaser' ? 'phaser'
      : input.type === 'wah' ? 'wah'
      : input.type === 'chorus' ? 'chorus'
      : input.type === 'delay' ? 'delay'
      : 'rat';
    const rawPos = input.position;
    const position =
      typeof rawPos === 'number' && Number.isFinite(rawPos) ? Math.max(0, rawPos | 0) : undefined;
    const index = controller.addPedal(type, position);
    const label =
      type === 'tuner' ? 'Tuner'
        : type === 'sd1' ? 'SD-1'
          : type === 'ts' ? 'Screamer'
            : type === 'muff' ? 'Pi Fuzz'
              : type === 'gold' ? 'Myth'
                : type === 'comp' ? 'Squash'
                : type === 'gate' ? 'Curfew'
                  : type === 'phaser' ? 'Phaser'
                    : type === 'wah' ? 'Weeper'
                    : type === 'chorus' ? 'Ensemble'
                    : type === 'delay' ? 'Echoman'
                    : 'RAT';
    return {
      content: JSON.stringify({ applied: { added: type, index } }),
      chip: `+ ${label} #${index + 1}`,
    };
  }

  if (name === 'remove_pedal') {
    const rig = controller.getRig();
    const index = Math.max(0, Number(input.index) | 0);
    if (index >= rig.pedals.length) {
      return {
        content: JSON.stringify({ error: `no pedal at index ${index}` }),
        chip: `? remove ${index + 1}`,
      };
    }
    controller.removePedal(index);
    return {
      content: JSON.stringify({ applied: { removed: index } }),
      chip: `− Pedal #${index + 1}`,
    };
  }

  if (name === 'move_pedal') {
    const rig = controller.getRig();
    const from = Math.max(0, Number(input.from) | 0);
    const to = Math.max(0, Number(input.to) | 0);
    if (from >= rig.pedals.length) {
      return {
        content: JSON.stringify({ error: `no pedal at index ${from}` }),
        chip: `? move ${from + 1}`,
      };
    }
    controller.movePedal(from, to);
    return {
      content: JSON.stringify({ applied: { moved: from, to } }),
      chip: `Move #${from + 1} → #${to + 1}`,
    };
  }

  if (name === 'set_cab') {
    const cab =
      input.cab === 'brit412' ? 'brit412'
      : input.cab === 'orange412' ? 'orange412'
      : 'clean212';
    controller.setCab(cab);
    const cabName =
      cab === 'brit412' ? 'Brit 4×12'
      : cab === 'orange412' ? 'Orange 4×12'
      : 'Clean 2×12';
    return {
      content: JSON.stringify({ applied: { cab } }),
      chip: `Cab ${cabName}`,
    };
  }

  if (name === 'set_amp') {
    const type =
      input.type === 'jcm800' ? 'jcm800'
      : input.type === 'twin' ? 'twin'
      : input.type === 'ac30' ? 'ac30'
      : input.type === 'orange' ? 'orange'
      : input.type === 'rockerverb' ? 'rockerverb'
      : 'clean120';
    controller.setAmp(type);
    const chipName =
      type === 'jcm800' ? 'JCM800'
      : type === 'twin' ? 'Twin Sixty-Five'
      : type === 'ac30' ? 'Thirty'
      : type === 'orange' ? 'Overdrive 120'
      : type === 'rockerverb' ? 'Rocker Verb'
      : 'Clean 120';
    return {
      content: JSON.stringify({ applied: { amp: type } }),
      chip: `Amp ${chipName}`,
    };
  }

  if (name === 'set_switch') {
    const valid: SwitchName[] = ['bright', 'cab', 'chorus', 'vibrato', 'tremolo'];
    const sw = (valid.includes(input.name as SwitchName) ? input.name : 'bright') as SwitchName;
    const on = Boolean(input.on);
    controller.setSwitch(sw, on);
    const label = sw.charAt(0).toUpperCase() + sw.slice(1);
    return {
      content: JSON.stringify({ applied: { name: sw, on } }),
      chip: `${label} ${on ? 'on' : 'off'}`,
    };
  }

  return { content: JSON.stringify({ error: `unknown tool '${name}'` }), chip: '? tool' };
}
