// Clipper — CabConvolver (M5). See CabConvolver.h for the API and the latency
// contract. This file implements uniform-partitioned overlap-save convolution.
//
// ---------------------------------------------------------------------------
// Algorithm (uniform partitioned overlap-save, "one partition behind"):
//
//   Partition/block size P (= 128, the worklet quantum). FFT length N = 2P.
//   The IR is split into K = ceil(irLen / P) partitions h_0..h_{K-1}, each
//   zero-padded to N and forward-transformed once at prepare() -> H_k.
//
//   Per input block x (P samples):
//     1. Form the length-N time buffer [prev P samples, current P samples] and
//        forward-FFT it -> X. Push X into a frequency-domain delay line (FDL).
//     2. Y = sum_{k=0}^{K-1} FDL[k+1] * H_k        (note the +1 offset)
//        The +1 means the block just pushed is NOT used this cycle; it is first
//        consumed on the NEXT block. That single-block deferral is what gives the
//        convolver exactly one partition (P samples) of latency, matching
//        latencySamples() and the impulse-delay test.
//     3. y = IFFT(Y)/N; output = the last P samples of y (the overlap-save
//        "valid" region == the true linear convolution of the streamed input
//        with the IR, delayed by P).
//
//   Overlap-save requires the previous P input samples in the FFT window so the
//   circular convolution's last-P region equals linear convolution; overlap_[]
//   holds them.
// ---------------------------------------------------------------------------

#include "clipper/dsp/CabConvolver.h"

#include <algorithm>
#include <cmath>

