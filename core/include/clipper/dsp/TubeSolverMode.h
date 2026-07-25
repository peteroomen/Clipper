// Clipper — tube-solver precision switch (test instrumentation, docs §25).
//
// Every valve solver (TriodeStage / cathode follower, LtpInverter, the pentode
// plate & grid solves) AND the BJT solver (BjtStage's damped Newton, whose
// residual early-out landed with audit finding 12 — docs §34; the name kept its
// "tube" spelling rather than churning nine call sites) converges its per-sample
// Newton to a PRODUCTION tolerance chosen so the audio-band error of early exit
// sits far below -120 dBFS. The
// regression test (test_tube_solver.cpp) proves that choice by re-rendering the
// same program material with every tolerance tightened by 1000x ("reference mode")
// and asserting the two renders differ by less than -120 dBFS.
//
// This is a process-wide, test-only knob: the production value is 1.0 and nothing
// in the audio path ever changes it. It deliberately avoids per-instance plumbing
// through the amp composition tree (the solvers live 3 levels deep). Not intended
// to be toggled while audio threads are running.
//
// Header-only, zero-dependency, platform-free (native + WASM share it).

#ifndef CLIPPER_DSP_TUBE_SOLVER_MODE_H
#define CLIPPER_DSP_TUBE_SOLVER_MODE_H

namespace clipper::dsp {

// Multiplier applied to every solver's convergence tolerance — dimensionless, so
// it scales the valve solvers' step tolerances (volts) and BjtStage's residual
// tolerance (amps) alike. 1.0 in production; 1e-3 in the regression test's
// reference mode.
inline double& tubeSolverTolScale() {
    static double scale = 1.0;
    return scale;
}

// Test-only: switch every tube solver between production tolerances (false, the
// default) and 1000x-tighter reference tolerances (true).
inline void setTubeSolverReferenceMode(bool on) {
    tubeSolverTolScale() = on ? 1e-3 : 1.0;
}

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_TUBE_SOLVER_MODE_H
