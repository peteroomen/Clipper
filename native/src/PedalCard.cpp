#include "PedalCard.h"

namespace clipper::native {

namespace {
constexpr int kRackH = 22;      // the chain-position chip row
constexpr int kChipW = 24;
constexpr int kEyebrowH = 14;
constexpr int kLedD = 14;       // .led is 14px in pedal.css
constexpr int kKnobH = 88;
constexpr int kJackD = 16;      // .jack is 16px

// The per-type faces — the native sibling of the web FACES table. Same names, same
// accents, same morphology cues. Knob labels/order mirror each face's knob array;
// the underlying param ids are the ones the host automates.
//
// THE MODEL-LINE NUMBER IS PER CATEGORY, NOT PER BOARD — "Drive Nº2" is the second
// DRIVE, not the second pedal. Take the next number from the entries sharing your
// category word, never from the length of this table. The old board-wide sequence
// could not survive parallel slices: three landed at once and all three shipped
// "Nº8". These strings must match web/src/components/Pedal.tsx's FACES exactly.
const PedalFace kFaces[] = {
    // RAT — the reference: red, the round stomp, the vertical stack.
    {"Dirt N\xc2\xba""1 \xc2\xb7 Rodent-Type", "Rodent", skin::AccentId::Rat,
     Footswitch::Shape::Round, PedalFace::Layout::Stack, 38.0f,
     {{"Dist", pid::ratDist}, {"Filter", pid::ratFilter}, {"Level", pid::ratLevel}},
     pid::ratOn, PEDAL_RAT},
    // SD-1 — yellow, and the Boss-compact RUBBER TREADLE (the morphology cue).
    {"Drive N\xc2\xba""1 \xc2\xb7 Yellow", "Super Drive", skin::AccentId::Sd,
     Footswitch::Shape::Treadle, PedalFace::Layout::Stack, 26.0f,
     {{"Drive", pid::sdDrive}, {"Tone", pid::sdTone}, {"Level", pid::sdLevel}},
     pid::sdOn, PEDAL_SD},
    // TS — the green box, and the Ibanez-format hinged metal PAD.
    {"Drive N\xc2\xba""2 \xc2\xb7 Green", "Screamer", skin::AccentId::Ts,
     Footswitch::Shape::Pad, PedalFace::Layout::Stack, 30.0f,
     {{"Drive", pid::tsDrive}, {"Tone", pid::tsTone}, {"Level", pid::tsLevel}},
     pid::tsOn, PEDAL_TS},
    // Muff — violet, the big stomp, and the classic three-knob TRIANGLE. Knob order
    // is the triangle placement: Sustain top-left, Volume top-right, Tone below.
    {"Fuzz N\xc2\xba""1 \xc2\xb7 Pi", "Pi", skin::AccentId::Muff,
     Footswitch::Shape::BigRound, PedalFace::Layout::Triangle, 44.0f,
     {{"Sustain", pid::muffSustain}, {"Volume", pid::muffVolume}, {"Tone", pid::muffTone}},
     pid::muffOn, PEDAL_MUFF},
    // Phaser — burnt orange, and the iconic ONE big knob.
    {"Phaser N\xc2\xba""1 \xc2\xb7 Script", "Ninety", skin::AccentId::Phaser,
     Footswitch::Shape::Round, PedalFace::Layout::Single, 34.0f,
     {{"Speed", pid::phaserSpeed}},
     pid::phaserOn, PEDAL_PHASER},
    // GOLD — the hoarded gold box. Its morphology cue is the milled NAMEPLATE band:
    // the original is remembered for a colour and for being ENGRAVED rather than
    // printed, so native takes the colour and the engraving IDEA. Type only — the
    // figure on the real enclosure IS the trademark and is deliberately absent, as
    // are the words Klon/Centaur/KTR. "Myth" is the wink at what the box became.
    {"Drive N\xc2\xba""3 \xc2\xb7 Gold", "Myth", skin::AccentId::Gold,
     Footswitch::Shape::Round, PedalFace::Layout::Plate, 30.0f,
     {{"Gain", pid::goldGain}, {"Treble", pid::goldTreble}, {"Output", pid::goldLevel}},
     pid::goldOn, PEDAL_GOLD},
    // This array was once indexed BY PedalType, and its Squash/Weeper entries were
    // SWAPPED (Squash at 6, Weeper at 7, while PEDAL_WAH = 6 and PEDAL_COMP = 7), so
    // a wah card drew the compressor's face and vice versa. The menu stayed right
    // because pedalMenuLabel uses explicit `case` labels — which is exactly why the
    // two could disagree. Each entry now carries its own PedalType and lookup is
    // KEYED, so position means nothing and that class of bug cannot recur.
    // SQUASH — M13.1, the first DYNAMICS pedal and the first non-dirt box. Its
    // morphology cue is simply that it has TWO knobs where every dirt box has
    // three: a small MXR-format enclosure over a round stomp. Teal accent (the
    // real pedal is red, and Rat owns red here). No MXR/Dyna Comp/Ross text.
    {"Dynamics N\xc2\xba""1 \xc2\xb7 Squash", "Squash", skin::AccentId::Comp,
     Footswitch::Shape::Round, PedalFace::Layout::Stack, 34.0f,
     {{"Sustain", pid::compSustain}, {"Level", pid::compLevel}},
     pid::compOn, PEDAL_COMP},
    // WAH "Weeper" (docs §58) — the board's first FILTER pedal, and the first one
    // whose real enclosure is a ROCKING TREADLE rather than a box. The treadle
    // footswitch shape is that morphology cue (shared with the Boss-compact SD-1
    // here; the web front-end draws its own ribbed 'rocker' plate). TEAL accent,
    // the furthest hue from the six dirt/mod pedals. No Dunlop/Cry Baby/Vox/
    // Mu-Tron wording anywhere. POSITION / SENSE / VOICE: SENSE at 0 is a plain
    // manual wah, above 0 an envelope follower sweeps the SAME tank.
    {"Filter N\xc2\xba""1 \xc2\xb7 Treadle", "Weeper", skin::AccentId::Wah,
     Footswitch::Shape::Treadle, PedalFace::Layout::Stack, 26.0f,
     {{"Position", pid::wahPosition}, {"Sense", pid::wahSense}, {"Voice", pid::wahVoice}},
     pid::wahOn, PEDAL_WAH},
    // CURFEW — M13.6a, the board's first UTILITY: it makes no sound of its own,
    // it takes one away. Shares the compressor's two-knob 'stack' geometry (two
    // knobs where a dirt box has three is the cue for "not a voice") and is told
    // apart by the SLATE accent — deliberately the most muted colour on the
    // board, because a gate is plumbing. No Boss/NS-2/ISP/Decimator text.
    {"Utility N\xc2\xba""1 \xc2\xb7 Gate", "Curfew", skin::AccentId::Gate,
     Footswitch::Shape::Round, PedalFace::Layout::Stack, 34.0f,
     {{"Thresh", pid::gateThreshold}, {"Decay", pid::gateDecay}},
     pid::gateOn, PEDAL_GATE},
    // LUMEN — M13.3, the second DYNAMICS voice and the board's first three-slot
    // pedal whose middle slot is a real (discrete) control. Takes the Compact
    // anatomy because a levelling amplifier is a studio box rather than a stomp
    // (native has four layouts and 'compact' is a WEB CSS variant, so the card
    // takes Stack with the SD-1's rectangular Pad footswitch), and the PERIWINKLE
    // accent: a T4B's electroluminescent panel glows blue-
    // green, pushed to blue-violet so it separates from the delay's deep azure
    // and the Muff's magenta-violet. No Teletronix/UREI/LA-2A text.
    {"Dynamics N\xc2\xba""2 \xc2\xb7 Leveler", "Lumen", skin::AccentId::Opto,
     Footswitch::Shape::Pad, PedalFace::Layout::Stack, 30.0f,
     {{"Peak", pid::optoPeakReduction},
      {"Mode", pid::optoMode, {"COMP", "LIMIT"}},
      {"Gain", pid::optoGain}},
     pid::optoOn, PEDAL_OPTO},
    // SWIRL — M13.5, the Uni-Vibe: the board's SECOND phaser, and the face has to
    // say so before a knob is touched. Morphology cue is the real thing's tall,
    // upright box, so it takes the Stack anatomy with a Round stomp. AMBER accent
    // (the incandescent lamp inside it), which is the warmest hue on a board whose
    // other modulation cards are the phaser's orange-red and the CE-1's magenta.
    // No Univox / Uni-Vibe / Shin-ei wording anywhere. SPEED / INTENSITY / MODE,
    // and MODE is a DISCRETE two-position switch in the model.
    {"Modulation N\xc2\xba""2 \xc2\xb7 Photocell", "Swirl", skin::AccentId::Vibe,
     Footswitch::Shape::Round, PedalFace::Layout::Stack, 34.0f,
     {{"Speed", pid::vibeSpeed},
      {"Intensity", pid::vibeIntensity},
      {"Mode", pid::vibeMode, {"CHORUS", "VIBRATO"}}},
     pid::vibeOn, PEDAL_VIBE},
    // BASEMENT — M13.10, the board's FIRST pitch shifter and its SECOND
    // one-knob face, so the accent has to carry the whole distinction from the
    // Ninety: GRAPHITE-CYAN against the phaser's burnt orange. One knob because
    // the reference has one control and no MIX (docs §70.2), and it is a
    // 9-position ROTARY the core quantizes rather than a sweep. No DigiTech /
    // Drop / Whammy wording anywhere.
    {"Pitch N\xc2\xba""1 \xc2\xb7 Poly", "Cellar", skin::AccentId::Drop,
     Footswitch::Shape::Round, PedalFace::Layout::Single, 34.0f,
     // NINE positions: 1..7 semitones down, the octave, then the octave WITH the
     // dry signal summed. OCT+DRY is the last click and the original pitch is
     // audible alongside the octave there — faithful to the reference, and the
     // reason the position has to be NAMED rather than read "100".
     {{"Drop", pid::dropAmount,
       {"-1", "-2", "-3", "-4", "-5", "-6", "-7", "OCT", "OCT+DRY"}}},
     pid::dropOn, PEDAL_DROP},
    // DECADE — M13.6, the board's first EQ and the ONLY face with more than three
    // controls: ten band knobs in a 2x5 grid over a GAIN/VOLUME row. SILVER
    // accent, because the reference family is a plain metal box whose identity is
    // the control bank itself and silver is the one accent no other pedal here
    // uses. No MXR / M108 / Dunlop wording anywhere.
    //
    // KNOWN DIVERGENCE FROM THE WEB, recorded rather than left to be discovered:
    // the web face draws ten vertical FADERS (a graphic EQ is bought so the curve
    // can be read off the slider positions) and this one draws knobs, because the
    // native kit has no fader widget yet and inventing one was out of this
    // slice's scope. The PARAMETERS, their ids and their behaviour are identical;
    // only the widget differs. A native Fader is the named follow-up.
    {"EQ N\xc2\xba""1 \xc2\xb7 Ten Band", "Decade", skin::AccentId::Eq,
     Footswitch::Shape::Round, PedalFace::Layout::Bank, 30.0f,
     {{"31 Hz", pid::eqBand31},
      {"63 Hz", pid::eqBand63},
      {"125 Hz", pid::eqBand125},
      {"250 Hz", pid::eqBand250},
      {"500 Hz", pid::eqBand500},
      {"1 kHz", pid::eqBand1k},
      {"2 kHz", pid::eqBand2k},
      {"4 kHz", pid::eqBand4k},
      {"8 kHz", pid::eqBand8k},
      {"16 kHz", pid::eqBand16k},
      {"Gain", pid::eqGain}, {"Vol", pid::eqVolume}},
     pid::eqOn, PEDAL_EQ},
    // ENSEMBLE — M13.7, the CE-1 Chorus Ensemble: the second MODULATION pedal and
    // the first whose circuit the project already owned (it is the JC-120 amp's
    // chorus in a floor box — docs §62). Morphology cue is the real CE-1's big,
    // wide, low enclosure, so it takes the milled Plate anatomy rather than the
    // compact Stack. MAGENTA accent: the phaser owns orange and this is the second
    // modulation box, so the hue separates the family. No Boss/Roland/CE-1 text.
    // RATE / DEPTH / MODE, and MODE is a DISCRETE two-position switch in the model.
    {"Modulation N\xc2\xba""1 \xc2\xb7 Ensemble", "Ensemble", skin::AccentId::Chorus,
     Footswitch::Shape::Round, PedalFace::Layout::Plate, 34.0f,
     {{"Rate", pid::ce1Rate},
      {"Depth", pid::ce1Depth},
      {"Mode", pid::ce1Mode, {"CHORUS", "VIBRATO"}}},
     pid::ce1On, PEDAL_CHORUS},
    // ECHOMAN (docs §60) — M13.4, the board's FIRST DELAY and a new DSP family.
    // Its morphology cue is simply that a Memory Man is a WIDE box: the card is a
    // plain three-knob Stack here (native lays cards out on a fixed rail, so width
    // is not a card-level property the way it is on the web face), and the
    // identity is carried by the DEEP BLUE accent — the furthest hue on the board
    // from every dirt/mod/filter box. DELAY / FEEDBACK / BLEND. No
    // Electro-Harmonix / Memory Man / Deluxe wording anywhere.
    {"Delay N\xc2\xba""1 \xc2\xb7 Bucket Brigade", "Echoman", skin::AccentId::Delay,
     Footswitch::Shape::Round, PedalFace::Layout::Stack, 30.0f,
     {{"Delay", pid::delayTime}, {"Feedback", pid::delayFeedback}, {"Blend", pid::delayBlend}},
     pid::delayOn, PEDAL_DELAY},
};
}  // namespace

constexpr int kNumFaces = static_cast<int>(sizeof(kFaces) / sizeof(kFaces[0]));

const PedalFace& pedalFace(int type) {
    for (int i = 0; i < kNumFaces; ++i)
        if (kFaces[i].type == type) return kFaces[i];
    // A reserved-but-unfilled PedalType. Callers that could reach one filter on
    // pedalHasFace() first; this fallback exists so a stale saved board can never
    // dereference a NULL face.
    return kFaces[0];
}

bool pedalHasFace(int type) {
    for (int i = 0; i < kNumFaces; ++i)
        if (kFaces[i].type == type) return true;
    return false;
}

juce::String pedalMenuLabel(int type) {
    switch (type) {
        case PEDAL_RAT:    return "Rodent - RAT-type distortion";
        case PEDAL_SD:     return "Super Drive - SD-type overdrive";
        case PEDAL_TS:     return "Screamer - green overdrive";
        case PEDAL_MUFF:   return "Pi - big-box fuzz";
        case PEDAL_PHASER: return "Ninety - script phaser";
        case PEDAL_GOLD:   return "Myth - gold transparent overdrive";
        case PEDAL_COMP:   return "Squash - OTA compressor";
        case PEDAL_GATE:   return "Curfew - noise gate";
        case PEDAL_WAH:    return "Weeper - wah / envelope filter";
        case PEDAL_CHORUS: return "Ensemble - CE-1 chorus / vibrato";
        case PEDAL_DELAY:  return "Echoman - BBD analog delay";
        // PRE-EXISTING GAP, fixed in passing (M13.5): PEDAL_OPTO had no case
        // here, so the Lumen showed in the gear tray and swap menu as the
        // bare word "Pedal" while its CARD drew correctly. Same class of bug
        // as docs §61.10/§62, opposite direction.
        case PEDAL_OPTO:   return "Lumen - optical compressor";
        case PEDAL_VIBE:   return "Swirl - Uni-Vibe photocell phaser";
        case PEDAL_DROP:   return "Cellar - polyphonic drop-tune";
        // Added WITH the face, not after it: §67.10 found the Lumen shipping
        // with a card but no menu case, showing as the bare word "Pedal".
        case PEDAL_EQ:     return "Decade - ten-band graphic EQ";
        default:           return "Pedal";
    }
}

PedalCard::PedalCard(ClipperAudioProcessor& p, int type) : proc_(p), type_(type) {
    const PedalFace& face = pedalFace(type_);

    for (const auto& k : face.knobs) {
        auto knob = std::make_unique<NeuKnob>();
        knob->setName(k.name);
        knob->setAccent(skin::accent(face.accent));
        if (!k.positions.empty()) {
            juce::StringArray labels;
            for (const char* l : k.positions) labels.add(l);
            knob->setPositions(labels);
        }
        addAndMakeVisible(*knob);
        knobAttach_.push_back(
            std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                proc_.apvts, k.paramId, knob->slider()));
        knobs_.push_back(std::move(knob));
    }