namespace clipper::dsp {

void CabConvolver::prepare(double sampleRate, const float* ir, int irLength,
                           double irSampleRate, int partitionSize) {
    partition_ = partitionSize > 0 ? partitionSize : 128;
    fftSize_ = partition_ * 2;
    fft_.prepare(fftSize_);

    // --- Resample the IR to the engine rate if needed (linear interpolation).
    // Linear resampling is a low-fidelity choice, but the M5 cab IR is a smooth,
    // band-limited synthetic response (rolled off well below Nyquist), so linear
    // interpolation introduces negligible error here. A windowed-sinc resampler
    // is a future refinement for real user-uploaded IRs. ---
    std::vector<float> resampled;
    const float* src = ir;
    int srcLen = irLength;
    if (irSampleRate > 0.0 && std::fabs(irSampleRate - sampleRate) > 1e-6 &&
        irLength > 1) {
        const double ratio = sampleRate / irSampleRate;
        const int outLen = std::max(1, static_cast<int>(std::ceil(irLength * ratio)));
        resampled.resize(static_cast<size_t>(outLen));
        for (int i = 0; i < outLen; ++i) {
            const double sp = i / ratio;  // source position
            const int i0 = static_cast<int>(sp);
            const double frac = sp - i0;
            const float a = ir[std::min(i0, irLength - 1)];
            const float b = ir[std::min(i0 + 1, irLength - 1)];
            resampled[static_cast<size_t>(i)] =
                static_cast<float>(a + (b - a) * frac);
        }
        src = resampled.data();
        srcLen = outLen;
    }

    // --- Partition the IR and forward-transform each partition once. ---
    numPartitions_ = (srcLen + partition_ - 1) / partition_;
    if (numPartitions_ < 1) numPartitions_ = 1;

    const size_t nBins = static_cast<size_t>(fftSize_);
    irRe_.assign(static_cast<size_t>(numPartitions_) * nBins, 0.0);
    irIm_.assign(static_cast<size_t>(numPartitions_) * nBins, 0.0);

    std::vector<double> re(nBins, 0.0), im(nBins, 0.0);
    for (int k = 0; k < numPartitions_; ++k) {
        std::fill(re.begin(), re.end(), 0.0);
        std::fill(im.begin(), im.end(), 0.0);
        const int base = k * partition_;
        for (int i = 0; i < partition_; ++i) {
            const int idx = base + i;
            if (idx < srcLen) re[static_cast<size_t>(i)] = src[idx];
        }
        fft_.transform(re.data(), im.data(), /*inverse=*/false);
        const size_t off = static_cast<size_t>(k) * nBins;
        std::copy(re.begin(), re.end(), irRe_.begin() + static_cast<long>(off));
        std::copy(im.begin(), im.end(), irIm_.begin() + static_cast<long>(off));
    }

    // --- FDL depth K+1 (see the +1 offset note in the header comment). ---
    fdlDepth_ = numPartitions_ + 1;
    fdlRe_.assign(static_cast<size_t>(fdlDepth_) * nBins, 0.0);
    fdlIm_.assign(static_cast<size_t>(fdlDepth_) * nBins, 0.0);

    timeRe_.assign(nBins, 0.0);
    timeIm_.assign(nBins, 0.0);
    accRe_.assign(nBins, 0.0);
    accIm_.assign(nBins, 0.0);
    overlap_.assign(static_cast<size_t>(partition_), 0.0f);

    reset();
}

void CabConvolver::reset() {
    std::fill(fdlRe_.begin(), fdlRe_.end(), 0.0);
    std::fill(fdlIm_.begin(), fdlIm_.end(), 0.0);
    std::fill(overlap_.begin(), overlap_.end(), 0.0f);
    fdlHead_ = 0;
}

void CabConvolver::process(const float* in, float* out, int numFrames) {
    int off = 0;
    while (off < numFrames) {
        const int n = std::min(partition_, numFrames - off);
        if (n == partition_) {
            processBlock(in + off, out + off);
        } else {
            // Tail shorter than a partition: zero-pad into a local block. This
            // path is only hit when the caller's total length is not a multiple
            // of the partition (tests hand us whole signals). Stack buffers keep
            // the audio-thread block path (n == partition_) allocation-free.
            float tmpIn[4096];
            float tmpOut[4096];
            for (int i = 0; i < partition_; ++i)
                tmpIn[i] = (i < n) ? in[off + i] : 0.0f;
            processBlock(tmpIn, tmpOut);
            for (int i = 0; i < n; ++i) out[off + i] = tmpOut[i];
        }
        off += n;
    }
}

void CabConvolver::processBlock(const float* in, float* out) {
    const int P = partition_;
    const int N = fftSize_;
    const size_t nBins = static_cast<size_t>(N);

    // 1. Time window = [prev P samples, current P samples]; forward FFT.
    for (int i = 0; i < P; ++i) {
        timeRe_[static_cast<size_t>(i)] = overlap_[static_cast<size_t>(i)];
        timeRe_[static_cast<size_t>(P + i)] = in[i];
        timeIm_[static_cast<size_t>(i)] = 0.0;
        timeIm_[static_cast<size_t>(P + i)] = 0.0;
    }
    fft_.transform(timeRe_.data(), timeIm_.data(), /*inverse=*/false);

    // Push the spectrum into the FDL (newest at fdlHead_).
    {
        const size_t hoff = static_cast<size_t>(fdlHead_) * nBins;
        std::copy(timeRe_.begin(), timeRe_.end(), fdlRe_.begin() + static_cast<long>(hoff));
        std::copy(timeIm_.begin(), timeIm_.end(), fdlIm_.begin() + static_cast<long>(hoff));
    }

    // 2. Y = sum_{k=0}^{K-1} FDL[head-1-k] * H_k  (the "head-1" == the +1 offset
    //    that defers the newest block by one, realizing the one-partition delay).
    std::fill(accRe_.begin(), accRe_.end(), 0.0);
    std::fill(accIm_.begin(), accIm_.end(), 0.0);
    for (int k = 0; k < numPartitions_; ++k) {
        int slot = fdlHead_ - 1 - k;
        slot %= fdlDepth_;
        if (slot < 0) slot += fdlDepth_;
        const size_t soff = static_cast<size_t>(slot) * nBins;
        const size_t hoff = static_cast<size_t>(k) * nBins;
        for (size_t b = 0; b < nBins; ++b) {
            const double xr = fdlRe_[soff + b], xi = fdlIm_[soff + b];
            const double hr = irRe_[hoff + b], hi = irIm_[hoff + b];
            accRe_[b] += xr * hr - xi * hi;
            accIm_[b] += xr * hi + xi * hr;
        }
    }

    // 3. Inverse FFT, scale by 1/N; output = last P samples (overlap-save valid).
    fft_.transform(accRe_.data(), accIm_.data(), /*inverse=*/true);
    const double inv = 1.0 / N;

    // Save THIS block's input as next block's overlap FIRST, THEN write output.
    // Order matters: callers process IN-PLACE (in == out, e.g. amp_process), so
    // once we write out[i] the aliased in[i] is gone — capturing the overlap from
    // `in` afterward would save the OUTPUT and corrupt the next block's window.
    // (Latent since M5: harmless-looking on the smooth default IR, but it filled
    // notches / added per-block error on any peaky IR — a user cab or a comb.)
    for (int i = 0; i < P; ++i) overlap_[static_cast<size_t>(i)] = in[i];
    for (int i = 0; i < P; ++i)
        out[i] = static_cast<float>(accRe_[static_cast<size_t>(P + i)] * inv);
    fdlHead_ = (fdlHead_ + 1) % fdlDepth_;
}

}  // namespace clipper::dsp
