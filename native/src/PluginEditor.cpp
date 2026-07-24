#include "PluginEditor.h"

#ifndef CLIPPER_GIT_HASH
#define CLIPPER_GIT_HASH "dev"
#endif

namespace clipper::native {

namespace {
constexpr int kMargin = 22;
constexpr int kGap = 16;
// The gap BETWEEN chain units is wider than the general gap: a patch cable needs a
// span to sag across, or it reads as a smudge between two touching boxes.
constexpr int kChainGap = 40;
constexpr int kTopBar = 66;
constexpr int kInputW = 106;
constexpr int kTrayW = 124;
constexpr int kAmpMinW = 356;
// A pedal card's preferred width scales with its knob count (the phaser's one-knob
// face is genuinely narrower — its morphology). Cards breathe between preferred and
// minimum as the board fills up; below the minimum a card stops being a pedal and
// starts being a sliver, so the WINDOW grows instead (see requiredBoardWidth).
constexpr int kPedalMaxW = 212;
constexpr int kPedalMinW = 150;
constexpr int kPhaserMinW = 112;  // the one-knob face genuinely needs less
constexpr int kKnobCellW = 66;
constexpr int kKnobCellH = 92;
constexpr int kKnobCellMinH = 46;
constexpr int kMinHeight = 560;

int preferredPedalWidth(int type) {
    const int knobs = (int)pedalFace(type).knobs.size();
    return juce::jlimit(kPedalMinW, kPedalMaxW, 46 + 56 * knobs);
}

int minimumPedalWidth(int type) {
    return pedalFace(type).knobs.size() <= 1 ? kPhaserMinW : kPedalMinW;
}
}  // namespace

ClipperAudioProcessorEditor::ClipperAudioProcessorEditor(ClipperAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), proc_(p) {
    setLookAndFeel(&lnf_);

    // ---- knob helper: name, accent, visible + APVTS slider attachment ----------
    auto knob = [this](NeuKnob& k, std::unique_ptr<SliderAttach>& at, const char* id,
                       const juce::String& nm, juce::Colour ac) {
        k.setName(nm);
        k.setAccent(ac);
        addAndMakeVisible(k);
        at = std::make_unique<SliderAttach>(proc_.apvts, id, k.slider());
    };

    // ---- INPUT ----------------------------------------------------------------
    knob(inputTrim_, inputTrimAttach_, pid::inputTrim, "Trim", skin::accentTwin);

    // ---- THE BOARD ------------------------------------------------------------
    // Cards are built from the processor's chain (see rebuildBoard); the gear tray
    // adds whatever is not on the board yet.
    trayAdd_.setTint(skin::benchInkDim);
    addAndMakeVisible(trayAdd_);
    trayAdd_.onClick = [this] { showTrayMenu(); };

    // ---- AMP knobs (superset; per-voice visibility set in updateAmpFace) -------
    knob(volume_, volumeAttach_, pid::volume, "Vol", skin::accentClean);
    knob(bass_, bassAttach_, pid::bass, "Bass", skin::accentClean);
    knob(middle_, middleAttach_, pid::middle, "Mid", skin::accentClean);
    knob(treble_, trebleAttach_, pid::treble, "Treble", skin::accentClean);
    knob(presence_, presenceAttach_, pid::jcmPresence, "Presence", skin::accentJcm);
    knob(master_, masterAttach_, pid::jcmMaster, "Master", skin::accentJcm);
    knob(gain_, gainAttach_, pid::jcmGain, "Gain", skin::accentJcm);
    knob(reverb_, reverbAttach_, pid::reverb, "Reverb", skin::accentClean);
    knob(modSpeed_, modSpeedAttach_, pid::chorusSpeed, "Speed", skin::accentClean);
    knob(modDepth_, modDepthAttach_, pid::chorusDepth, "Depth", skin::accentClean);

    // Levers + power + chorus mode.
    bright_.setCaption("Bright");
    cab_.setCaption("Cab");
    addAndMakeVisible(bright_);
    addAndMakeVisible(cab_);
    addAndMakeVisible(power_);
    addAndMakeVisible(chorusMode_);
    brightAttach_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::bright),
        [this](float v) { bright_.setOn(v >= 0.5f); }, nullptr);
    bright_.onClick = [this] {
        brightAttach_->setValueAsCompleteGesture(bright_.isOn() ? 0.0f : 1.0f);
    };
    cabAttach_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::cab), [this](float v) { cab_.setOn(v >= 0.5f); },
        nullptr);
    cab_.onClick = [this] {
        cabAttach_->setValueAsCompleteGesture(cab_.isOn() ? 0.0f : 1.0f);
    };
    ampOnAttach_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::ampOn),
        [this](float v) { power_.setOn(v >= 0.5f); updateEnablement(); }, nullptr);
    power_.onClick = [this] {
        ampOnAttach_->setValueAsCompleteGesture(power_.isOn() ? 0.0f : 1.0f);
    };
    chorusModeAttach_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::chorusMode),
        [this](float v) { chorusMode_.setSelected((int)(v + 0.5f)); }, nullptr);
    chorusMode_.onSelect = [this](int idx) {
        chorusModeAttach_->setValueAsCompleteGesture((float)idx);
    };

    // ---- top-bar selectors ----------------------------------------------------
    ampVoiceBox_.addItemList({"Clean 120", "Eight Hundred", "Twin Sixty-Five", "Thirty"}, 1);
    addAndMakeVisible(ampVoiceBox_);
    ampVoiceAttach_ = std::make_unique<ComboAttach>(proc_.apvts, pid::ampModel, ampVoiceBox_);
    oversampleBox_.addItemList({juce::String::fromUTF8("1×"), juce::String::fromUTF8("2×"),
                               juce::String::fromUTF8("4×"), juce::String::fromUTF8("8×")},
                              1);
    addAndMakeVisible(oversampleBox_);
    oversampleAttach_ =
        std::make_unique<ComboAttach>(proc_.apvts, pid::oversampling, oversampleBox_);

    // Re-layout the amp face whenever the voice parameter changes (user or host).
    ampModelListen_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::ampModel), [this](float) { updateAmpFace(); },
        nullptr);

    // ---- build stamp ----------------------------------------------------------
    buildStamp_.setText(juce::String("build ") + CLIPPER_GIT_HASH, juce::dontSendNotification);
    buildStamp_.setJustificationType(juce::Justification::centredRight);
    buildStamp_.setColour(juce::Label::textColourId, skin::benchFaint);
    buildStamp_.setFont(skin::monoFont(11.0f));
    addAndMakeVisible(buildStamp_);

    setResizable(true, true);
    // Wide enough for a FULL board (five pedals + input + tray + amp) at the top of
    // the range; the minimum still fits a couple of pedals with the cards squeezed.
    setResizeLimits(1040, 560, 2200, 1000);
    setSize(1360, 640);

    rebuildBoard();
    refreshFromState();  // pull face + toggle + dim states from the APVTS
    startTimer(250);     // watch for board changes made outside this editor
}

