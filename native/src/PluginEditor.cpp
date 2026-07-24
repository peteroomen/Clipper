#include "PluginEditor.h"

#ifndef CLIPPER_GIT_HASH
#define CLIPPER_GIT_HASH "dev"
#endif

namespace clipper::native {

namespace {
constexpr int kMargin = 22;
constexpr int kGap = 16;
constexpr int kTopBar = 66;
constexpr int kInputW = 118;
constexpr int kPedalW = 214;
constexpr int kKnobCellW = 66;
constexpr int kKnobCellH = 92;
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

    // ---- RAT ("Rodent", red) --------------------------------------------------
    knob(ratDist_, ratDistAttach_, pid::ratDist, "Dist", skin::accentRat);
    knob(ratFilter_, ratFilterAttach_, pid::ratFilter, "Filter", skin::accentRat);
    knob(ratLevel_, ratLevelAttach_, pid::ratLevel, "Level", skin::accentRat);
    ratSwitch_.setAccent(skin::accentRat);
    ratSwitch_.setCaption("Stomp");
    addAndMakeVisible(ratSwitch_);
    ratOnAttach_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::ratOn),
        [this](float v) { ratSwitch_.setOn(v >= 0.5f); updateEnablement(); }, nullptr);
    ratSwitch_.onClick = [this] {
        ratOnAttach_->setValueAsCompleteGesture(ratSwitch_.isOn() ? 0.0f : 1.0f);
    };

    // ---- SD-1 ("Super Drive", yellow) -----------------------------------------
    knob(sdDrive_, sdDriveAttach_, pid::sdDrive, "Drive", skin::accentSd);
    knob(sdTone_, sdToneAttach_, pid::sdTone, "Tone", skin::accentSd);
    knob(sdLevel_, sdLevelAttach_, pid::sdLevel, "Level", skin::accentSd);
    sdSwitch_.setAccent(skin::accentSd);
    sdSwitch_.setCaption("Stomp");
    addAndMakeVisible(sdSwitch_);
    sdOnAttach_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::sdOn),
        [this](float v) { sdSwitch_.setOn(v >= 0.5f); updateEnablement(); }, nullptr);
    sdSwitch_.onClick = [this] {
        sdOnAttach_->setValueAsCompleteGesture(sdSwitch_.isOn() ? 0.0f : 1.0f);
    };

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
    setResizeLimits(1000, 540, 1560, 900);
    setSize(1220, 604);

    refreshFromState();  // pull face + toggle + dim states from the APVTS
}

ClipperAudioProcessorEditor::~ClipperAudioProcessorEditor() { setLookAndFeel(nullptr); }

