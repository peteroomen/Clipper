// clipper-render — native CLI harness for the RAT model (M1). NOT part of the
// WASM build. Renders a WAV (or a generated test signal) through
// clipper::dsp::RatModel and writes a WAV out; optionally dumps a magnitude
// spectrum of the last second for eyeballing harmonic content.
//
// Usage:
//   clipper-render in.wav out.wav [--distortion D] [--filter F] [--level L]
//   clipper-render --gen sine:220:2.0   out.wav [params] [--sr 48000]
//   clipper-render --gen sweep:20:20000:4.0 out.wav [params]
//   ... [--spectrum spec.csv]
//
// Params are normalized knob positions in [0,1]; defaults distortion 0.7,
// filter 0.4, level 0.8. Generated-signal amplitude defaults to 0.3 (== 0.3 V,
// a hot-ish guitar DI peak). Output is 32-bit float mono WAV.
//
// dr_wav (public domain, mackron/dr_libs) is vendored under
// core/tools/third_party and is included ONLY by this tool, never by the DSP
// core.

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "clipper/dsp/RatModel.h"
#include "measure/AliasMetric.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586;

struct Args {
    std::string inFile;
    std::string outFile;
    std::string gen;       // e.g. "sine:220:2.0" or "sweep:20:20000:4.0"
    std::string spectrum;  // CSV path, empty = none
    float distortion = 0.7f;
    float filter = 0.4f;
    float level = 0.8f;
    double sampleRate = 48000.0;  // used for --gen only
    float amplitude = 0.3f;       // used for --gen only
    int os = 4;                   // oversampling factor (1/2/4/8), M2 default 4
    std::string stage2 = "wdf";   // nonlinear stage: "wdf" or "adaa"
    bool aliasReport = false;     // print the alias metric table and exit
};

[[noreturn]] void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s in.wav out.wav [--distortion D] [--filter F] [--level L] [--spectrum s.csv]\n"
        "  %s --gen sine:FREQ:SECONDS out.wav [params] [--sr SR] [--amp A] [--spectrum s.csv]\n"
        "  %s --gen sweep:F0:F1:SECONDS out.wav [params]\n"
        "  %s --alias-report [--sr SR] [--distortion D] [--stage2 wdf|adaa]\n"
        "Params are knob positions in [0,1] (defaults: distortion 0.7, filter 0.4, level 0.8).\n"
        "M2 flags: --os 1|2|4|8 (oversampling, default 4), --stage2 wdf|adaa (default wdf),\n"
        "          --alias-report (print the aliasing metric table for os=1/2/4/8 and exit).\n",
        argv0, argv0, argv0, argv0);
    std::exit(2);
}