ClipperAudioProcessorEditor::~ClipperAudioProcessorEditor() {
    stopTimer();
    setLookAndFeel(nullptr);
}

void ClipperAudioProcessorEditor::refreshFromState() {
    if (boardVersion_ != proc_.chainVersion()) rebuildBoard();
    brightAttach_->sendInitialUpdate();
    cabAttach_->sendInitialUpdate();
    ampOnAttach_->sendInitialUpdate();
    chorusModeAttach_->sendInitialUpdate();
    updateAmpFace();  // reads ampModel from the param, lays out + repaints
}

void ClipperAudioProcessorEditor::timerCallback() {
    // The board can change under us: a host session load, or a preset switch. The
    // version counter makes that cheap to notice.
    if (boardVersion_ != proc_.chainVersion()) {
        rebuildBoard();
        resized();
        repaint();
    }
}

// ---------------------------------------------------------------------------
// The board: cards, the gear tray, and the chain edits they request
// ---------------------------------------------------------------------------
void ClipperAudioProcessorEditor::rebuildBoard() {
    cards_.clear();
    const std::vector<int> chain = proc_.chainOrder();
    for (int i = 0; i < (int)chain.size(); ++i) {
        auto* card = new PedalCard(proc_, chain[(size_t)i]);
        cards_.add(card);
        addAndMakeVisible(card);
        card->setPosition(i, (int)chain.size());
        // The callbacks look their own index UP at call time rather than capturing
        // it: a drag permutes the card array under them, and a stale captured index
        // would move the wrong pedal.
        card->onMoveLeft = [this, card] { movePedal(cards_.indexOf(card), cards_.indexOf(card) - 1); };
        card->onMoveRight = [this, card] { movePedal(cards_.indexOf(card), cards_.indexOf(card) + 1); };
        card->onRemove = [this, card] { removePedalAt(cards_.indexOf(card)); };
        card->onSwap = [this, card](int t) { swapPedalAt(cards_.indexOf(card), t); };
        card->onDragTo = [this, card](int x) { dragCardTo(cards_.indexOf(card), x); };
        card->onDragEnd = [this] {
            draggingCard_ = -1;
            repaint();
        };
    }
    // Every type already on the board is unavailable to add.
    trayAdd_.setChipEnabled((int)chain.size() < kMaxChain);
    boardVersion_ = proc_.chainVersion();
    applyBoardSizeFloor();
}

