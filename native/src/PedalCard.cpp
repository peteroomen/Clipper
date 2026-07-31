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
const PedalFace kFaces[PEDAL_TYPE_COUNT] = {
    // RAT — the reference: red, the round stomp, the vertical stack.
    {"Dirt N\xc2\xba""1 \xc2\xb7 Rodent-Type", "Rodent", skin::AccentId::Rat,
     Footswitch::Shape::Round, PedalFace::Layout::Stack, 38.0f,
     {{"Dist", pid::ratDist}, {"Filter", pid::ratFilter}, {"Level", pid::ratLevel}},
     pid::ratOn},
    // SD-1 — yellow, and the Boss-compact RUBBER TREADLE (the morphology cue).
    {"Drive N\xc2\xba""2 \xc2\xb7 Yellow", "Super Drive", skin::AccentId::Sd,
     Footswitch::Shape::Treadle, PedalFace::Layout::Stack, 26.0f,
     {{"Drive", pid::sdDrive}, {"Tone", pid::sdTone}, {"Level", pid::sdLevel}},
     pid::sdOn},
    // TS — the green box, and the Ibanez-format hinged metal PAD.
    {"Drive N\xc2\xba""3 \xc2\xb7 Green", "Screamer", skin::AccentId::Ts,
     Footswitch::Shape::Pad, PedalFace::Layout::Stack, 30.0f,
     {{"Drive", pid::tsDrive}, {"Tone", pid::tsTone}, {"Level", pid::tsLevel}},
     pid::tsOn},
    // Muff — violet, the big stomp, and the classic three-knob TRIANGLE. Knob order
    // is the triangle placement: Sustain top-left, Volume top-right, Tone below.
    {"Fuzz N\xc2\xba""5 \xc2\xb7 Pi", "Pi", skin::AccentId::Muff,
     Footswitch::Shape::BigRound, PedalFace::Layout::Triangle, 44.0f,
     {{"Sustain", pid::muffSustain}, {"Volume", pid::muffVolume}, {"Tone", pid::muffTone}},
     pid::muffOn},
    // Phaser — burnt orange, and the iconic ONE big knob.
    {"Phaser N\xc2\xba""4 \xc2\xb7 Script", "Ninety", skin::AccentId::Phaser,
     Footswitch::Shape::Round, PedalFace::Layout::Single, 34.0f,
     {{"Speed", pid::phaserSpeed}},
     pid::phaserOn},
    // GOLD — the hoarded gold box. Its morphology cue is the milled NAMEPLATE band:
    // the original is remembered for a colour and for being ENGRAVED rather than
    // printed, so native takes the colour and the engraving IDEA. Type only — the
    // figure on the real enclosure IS the trademark and is deliberately absent, as
    // are the words Klon/Centaur/KTR. "Myth" is the wink at what the box became.
    {"Drive N\xc2\xba""6 \xc2\xb7 Gold", "Myth", skin::AccentId::Gold,
     Footswitch::Shape::Round, PedalFace::Layout::Plate, 30.0f,
     {{"Gain", pid::goldGain}, {"Treble", pid::goldTreble}, {"Output", pid::goldLevel}},
     pid::goldOn},
    // SQUASH — M13.1, the first DYNAMICS pedal and the first non-dirt box. Its
    // morphology cue is simply that it has TWO knobs where every dirt box has
    // three: a small MXR-format enclosure over a round stomp. Teal accent (the
    // real pedal is red, and Rat owns red here). No MXR/Dyna Comp/Ross text.
    {"Dynamics N\xc2\xba""7 \xc2\xb7 Squash", "Squash", skin::AccentId::Comp,
     Footswitch::Shape::Round, PedalFace::Layout::Stack, 34.0f,
     {{"Sustain", pid::compSustain}, {"Level", pid::compLevel}},
     pid::compOn},
    // WAH "Weeper" (docs §58) — the board's first FILTER pedal, and the first one
    // whose real enclosure is a ROCKING TREADLE rather than a box. The treadle
    // footswitch shape is that morphology cue (shared with the Boss-compact SD-1
    // here; the web front-end draws its own ribbed 'rocker' plate). TEAL accent,
    // the furthest hue from the six dirt/mod pedals. No Dunlop/Cry Baby/Vox/
    // Mu-Tron wording anywhere. POSITION / SENSE / VOICE: SENSE at 0 is a plain
    // manual wah, above 0 an envelope follower sweeps the SAME tank.
    {"Filter N\xc2\xba""7 \xc2\xb7 Treadle", "Weeper", skin::AccentId::Wah,
     Footswitch::Shape::Treadle, PedalFace::Layout::Stack, 26.0f,
     {{"Position", pid::wahPosition}, {"Sense", pid::wahSense}, {"Voice", pid::wahVoice}},
     pid::wahOn},
};
}  // namespace

const PedalFace& pedalFace(int type) {
    return kFaces[juce::jlimit(0, PEDAL_TYPE_COUNT - 1, type)];
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
        case PEDAL_WAH:    return "Weeper - wah / envelope filter";
        default:           return "Pedal";
    }
}

PedalCard::PedalCard(ClipperAudioProcessor& p, int type) : proc_(p), type_(type) {
    const PedalFace& face = pedalFace(type_);

    for (const auto& k : face.knobs) {
        auto knob = std::make_unique<NeuKnob>();
        knob->setName(k.name);
        knob->setAccent(skin::accent(face.accent));
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
            if (t == type_) continue;
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
