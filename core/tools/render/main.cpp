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
#include "clipper/dsp/TriodeStage.h"
#include "clipper/dsp/Jcm800Preamp.h"
#include "clipper/dsp/SdModel.h"
#include "clipper/dsp/AmpModel.h"
#include "clipper/dsp/CabConvolver.h"
#include "clipper/dsp/CabIR.h"
#include "clipper/dsp/OutputLimiter.h"
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
    bool idealOpAmp = false;      // M6.5: bypass the LM308 op-amp model (A/B)
    std::string chain = "rat";    // "rat" (pedal) or "clean" (amp+cab+limiter)
    std::string cab = "clean212"; // clean chain cab: "clean212" or "brit412"
    std::string pedal = "rat";    // M8: "rat" or "sd1" (SD-1 overdrive)
    float limThresh = 0.97f;      // M6.5: output soft-limiter threshold (clean chain)
    bool triode = false;          // M9.1: run a single 12AX7 TriodeStage (stage alone)
    float triodeDrive = 3.0f;     // input-gain multiplier into the grid (volts)
    float triodeCathodeUf = 0.68f;  // cathode bypass cap (uF); 0 = unbypassed
    bool jcmPre = false;          // M9.2: run the JCM800 2204 preamp (4x triode + TMB)
    float jcmDrive = 1.0f;        // input-gain multiplier into V1A's grid (volts)
    float jcmGain = 0.5f;         // GAIN knob (preamp volume, audio taper)
    float jcmMaster = 0.5f;       // MASTER volume knob (audio taper)
    float jcmBass = 0.5f, jcmMid = 0.5f, jcmTreble = 0.5f;  // TMB tone knobs
};

// Output soft limiter, parameterized by threshold so the clean-path A/B can
// contrast the OLD (0.9) and NEW (0.97) staging. Mirrors the tanh knee in
// clipper::dsp::OutputLimiter / the worklet. (Inlined here — kept independent of
// OutputLimiter.h so the A/B script can build an OLD tree that predates it.)
float softLimit(float x, float t) {
    const float k = 1.0f - t;
    if (x > t) return t + k * std::tanh((x - t) / k);
    if (x < -t) return -t + k * std::tanh((x + t) / k);
    return x;
}