// A board needs bench to stand on. The window's MINIMUM width tracks the current
// chain: input + every card at its minimum + the tray + the amp at its minimum +
// the cable gaps. If the window is already narrower than that, it grows — better an
// editor that asks for room than one that grinds five pedals into unreadable
// slivers and pushes the amp off its own card.
void ClipperAudioProcessorEditor::applyBoardSizeFloor() {
    if (sizingFloor_) return;  // setSize re-enters resized(); don't spiral
    int need = 2 * kMargin + kInputW + kTrayW + kAmpMinW + kChainGap * (cards_.size() + 2);
    for (auto* c : cards_) need += minimumPedalWidth(c->type());
    need = juce::jlimit(1040, 2200, need);
    setResizeLimits(need, kMinHeight, 2200, 1200);
    if (getWidth() < need) {
        const juce::ScopedValueSetter<bool> guard(sizingFloor_, true);
        setSize(need, juce::jmax(kMinHeight, getHeight()));
    }
}

// Rebuild AFTER the current event finishes. An add/remove/swap is requested from a
// chip's own click callback, and rebuilding inline would delete that very chip
// while its mouse handler is still on the stack.
void ClipperAudioProcessorEditor::scheduleBoardRefresh() {
    juce::Component::SafePointer<ClipperAudioProcessorEditor> self(this);
    juce::MessageManager::callAsync([self] {
        if (self == nullptr) return;
        self->rebuildBoard();
        self->resized();
        self->repaint();
    });
}

void ClipperAudioProcessorEditor::showTrayMenu() {
    const std::vector<int> chain = proc_.chainOrder();
    juce::PopupMenu m;
    m.addSectionHeader("Available pedals");
    for (int t = 0; t < PEDAL_TYPE_COUNT; ++t) {
        bool taken = false;  // each pedal type is instantiable once
        for (int have : chain) taken = taken || have == t;
        if (!taken) m.addItem(t + 1, pedalMenuLabel(t));
    }
    // The tuner is display-only and has no native implementation yet — say so
    // rather than offer a pedal that would do nothing.
    m.addSectionHeader("Not yet native");
    m.addItem(100, "Chromatic tuner (web only)", false, false);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&trayAdd_),
                    [this](int result) {
                        if (result > 0 && result <= PEDAL_TYPE_COUNT) addPedal(result - 1);
                    });
}

void ClipperAudioProcessorEditor::addPedal(int type) {
    std::vector<int> chain = proc_.chainOrder();
    for (int have : chain)
        if (have == type) return;  // already on the board
    chain.push_back(type);
    proc_.setChainOrder(chain);
    // A pedal dropped on the board arrives ENGAGED, like the web's makePedal().
    if (auto* p = proc_.apvts.getParameter(pedalFace(type).onParamId)) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(1.0f);
        p->endChangeGesture();
    }
    scheduleBoardRefresh();
}

