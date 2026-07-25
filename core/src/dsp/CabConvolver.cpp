// Clipper — CabConvolver (M5). See CabConvolver.h for the API and the latency
// contract. This file implements uniform-partitioned overlap-save convolution.
//
// ---------------------------------------------------------------------------
// Algorithm (uniform partitioned overlap-save, one partition of latency):
//
//   Partition/block size P (= 128, the worklet quantum). FFT length N = 2P.
//   The IR is split into K = ceil(irLen / P) partitions h_0..h_{K-1}, each
//   zero-padded to N and forward-transformed once at prepare() -> H_k.
//
//   Per input block x_m (P samples):
//     1. Form the length-N time buffer [prev P samples, current P samples] and
//        forward-FFT it -> X_m. Push X_m into a frequency-domain delay line (FDL).
//     2. Z_m = sum_{k=0}^{K-1} FDL[head - k] * H_k = sum_k X_{m-k} * H_k
//     3. z = IFFT(Z_m)/N; the last P samples of z are the overlap-save "valid"
//        region == the true linear convolution of the streamed input with the IR
//        at sample positions [mP, (m+1)P), with NO delay.
//
//   Overlap-save requires the previous P input samples in the FFT window so the
//   circular convolution's last-P region equals linear convolution; overlap_[]
//   holds them.
//
// ---------------------------------------------------------------------------
// Latency, and why the FIFO is free (2026-07-24 audit finding 3):
//
//   The convolver's contract is one partition (P) of latency. Note that step 2
//   consumes X_m as soon as block m is complete, so the value it produces —
//   the un-delayed convolution of positions [mP, (m+1)P) — is exactly what the
//   output stream needs at positions [(m+1)P, (m+2)P). In other words, output
//   block m+1 is computable the moment input block m lands: the latency buys a
//   whole partition of slack.
//
//   That slack is spent on the FIFO instead of on FDL indexing. process()
//   accumulates input in inFifo_ and only ever calls processBlock() on a
//   genuinely full partition; output is drained from outFifo_, which reset()
//   seeds with P zeros — precisely the leading silence the P-sample latency
//   guarantees. The emitted stream is therefore
//
//       [P zeros] ++ Z_0 ++ Z_1 ++ Z_2 ++ ...
//
//   which is term-for-term what the pre-FIFO implementation emitted when it was
//   fed exact P-sample blocks (it used a `head - 1 - k` FDL offset, i.e. it held
//   the deferral inside the FDL and emitted an all-zero first block). Same
//   complex products, same k order, same double accumulator => the aligned path
//   is BIT-identical, and latencySamples() is unchanged and now correct for every
//   block size.
//
//   No underflow is possible: after K total input samples the convolver has
//   produced P + floor(K/P)*P output samples and emitted K, so
//       available = P + floor(K/P)*P - K = P - (K mod P), which is in [1, P].
//   A drain of s samples happens with K already advanced but only K-s emitted, so
//   the pre-drain availability is P - (K mod P) + s >= s. Never short.
//   Segmenting each process() call at (P - inFill_) runs at most one block per
//   segment and bounds the ring at 2P, so every buffer is prepare()-sized and the
//   audio thread allocates nothing.
//
//   (The bug this replaced: a short block was zero-padded to a whole partition and
//   run anyway. The FDL and overlap_ advanced by a FULL partition for a partial
//   partition of real input and P-n computed samples were discarded, so the stream
//   was permanently misaligned and every later call inherited it. Measured error
//   on 100-sample blocks exceeded the reference peak.)
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

    // FIFOs. inFifo_/blockOut_ are one partition; the output ring needs 2P (see
    // the availability bound in the header comment). Sized HERE so process()
    // never allocates — and so a large partitionSize can no longer overflow the
    // stack, which the old 4096-element local arrays could.
    inFifo_.assign(static_cast<size_t>(partition_), 0.0f);
    blockOut_.assign(static_cast<size_t>(partition_), 0.0f);
    outFifo_.assign(static_cast<size_t>(partition_) * 2, 0.0f);

    reset();
}

