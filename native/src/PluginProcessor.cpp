#include "PluginProcessor.h"

#include <atomic>

#include "CabIrFile.h"
#include "PluginEditor.h"

namespace clipper::native {

namespace {
// The cab/IR picker's choice parameter. Index order == clipper::native::CabChoice
// == the C ABI's built-in indices == web/src/rig.ts CabChoice, so the same integer
// means the same cab everywhere.
// NOTE the ORDER: "Orange 4x12" is APPENDED last (index 3), not inserted next to
// the other built-ins, because this array indexes a stored APVTS choice parameter
// and inserting would re-point every saved session that says "Custom IR".
// clipper::native::CabChoice carries the same reasoning.
const juce::StringArray kCabChoices{juce::String::fromUTF8("Clean 2\xc3\x97" "12"),
                                    juce::String::fromUTF8("Brit 4\xc3\x97" "12"),
                                    "Custom IR",
                                    juce::String::fromUTF8("Orange 4\xc3\x97" "12"),
                                    juce::String::fromUTF8("Tweed 1\xc3\x97" "8")};
// The APVTS state child holding the custom IR's file path.
const juce::Identifier kCabNode{"cab"};
const juce::Identifier kCabCustomPath{"customIr"};

// The four legal oversampling factors, indexed by the APVTS choice parameter.
const juce::StringArray kOversampleChoices{"1x", "2x", "4x", "8x"};
constexpr int kOversampleFactors[] = {1, 2, 4, 8};
const juce::StringArray kChorusChoices{"Off", "Chorus", "Vibrato"};
// Amp voice choice labels, indexed by clipper::native::AmpModel — the index is
// Params::ampModel, host automation and saved sessions all at once. APPEND ONLY:
// inserting one would silently re-voice every saved rig.
//
// THIS LIST IS THE BUG M10.4 SHIPPED. The Mesa was wired into the engine as voice
// 6 and this array was left at six entries, so the Choice parameter could not
// express the value and the voice was unreachable from the plugin UI. A static
// assert against AMP_MODEL_COUNT now makes that a build error rather than a
// silently missing amp.
const juce::StringArray kAmpModelChoices{"Clean 120", "JCM800", "Twin Sixty-Five",
                                        "AC30", "Overdrive 120", "Rocker Verb",
                                        "Dual Rectifier", "Cadet"};
// The whole point of AMP_MODEL_COUNT: a voice added to the engine and not to this
// array is a build error here, not an amp nobody can select. juce::StringArray's
// size is a runtime value, so this is checked in createParameterLayout() with a
// jassert plus the reachability test in native/tests/amp_voice_test.cpp; the
// constant below is what both compare against.
constexpr int kAmpModelCount = clipper::native::AMP_MODEL_COUNT;

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

// Pack a chain into a uint64 the audio thread can read atomically: a 4-bit length
// in bits 0..3, then kMaxChain 4-bit type slots above it.
//
// This was a uint32 and its comment said there was "room for a seventh slot before
// this needs revisiting". The EIGHTH pedal type (M13.1's compressor, landing
// alongside §58's wah) is what crossed it: 4 × (8 + 1) = 36 bits, and the
// static_assert below fired exactly as designed rather than letting a board
// silently truncate. Widened to uint64, which is still ONE lock-free word on every
// platform this ships to — asserted below rather than assumed, because a
// non-lock-free atomic here would put a mutex on the audio thread.
constexpr int kChainField = 4;                       // bits per length/type field
constexpr juce::uint64 kChainMask = (1ull << kChainField) - 1ull;
static_assert(PEDAL_TYPE_COUNT <= (1 << kChainField),
              "a pedal type id no longer fits its packed field");
static_assert(kMaxChain <= (1 << kChainField),
              "the chain length no longer fits its packed field");
static_assert(kChainField * (kMaxChain + 1) <= 64,
              "the packed chain no longer fits a lock-free uint64");
static_assert(std::atomic<juce::uint64>::is_always_lock_free,
              "packedChain_ must be lock-free: the audio thread reads it");

juce::uint64 packChain(const std::vector<int>& types) {
    juce::uint64 v = static_cast<juce::uint64>(
        juce::jlimit<int>(0, kMaxChain, static_cast<int>(types.size())));
    for (int i = 0; i < static_cast<int>(types.size()) && i < kMaxChain; ++i) {
        const juce::uint64 t = static_cast<juce::uint64>(
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

    // Post-v1.1 — the "Weeper" wah. Defaults mirror the web's WAH_KNOB_DEFAULTS:
    // POSITION 0.35 (a low-mid vowel), SENSE 0.0 (opens as a MANUAL wah — the
    // envelope contributes exactly nothing until asked for), VOICE 0.5 (the stock
    // 33 kOhm R7). All three are ordinary automatable knobs; POSITION in
    // particular is meant to be automated, which is how a DAW plays a wah.
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::wahOn, 1}, "Weeper On", true));
    layout.add(knob(pid::wahPosition, "Weeper Position", 0.35f));
    layout.add(knob(pid::wahSense, "Weeper Sense", 0.0f));
    layout.add(knob(pid::wahVoice, "Weeper Voice", 0.5f));
    // M13.1 — the "Squash" OTA compressor. TWO knobs, because the pedal has two:
    // SUSTAIN (which is NOT a threshold — it sets the OTA's idle bias current) and
    // LEVEL. Defaults mirror the web's COMP_KNOB_DEFAULTS: 0.5 / 0.4, and 0.4
    // measures unity at a normal playing level (docs §59).
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::compOn, 1}, "Squash On", true));
    layout.add(knob(pid::compSustain, "Squash Sustain", 0.5f));
    layout.add(knob(pid::compLevel, "Squash Level", 0.4f));
    // M13.7 — the CE-1 "Ensemble" chorus. Defaults mirror the web's
    // CHORUS_KNOB_DEFAULTS: RATE 0.35 (~1.30 Hz in chorus mode), DEPTH 0.5, MODE
    // 0 = CHORUS.
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::ce1On, 1}, "Ensemble On", true));
    layout.add(knob(pid::ce1Rate, "Ensemble Rate", 0.35f));
    layout.add(knob(pid::ce1Depth, "Ensemble Depth", 0.5f));
    layout.add(knob(pid::ce1Mode, "Ensemble Mode", 0.0f));

    // M13.6a — the "Curfew" noise gate. TWO knobs, because the pedal has two:
    // THRESHOLD (which, unlike the compressor's SUSTAIN, really IS a threshold —
    // 40 dB of travel on the open level, 0.00 dB on the gain when open) and
    // DECAY. Defaults mirror the web's GATE_KNOB_DEFAULTS: 0.35 / 0.5 (docs §61).
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::gateOn, 1}, "Curfew On", true));
    layout.add(knob(pid::gateThreshold, "Curfew Threshold", 0.35f));
    layout.add(knob(pid::gateDecay, "Curfew Decay", 0.5f));
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::optoOn, 1}, "Lumen On", true));
    layout.add(knob(pid::optoPeakReduction, "Lumen Peak Reduction", 0.5f));
    layout.add(knob(pid::optoMode, "Lumen Mode", 0.0f));
    layout.add(knob(pid::optoGain, "Lumen Gain", 0.62f));
    // M13.5 the Uni-Vibe. Defaults MUST match VIBE_KNOB_DEFAULTS (web/src/rig.ts)
    // and VibeModel's constructor, or the plugin and the web app open differently.
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::vibeOn, 1}, "Swirl On", true));
    layout.add(knob(pid::vibeSpeed, "Swirl Speed", 0.35f));
    layout.add(knob(pid::vibeIntensity, "Swirl Intensity", 0.70f));
    layout.add(knob(pid::vibeMode, "Swirl Mode", 0.0f));
    // M13.10 drop-tune: one knob. Default 0.0f is the DROP 1 detent (one
    // semitone down), NOT "off" — the selector has no off position.
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::dropOn, 1}, "Cellar On", true));
    layout.add(knob(pid::dropAmount, "Cellar Amount", 0.0f));

    // M13.4 — the "Echoman" BBD analog delay. Defaults mirror web
    // DELAY_KNOB_DEFAULTS: 212 ms, a few repeats, wet behind the dry.
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::delayOn, 1}, "Echoman On", true));
    layout.add(knob(pid::delayTime, "Echoman Delay", 0.35f));
    layout.add(knob(pid::delayFeedback, "Echoman Feedback", 0.3f));
    layout.add(knob(pid::delayBlend, "Echoman Blend", 0.35f));

    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::ampOn, 1}, "Amp Power", true));
    // M9.4 amp voice (default index 0 == Clean 120).
    // Every engine voice must be offerable, or it is unreachable from any host.
    jassert(kAmpModelChoices.size() == kAmpModelCount);
    layout.add(std::make_unique<Choice>(juce::ParameterID{pid::ampModel, 1},
                                        "Amp Model", kAmpModelChoices, 0));
    layout.add(knob(pid::volume, "Volume", 0.4f));
    layout.add(knob(pid::bass, "Bass", 0.5f));
    layout.add(knob(pid::middle, "Middle", 0.5f));
    layout.add(knob(pid::treble, "Treble", 0.6f));
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::bright, 1}, "Bright", false));
    layout.add(std::make_unique<Bool>(juce::ParameterID{pid::cab, 1}, "Cab", true));
    // WHICH cab (the picker). Default index 0 == Clean 2x12, i.e. the cab every
    // build before this one was hard-wired to — a fresh instance and a pre-picker
    // session both render bit-identically to the old engine.
    layout.add(std::make_unique<Choice>(juce::ParameterID{pid::cabModel, 1},
                                        "Cab Model", kCabChoices, 0));

    // M9.4 JCM800-only knobs (bass/middle/treble above are shared with the Clean 120).
    layout.add(knob(pid::jcmGain, "JCM Gain", 0.5f));
    layout.add(knob(pid::jcmMaster, "JCM Master", 0.4f));
    layout.add(knob(pid::jcmPresence, "JCM Presence", 0.5f));

    // M10.3 Orange OR120-only knob. Default 0.2 == F.A.C. position 2 of 6, the same
    // opening position the web's AMP_KNOB_DEFAULTS uses.
    layout.add(knob(pid::orangeFac, "Orange F.A.C.", 0.2f));
    // M10.4 Mesa. Defaults MUST match ClipperEngine::Params' own and the web's
    // AMP_KNOB_DEFAULTS: MODE 1.0 is RED MODERN (the voice a Recto is bought
    // for), rectifier 0.0 is silicon, power 0.0 is BOLD.
    layout.add(knob(pid::mesaMode, "Mesa Mode", 1.0f));
    layout.add(knob(pid::mesaRectifier, "Mesa Rectifier", 0.0f));
    layout.add(knob(pid::mesaPowerMode, "Mesa Power Mode", 0.0f));

    // M10.10 Champ. 0.20 is the EDGE OF BREAKUP, derived from the amp's own
    // measured geometry rather than inherited from the shared volume default
    // (0.40 would open this amp at ~50 % THD — §63.14's "at the wall").
    layout.add(knob(pid::champVolume, "Champ Volume", 0.2f));

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
    // Host automation of the cab choice must reach the engine, so the picker is not
    // a dead parameter. parameterChanged() can fire on the AUDIO thread, hence the
    // AsyncUpdater hop — see the header.
    apvts.addParameterListener(pid::cabModel, this);
    // The default cab is the Clean 2x12, which is exactly what ClipperEngine::
    // prepare() builds anyway, so this only matters for a state load. It is
    // `immediate` because no audio thread exists yet.
    applyCabFromState(/*immediate=*/true);
}

