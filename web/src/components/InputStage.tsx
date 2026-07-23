// InputStage (M6.1) — the rig-header input calibration strip: a small neumorphic
// Trim knob (rig-level gain applied BEFORE the pedal, for matching real-world
// interface levels into the model) plus a peak meter fed by the worklet.
//
// The meter shows the POST-TRIM input level in dBFS with a marked "good" zone
// (~-12..-3 dBFS) — dial the trim so peaks sit in that zone and the RAT is
// driven the way the real pedal expects.

import { Knob } from './Knob';
import { INPUT_TRIM_UNITY_KNOB, trimKnobToDb } from '../params';

export interface InputStageProps {
  trim: number; // 0..1 knob position
  onTrim: (value: number) => void;
  peak: number | null; // linear post-trim peak from the worklet (null = idle)
  running: boolean;
}

// Meter scale: -48 dBFS (left) .. 0 dBFS (right).
const METER_MIN_DB = -48;
const METER_MAX_DB = 0;
// The "good" input zone the trim aims for.
const GOOD_LO_DB = -12;
const GOOD_HI_DB = -3;

const pctOf = (db: number) =>
  ((Math.min(METER_MAX_DB, Math.max(METER_MIN_DB, db)) - METER_MIN_DB) /
    (METER_MAX_DB - METER_MIN_DB)) *
  100;

export function InputStage({ trim, onTrim, peak, running }: InputStageProps) {
  const peakDb = peak != null && peak > 0 ? 20 * Math.log10(peak) : null;
  const fillPct = peakDb != null ? pctOf(peakDb) : 0;
  const goodLoPct = pctOf(GOOD_LO_DB);
  const goodHiPct = pctOf(GOOD_HI_DB);
  const inZone = peakDb != null && peakDb >= GOOD_LO_DB && peakDb <= GOOD_HI_DB;
  const hot = peakDb != null && peakDb > GOOD_HI_DB;

  const trimDbRaw = trimKnobToDb(trim);
  const trimDb = Math.abs(trimDbRaw) < 0.5 ? 0 : Math.round(trimDbRaw); // avoid "-0"/"+0"
  const trimLabel = `${trimDb > 0 ? '+' : ''}${trimDb} dB`;

  return (
    <section className="input-stage raised" aria-label="Input trim and level meter" data-testid="input-stage">
      <div className="input-knob">
        <Knob
          name="Trim"
          ariaLabel="Input trim"
          value={trim}
          defaultValue={INPUT_TRIM_UNITY_KNOB}
          onChange={onTrim}
          testId="knob-input-trim"
        />
      </div>

      <div className="input-meter-wrap">
        <div className="input-meter-head">
          <span className="input-meter-title">Input</span>
          <span className="input-meter-read mono" data-testid="input-peak-db">
            {peakDb != null ? `${peakDb.toFixed(0)} dBFS` : running ? '—' : 'idle'}
          </span>
          <span className="input-trim-read mono" data-testid="input-trim-db">
            {trimLabel}
          </span>
        </div>
        <div
          className={`input-meter well${inZone ? ' in-zone' : ''}${hot ? ' hot' : ''}`}
          data-testid="input-meter"
          role="meter"
          aria-label="Input peak level"
          aria-valuemin={METER_MIN_DB}
          aria-valuemax={METER_MAX_DB}
          aria-valuenow={peakDb != null ? Math.round(peakDb) : METER_MIN_DB}
        >
          <span
            className="input-meter-zone"
            style={{ left: `${goodLoPct}%`, width: `${goodHiPct - goodLoPct}%` }}
            aria-hidden="true"
          />
          <span className="input-meter-fill" style={{ width: `${fillPct}%` }} aria-hidden="true" />
        </div>
      </div>
    </section>
  );
}