void ClipperAudioProcessorEditor::refreshFromState() {
    ratOnAttach_->sendInitialUpdate();
    sdOnAttach_->sendInitialUpdate();
    brightAttach_->sendInitialUpdate();
    cabAttach_->sendInitialUpdate();
    ampOnAttach_->sendInitialUpdate();
    chorusModeAttach_->sendInitialUpdate();
    updateAmpFace();  // reads ampModel from the param, lays out + repaints
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
    const bool ratOn = proc_.apvts.getParameter(pid::ratOn)->getValue() >= 0.5f;
    const bool sdOn = proc_.apvts.getParameter(pid::sdOn)->getValue() >= 0.5f;
    const bool ampOn = proc_.apvts.getParameter(pid::ampOn)->getValue() >= 0.5f;

    ratDist_.setDimmed(!ratOn);
    ratFilter_.setDimmed(!ratOn);
    ratLevel_.setDimmed(!ratOn);
    sdDrive_.setDimmed(!sdOn);
    sdTone_.setDimmed(!sdOn);
    sdLevel_.setDimmed(!sdOn);
    for (NeuKnob* k : {&volume_, &bass_, &middle_, &treble_, &presence_, &master_, &gain_,
                       &reverb_, &modSpeed_, &modDepth_})
        k->setDimmed(!ampOn);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void ClipperAudioProcessorEditor::resized() {
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

    // Four cards left→right; amp takes the remainder.
    cardInput_ = row.removeFromLeft(kInputW);
    row.removeFromLeft(kGap);
    cardRat_ = row.removeFromLeft(kPedalW);
    row.removeFromLeft(kGap);
    cardSd_ = row.removeFromLeft(kPedalW);
    row.removeFromLeft(kGap);
    cardAmp_ = row;

    // INPUT card: one knob, dead-centred in the card (header floats at the top).
    {
        int cw = juce::jmin(kKnobCellW + 6, cardInput_.getWidth() - 12);
        inputTrim_.setBounds(cardInput_.withSizeKeepingCentre(cw, kKnobCellH));
    }

    // Pedal card: 3 knobs in a row + a round stomp; the knob-row + stomp group is
    // vertically centred in the body below the header (authentic pedal balance).
    auto layoutPedal = [](juce::Rectangle<int> card, NeuKnob& a, NeuKnob& b, NeuKnob& c,
                          Footswitch& fsw) {
        auto in = card.reduced(16);
        in.removeFromTop(74);  // eyebrow + wordmark header stays at top
        const int stompH = 156;
        const int gap = 16;
        const int groupH = kKnobCellH + gap + stompH;
        int pad = juce::jmax(0, (in.getHeight() - groupH) / 2);
        in.removeFromTop(pad);
        auto knobRow = in.removeFromTop(kKnobCellH);
        int cw = knobRow.getWidth() / 3;
        a.setBounds(knobRow.removeFromLeft(cw).reduced(2, 0));
        b.setBounds(knobRow.removeFromLeft(cw).reduced(2, 0));
        c.setBounds(knobRow.reduced(2, 0));
        in.removeFromTop(gap);
        int fw = juce::jmin(130, in.getWidth());
        fsw.setBounds(in.getCentreX() - fw / 2, in.getY(), fw, stompH);
    };
    layoutPedal(cardRat_, ratDist_, ratFilter_, ratLevel_, ratSwitch_);
    layoutPedal(cardSd_, sdDrive_, sdTone_, sdLevel_, sdSwitch_);

    layoutAmpCard(cardAmp_);
}

void ClipperAudioProcessorEditor::layoutAmpCard(juce::Rectangle<int> card) {
    auto in = card.reduced(20);
    in.removeFromTop(56);  // eyebrow + wordmark header stays at top

    auto cluster = in.removeFromRight(showBright_ ? 168 : 116);
    in.removeFromRight(kGap);
    auto knobArea = in;
    const int perRow = juce::jmax(1, knobArea.getWidth() / kKnobCellW);
    const int nPrimary = (int)ampPrimaryKnobs_.size();
    const int rows = (nPrimary + perRow - 1) / juce::jmax(1, perRow);

    // Total control-block height (primary rows + optional modulation row), then
    // vertically centre the block in the body so the card isn't top-heavy.
    const int modGap = 18;
    int blockH = rows * kKnobCellH + (rows - 1) * 6;
    if (!ampModKnobs_.empty()) blockH += modGap + kKnobCellH;
    int startY = knobArea.getY() + juce::jmax(0, (knobArea.getHeight() - blockH) / 2);

    int x = knobArea.getX();
    int y = startY;
    int col = 0;
    for (NeuKnob* k : ampPrimaryKnobs_) {
        if (col == perRow) {
            col = 0;
            x = knobArea.getX();
            y += kKnobCellH + 6;
        }
        k->setBounds(x, y, kKnobCellW, kKnobCellH);
        x += kKnobCellW;
        ++col;
    }
    int primaryBottom = startY + rows * kKnobCellH + (rows - 1) * 6;

    // Right cluster (Bright?/Cab/Power) aligned with the FIRST primary knob row.
    {
        auto c = cluster.withY(startY).withHeight(kKnobCellH);
        int slot = c.getWidth() / (showBright_ ? 3 : 2);
        if (showBright_) bright_.setBounds(c.removeFromLeft(slot).reduced(4, 0));
        cab_.setBounds(c.removeFromLeft(slot).reduced(4, 0));
        power_.setBounds(c.reduced(4, 0));
    }

    // Modulation sub-row (Chorus/Tremolo): caption + speed/depth (+ mode switch).
    ampModRowCaption_ = {};
    if (!ampModKnobs_.empty()) {
        int rowY = primaryBottom + modGap;
        int cx = knobArea.getX();
        ampModRowCaption_ = juce::Rectangle<int>(cx, rowY, 72, kKnobCellH);
        cx += 76;
        for (NeuKnob* k : ampModKnobs_) {
            k->setBounds(cx, rowY, kKnobCellW, kKnobCellH);
            cx += kKnobCellW + 4;
        }
        if (showMode_) chorusMode_.setBounds(cx + 6, rowY - 4, 92, kKnobCellH + 8);
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

    // INPUT card (neutral, centred header).
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
    }

    drawCard(cardRat_, juce::String::fromUTF8("Dirt Nº1 · Rodent-Type"), "Rodent",
             skin::accentRat, 40.0f);
    drawCard(cardSd_, juce::String::fromUTF8("Drive Nº2 · Yellow"), "Super Drive",
             skin::accentSd, 30.0f);
    drawCard(cardAmp_, ampEyebrow_, ampWordmark_, ampAccent_, 26.0f);

    // Modulation divider caption inside the amp card.
    if (!ampModRowCaption_.isEmpty()) {
        auto cap = ampModRowCaption_;
        g.setColour(skin::shDarker);
        g.drawLine((float)cap.getX(), (float)cap.getY() + 2, (float)cardAmp_.getRight() - 24,
                   (float)cap.getY() + 2, 1.0f);
        g.setColour(skin::inkFaint);
        g.setFont(skin::wordmarkFont(15.0f));
        g.drawText(modCaption_.toUpperCase(), cap.getX(), cap.getCentreY() - 10,
                   cap.getWidth(), 20, juce::Justification::centredLeft);
    }
}

}  // namespace clipper::native