ClipperAudioProcessor::~ClipperAudioProcessor() {
    apvts.removeParameterListener(pid::cabModel, this);
    cancelPendingUpdate();
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

// ---------------------------------------------------------------------------
// THE CAB / IR PICKER — state (message thread)
// ---------------------------------------------------------------------------
int ClipperAudioProcessor::cabChoice() const {
    const int v = static_cast<int>(apvts.getRawParameterValue(pid::cabModel)->load());
    return juce::jlimit(0, CAB_CHOICE_COUNT - 1, v);
}

void ClipperAudioProcessor::setCabChoice(int choice) {
    auto* p = apvts.getParameter(pid::cabModel);
    if (p == nullptr) return;
    const int c = juce::jlimit(0, CAB_CHOICE_COUNT - 1, choice);
    p->beginChangeGesture();
    p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(c)));
    p->endChangeGesture();
    // The listener would get here on its own, but only after an async hop; doing it
    // now keeps a user's click synchronous with the UI that made it — and FORCED,
    // so re-picking the same entry is a real retry rather than a no-op.
    applyCabFromState(/*immediate=*/false, /*force=*/true);
}

juce::String ClipperAudioProcessor::customIrPath() const {
    auto node = apvts.state.getChildWithName(kCabNode);
    if (!node.isValid()) return {};
    return node.getProperty(kCabCustomPath).toString();
}