void ClipperAudioProcessorEditor::removePedalAt(int index) {
    std::vector<int> chain = proc_.chainOrder();
    if (index < 0 || index >= (int)chain.size()) return;
    chain.erase(chain.begin() + index);
    proc_.setChainOrder(chain);
    scheduleBoardRefresh();
}

void ClipperAudioProcessorEditor::swapPedalAt(int index, int newType) {
    std::vector<int> chain = proc_.chainOrder();
    if (index < 0 || index >= (int)chain.size()) return;
    for (int have : chain)
        if (have == newType) return;  // that type is already on the board
    chain[(size_t)index] = newType;
    proc_.setChainOrder(chain);
    if (auto* p = proc_.apvts.getParameter(pedalFace(newType).onParamId)) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(1.0f);
        p->endChangeGesture();
    }
    scheduleBoardRefresh();
}

void ClipperAudioProcessorEditor::movePedal(int from, int to) {
    std::vector<int> chain = proc_.chainOrder();
    const int n = (int)chain.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    const int moved = chain[(size_t)from];
    chain.erase(chain.begin() + from);
    chain.insert(chain.begin() + to, moved);
    proc_.setChainOrder(chain);

    // A MOVE keeps every card alive — it only permutes them. That matters: a live
    // drag calls this from inside a card's own mouse handler, so destroying and
    // recreating the cards here would pull the ground out from under it.
    cards_.move(from, to);
    for (int i = 0; i < cards_.size(); ++i) cards_[i]->setPosition(i, cards_.size());
    boardVersion_ = proc_.chainVersion();
    resized();
    repaint();
}

void ClipperAudioProcessorEditor::dragCardTo(int cardIndex, int parentX) {
    // Live reorder under the pointer — the same rule the web board uses: the drag
    // lands BEFORE the first card whose horizontal centre is right of the pointer.
    if (cardIndex < 0 || cardIndex >= cards_.size()) return;
    draggingCard_ = cardIndex;
    int target = cards_.size() - 1;
    for (int i = 0; i < cards_.size(); ++i) {
        if (parentX < cards_[i]->getBounds().getCentreX()) {
            target = i;
            break;
        }
    }
    if (target != cardIndex) movePedal(cardIndex, target);
}

