// Clipper — portable DSP core (M5).
//
// CabConvolver: uniform-partitioned FFT convolution of a mono signal with a
// speaker-cabinet impulse response. Hand-rolled radix-2 FFT (FFT.h), no external
// deps. Loads an IR at prepare(); if the IR's sample rate differs from the engine
// rate it is linearly resampled at load time (a quality compromise documented in
// the .cpp — fine for M5's procedurally generated cab).
//
// Latency: the partition size equals the worklet render quantum (128). The engine
// runs "one partition behind" — the current output block is formed from input
// blocks up to and including the PREVIOUS one — so the convolver adds exactly one
// partition (128 samples) of latency. latencySamples() reports it; an impulse fed
// in comes out as the IR delayed by exactly that many samples (verified by test).
//
// process() performs NO heap allocation: every FFT scratch, the frequency-domain
// delay line, and the partitioned IR spectra are allocated in prepare().
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
    // same buffer). Internally chunked into partition-sized blocks; the caller may
    // pass any length. No allocation.
    void process(const float* in, float* out, int numFrames);

    // Latency added by the convolver, in samples (== the partition size).
    int latencySamples() const { return partition_; }

    int partitionSize() const { return partition_; }
    int numPartitions() const { return numPartitions_; }

    // Reset the running state (input history + frequency-domain delay line) to
    // silence without reloading the IR.
    void reset();

private:
    void processBlock(const float* in, float* out);  // exactly `partition_` frames

    FFT fft_;
    int partition_ = 128;   // P (= block size)
    int fftSize_ = 256;     // N = 2P
    int numPartitions_ = 0;  // K = ceil(irLen / P)

    // Partitioned IR spectra: numPartitions_ blocks, each fftSize_ complex bins.
    std::vector<double> irRe_, irIm_;  // laid out [k*fftSize_ + bin]

    // Frequency-domain delay line of input-block spectra. Depth numPartitions_+1
    // (the "+1" realizes the one-partition latency: the newest input spectrum is
    // not consumed until the next block). Ring buffer; fdlHead_ = newest slot.
    std::vector<double> fdlRe_, fdlIm_;  // [slot*fftSize_ + bin]
    int fdlDepth_ = 0;
    int fdlHead_ = 0;

    // Scratch (all sized in prepare, reused every block).
    std::vector<double> timeRe_, timeIm_;  // fftSize_ FFT working buffers
    std::vector<double> accRe_, accIm_;    // fftSize_ accumulator
    std::vector<float> overlap_;           // previous partition_ input samples
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_CAB_CONVOLVER_H