void ClipperAudioProcessor::setCustomIrPath(const juce::String& path) {
    auto node = apvts.state.getOrCreateChildWithName(kCabNode, nullptr);
    node.setProperty(kCabCustomPath, path, nullptr);
}

bool ClipperAudioProcessor::loadCustomIrFile(const juce::File& file) {
    const CabIrFileResult ir = loadCabIrFile(file, engineRate_);
    if (!ir.ok) {
        // The previous cab keeps playing — a bad file must never leave the rig
        // silent or half-swapped. Say what went wrong and stop.
        cabNote_ = juce::String::fromUTF8("Could not load that IR (") + ir.error + ").";
        cabVersion_.fetch_add(1);
        return false;
    }
    setCustomIrPath(file.getFullPathName());
    customIrLabel_ = ir.label;
    setCabChoice(CAB_CUSTOM);  // applies it (and re-reads the path we just stored)
    if (ir.truncated)
        cabNote_ = juce::String::fromUTF8("Loaded \xe2\x80\x9c") + ir.label +
                   juce::String::fromUTF8("\xe2\x80\x9d \xe2\x80\x94 capped ") +
                   juce::String(ir.originalLength) + juce::String::fromUTF8(" \xe2\x86\x92 ") +
                   juce::String(static_cast<int>(ir.samples.size())) + " samples.";
    cabVersion_.fetch_add(1);
    return true;
}