void ClipperAudioProcessorEditor::updateAmpFace() {
    auto* mp = proc_.apvts.getParameter(pid::ampModel);
    ampModel_ = juce::jlimit(0, 3, (int)(mp->convertFrom0to1(mp->getValue()) + 0.5f));

    // Hide the whole superset, then re-show per voice.
    for (NeuKnob* k : {&volume_, &bass_, &middle_, &treble_, &presence_, &master_, &gain_,
                       &reverb_, &modSpeed_, &modDepth_})
        k->setVisible(false);
    ampPrimaryKnobs_.clear();
    ampModKnobs_.clear();
    modCaption_ = {};
    showMode_ = false;

    auto show = [](NeuKnob& k, const juce::String& nm, juce::Colour ac) {
        k.setName(nm);
        k.setAccent(ac);
        k.setVisible(true);
    };

    switch (ampModel_) {
        case 1:  // JCM800 "Eight Hundred" — gold. Presence Bass Mid Treble Master Gain Reverb
            ampWordmark_ = "Eight Hundred";
            ampEyebrow_ = juce::String::fromUTF8("Head Nº2 · Brit-Type");
            ampAccent_ = skin::accentJcm;
            showBright_ = false;
            show(presence_, "Presence", ampAccent_);
            show(bass_, "Bass", ampAccent_);
            show(middle_, "Mid", ampAccent_);
            show(treble_, "Treble", ampAccent_);
            show(master_, "Master", ampAccent_);
            show(gain_, "Gain", ampAccent_);
            show(reverb_, "Reverb", ampAccent_);
            ampPrimaryKnobs_ = {&presence_, &bass_, &middle_, &treble_, &master_, &gain_,
                                &reverb_};
            break;
        case 2:  // Twin "Twin Sixty-Five" — silver-blue. Vol Bass Mid Treble Reverb + tremolo
            ampWordmark_ = "Twin Sixty-Five";
            ampEyebrow_ = juce::String::fromUTF8("Combo Nº3 · Black-Panel");
            ampAccent_ = skin::accentTwin;
            showBright_ = true;
            show(volume_, "Vol", ampAccent_);
            show(bass_, "Bass", ampAccent_);
            show(middle_, "Mid", ampAccent_);
            show(treble_, "Treble", ampAccent_);
            show(reverb_, "Reverb", ampAccent_);
            show(modSpeed_, "Speed", ampAccent_);
            show(modDepth_, "Intensity", ampAccent_);
            ampPrimaryKnobs_ = {&volume_, &bass_, &middle_, &treble_, &reverb_};
            ampModKnobs_ = {&modSpeed_, &modDepth_};
            modCaption_ = "Tremolo";
            break;
        case 3:  // AC30 "Thirty" — copper. Vol Bass Treble Cut Reverb
            ampWordmark_ = "Thirty";
            ampEyebrow_ = juce::String::fromUTF8("Combo Nº4 · Top-Boost");
            ampAccent_ = skin::accentAc30;
            showBright_ = false;
            show(volume_, "Vol", ampAccent_);
            show(bass_, "Bass", ampAccent_);
            show(treble_, "Treble", ampAccent_);
            show(presence_, "Cut", ampAccent_);  // presence param reused as TOP CUT
            show(reverb_, "Reverb", ampAccent_);
            ampPrimaryKnobs_ = {&volume_, &bass_, &treble_, &presence_, &reverb_};
            break;
        default:  // Clean 120 — red. Vol Bass Mid Treble Reverb + chorus
            ampModel_ = 0;
            ampWordmark_ = "Clean 120";
            ampEyebrow_ = juce::String::fromUTF8("Solid State · Stereo");
            ampAccent_ = skin::accentClean;
            showBright_ = true;
            showMode_ = true;
            show(volume_, "Vol", ampAccent_);
            show(bass_, "Bass", ampAccent_);
            show(middle_, "Mid", ampAccent_);
            show(treble_, "Treble", ampAccent_);
            show(reverb_, "Reverb", ampAccent_);
            show(modSpeed_, "Speed", ampAccent_);
            show(modDepth_, "Depth", ampAccent_);
            ampPrimaryKnobs_ = {&volume_, &bass_, &middle_, &treble_, &reverb_};
            ampModKnobs_ = {&modSpeed_, &modDepth_};
            modCaption_ = "Chorus";
            break;
    }

    bright_.setAccent(ampAccent_);
    cab_.setAccent(ampAccent_);
    power_.setAccent(ampAccent_);
    bright_.setVisible(showBright_);
    chorusMode_.setVisible(showMode_);

    updateEnablement();
    resized();
    repaint();
}