void CabConvolver::reset() {
    std::fill(fdlRe_.begin(), fdlRe_.end(), 0.0);
    std::fill(fdlIm_.begin(), fdlIm_.end(), 0.0);
    std::fill(overlap_.begin(), overlap_.end(), 0.0f);
    fdlHead_ = 0;

    std::fill(inFifo_.begin(), inFifo_.end(), 0.0f);
    inFill_ = 0;
    std::fill(blockOut_.begin(), blockOut_.end(), 0.0f);
    // Seed the output FIFO with the convolver's one partition of guaranteed
    // leading silence. This is what makes the FIFO latency-free: it stands in for
    // the all-zero first output block that the FDL deferral used to emit.
    std::fill(outFifo_.begin(), outFifo_.end(), 0.0f);
    outRead_ = 0;
    outCount_ = partition_;
}

void CabConvolver::pushOutput(const float* src, int n) {
    const int cap = static_cast<int>(outFifo_.size());
    int w = outRead_ + outCount_;
    if (w >= cap) w -= cap;
    for (int i = 0; i < n; ++i) {
        outFifo_[static_cast<size_t>(w)] = src[i];
        if (++w == cap) w = 0;
    }
    outCount_ += n;
}

void CabConvolver::popOutput(float* dst, int n) {
    const int cap = static_cast<int>(outFifo_.size());
    for (int i = 0; i < n; ++i) {
        dst[i] = outFifo_[static_cast<size_t>(outRead_)];
        if (++outRead_ == cap) outRead_ = 0;
    }
    outCount_ -= n;
}

void CabConvolver::process(const float* in, float* out, int numFrames) {
    if (numFrames <= 0) return;

    int off = 0;
    while (off < numFrames) {
        // Fill at most up to the next partition boundary. Guarantees s >= 1, that
        // inFifo_ never overruns, and that at most ONE block runs per segment
        // (which is what bounds the output ring at 2P).
        const int s = std::min(numFrames - off, partition_ - inFill_);

        // Read the caller's input BEFORE writing the caller's output: in and out
        // may be the same buffer (amp_process runs the cab in place), and this
        // segment's out[] overwrites exactly this segment's in[].
        for (int i = 0; i < s; ++i)
            inFifo_[static_cast<size_t>(inFill_ + i)] = in[off + i];
        inFill_ += s;

        if (inFill_ == partition_) {
            processBlock(inFifo_.data(), blockOut_.data());
            inFill_ = 0;
            pushOutput(blockOut_.data(), partition_);
        }

        // Always drainable: available = P - (K mod P) + s >= s. Never advance the
        // FDL to satisfy a partial block — that was the bug.
        popOutput(out + off, s);
        off += s;
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

    // 2. Z = sum_{k=0}^{K-1} FDL[head-k] * H_k — the newest block IS consumed
    //    this cycle, so this is the un-delayed convolution of the input positions
    //    this block covers. The one-partition delay lives in the output FIFO's
    //    seed zeros instead (see the latency note at the top of this file).
    std::fill(accRe_.begin(), accRe_.end(), 0.0);
    std::fill(accIm_.begin(), accIm_.end(), 0.0);
    for (int k = 0; k < numPartitions_; ++k) {
        int slot = fdlHead_ - k;
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
    // The in-place hazard that made this ordering load-bearing (callers run the
    // cab with in == out, e.g. amp_process; once out[i] is written the aliased
    // in[i] is gone, so capturing the overlap afterward would save the OUTPUT and
    // corrupt the next window) can no longer reach here: `in` is inFifo_ and `out`
    // is blockOut_, both convolver-owned and never aliased. The aliasing is now
    // handled once, in process(). Ordering kept so the hazard cannot come back if
    // this ever gets called with caller buffers again.
    for (int i = 0; i < P; ++i) overlap_[static_cast<size_t>(i)] = in[i];
    for (int i = 0; i < P; ++i)
        out[i] = static_cast<float>(accRe_[static_cast<size_t>(P + i)] * inv);
    fdlHead_ = (fdlHead_ + 1) % fdlDepth_;
}

}  // namespace clipper::dsp