juce::String ClipperAudioProcessor::cabLabel() const {
    switch (cabChoice()) {
        case CAB_BRIT412:   return kCabChoices[CAB_BRIT412];
        case CAB_ORANGE412: return kCabChoices[CAB_ORANGE412];
        case CAB_CUSTOM:  return customIrLabel_.isNotEmpty() ? customIrLabel_
                                                             : juce::String("Custom IR");
        default:          return kCabChoices[CAB_CLEAN212];
    }
}

// Build the selected cab into the engine's SPARE convolver pair. This is where the
// feature's file I/O and allocation live; the engine's audio thread only ever flips
// an index (ClipperEngine's banner).
void ClipperAudioProcessor::applyCabFromState(bool immediate, bool force) {
    const int choice = cabChoice();
    const juce::String path = choice == CAB_CUSTOM ? customIrPath() : juce::String();
    const juce::String sig = juce::String(choice) + "|" + path;
    // Idempotence. `immediate` (a constructor / prepareToPlay rebuild) always runs;
    // everything else skips a request the engine already holds, so one user click
    // is one declick fade — see lastCabSig_ in the header.
    if (!immediate && !force && sig == lastCabSig_) return;
    lastCabSig_ = sig;

    if (choice == CAB_CUSTOM) {
        const CabIrFileResult ir =
            path.isNotEmpty() ? loadCabIrFile(juce::File(path), engineRate_)
                              : CabIrFileResult{};
        if (ir.ok) {
            customIrLabel_ = ir.label;
            cabNote_ = ir.truncated ? juce::String("IR capped to ") +
                                          juce::String(static_cast<int>(ir.samples.size())) +
                                          " samples."
                                    : juce::String();
            engine_.prepareCabCustom(ir.samples.data(),
                                     static_cast<int>(ir.samples.size()), ir.sampleRate);
        } else {
            // MISSING / UNREADABLE IR. The web's convention (App.tsx: a restored rig
            // that says cabModel:'custom' with no IR data behind it) is to fall back
            // to the Clean 2x12 and SAY SO rather than play a rig with no cab. Native
            // does the same — with one deliberate difference: the PATH is kept in the
            // state tree. A DAW session opened on another machine, or before an
            // external drive is mounted, can be put right by re-selecting Custom
            // instead of having to find the file again.
            customIrLabel_ = {};
            cabNote_ = path.isEmpty()
                           ? juce::String::fromUTF8(
                                 "No IR loaded yet \xe2\x80\x94 using the Clean 2\xc3\x97" "12.")
                           : juce::String::fromUTF8(
                                 "IR not found \xe2\x80\x94 using the Clean 2\xc3\x97" "12.");
            engine_.prepareCabBuiltin(CAB_CLEAN212);
            // Reflect the fallback in the parameter, so the UI, the host and the
            // saved session all agree on what is actually playing. Record the
            // EFFECTIVE signature first: the parameter write re-enters through the
            // listener, and this is what makes that pass a no-op instead of a
            // second prepare and a second fade.
            lastCabSig_ = juce::String(CAB_CLEAN212) + "|";
            if (auto* p = apvts.getParameter(pid::cabModel)) {
                p->beginChangeGesture();
                p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(CAB_CLEAN212)));
                p->endChangeGesture();
            }
        }
    } else {
        cabNote_ = {};
        engine_.prepareCabBuiltin(choice);
    }

    if (immediate) engine_.commitCabNow();
    cabVersion_.fetch_add(1);
}