[[noreturn]] void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s in.wav out.wav [--distortion D] [--filter F] [--level L] [--spectrum s.csv]\n"
        "  %s --gen sine:FREQ:SECONDS out.wav [params] [--sr SR] [--amp A] [--spectrum s.csv]\n"
        "  %s --gen sweep:F0:F1:SECONDS out.wav [params]   (also: pluck:FREQ:SECONDS)\n"
        "  %s --alias-report [--sr SR] [--distortion D] [--stage2 wdf|adaa]\n"
        "Params are knob positions in [0,1] (defaults: distortion 0.7, filter 0.4, level 0.8).\n"
        "M2 flags: --os 1|2|4|8 (oversampling, default 4), --stage2 wdf|adaa (default wdf),\n"
        "          --alias-report (print the aliasing metric table for os=1/2/4/8 and exit).\n"
        "M6.5:     --ideal-opamp (bypass the op-amp model: ideal op-amp A/B),\n"
        "M8:       --pedal rat|sd1 (rat = RAT hard clip; sd1 = SD-1 soft/asymmetric;\n"
        "          for sd1 the knob flags map positionally: --distortion=DRIVE, --filter=TONE),\n"
        "          --chain rat|clean (clean = amp+cab+limiter, pedal-bypassed path),\n"
        "          --cab clean212|brit412 (clean-chain cab; brit412 = the darker 4x12),\n"
        "          --limiter-thresh T (clean-chain output soft-limiter threshold, default 0.97).\n"
        "M9.1:     --triode (render a single 12AX7 common-cathode stage alone),\n"
        "          --triode-drive D (input-gain multiplier into the grid, default 3.0),\n"
        "          --triode-cathode UF (cathode bypass cap in uF: 0.68 default, 0 unbypassed, 22 full),\n"
        "          --os 1|2|4|8 also selects the triode oversampling (default 4).\n"
        "M9.2:     --jcm-pre (render the JCM800 2204 preamp: 4x 12AX7 + cathode follower + TMB),\n"
        "          --jcm-drive D (input-gain into V1A's grid, default 1.0),\n"
        "          --jcm-gain G / --jcm-master M (0..1 audio-taper knobs),\n"
        "          --jcm-bass B / --jcm-mid MI / --jcm-treble T (0..1 tone stack),\n"
        "          --os 1|2|4|8 selects the per-stage oversampling (default 4).\n",
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
    if (parts[0] == "pluck" && parts.size() == 3) {
        // A plucked-string-like burst: fast attack, exponentially decaying sum of
        // a few harmonics (brighter partials decay faster). Tool-only test signal
        // for A/B evidence — realistic dynamics without needing a WAV.
        const double f = std::atof(parts[1].c_str());
        const double secs = std::atof(parts[2].c_str());
        const int n = static_cast<int>(secs * fs);
        sig.resize(static_cast<size_t>(n));
        const double attackS = 0.004;  // 4 ms pick attack
        for (int i = 0; i < n; ++i) {
            const double t = i / fs;
            const double attack = 1.0 - std::exp(-t / attackS);
            double s = 0.0;
            for (int h = 1; h <= 6; ++h) {
                const double decay = std::exp(-t * (2.0 + 1.2 * h));  // s^-1
                s += (1.0 / h) * decay * std::sin(kTwoPi * f * h * t);
            }
            sig[static_cast<size_t>(i)] = amp * static_cast<float>(attack * s);
        }
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
        else if (s == "--ideal-opamp") a.idealOpAmp = true;
        else if (s == "--pedal") a.pedal = need("--pedal");
        else if (s == "--chain") a.chain = need("--chain");
        else if (s == "--limiter-thresh") a.limThresh = std::atof(need("--limiter-thresh"));
        else if (s == "--cab") a.cab = need("--cab");
        else if (s == "--triode") a.triode = true;
        else if (s == "--triode-drive") a.triodeDrive = std::atof(need("--triode-drive"));
        else if (s == "--triode-cathode") a.triodeCathodeUf = std::atof(need("--triode-cathode"));
        else if (s == "--jcm-pre") a.jcmPre = true;
        else if (s == "--jcm-drive") a.jcmDrive = std::atof(need("--jcm-drive"));
        else if (s == "--jcm-gain") a.jcmGain = std::atof(need("--jcm-gain"));
        else if (s == "--jcm-master") a.jcmMaster = std::atof(need("--jcm-master"));
        else if (s == "--jcm-bass") a.jcmBass = std::atof(need("--jcm-bass"));
        else if (s == "--jcm-mid") a.jcmMid = std::atof(need("--jcm-mid"));
        else if (s == "--jcm-treble") a.jcmTreble = std::atof(need("--jcm-treble"));
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
            m.setIdealOpAmp(a.idealOpAmp);
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

    std::vector<float> out(input.size(), 0.0f);
    double triodePeakVolts = 0.0;  // reported for the --triode chain
    double jcmPeakVolts = 0.0;     // reported for the --jcm-pre chain

    if (a.jcmPre) {
        // M9.2: the full JCM800 2204 preamp (4x 12AX7 + cathode follower + TMB tone
        // stack + master). The grid drive is the input scaled by --jcm-drive (a
        // bare DI barely moves V1A). Output is the preamp voltage (tens of volts at
        // high gain), peak-normalized to 0.9 for the WAV (raw peak reported below).
        clipper::dsp::Jcm800Preamp pre;
        pre.prepare(fs, 128);
        pre.setOversampling(a.os);
        pre.setParameter(clipper::dsp::Jcm800Preamp::PARAM_GAIN, a.jcmGain);
        pre.setParameter(clipper::dsp::Jcm800Preamp::PARAM_MASTER, a.jcmMaster);
        pre.setParameter(clipper::dsp::Jcm800Preamp::PARAM_BASS, a.jcmBass);
        pre.setParameter(clipper::dsp::Jcm800Preamp::PARAM_MID, a.jcmMid);
        pre.setParameter(clipper::dsp::Jcm800Preamp::PARAM_TREBLE, a.jcmTreble);
        std::vector<float> grid(input.size());
        for (size_t i = 0; i < input.size(); ++i) grid[i] = input[i] * a.jcmDrive;
        if (!grid.empty())
            pre.process(grid.data(), out.data(), static_cast<int>(grid.size()));
        for (float v : out)
            jcmPeakVolts = std::max(jcmPeakVolts, static_cast<double>(std::fabs(v)));
        const double norm = jcmPeakVolts > 1e-9 ? 0.9 / jcmPeakVolts : 1.0;
        for (float& v : out) v = static_cast<float>(v * norm);
    } else if (a.triode) {
        // M9.1: a single 12AX7 common-cathode stage, alone, for listening. The
        // grid drive is the input scaled by --triode-drive (a bare 0.3 V DI barely
        // moves a 12AX7; ~1-3 V grid is where it distorts). Output is the plate AC
        // (next-grid) voltage in the tens of volts, peak-normalized to 0.9 for the
        // WAV (raw plate peak reported below).
        clipper::dsp::TriodeStage stage;
        clipper::dsp::TriodeStage::Config cfg;
        cfg.Ck = a.triodeCathodeUf > 0.0f
                     ? static_cast<double>(a.triodeCathodeUf) * 1e-6
                     : 0.0;
        stage.configure(cfg);
        stage.prepare(fs, 128);
        stage.setOversampling(a.os);
        std::vector<float> grid(input.size());
        for (size_t i = 0; i < input.size(); ++i) grid[i] = input[i] * a.triodeDrive;
        if (!grid.empty())
            stage.process(grid.data(), out.data(), static_cast<int>(grid.size()));
        for (float v : out) triodePeakVolts = std::max(triodePeakVolts,
                                                       static_cast<double>(std::fabs(v)));
        const double norm = triodePeakVolts > 1e-9 ? 0.9 / triodePeakVolts : 1.0;
        for (float& v : out) v = static_cast<float>(v * norm);
    } else if (a.chain == "clean") {
        // Clean (pedal-bypassed) chain: input -> amp (default rig: vol 0.4, tone
        // flat, treble 0.6, bright off) -> default cab -> output soft limiter.
        // This is the fizz-suspect path from the M6.5 clean-path fix; the limiter
        // threshold (--limiter-thresh) contrasts OLD 0.9 vs NEW 0.97 staging.
        clipper::dsp::AmpModel amp;
        amp.prepare(fs, 128);
        amp.setParameter(clipper::dsp::AmpModel::PARAM_VOLUME, 0.4f);
        amp.setParameter(clipper::dsp::AmpModel::PARAM_BASS, 0.5f);
        amp.setParameter(clipper::dsp::AmpModel::PARAM_MIDDLE, 0.5f);
        amp.setParameter(clipper::dsp::AmpModel::PARAM_TREBLE, 0.6f);
        amp.setParameter(clipper::dsp::AmpModel::PARAM_BRIGHT, 0.0f);
        if (!input.empty())
            amp.process(input.data(), out.data(), static_cast<int>(input.size()));
        auto ir = a.cab == "brit412" ? clipper::dsp::generateBrit4x12IR(fs)
                                     : clipper::dsp::generateDefaultCab2x12IR(fs);
        clipper::dsp::CabConvolver cab;
        cab.prepare(fs, ir.data(), static_cast<int>(ir.size()), fs, 128);
        if (!out.empty())
            cab.process(out.data(), out.data(), static_cast<int>(out.size()));
        // M6.6: the shipped limiter is the gain-riding clipper::dsp::OutputLimiter.
        // --limiter-thresh sets its ceiling; the legacy tanh softLimit() above is
        // kept ONLY for the A/B script's OLD-tree contrast builds.
        {
            clipper::dsp::OutputLimiter lim(a.limThresh);
            lim.prepare(fs);
            if (!out.empty()) lim.processMono(out.data(), static_cast<int>(out.size()));
        }
    } else if (a.pedal == "sd1") {
        // M8: process through the SD-1 overdrive (soft, asymmetric feedback clip).
        // Knob flags map positionally: --distortion -> DRIVE, --filter -> TONE.
        clipper::dsp::SdModel model;
        model.prepare(fs, 128);
        model.setOversampling(a.os);
        model.setIdealOpAmp(a.idealOpAmp);
        model.setParameter(clipper::dsp::SdModel::PARAM_DRIVE, a.distortion);
        model.setParameter(clipper::dsp::SdModel::PARAM_TONE, a.filter);
        model.setParameter(clipper::dsp::SdModel::PARAM_LEVEL, a.level);
        if (!input.empty())
            model.process(input.data(), out.data(), static_cast<int>(input.size()));
    } else {
        // Process through the RAT pedal model.
        clipper::dsp::RatModel model;
        model.prepare(fs, 128);
        model.setOversampling(a.os);
        model.setStage2Mode(stage2Mode);
        model.setIdealOpAmp(a.idealOpAmp);
        model.setParameter(clipper::dsp::RatModel::PARAM_DISTORTION, a.distortion);
        model.setParameter(clipper::dsp::RatModel::PARAM_FILTER, a.filter);
        model.setParameter(clipper::dsp::RatModel::PARAM_LEVEL, a.level);
        if (!input.empty())
            model.process(input.data(), out.data(), static_cast<int>(input.size()));
    }

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
    if (a.jcmPre) {
        std::printf(
            "Rendered %zu frames @ %.0f Hz -> %s  (chain=JCM800 preamp, drive=%.2f, "
            "gain=%.2f master=%.2f bass=%.2f mid=%.2f treble=%.2f, os=%dx)\n"
            "  raw preamp peak=%.1f V (normalized to 0.9)  out rms=%.4f\n",
            out.size(), fs, a.outFile.c_str(), a.jcmDrive, a.jcmGain, a.jcmMaster,
            a.jcmBass, a.jcmMid, a.jcmTreble, a.os, jcmPeakVolts, rms);
    } else if (a.triode) {
        std::printf(
            "Rendered %zu frames @ %.0f Hz -> %s  (chain=triode 12AX7, drive=%.2f, "
            "cathode=%.2f uF, os=%dx)\n  raw plate peak=%.1f V (normalized to 0.9)  "
            "out rms=%.4f\n",
            out.size(), fs, a.outFile.c_str(), a.triodeDrive, a.triodeCathodeUf, a.os,
            triodePeakVolts, rms);
    } else if (a.chain == "clean") {
        // Post-attack residual: RMS over the LAST 25% of the render, well after a
        // pluck has decayed. A noise-tail cab re-fires a colored-noise burst per
        // pick that lingers here; the modal cab decays to near silence. This is
        // the demonstrable "no post-attack noise burst" the fizz fix delivers.
        double tailRms = 0.0;
        const size_t ts = out.size() - out.size() / 4;
        for (size_t i = ts; i < out.size(); ++i) tailRms += static_cast<double>(out[i]) * out[i];
        tailRms = out.size() > ts ? std::sqrt(tailRms / (out.size() - ts)) : 0.0;
        const double tailDb = 20.0 * std::log10(tailRms / (rms + 1e-30) + 1e-30);
        std::printf(
            "Rendered %zu frames @ %.0f Hz -> %s  (chain=clean amp:vol0.4/treble0.6+cab=%s, "
            "limiter-thresh=%.2f)\n  peak=%.4f  rms=%.4f  post-attack residual=%.6f (%.1f dB re rms)\n",
            out.size(), fs, a.outFile.c_str(), a.cab.c_str(), a.limThresh, peak, rms, tailRms, tailDb);
    } else if (a.pedal == "sd1") {
        std::printf(
            "Rendered %zu frames @ %.0f Hz -> %s  (pedal=sd1 drive=%.2f tone=%.2f level=%.2f "
            "os=%dx ideal-opamp=%d)\n"
            "  peak=%.4f  rms=%.4f\n",
            out.size(), fs, a.outFile.c_str(), a.distortion, a.filter, a.level, a.os,
            a.idealOpAmp ? 1 : 0, peak, rms);
    } else {
        std::printf(
            "Rendered %zu frames @ %.0f Hz -> %s  (pedal=rat dist=%.2f filter=%.2f level=%.2f "
            "os=%dx stage2=%s ideal-opamp=%d)\n"
            "  peak=%.4f  rms=%.4f\n",
            out.size(), fs, a.outFile.c_str(), a.distortion, a.filter, a.level, a.os,
            a.stage2.c_str(), a.idealOpAmp ? 1 : 0, peak, rms);
    }

    if (!a.spectrum.empty() && !out.empty()) writeSpectrum(a.spectrum, out, fs);
    return 0;
}
