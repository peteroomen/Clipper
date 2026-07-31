#include "CabIrFile.h"

#include <cmath>

namespace clipper::native {

namespace {

// Average every channel into one mono buffer — web/src/cab.ts monoize(). Taking
// channel 0 instead would quietly change the voicing of any true-stereo IR.
std::vector<float> monoise(const juce::AudioBuffer<float>& buf) {
    const int n = buf.getNumSamples();
    const int ch = juce::jmax(1, buf.getNumChannels());
    std::vector<float> out(static_cast<size_t>(juce::jmax(0, n)), 0.0f);
    for (int c = 0; c < buf.getNumChannels(); ++c) {
        const float* src = buf.getReadPointer(c);
        for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] += src[i];
    }
    if (ch > 1)
        for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] /= static_cast<float>(ch);
    return out;
}

// Sample-rate convert with JUCE's order-5 Lagrange interpolator. The input is
// zero-padded so the interpolator's look-ahead can never read past the buffer, and
// the output length is FLOORED for the same reason.
std::vector<float> resampleMono(const std::vector<float>& in, double fromRate,
                                double toRate) {
    if (in.empty() || fromRate <= 0.0 || toRate <= 0.0 ||
        std::fabs(fromRate - toRate) < 1e-6)
        return in;

    const double ratio = fromRate / toRate;  // input samples consumed per output
    const int outLen = static_cast<int>(std::floor(static_cast<double>(in.size()) / ratio));
    if (outLen <= 0) return {};

    // Pad: process() consumes ceil(outLen * ratio) input samples plus the
    // interpolator's own history.
    std::vector<float> padded = in;
    padded.resize(in.size() + 16, 0.0f);

    std::vector<float> out(static_cast<size_t>(outLen), 0.0f);
    juce::LagrangeInterpolator interp;
    interp.reset();
    interp.process(ratio, padded.data(), out.data(), outLen);
    return out;
}

// Cap to kMaxIrSamples with a raised-cosine fade on the tail — web capLength().
void capLength(std::vector<float>& s, bool& truncated) {
    truncated = false;
    if (static_cast<int>(s.size()) <= kMaxIrSamples) return;
    truncated = true;
    s.resize(static_cast<size_t>(kMaxIrSamples));
    const int start = kMaxIrSamples - kIrTruncateFade;
    for (int i = start; i < kMaxIrSamples; ++i) {
        const double t = static_cast<double>(i - start) / kIrTruncateFade;  // 0..1
        s[static_cast<size_t>(i)] *= static_cast<float>(0.5 * (1.0 + std::cos(M_PI * t)));
    }
}

}  // namespace

CabIrFileResult loadCabIrFile(const juce::File& file, double engineRate) {
    CabIrFileResult r;
    r.sampleRate = engineRate > 0.0 ? engineRate : 48000.0;

    if (file == juce::File() || !file.existsAsFile()) {
        r.error = "IR file not found";
        return r;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();  // WAV + AIFF on every platform
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr) {
        r.error = "unreadable audio file";
        return r;
    }
    if (reader->numChannels < 1 || reader->lengthInSamples < 1 ||
        reader->sampleRate <= 0.0) {
        r.error = "empty audio file";
        return r;
    }

    const juce::int64 maxRead =
        static_cast<juce::int64>(kMaxIrSourceSeconds * reader->sampleRate);
    const int n = static_cast<int>(juce::jmin(reader->lengthInSamples, maxRead));
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), n);
    buf.clear();
    if (!reader->read(&buf, 0, n, 0, true, true)) {
        r.error = "could not decode the audio file";
        return r;
    }

    std::vector<float> mono = monoise(buf);
    mono = resampleMono(mono, reader->sampleRate, r.sampleRate);
    if (mono.empty()) {
        r.error = "IR is too short after resampling";
        return r;
    }
    r.originalLength = static_cast<int>(mono.size());
    capLength(mono, r.truncated);

    // An all-zero file would peak-normalize to nothing and leave the rig silent —
    // that is a broken IR, not a quiet one, so reject it with a message.
    float peak = 0.0f;
    for (float v : mono) peak = juce::jmax(peak, std::fabs(v));
    if (!(peak > 0.0f) || !std::isfinite(peak)) {
        r.error = "IR is silent or contains non-finite samples";
        return r;
    }

    r.samples = std::move(mono);
    r.label = file.getFileNameWithoutExtension().substring(0, 40);
    if (r.label.isEmpty()) r.label = "Custom IR";
    r.ok = true;
    return r;
}

}  // namespace clipper::native