    stomp_.setAccent(skin::accent(face.accent));
    stomp_.setShape(face.shape);
    // Only the round stomps carry a caption; the treadle/pad wear their name
    // embossed on the face itself (and nothing sits below them), as in the web.
    const bool round = face.shape == Footswitch::Shape::Round ||
                       face.shape == Footswitch::Shape::BigRound;
    stomp_.setCaption(round ? "Stomp" : "");
    // Only the SD-1's rubber treadle wears its name embossed on the pad (the web's
    // .treadle-wordmark). The TS's metal plate is bare — its script name lives on
    // the mid body, where the header wordmark already puts it.
    if (face.shape == Footswitch::Shape::Treadle) stomp_.setWordmark(face.wordmark);
    addAndMakeVisible(stomp_);

    onAttach_ = std::make_unique<juce::ParameterAttachment>(
        *proc_.apvts.getParameter(face.onParamId),
        [this](float v) {
            engaged_ = v >= 0.5f;
            refreshEngaged();
        },
        nullptr);
    stomp_.onClick = [this] {
        onAttach_->setValueAsCompleteGesture(engaged_ ? 0.0f : 1.0f);
    };

    // ---- the chain-position rack ------------------------------------------
    for (ChipButton* c : {&grip_, &left_, &right_, &swap_, &remove_}) addAndMakeVisible(*c);
    grip_.setTint(skin::inkFaint);
    grip_.onDrag = [this](int x) { if (onDragTo) onDragTo(getX() + x); };
    grip_.onDragEnd = [this] { if (onDragEnd) onDragEnd(); };
    left_.onClick = [this] { if (onMoveLeft) onMoveLeft(); };
    right_.onClick = [this] { if (onMoveRight) onMoveRight(); };
    remove_.setTint(skin::accent(skin::AccentId::Rat));
    remove_.onClick = [this] { if (onRemove) onRemove(); };
    swap_.onClick = [this] {
        juce::PopupMenu m;
        m.addSectionHeader("Swap for");
        const std::vector<int> board = proc_.chainOrder();
        for (int t = 0; t < PEDAL_TYPE_COUNT; ++t) {
            if (t == type_ || !pedalHasFace(t)) continue;
            bool taken = false;  // each type is instantiable once
            for (int have : board) taken = taken || have == t;
            if (taken) continue;
            m.addItem(t + 1, pedalMenuLabel(t));
        }
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&swap_),
                        [this](int result) {
                            if (result > 0 && onSwap) onSwap(result - 1);
                        });
    };

    onAttach_->sendInitialUpdate();
}

