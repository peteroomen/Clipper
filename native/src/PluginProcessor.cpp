#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace clipper::native {

namespace {
// The four legal oversampling factors, indexed by the APVTS choice parameter.
const juce::StringArray kOversampleChoices{"1x", "2x", "4x", "8x"};
constexpr int kOversampleFactors[] = {1, 2, 4, 8};
const juce::StringArray kChorusChoices{"Off", "Chorus", "Vibrato"};
// M9.4/M10.1/M10.2 amp voice: choice index 0 == Clean 120, 1 == JCM800, 2 == Twin,
// 3 == AC30 (matches Params::ampModel).
const juce::StringArray kAmpModelChoices{"Clean 120", "JCM800", "Twin Sixty-Five", "AC30"};

// A plain 0..1 knob parameter (the core owns the taper law, so the host sees a
// linear normalized position — identical to the web knobs).
std::unique_ptr<juce::AudioParameterFloat> knob(const char* id, const char* name,
                                                float def) {
    return std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>(0.0f, 1.0f),
        def);
}

// The APVTS state child holding the board (chain order + membership).
const juce::Identifier kBoardNode{"board"};
const juce::Identifier kBoardOrder{"order"};

// Pack a chain into a uint32 the audio thread can read atomically: a 4-bit length
// in bits 0..3, then kMaxChain 4-bit type slots above it. With six pedal types the
// board is 4 + 6 × 4 = 28 bits — comfortably one lock-free word, with a spare type
// value per slot and room for a seventh slot before this needs revisiting.
constexpr int kChainField = 4;                       // bits per length/type field
constexpr juce::uint32 kChainMask = (1u << kChainField) - 1u;
static_assert(PEDAL_TYPE_COUNT <= (1 << kChainField),
              "a pedal type id no longer fits its packed field");
static_assert(kMaxChain <= (1 << kChainField),
              "the chain length no longer fits its packed field");
static_assert(kChainField * (kMaxChain + 1) <= 32,
              "the packed chain no longer fits a lock-free uint32");

juce::uint32 packChain(const std::vector<int>& types) {
    juce::uint32 v = static_cast<juce::uint32>(
        juce::jlimit<int>(0, kMaxChain, static_cast<int>(types.size())));
    for (int i = 0; i < static_cast<int>(types.size()) && i < kMaxChain; ++i) {
        const juce::uint32 t = static_cast<juce::uint32>(
            juce::jlimit(0, PEDAL_TYPE_COUNT - 1, types[static_cast<size_t>(i)]));
        v |= t << (kChainField * (i + 1));
    }
    return v;
}

// "rat,muff,phaser" <-> the type list. Unknown keys and duplicates are dropped
// (each type is instantiable once), so malformed state degrades to a valid board.
std::vector<int> parseChain(const juce::String& csv) {
    std::vector<int> out;
    juce::StringArray parts = juce::StringArray::fromTokens(csv, ",", "");
    for (auto& raw : parts) {
        const juce::String key = raw.trim();
        if (key.isEmpty()) continue;
        const int t = pedalTypeFromKey(key.toRawUTF8());
        if (t < 0) continue;
        bool dup = false;
        for (int have : out) dup = dup || have == t;
        if (!dup && static_cast<int>(out.size()) < kMaxChain) out.push_back(t);
    }
    return out;
}

juce::String chainToString(const std::vector<int>& types) {
    juce::StringArray keys;
    for (int t : types)
        if (const char* k = pedalTypeKey(t)) keys.add(k);
    return keys.joinIntoString(",");
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

    // Native parity: the pedals the fixed chain never had. Every knob set is FULLY
    // automatable and always present (APVTS layouts are static — a parameter cannot
    // appear when a pedal is added to the board), so a pedal that is off the board
    // simply has its params ignored by the engine. Defaults mirror the web's
    // TS_KNOB_DEFAULTS / MUFF_KNOB_DEFAULTS / PHASER_KNOB_DEFAULTS, and each
    // engaged-flag defaults TRUE because web makePedal() drops a pedal on the board
    // already engaged (the SD-1's legacy `false` default is left untouched).
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::tsOn, 1}, "Screamer On", true));
    layout.add(knob(pid::tsDrive, "Screamer Drive", 0.5f));
    layout.add(knob(pid::tsTone, "Screamer Tone", 0.5f));
    layout.add(knob(pid::tsLevel, "Screamer Level", 0.75f));

    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::muffOn, 1}, "Pi On", true));
    layout.add(knob(pid::muffSustain, "Pi Sustain", 0.6f));
    layout.add(knob(pid::muffTone, "Pi Tone", 0.5f));
    layout.add(knob(pid::muffVolume, "Pi Volume", 0.6f));

    // The phaser has exactly ONE real control (SPEED). The web carries two unused
    // slots for its shared pedal shape; exposing them as host parameters would be
    // lying about what the pedal does, so they are simply not here.
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::phaserOn, 1}, "Ninety On", true));
    layout.add(knob(pid::phaserSpeed, "Ninety Speed", 0.35f));

    // v1.1 item 6 — the GOLD "Myth" transparent overdrive. Defaults mirror the web's
    // GOLD_KNOB_DEFAULTS: a mostly-clean 0.35 gain, flat (0.5) treble, and a 0.7
    // output that opens as a modest boost, which is what this box is for.
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::goldOn, 1}, "Myth On", true));
    layout.add(knob(pid::goldGain, "Myth Gain", 0.35f));
    layout.add(knob(pid::goldTreble, "Myth Treble", 0.5f));
    layout.add(knob(pid::goldLevel, "Myth Output", 0.7f));

    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::ampOn, 1}, "Amp Power", true));
    // M9.4 amp voice (default index 0 == Clean 120).
    layout.add(std::make_unique<Choice>(juce::ParameterID{pid::ampModel, 1},
                                        "Amp Model", kAmpModelChoices, 0));
    layout.add(knob(pid::volume, "Volume", 0.4f));
    layout.add(knob(pid::bass, "Bass", 0.5f));
    layout.add(knob(pid::middle, "Middle", 0.5f));
    layout.add(knob(pid::treble, "Treble", 0.6f));
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::bright, 1}, "Bright", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::cab, 1}, "Cab", true));

    // M9.4 JCM800-only knobs (bass/middle/treble above are shared with the Clean 120).
    layout.add(knob(pid::jcmGain, "JCM Gain", 0.5f));
    layout.add(knob(pid::jcmMaster, "JCM Master", 0.4f));
    layout.add(knob(pid::jcmPresence, "JCM Presence", 0.5f));

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
      apvts(*this, nullptr, "state", makeLayout()) {
    // A fresh instance opens on the web app's default rig: one RAT on the board.
    setChainOrder(defaultChain());
}