void ClipperAudioProcessorEditor::updateEnablement() {
    // Each pedal card dims its own knobs off its engaged parameter; only the amp
    // block is the editor's business.
    const bool ampOn = proc_.apvts.getParameter(pid::ampOn)->getValue() >= 0.5f;
    for (NeuKnob* k : {&volume_, &bass_, &middle_, &treble_, &presence_, &master_, &gain_,
                       &reverb_, &modSpeed_, &modDepth_})
        k->setDimmed(!ampOn);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void ClipperAudioProcessorEditor::resized() {
    // The floor is enforced HERE as well as on a board change, so that a host (or a
    // headless tool) that sets a size directly — bypassing the resize constrainer —
    // still cannot squeeze the board below what it can legibly draw.
    applyBoardSizeFloor();

    auto r = getLocalBounds().reduced(kMargin);

    // Top bar: title left; amp-voice + oversample pills right.
    auto top = r.removeFromTop(kTopBar);
    {
        auto rightPills = top.removeFromRight(360);
        auto voice = rightPills.removeFromLeft(184);
        auto os = rightPills;
        ampVoiceBox_.setBounds(voice.withTrimmedTop(22).withHeight(34).reduced(4, 0));
        oversampleBox_.setBounds(os.withTrimmedTop(22).withHeight(34).reduced(4, 0));
    }

    r.removeFromTop(6);
    buildStamp_.setBounds(getWidth() - 240 - kMargin, getHeight() - 20, 240, 16);
    auto row = r;
    row.removeFromBottom(18);  // reserve for the build-stamp line

    // ---- the LEFT-TO-RIGHT signal chain --------------------------------------
    // INPUT · pedal cards (in chain order) · gear tray · AMP. The pedal cards take
    // their preferred widths, shrinking together only if the board has filled up
    // beyond what the window can show at full size.
    const int nCards = cards_.size();
    int wanted = 0;
    for (auto* c : cards_) wanted += preferredPedalWidth(c->type());
    const int fixed = kInputW + kTrayW + kAmpMinW + kChainGap * (nCards + 2);
    const int available = juce::jmax(0, row.getWidth() - fixed);
    // A single scale factor keeps the relative widths (the phaser stays the narrow
    // one-knob box) while the whole board breathes in and out with the window. It
    // can only shrink cards to their MINIMUM: a crowded board grows the window
    // instead of grinding its pedals into slivers.
    const double squeeze =
        (wanted > 0 && wanted > available) ? (double)available / (double)wanted : 1.0;

    cardInput_ = row.removeFromLeft(kInputW);
    row.removeFromLeft(kChainGap);
    for (auto* card : cards_) {
        const int w = juce::jmax(minimumPedalWidth(card->type()),
                                 (int)std::lround(preferredPedalWidth(card->type()) * squeeze));
        card->setBounds(row.removeFromLeft(w));
        row.removeFromLeft(kChainGap);
    }
    trayBounds_ = row.removeFromLeft(kTrayW);
    row.removeFromLeft(kChainGap);
    cardAmp_ = row;

    // The tray sits in the chain, but ABOVE the jack line, so the cable running
    // from the last pedal to the amp passes cleanly beneath it.
    trayAdd_.setBounds(trayBounds_.getX(),
                       trayBounds_.getY() + (int)(trayBounds_.getHeight() * 0.42) - 74,
                       trayBounds_.getWidth(), 42);

    // INPUT card: one knob, dead-centred in the card (header floats at the top).
    {
        int cw = juce::jmin(kKnobCellW + 6, cardInput_.getWidth() - 12);
        inputTrim_.setBounds(cardInput_.withSizeKeepingCentre(cw, kKnobCellH));
    }

    layoutAmpCard(cardAmp_);
}

void ClipperAudioProcessorEditor::layoutAmpCard(juce::Rectangle<int> card) {
    auto in = card.reduced(20);
    in.removeFromTop(56);  // eyebrow + wordmark header stays at top

    // The Bright/Cab/Power cluster gives ground when the card is narrow, so the tone
    // knobs never get starved into a single column.
    const int clusterWant = showBright_ ? 168 : 116;
    auto cluster = in.removeFromRight(juce::jlimit(96, clusterWant, in.getWidth() / 3));
    in.removeFromRight(kGap);
    auto knobArea = in;
    const int nPrimary = (int)ampPrimaryKnobs_.size();
    // Columns come from a MINIMUM cell width, not the preferred one: dividing by the
    // preferred 66 px sat on a knife edge (a 131 px area gave ONE column, five rows,
    // and the block then ran off the bottom of the card). Cells then shrink to fit
    // the columns actually chosen.
    const int perRow = juce::jlimit(1, juce::jmax(1, nPrimary), knobArea.getWidth() / 52);
    const int cellW = juce::jmin(kKnobCellW, knobArea.getWidth() / juce::jmax(1, perRow));
    const int rows = (nPrimary + perRow - 1) / juce::jmax(1, perRow);

    // Total control-block height (primary rows + optional modulation row), then
    // vertically centre the block in the body so the card isn't top-heavy.
    //
    // The modulation row needs a REAL gap, not a hairline: its knobs draw a value
    // arc that floats outside the knob body, right up to the top of their cell, and
    // the divider line used to be drawn at that same y — so the arcs (and the mode
    // switch, which was deliberately nudged 4 px above the row) crossed it. The gap
    // is now wide enough to hold the divider in its MIDDLE, clear of both rows.
    const int modGap = 40;
    const bool hasMod = !ampModKnobs_.empty();
    // The cell HEIGHT adapts: a narrow amp card wraps its tone knobs onto more rows,
    // and at a small window those rows must still fit INSIDE the card. Previously
    // they simply ran off the bottom (and the last row collided with the mod row).
    const int totalRows = rows + (hasMod ? 1 : 0);
    const int spacing = (rows - 1) * 6 + (hasMod ? modGap : 0);
    const int cellH = juce::jlimit(kKnobCellMinH, kKnobCellH,
                                   (knobArea.getHeight() - spacing) /
                                       juce::jmax(1, totalRows));
    int blockH = rows * cellH + (rows - 1) * 6;
    if (hasMod) blockH += modGap + cellH;
    int startY = knobArea.getY() + juce::jmax(0, (knobArea.getHeight() - blockH) / 2);

    int x = knobArea.getX();
    int y = startY;
    int col = 0;
    for (NeuKnob* k : ampPrimaryKnobs_) {
        if (col == perRow) {
            col = 0;
            x = knobArea.getX();
            y += cellH + 6;
        }
        k->setBounds(x, y, cellW, cellH);
        x += cellW;
        ++col;
    }
    int primaryBottom = startY + rows * cellH + (rows - 1) * 6;

    // Right cluster (Bright?/Cab/Power) aligned with the FIRST primary knob row.
    {
        auto c = cluster.withY(startY).withHeight(juce::jmax(cellH, 84));
        int slot = c.getWidth() / (showBright_ ? 3 : 2);
        if (showBright_) bright_.setBounds(c.removeFromLeft(slot).reduced(4, 0));
        cab_.setBounds(c.removeFromLeft(slot).reduced(4, 0));
        power_.setBounds(c.reduced(4, 0));
    }

    // Modulation sub-row (Chorus/Tremolo): a divider centred in its own gap, then
    // the caption + speed/depth (+ the mode switch), all strictly BELOW the line.
    ampModRowCaption_ = {};
    ampModDividerY_ = 0;
    if (hasMod) {
        const int rowY = primaryBottom + modGap;
        ampModDividerY_ = primaryBottom + modGap / 2;  // the line's own band
        int cx = knobArea.getX();
        // The caption sits beside the knobs, vertically centred on the row — it no
        // longer straddles the divider.
        ampModRowCaption_ = juce::Rectangle<int>(cx, rowY, 72, cellH);
        cx += 76;
        for (NeuKnob* k : ampModKnobs_) {
            k->setBounds(cx, rowY, cellW, cellH);
            cx += cellW + 4;
        }
        // The mode switch starts ON the row line (never above it) and is never
        // taller than the row, so it cannot reach back over the divider.
        if (showMode_) chorusMode_.setBounds(cx + 10, rowY, 92, cellH);
    }
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void ClipperAudioProcessorEditor::paint(juce::Graphics& g) {
    // The light porcelain bench (a soft vertical lift).
    juce::ColourGradient bench(skin::ground.brighter(0.035f), getWidth() * 0.5f, 0.0f,
                               skin::groundDeep, getWidth() * 0.5f, (float)getHeight(), false);
    g.setGradientFill(bench);
    g.fillRect(getLocalBounds());

    // Title wordmark.
    g.setColour(skin::benchInkDim);
    g.setFont(skin::wordmarkFont(30.0f));
    g.drawText("CLIPPER", kMargin, kMargin - 2, 320, 40, juce::Justification::centredLeft);
    g.setColour(skin::benchFaint);
    g.setFont(skin::monoFont(10.5f));
    g.drawText(juce::String::fromUTF8("GUITAR RIG · NATIVE"), kMargin, kMargin + 34, 320,
               14, juce::Justification::centredLeft);

    // Pill captions.
    auto pillCaption = [&](juce::Component& box, const juce::String& text) {
        g.setColour(skin::benchInkDim);
        g.setFont(skin::monoFont(10.0f));
        g.drawText(text, box.getX(), box.getY() - 16, box.getWidth(), 14,
                   juce::Justification::centredLeft);
    };
    pillCaption(ampVoiceBox_, "AMP VOICE");
    pillCaption(oversampleBox_, "OVERSAMPLE");

    // ---- PATCH CABLES ---------------------------------------------------------
    // Drawn BEFORE the enclosures (and before every child component, which JUCE
    // paints after the parent), so each cable end disappears under the chassis and
    // into its socket — the web's z-order, where the cable layer sits behind the
    // units. The run is: input.out -> card[0].in, card[i].out -> card[i+1].in, and
    // the last out -> amp.in. An EMPTY board is valid: one cable spans input to amp.
    {
        const float jackY = (float)cardInput_.getY() + cardInput_.getHeight() * 0.42f;
        juce::Point<float> prevOut{(float)cardInput_.getRight(), jackY};
        for (auto* card : cards_) {
            skin::drawCable(g, prevOut, card->inJack());
            prevOut = card->outJack();
        }
        skin::drawCable(g, prevOut,
                        {(float)cardAmp_.getX(),
                         (float)cardAmp_.getY() + cardAmp_.getHeight() * 0.42f});
    }

    auto drawCard = [&](juce::Rectangle<int> card, const juce::String& eyebrow,
                        const juce::String& wordmark, juce::Colour accent, float wordSize) {
        if (card.isEmpty()) return;
        skin::drawChassisCard(g, card.toFloat(), 24.0f);
        auto head = card.reduced(18, 14);
        g.setColour(skin::inkFaint);
        g.setFont(skin::monoFont(9.5f));
        g.drawText(eyebrow.toUpperCase(), head.getX(), head.getY(), head.getWidth(), 14,
                   juce::Justification::centredLeft);
        g.setColour(accent);
        g.setFont(skin::wordmarkFont(wordSize));
        g.drawText(wordmark, head.getX(), head.getY() + 14, head.getWidth(),
                   (int)wordSize + 6, juce::Justification::centredLeft);
    };

    // INPUT card (neutral, centred header) + its out jack.
    if (!cardInput_.isEmpty()) {
        skin::drawChassisCard(g, cardInput_.toFloat(), 24.0f);
        auto head = cardInput_.reduced(14, 14);
        g.setColour(skin::inkFaint);
        g.setFont(skin::monoFont(9.5f));
        g.drawText("TRIM", head.getX(), head.getY(), head.getWidth(), 14,
                   juce::Justification::centred);
        g.setColour(skin::ink);
        g.setFont(skin::wordmarkFont(21.0f));
        g.drawText("Input", head.getX(), head.getY() + 14, head.getWidth(), 26,
                   juce::Justification::centred);
        skin::drawJack(g, {(float)cardInput_.getRight(),
                           (float)cardInput_.getY() + cardInput_.getHeight() * 0.42f},
                       16.0f);
    }

    drawCard(cardAmp_, ampEyebrow_, ampWordmark_, ampAccent_, 26.0f);
    if (!cardAmp_.isEmpty())
        skin::drawJack(g, {(float)cardAmp_.getX(),
                           (float)cardAmp_.getY() + cardAmp_.getHeight() * 0.42f},
                       16.0f);

    // Gear-tray caption, above the add button and clear of the cable line.
    if (!trayBounds_.isEmpty()) {
        g.setColour(skin::benchFaint);
        g.setFont(skin::monoFont(9.5f));
        g.drawText(cards_.isEmpty() ? juce::String("EMPTY CHAIN") : juce::String("GEAR TRAY"),
                   trayBounds_.getX(), trayAdd_.getY() - 18, trayBounds_.getWidth(), 14,
                   juce::Justification::centred);
    }

    // Modulation row divider + caption inside the amp card. The line lives in the
    // MIDDLE of the gap between the tone rows and the mod row, so neither the mod
    // knobs' floating value arcs nor the mode switch can touch it.
    if (!ampModRowCaption_.isEmpty()) {
        auto cap = ampModRowCaption_;
        g.setColour(skin::shDarker);
        g.drawLine((float)cap.getX(), (float)ampModDividerY_,
                   (float)cardAmp_.getRight() - 24, (float)ampModDividerY_, 1.0f);
        g.setColour(skin::inkFaint);
        g.setFont(skin::wordmarkFont(15.0f));
        g.drawText(modCaption_.toUpperCase(), cap.getX(), cap.getCentreY() - 10,
                   cap.getWidth(), 20, juce::Justification::centredLeft);
    }
}

}  // namespace clipper::native
