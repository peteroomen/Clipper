// Clipper — portable DSP core (M5).
//
// CabConvolver: uniform-partitioned FFT convolution of a mono signal with a
// speaker-cabinet impulse response. Hand-rolled radix-2 FFT (FFT.h), no external
// deps. Loads an IR at prepare(); if the IR's sample rate differs from the engine
// rate it is linearly resampled at load time (a quality compromise documented in
// the .cpp — fine for M5's procedurally generated cab).
//
// Latency: the partition size equals the worklet render quantum (128), and the
// convolver adds exactly one partition (128 samples) of latency — it cannot emit a
// sample until the partition containing it is complete. latencySamples() reports
// it; an impulse fed in comes out as the IR delayed by exactly that many samples
// (verified by test). The delay is materialized as the one partition of zeros that
// reset() seeds into the output FIFO, NOT as an offset in the frequency-delay line.
//
// Block-size independence: process() accepts ANY numFrames, including sizes that
// are not a multiple of the partition and sizes that vary from call to call, and
// produces the SAME sample stream in every case — bit-identically so. Input is
// accumulated in a FIFO and the FFT machinery is only ever advanced on a full
// partition; output is drained from a FIFO seeded with the one partition of
// leading zeros that the latency guarantees. See the algorithm comment at the top
// of CabConvolver.cpp for why this costs no extra latency. (Before this was
// FIFO'd, a short block was zero-padded and run anyway, which advanced the
// frequency-delay line by a whole partition for a partial block's worth of input
// and permanently misaligned the stream — 2026-07-24 audit finding 3.)
//
// process() performs NO heap allocation: every FFT scratch, the frequency-domain
// delay line, the FIFOs, and the partitioned IR spectra are allocated in
// prepare().
//
// Platform-free C++17 (no OS/browser/Emscripten includes).

#ifndef CLIPPER_DSP_CAB_CONVOLVER_H
#define CLIPPER_DSP_CAB_CONVOLVER_H

#include <vector>

#include "clipper/dsp/FFT.h"

namespace clipper::dsp {

class CabConvolver {
public:
    CabConvolver() = default;

    // Prepare for a given engine sample rate and partition size (defaults to the
    // 128-sample worklet quantum), then load the impulse response `ir` sampled at
    // `irSampleRate`. If irSampleRate != sampleRate the IR is linearly resampled.
    // Allocates all scratch here. Safe to call again to swap IRs.
    void prepare(double sampleRate, const float* ir, int irLength,
                 double irSampleRate, int partitionSize = 128);

    // Process numFrames of mono audio, in -> out (in and out may alias / be the
    // same buffer). The caller may pass ANY length — larger than, smaller than or
    // coprime with the partition, and a different length every call; the output
    // stream is identical either way. numFrames <= 0 is a no-op. No allocation.
    void process(const float* in, float* out, int numFrames);

    // Latency added by the convolver, in samples (== the partition size). Holds
    // for every block size, not just partition-aligned ones.
    int latencySamples() const { return partition_; }

    int partitionSize() const { return partition_; }
    int numPartitions() const { return numPartitions_; }

    // Reset the running state (input history, frequency-domain delay line, both
    // FIFOs) to silence without reloading the IR. Re-seeds the output FIFO with the
    // one partition of leading silence, so the latency contract holds again from
    // the next sample.
    void reset();

private:
    // Consumes exactly `partition_` input frames and produces exactly `partition_`
    // output frames. `in` and `out` are never the caller's buffers (they are
    // inFifo_ / blockOut_), so they never alias.
    void processBlock(const float* in, float* out);

    // Output FIFO helpers (ring of capacity 2*partition_ — see the availability
    // bound in the .cpp).
    void pushOutput(const float* src, int n);
    void popOutput(float* dst, int n);

    FFT fft_;
    int partition_ = 128;   // P (= block size)
    int fftSize_ = 256;     // N = 2P
    int numPartitions_ = 0;  // K = ceil(irLen / P)

    // Partitioned IR spectra: numPartitions_ blocks, each fftSize_ complex bins.
    std::vector<double> irRe_, irIm_;  // laid out [k*fftSize_ + bin]

    // Frequency-domain delay line of input-block spectra. The sum reads the newest
    // numPartitions_ slots (fdlHead_ - k), including the one just pushed — the
    // one-partition latency is NOT here, it is the output FIFO's seed zeros. Depth
    // is numPartitions_+1, one spare slot. Ring buffer; fdlHead_ = newest slot.
    std::vector<double> fdlRe_, fdlIm_;  // [slot*fftSize_ + bin]
    int fdlDepth_ = 0;
    int fdlHead_ = 0;

    // Scratch (all sized in prepare, reused every block).
    std::vector<double> timeRe_, timeIm_;  // fftSize_ FFT working buffers
    std::vector<double> accRe_, accIm_;    // fftSize_ accumulator
    std::vector<float> overlap_;           // previous partition_ input samples

    // Sample-accurate FIFOs, so any block size yields the same stream.
    // inFifo_ is filled linearly and consumed all at once (size partition_).
    std::vector<float> inFifo_;
    int inFill_ = 0;
    std::vector<float> blockOut_;  // partition_ — processBlock's destination
    // Output ring, capacity 2*partition_. Seeded by reset() with partition_ zeros
    // (the convolver's guaranteed leading silence), which is what lets the FIFO
    // exist without adding a partition of latency.
    std::vector<float> outFifo_;
    int outRead_ = 0;
    int outCount_ = 0;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_CAB_CONVOLVER_H