void ClipperAudioProcessor::parameterChanged(const juce::String& id, float) {
    // May be the AUDIO thread (host automation). Do nothing here but wake the
    // message thread — applyCabFromState() reads files and allocates.
    if (id == pid::cabModel) triggerAsyncUpdate();
}

void ClipperAudioProcessor::handleAsyncUpdate() {
    applyCabFromState(/*immediate=*/false);
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
    p.wahOn = f(pid::wahOn) >= 0.5f;
    p.wahPosition = f(pid::wahPosition);
    p.wahSense = f(pid::wahSense);
    p.wahVoice = f(pid::wahVoice);
    p.compOn = f(pid::compOn) >= 0.5f;
    p.compSustain = f(pid::compSustain);
    p.compLevel = f(pid::compLevel);
    p.gateOn = f(pid::gateOn) >= 0.5f;
    p.gateThreshold = f(pid::gateThreshold);
    p.gateDecay = f(pid::gateDecay);
    p.optoOn = f(pid::optoOn) >= 0.5f;
    p.optoPeakReduction = f(pid::optoPeakReduction);
    p.optoMode = f(pid::optoMode);
    p.optoGain = f(pid::optoGain);
    p.vibeOn = f(pid::vibeOn) >= 0.5f;
    p.vibeSpeed = f(pid::vibeSpeed);
    p.vibeIntensity = f(pid::vibeIntensity);
    p.vibeMode = f(pid::vibeMode);
    p.dropOn = f(pid::dropOn) >= 0.5f;
    p.dropAmount = f(pid::dropAmount);
    p.ce1On = f(pid::ce1On) >= 0.5f;
    p.ce1Rate = f(pid::ce1Rate);
    p.ce1Depth = f(pid::ce1Depth);
    p.ce1Mode = f(pid::ce1Mode);
    p.delayOn = f(pid::delayOn) >= 0.5f;
    p.delayTime = f(pid::delayTime);
    p.delayFeedback = f(pid::delayFeedback);
    p.delayBlend = f(pid::delayBlend);

    // The board: unpack the lock-free snapshot the message thread published (never
    // touch the ValueTree here — this runs on the audio thread).
    {
        const juce::uint64 v = packedChain_.load();
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
    p.orangeFac = f(pid::orangeFac);
    p.mesaMode = f(pid::mesaMode);
    p.mesaRectifier = f(pid::mesaRectifier);
    p.mesaPowerMode = f(pid::mesaPowerMode);
    p.champVolume = f(pid::champVolume);
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
    engineRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    engine_.setParams(p);
    // prepare() rebuilds the SELECTED cab at the new rate from the engine's own
    // source, so a built-in survives a rate change with no work here.
    engine_.prepare(sampleRate, juce::jmax(1, samplesPerBlock));
    // A CUSTOM IR does need re-reading: the samples the engine holds were resampled
    // to the OLD engine rate, and re-decoding the file at the new one beats letting
    // the convolver linearly resample them again. No audio thread is running inside
    // prepareToPlay, so this can commit immediately.
    if (cabChoice() == CAB_CUSTOM) applyCabFromState(/*immediate=*/true);
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
            // The cab travels with the session too: the choice is a parameter (so
            // replaceState already restored it) and the custom IR's path is a
            // property of the tree we just swapped in. Rebuild from both — declicked
            // rather than immediate, because a host may load a preset while the
            // transport is rolling. A missing file falls back here (see
            // applyCabFromState), which is exactly where a moved/absent IR shows up.
            applyCabFromState(/*immediate=*/false);
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