PedalCard::~PedalCard() = default;

void PedalCard::setPosition(int index, int total) {
    index_ = index;
    total_ = total;
    left_.setChipEnabled(index > 0);
    right_.setChipEnabled(index < total - 1);
    repaint();
}

void PedalCard::applyTheme() {
    const PedalFace& face = pedalFace(type_);
    const juce::Colour ac = skin::accent(face.accent);
    for (auto& k : knobs_) k->setAccent(ac);
    stomp_.setAccent(ac);
    remove_.setTint(skin::accent(skin::AccentId::Rat));
    // The card's own knobs stay dimmed/lit as they were — re-apply so the dimmed
    // variants pick the new accent up too.
    refreshEngaged();
}

void PedalCard::refreshEngaged() {
    for (auto& k : knobs_) k->setDimmed(!engaged_);
    stomp_.setEngaged(engaged_);
    repaint();
}

juce::Point<float> PedalCard::inJack() const {
    return {(float)getX(), (float)getY() + (float)getHeight() * 0.42f};
}

juce::Point<float> PedalCard::outJack() const {
    return {(float)getRight(), (float)getY() + (float)getHeight() * 0.42f};
}

void PedalCard::resized() {
    auto r = getLocalBounds().reduced(14, 12);

    // Rack row across the top: grip · pos · ◀ ▶ · ⇄ ✕. Five chips plus the position
    // number do not fit a minimum-width card, so the chips shrink first and the
    // position number is the one thing that drops — the chips are the controls.
    {
        auto rack = r.removeFromTop(kRackH);
        const int chip = juce::jlimit(19, kChipW, (rack.getWidth() - 18) / 5);
        posWidth_ = rack.getWidth() >= 5 * chip + 18 ? 18 : 0;
        grip_.setBounds(rack.removeFromLeft(chip).reduced(1));
        rack.removeFromLeft(posWidth_);  // the position number is painted here
        left_.setBounds(rack.removeFromLeft(chip).reduced(1));
        right_.setBounds(rack.removeFromLeft(chip).reduced(1));
        remove_.setBounds(rack.removeFromRight(chip).reduced(1));
        swap_.setBounds(rack.removeFromRight(chip).reduced(1));
    }
    r.removeFromTop(8);

    // Header: the eyebrow line, with the LED parked at its right end (.pedal-top).
    {
        auto head = r.removeFromTop(juce::jmax(kEyebrowH, kLedD));
        headerBounds_ = head;
        // Inset from the card edge by more than the LED's own width: a LIT jewel
        // throws a halo about three times its radius, and parked hard against the
        // chassis edge that halo spills onto the bench.
        ledBounds_ = juce::Rectangle<int>(head.getRight() - kLedD - 6,
                                          head.getCentreY() - kLedD / 2, kLedD, kLedD);
    }
    const PedalFace& face = pedalFace(type_);
    // The 'plate' face carries its name ENGRAVED in the mid-body nameplate, not as a
    // hero line under the eyebrow — so it spends that vertical band on the knobs.
    if (face.layout == PedalFace::Layout::Plate) {
        wordmarkBounds_ = {};
        r.removeFromTop(4);
    } else {
        wordmarkBounds_ = r.removeFromTop((int)face.wordmarkSize + 8);
        r.removeFromTop(6);
    }

    // Knobs, then (on the plate face) the nameplate, then the footswitch.
    plateBounds_ = {};
    if (face.layout == PedalFace::Layout::Triangle && knobs_.size() == 3) {
        // The Muff triangle: two across the top, the third centred below them.
        auto top = r.removeFromTop(kKnobH);
        const int cw = top.getWidth() / 2;
        knobs_[0]->setBounds(top.removeFromLeft(cw).reduced(4, 0));
        knobs_[1]->setBounds(top.reduced(4, 0));
        auto bottom = r.removeFromTop(kKnobH);
        knobs_[2]->setBounds(bottom.withSizeKeepingCentre(juce::jmin(cw, 76), kKnobH));
    } else if (face.layout == PedalFace::Layout::Single && knobs_.size() == 1) {
        // The phaser's one big knob, centred (the [data-face=single] face).
        auto row = r.removeFromTop(juce::jmax(kKnobH, juce::jmin(120, r.getHeight() / 2)));
        knobs_[0]->setBounds(row.withSizeKeepingCentre(
            juce::jmin(row.getWidth(), row.getHeight()), row.getHeight()));
    } else if (face.layout == PedalFace::Layout::Bank && knobs_.size() == 12) {
        // The EQ bank: ten band knobs in 2 rows of 5, then GAIN/VOLUME centred
        // under them. Deliberately NOT the generic row below — twelve controls
        // sharing one row would give each about six pixels.
        const int bandH = juce::jmax(kKnobH - 10, 44);
        for (int row = 0; row < 2; ++row) {
            auto strip = r.removeFromTop(bandH);
            const int cw = strip.getWidth() / 5;
            for (int i = 0; i < 5; ++i)
                knobs_[(size_t)(row * 5 + i)]->setBounds(strip.removeFromLeft(cw).reduced(1, 0));
        }
        r.removeFromTop(4);
        auto out = r.removeFromTop(kKnobH);
        const int ow = juce::jmin(out.getWidth() / 2, 70);
        auto centred = out.withSizeKeepingCentre(ow * 2, out.getHeight());
        knobs_[10]->setBounds(centred.removeFromLeft(ow).reduced(3, 0));
        knobs_[11]->setBounds(centred.reduced(3, 0));
    } else {
        auto row = r.removeFromTop(kKnobH);
        const int cw = row.getWidth() / juce::jmax(1, (int)knobs_.size());
        for (auto& k : knobs_) k->setBounds(row.removeFromLeft(cw).reduced(2, 0));
    }

    // The milled NAMEPLATE band sits between the knob row and the stomp (the web's
    // .name-plate, margin 2px 0 18px). It takes a fixed slice so the engraving keeps
    // its proportions however tall the card gets.
    if (face.layout == PedalFace::Layout::Plate) {
        r.removeFromTop(2);
        plateBounds_ = r.removeFromTop(juce::jlimit(30, 46, r.getHeight() / 3));
        r.removeFromTop(14);
    }

    r.removeFromTop(12);
    // A few pixels of air at the bottom: the treadle/pad hug the foot of their zone
    // and their cast shadow needs somewhere to fall (child components clip).
    stomp_.setBounds(r.withTrimmedBottom(6));
}

