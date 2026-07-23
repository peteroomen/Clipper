#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace clipper::native {

namespace {
// The four legal oversampling factors, indexed by the APVTS choice parameter.
const juce::StringArray kOversampleChoices{"1x", "2x", "4x", "8x"};
constexpr int kOversampleFactors[] = {1, 2, 4, 8};
const juce::StringArray kChorusChoices{"Off", "Chorus", "Vibrato"};

// A plain 0..1 knob parameter (the core owns the taper law, so the host sees a
// linear normalized position — identical to the web knobs).
std::unique_ptr<juce::AudioParameterFloat> knob(const char* id, const char* name,
                                                float def) {
    return std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>(0.0f, 1.0f),
        def);
}
}  // namespace

juce::AudioProcessorValueTreeState::ParameterLayout
ClipperAudioProcessor::makeLayout() {
    using Bool = juce::AudioParameterBool;
    using Choice = juce::AudioParameterChoice;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Defaults mirror web/src/rig.ts (DEFAULT_RIG / *_KNOB_DEFAULTS).
    layout.add(knob(pid::inputTrim, "Input Trim", 1.0f / 3.0f));

    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::ratOn, 1}, "RAT On", true));
    layout.add(knob(pid::ratDist, "RAT Distortion", 0.7f));
    layout.add(knob(pid::ratFilter, "RAT Filter", 0.4f));
    layout.add(knob(pid::ratLevel, "RAT Level", 0.8f));

    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::sdOn, 1}, "SD-1 On", false));
    layout.add(knob(pid::sdDrive, "SD-1 Drive", 0.5f));
    layout.add(knob(pid::sdTone, "SD-1 Tone", 0.5f));
    layout.add(knob(pid::sdLevel, "SD-1 Level", 0.7f));

    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::ampOn, 1}, "Amp Power", true));
    layout.add(knob(pid::volume, "Volume", 0.4f));
    layout.add(knob(pid::bass, "Bass", 0.5f));
    layout.add(knob(pid::middle, "Middle", 0.5f));
    layout.add(knob(pid::treble, "Treble", 0.6f));
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::bright, 1}, "Bright", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::cab, 1}, "Cab", true));

    layout.add(std::make_unique<Choice>(juce::ParameterID{pid::chorusMode, 1},
                                        "Chorus Mode", kChorusChoices, 0));
    layout.add(knob(pid::chorusSpeed, "Chorus Speed", 0.3f));
    layout.add(knob(pid::chorusDepth, "Chorus Depth", 0.5f));

    // M6.7 spring reverb: single wet/dry MIX knob, default 0 (dry).
    layout.add(knob(pid::reverb, "Reverb", 0.0f));

    // Oversampling default index 2 == 4x.
    layout.add(std::make_unique<Choice>(juce::ParameterID{pid::oversampling, 1},
                                        "Oversampling", kOversampleChoices, 2));
    return layout;
}

ClipperAudioProcessor::ClipperAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::mono(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(),
                                           true)),
      apvts(*this, nullptr, "state", makeLayout()) {}

Params ClipperAudioProcessor::snapshotParams() const {
    Params p;
    auto f = [this](const char* id) {
        return apvts.getRawParameterValue(id)->load();
    };
    p.inputTrim = f(pid::inputTrim);
    p.ratOn = f(pid::ratOn) >= 0.5f;
    p.ratDist = f(pid::ratDist);
    p.ratFilter = f(pid::ratFilter);
    p.ratLevel = f(pid::ratLevel);
    p.sdOn = f(pid::sdOn) >= 0.5f;
    p.sdDrive = f(pid::sdDrive);
    p.sdTone = f(pid::sdTone);
    p.sdLevel = f(pid::sdLevel);
    p.ampOn = f(pid::ampOn) >= 0.5f;
    p.volume = f(pid::volume);
    p.bass = f(pid::bass);
    p.middle = f(pid::middle);
    p.treble = f(pid::treble);
    p.bright = f(pid::bright) >= 0.5f;
    p.cab = f(pid::cab) >= 0.5f;
    p.chorusMode = static_cast<int>(f(pid::chorusMode));  // choice index == mode
    p.chorusSpeed = f(pid::chorusSpeed);
    p.chorusDepth = f(pid::chorusDepth);
    p.reverb = f(pid::reverb);
    p.oversampling = kOversampleFactors[static_cast<int>(f(pid::oversampling)) & 3];
    return p;
}

void ClipperAudioProcessor::updateLatency(const Params& p) {
    // latencySamples() reads the engine's live cab/OS state, which prepare() has
    // already set from `p`; report to the host only when it changes.
    const int lat = engine_.latencySamples();
    if (lat != lastReportedLatency_) {
        setLatencySamples(lat);
        lastReportedLatency_ = lat;
    }
    juce::ignoreUnused(p);
}

void ClipperAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    const Params p = snapshotParams();
    engine_.setParams(p);
    engine_.prepare(sampleRate, juce::jmax(1, samplesPerBlock));
    lastReportedLatency_ = -1;
    updateLatency(p);
}

bool ClipperAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    // Mono in -> stereo out is the intended shape; also accept stereo in (we take
    // channel 0 as the mono source) so DAWs that only offer stereo tracks work.
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo()) return false;
    const auto in = layouts.getMainInputChannelSet();
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void ClipperAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int numFrames = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    // Snapshot params and apply ONLY the changed ones (realtime-safe: no allocation,
    // and — crucially — unchanged high-gain knobs are never re-seeded, so a steady
    // chain is bit-identical to a single-shot core render). Oversampling/cab toggles
    // are handled inside updateParams / process; latency is republished on change.
    const Params p = snapshotParams();
    engine_.updateParams(p);
    updateLatency(p);

    // Mono source = input channel 0 (or silence if the host gave us no input).
    const float* in = numIn > 0 ? buffer.getReadPointer(0) : nullptr;
    static thread_local std::vector<float> monoIn;
    monoIn.assign(static_cast<size_t>(numFrames), 0.0f);
    if (in) std::copy(in, in + numFrames, monoIn.begin());

    // Engine writes stereo into channels 0 (L) and 1 (R). Guard for a mono-out
    // host by pointing R at a scratch buffer.
    float* outL = numOut > 0 ? buffer.getWritePointer(0) : monoIn.data();
    static thread_local std::vector<float> scratchR;
    scratchR.assign(static_cast<size_t>(numFrames), 0.0f);
    float* outR = numOut > 1 ? buffer.getWritePointer(1) : scratchR.data();

    engine_.process(monoIn.data(), outL, outR, numFrames);

    // Clear any output channels beyond the stereo pair.
    for (int ch = 2; ch < numOut; ++ch) buffer.clear(ch, 0, numFrames);
}

void ClipperAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void ClipperAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes)) {
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
    }
}

juce::AudioProcessorEditor* ClipperAudioProcessor::createEditor() {
    return new ClipperAudioProcessorEditor(*this);
}

}  // namespace clipper::native

// The plugin entry point JUCE's wrapper code calls.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new clipper::native::ClipperAudioProcessor();
}
