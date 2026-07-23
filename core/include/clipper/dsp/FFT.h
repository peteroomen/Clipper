// Clipper — portable DSP core.
//
// A tiny hand-rolled radix-2 Cooley–Tukey complex FFT, no external deps. Used by
// the M5 partitioned cab convolver (CabConvolver.h) for fast block convolution.
//
// Design notes:
//   - Iterative, in-place, decimation-in-time. Bit-reversal permutation and the
//     twiddle-factor table are precomputed once in prepare(); transform()
//     performs ZERO allocation, so it is safe on the audio thread.
//   - Complex interleaved is avoided: separate real[] / imag[] arrays (length N)
//     keep the inner loops branch-light and the caller's real-signal path simple
//     (imag[] = 0 on input).
//   - Size N must be a power of two. For the cab convolver N = 2 * partition =
//     256, so the transform is small and cheap.
//
// This header is platform-free (C++17, only <vector>/<cmath>): it must compile
// natively and under Emscripten.

#ifndef CLIPPER_DSP_FFT_H
#define CLIPPER_DSP_FFT_H

#include <cmath>
#include <cstddef>
#include <vector>

namespace clipper::dsp {

class FFT {
public:
    // Prepare an FFT of size n (must be a power of two). Precomputes the
    // bit-reversal index table and the cos/sin twiddle tables. Allocates here,
    // never in transform().
    void prepare(int n) {
        n_ = n;
        // log2(n)
        levels_ = 0;
        for (int t = n; t > 1; t >>= 1) ++levels_;

        bitrev_.assign(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < levels_; ++b)
                if (i & (1 << b)) r |= 1 << (levels_ - 1 - b);
            bitrev_[static_cast<size_t>(i)] = r;
        }
        // Twiddles for the forward transform: exp(-j*2*pi*k/n), k=0..n/2-1.
        cos_.assign(static_cast<size_t>(n / 2), 0.0);
        sin_.assign(static_cast<size_t>(n / 2), 0.0);
        for (int k = 0; k < n / 2; ++k) {
            const double a = -2.0 * kPi * k / n;
            cos_[static_cast<size_t>(k)] = std::cos(a);
            sin_[static_cast<size_t>(k)] = std::sin(a);
        }
    }

    int size() const { return n_; }

    // In-place FFT. inverse=false is the forward transform (twiddle sign as
    // tabled); inverse=true conjugates the twiddles. Neither scales the result —
    // the caller divides by N after an inverse transform if it wants the true
    // inverse. re/imag must each be length N. No allocation.
    void transform(double* re, double* im, bool inverse) const {
        const int n = n_;
        // Bit-reversal permutation.
        for (int i = 0; i < n; ++i) {
            const int j = bitrev_[static_cast<size_t>(i)];
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        // Butterflies, level by level.
        for (int half = 1; half < n; half <<= 1) {
            const int span = half << 1;
            const int step = n / span;  // stride into the twiddle table
            for (int start = 0; start < n; start += span) {
                for (int k = 0; k < half; ++k) {
                    const int ti = k * step;
                    const double wc = cos_[static_cast<size_t>(ti)];
                    const double ws = inverse ? -sin_[static_cast<size_t>(ti)]
                                              : sin_[static_cast<size_t>(ti)];
                    const int a = start + k;
                    const int b = a + half;
                    const double xr = re[b] * wc - im[b] * ws;
                    const double xi = re[b] * ws + im[b] * wc;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
            }
        }
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    int n_ = 0;
    int levels_ = 0;
    std::vector<int> bitrev_;
    std::vector<double> cos_, sin_;
};

}  // namespace clipper::dsp

#endif  // CLIPPER_DSP_FFT_H