void PedalCard::paint(juce::Graphics& g) {
    const PedalFace& face = pedalFace(type_);
    const juce::Colour accent = skin::accent(face.accent);
    auto card = getLocalBounds().toFloat();
    // BODY ONLY. A child component's paint is clipped to its own bounds, so the
    // cast shadow this card throws on the rail is painted by the board content
    // (the parent) before the cards draw — see paintBoardContent. Painting it here
    // is what made the pedals read flat next to the input/amp cards, which the
    // editor paints itself (visual pass 3).
    skin::drawChassisBody(g, card, 24.0f);

    // Chain position, in the gap the rack row left for it.
    if (posWidth_ > 0) {
        g.setColour(skin::inkFaint);
        g.setFont(skin::monoFont(10.0f));
        g.drawText(juce::String(index_ + 1), grip_.getRight(), grip_.getY(), posWidth_,
                   grip_.getHeight(), juce::Justification::centred);
    }

    // Eyebrow + hero wordmark.
    g.setColour(skin::inkFaint);
    g.setFont(skin::monoFont(9.5f));
    g.drawText(juce::String::fromUTF8(face.eyebrow).toUpperCase(),
               headerBounds_.withTrimmedRight(kLedD + 20), juce::Justification::centredLeft);
    if (!wordmarkBounds_.isEmpty()) {
        g.setColour(accent.withAlpha(engaged_ ? 1.0f : 0.62f));
        g.setFont(skin::wordmarkFont(face.wordmarkSize));
        g.drawText(face.wordmark, wordmarkBounds_, juce::Justification::centredLeft);
    }

    // The GOLD box's engraved nameplate — this face's whole morphology cue.
    if (!plateBounds_.isEmpty())
        skin::drawNamePlate(g, plateBounds_.toFloat(), accent, face.wordmark, engaged_);

    // The side jacks, drawn ON the chassis edge so the cable ends tuck into them.
    skin::drawJack(g, {0.0f, card.getHeight() * 0.42f}, (float)kJackD);
    skin::drawJack(g, {card.getWidth(), card.getHeight() * 0.42f}, (float)kJackD);

    // The LED goes on LAST, so nothing (least of all a neighbour's drop shadow) can
    // paint over its halo. Lit iff the pedal is engaged — the web's `.pedal.on .led`.
    skin::drawJewel(g, ledBounds_.toFloat(), accent, engaged_);
}

}  // namespace clipper::native
