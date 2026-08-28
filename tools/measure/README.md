# `tools/measure/` — one-off measurement harnesses

Standalone programs that produce the numbers a slice's write-up is argued from.
They are **deliberately not in CMake and not in CI**: each one is built by hand
when its question is being asked, and kept afterwards so the next slice can
reproduce the answer instead of re-deriving it. Nothing here is on any audio
path, and nothing here is in the WASM artifact's hash closure (which covers
`core/src/`, `core/include/`, the worklet, and `build-wasm.sh`'s emcc flags).

Build any of them against an existing core build:

```sh
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
c++ -std=c++17 -O2 -I core/include -I tools/measure \
    tools/measure/<file>.cpp -o build/<name>
```

`drop_rich_triad.cpp` additionally links the core, because it drives the shipped
pedal rather than a bare primitive:

```sh
c++ -std=c++17 -O2 -I core/include -I tools/measure \
    tools/measure/drop_rich_triad.cpp \
    build/libclipper_dsp.a build/libclipper_core.a -o build/drop_rich
```

## The pitch-shifter battery (docs §74, ADR 027)

| file | what it answers |
| --- | --- |
| `bench_shifter.h` | the shared battery — pitch accuracy (worst absolute cents AND spread), non-harmonic artifact floor, transient rise/peak, stimuli including a harmonically rich triad. Both shifters go through it identically, so the comparison is against this machine rather than against docs prose. |
| `PhaseVocoderShifter.h` | **the REFUSED candidate.** §70 named "a frequency-domain shifter" as the fix for the Cellar's XFAIL; §74 built it, and it is exact (0.000 cents) at 171.6 ms of latency and *worse than the shipped SOLA* at 83.7 ms. Kept, not deleted, so nobody rebuilds it to reach the same answer. It lives here and not under `core/include/` for that reason. |
| `shifter_baseline.cpp` | the shipped SOLA shifter's full baseline, plus the estimator's self-validation. |
| `shifter_pv.cpp` | the candidate at N = 2048 / 4096 / 8192. |
| `shifter_pv_probe.cpp` | attribution: is the octave blow-up the shifter's or the estimator's? (It is the shifter's — the rendered spectrum has no peak at the target at all.) |
| `shifter_head_to_head.cpp` | **the table the refusal was decided on.** |
| `drop_rich_triad.cpp` | the same accuracy sweep through `DropModel` — its detents, its blocking — which is what found the octave defect §70 missed. |

The per-partial frequency estimator these depend on is **not** here: it is
`core/tests/support/PartialFreq.h`, next to `LtpProbe.h` and `DcOffset.h`,
because a bar set by an unvalidated measurement is worse than no bar.