std::vector<int> ClipperAudioProcessor::chainOrder() const {
    auto node = apvts.state.getChildWithName(kBoardNode);
    if (!node.isValid()) return defaultChain();
    return parseChain(node.getProperty(kBoardOrder).toString());
}

void ClipperAudioProcessor::setChainOrder(const std::vector<int>& types) {
    // Normalize (drop unknowns/duplicates/overflow) by round-tripping through the
    // string form, so the tree and the packed atomic can never disagree.
    const std::vector<int> clean = parseChain(chainToString(types));
    auto node = apvts.state.getOrCreateChildWithName(kBoardNode, nullptr);
    node.setProperty(kBoardOrder, chainToString(clean), nullptr);
    packedChain_.store(packChain(clean));
    chainVersion_.fetch_add(1);
}

void ClipperAudioProcessor::syncChainFromState(bool legacyFallback) {
    auto node = apvts.state.getChildWithName(kBoardNode);
    if (!node.isValid()) {
        // No board node: this state was written by the PRE-PARITY build, whose
        // chain was the hard-wired RAT → SD-1 pair. Restore that pair so the
        // session sounds exactly as it did (its ratOn/sdOn flags do the rest).
        setChainOrder(legacyFallback ? legacyChain() : defaultChain());
        return;
    }
    std::vector<int> types = parseChain(node.getProperty(kBoardOrder).toString());
    setChainOrder(types);  // re-publishes the atomic + bumps the version
}

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
    p.tsOn = f(pid::tsOn) >= 0.5f;
    p.tsDrive = f(pid::tsDrive);
    p.tsTone = f(pid::tsTone);
    p.tsLevel = f(pid::tsLevel);
    p.muffOn = f(pid::muffOn) >= 0.5f;
    p.muffSustain = f(pid::muffSustain);
    p.muffTone = f(pid::muffTone);
    p.muffVolume = f(pid::muffVolume);
    p.phaserOn = f(pid::phaserOn) >= 0.5f;
    p.phaserSpeed = f(pid::phaserSpeed);
    p.goldOn = f(pid::goldOn) >= 0.5f;
    p.goldGain = f(pid::goldGain);
    p.goldTreble = f(pid::goldTreble);
    p.goldLevel = f(pid::goldLevel);

    // The board: unpack the lock-free snapshot the message thread published (never
    // touch the ValueTree here — this runs on the audio thread).
    {
        const juce::uint32 v = packedChain_.load();
        const int len = juce::jlimit(0, kMaxChain, static_cast<int>(v & kChainMask));
        p.chainLength = len;
        for (int i = 0; i < kMaxChain; ++i)
            p.chain[i] = i < len ? juce::jlimit(0, PEDAL_TYPE_COUNT - 1,
                                                static_cast<int>(
                                                    (v >> (kChainField * (i + 1))) & kChainMask))
                                 : 0;
    }
    p.ampOn = f(pid::ampOn) >= 0.5f;
    p.ampModel = static_cast<int>(f(pid::ampModel));  // choice index == model id
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
    p.jcmGain = f(pid::jcmGain);
    p.jcmMaster = f(pid::jcmMaster);
    p.jcmPresence = f(pid::jcmPresence);
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
        if (xml->hasTagName(apvts.state.getType())) {
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
            // replaceState swapped in the saved tree wholesale — republish the board
            // to the audio thread (and migrate a pre-parity session, which has no
            // board node, back onto its old fixed RAT → SD-1 pair).
            syncChainFromState(/*legacyFallback=*/true);
        }
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