// Split "a:b:c" on ':'.
std::vector<std::string> splitColon(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(':', start);
        out.push_back(s.substr(start, p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

// Generate a test signal into `sig`. Returns false on parse error.
bool generate(const Args& a, std::vector<float>& sig) {
    auto parts = splitColon(a.gen);
    if (parts.empty()) return false;
    const double fs = a.sampleRate;
    const float amp = a.amplitude;
    if (parts[0] == "sine" && parts.size() == 3) {
        const double f = std::atof(parts[1].c_str());
        const double secs = std::atof(parts[2].c_str());
        const int n = static_cast<int>(secs * fs);
        sig.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            sig[static_cast<size_t>(i)] =
                amp * static_cast<float>(std::sin(kTwoPi * f * i / fs));
        return true;
    }
    if (parts[0] == "sweep" && parts.size() == 4) {
        const double f0 = std::atof(parts[1].c_str());
        const double f1 = std::atof(parts[2].c_str());
        const double secs = std::atof(parts[3].c_str());
        const int n = static_cast<int>(secs * fs);
        sig.resize(static_cast<size_t>(n));
        // Exponential (log) sweep: phase = 2*pi*integral of f(t).
        const double k = std::log(f1 / f0) / secs;
        for (int i = 0; i < n; ++i) {
            const double t = i / fs;
            const double phase = kTwoPi * f0 * (std::exp(k * t) - 1.0) / k;
            sig[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(phase));
        }
        return true;
    }
    return false;
}

// Classic Goertzel magnitude estimate (windowed) at frequency f.
double goertzelAmp(const std::vector<float>& x, const std::vector<double>& win,
                   double winSum, double f, double fs) {
    const double w = kTwoPi * f / fs;
    const double cw = std::cos(w), sw = std::sin(w), coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    const size_t n = x.size();
    for (size_t i = 0; i < n; ++i) {
        const double s0 = x[i] * win[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * sw;
    return 2.0 * std::sqrt(re * re + im * im) / winSum;
}

// Dump a magnitude spectrum (freq,magnitude_db) of the last second of `out`.
// Bin spacing is ~1 Hz (proper DFT resolution for a 1 s window) up to 20 kHz, so
// harmonics of a periodic tone land on bins. dB is 20*log10(amplitude).
void writeSpectrum(const std::string& path, const std::vector<float>& out,
                   double fs) {
    const size_t total = out.size();
    const size_t win = static_cast<size_t>(fs);  // 1 second
    const size_t n = win < total ? win : total;
    std::vector<float> seg(out.end() - static_cast<long>(n), out.end());

    // Hann window.
    std::vector<double> w(n);
    double winSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        w[i] = 0.5 * (1.0 - std::cos(kTwoPi * i / (n - 1)));
        winSum += w[i];
    }

    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "warning: cannot open spectrum file %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "freq,magnitude_db\n");
    const double maxHz = std::min(20000.0, fs * 0.5);
    const double binHz = fs / static_cast<double>(n);
    for (double freq = 0.0; freq <= maxHz; freq += binHz) {
        const double amp = goertzelAmp(seg, w, winSum, freq, fs);
        const double db = 20.0 * std::log10(amp + 1e-12);
        std::fprintf(f, "%.2f,%.2f\n", freq, db);
    }
    std::fclose(f);
    std::printf("Wrote spectrum: %s (%.0f Hz resolution, up to %.0f Hz)\n",
                path.c_str(), binHz, maxHz);
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "--distortion") a.distortion = std::atof(need("--distortion"));
        else if (s == "--filter") a.filter = std::atof(need("--filter"));
        else if (s == "--level") a.level = std::atof(need("--level"));
        else if (s == "--gen") a.gen = need("--gen");
        else if (s == "--spectrum") a.spectrum = need("--spectrum");
        else if (s == "--sr") a.sampleRate = std::atof(need("--sr"));
        else if (s == "--amp") a.amplitude = std::atof(need("--amp"));
        else if (s == "--os") a.os = std::atoi(need("--os"));
        else if (s == "--stage2") a.stage2 = need("--stage2");
        else if (s == "--alias-report") a.aliasReport = true;
        else if (s == "-h" || s == "--help") usage(argv[0]);
        else if (!s.empty() && s[0] == '-') {
            std::fprintf(stderr, "error: unknown flag %s\n", s.c_str());
            usage(argv[0]);
        } else positional.push_back(s);
    }

    const int stage2Mode = (a.stage2 == "adaa")
                               ? clipper::dsp::RatModel::STAGE2_ADAA
                               : clipper::dsp::RatModel::STAGE2_WDF;

    // --alias-report: render a high sine (whose harmonics fold to predictable
    // inharmonic bins) at high distortion through os=1/2/4/8 and print the
    // signal-to-worst-alias and summed-alias metrics. See AliasMetric.h.
    if (a.aliasReport) {
        const double fs = a.sampleRate;
        const double f0 = 4186.0;  // C8; folds cleanly at fs=44100
        const int n = static_cast<int>(1.0 * fs);
        std::vector<float> sig(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            sig[static_cast<size_t>(i)] =
                a.amplitude * static_cast<float>(std::sin(kTwoPi * f0 * i / fs));
        std::printf(
            "Alias report: f0=%.0f Hz, fs=%.0f Hz, dist=%.2f, filter=0 (bright), stage2=%s\n",
            f0, fs, a.distortion, a.stage2.c_str());
        std::printf("  os  worst-alias(dB)  sum-alias(dB)  fund-amp   latency(smp)\n");
        for (int os : {1, 2, 4, 8}) {
            clipper::dsp::RatModel m;
            m.prepare(fs, 128);
            m.setOversampling(os);
            m.setStage2Mode(stage2Mode);
            m.setParameter(clipper::dsp::RatModel::PARAM_DISTORTION, a.distortion);
            m.setParameter(clipper::dsp::RatModel::PARAM_FILTER, 0.0f);
            m.setParameter(clipper::dsp::RatModel::PARAM_LEVEL, 0.9f);
            std::vector<float> out(sig.size(), 0.0f);
            m.process(sig.data(), out.data(), static_cast<int>(sig.size()));
            const clipper::measure::AliasReport r =
                clipper::measure::measureAliasing(out, fs, f0);
            std::printf("  %2d   %11.1f    %11.1f    %.5f   %d\n", os, r.worstAliasDb,
                        r.sumAliasDb, r.fundAmp, m.latencySamples());
        }
        return 0;
    }

    // Resolve input source and output path.
    std::vector<float> input;
    double fs = a.sampleRate;
    if (!a.gen.empty()) {
        if (positional.size() != 1) {
            std::fprintf(stderr, "error: --gen mode takes exactly one positional (out.wav)\n");
            usage(argv[0]);
        }
        a.outFile = positional[0];
        if (!generate(a, input)) {
            std::fprintf(stderr, "error: bad --gen spec '%s'\n", a.gen.c_str());
            usage(argv[0]);
        }
        fs = a.sampleRate;
    } else {
        if (positional.size() != 2) usage(argv[0]);
        a.inFile = positional[0];
        a.outFile = positional[1];
        unsigned int channels = 0, sr = 0;
        drwav_uint64 frames = 0;
        float* raw = drwav_open_file_and_read_pcm_frames_f32(
            a.inFile.c_str(), &channels, &sr, &frames, nullptr);
        if (!raw) {
            std::fprintf(stderr, "error: cannot read WAV %s\n", a.inFile.c_str());
            return 1;
        }
        fs = sr;
        input.resize(static_cast<size_t>(frames));
        // Downmix to mono (average channels).
        for (drwav_uint64 i = 0; i < frames; ++i) {
            double acc = 0.0;
            for (unsigned c = 0; c < channels; ++c)
                acc += raw[i * channels + c];
            input[static_cast<size_t>(i)] =
                static_cast<float>(acc / (channels ? channels : 1));
        }
        drwav_free(raw, nullptr);
    }

    // Process through the model.
    clipper::dsp::RatModel model;
    model.prepare(fs, 128);
    model.setOversampling(a.os);
    model.setStage2Mode(stage2Mode);
    model.setParameter(clipper::dsp::RatModel::PARAM_DISTORTION, a.distortion);
    model.setParameter(clipper::dsp::RatModel::PARAM_FILTER, a.filter);
    model.setParameter(clipper::dsp::RatModel::PARAM_LEVEL, a.level);

    std::vector<float> out(input.size(), 0.0f);
    if (!input.empty())
        model.process(input.data(), out.data(), static_cast<int>(input.size()));

    // Write 32-bit float mono WAV.
    drwav_data_format fmt;
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = 1;
    fmt.sampleRate = static_cast<drwav_uint32>(fs + 0.5);
    fmt.bitsPerSample = 32;
    drwav wav;
    if (!drwav_init_file_write(&wav, a.outFile.c_str(), &fmt, nullptr)) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", a.outFile.c_str());
        return 1;
    }
    drwav_write_pcm_frames(&wav, out.size(), out.data());
    drwav_uninit(&wav);

    // Peak/RMS report.
    double peak = 0.0, rms = 0.0;
    for (float v : out) {
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
        rms += static_cast<double>(v) * v;
    }
    rms = out.empty() ? 0.0 : std::sqrt(rms / out.size());
    std::printf(
        "Rendered %zu frames @ %.0f Hz -> %s  (dist=%.2f filter=%.2f level=%.2f "
        "os=%dx stage2=%s)\n"
        "  peak=%.4f  rms=%.4f  latency=%d smp\n",
        out.size(), fs, a.outFile.c_str(), a.distortion, a.filter, a.level,
        model.oversampling(), a.stage2.c_str(), peak, rms, model.latencySamples());

    if (!a.spectrum.empty() && !out.empty()) writeSpectrum(a.spectrum, out, fs);
    return 0;
}
