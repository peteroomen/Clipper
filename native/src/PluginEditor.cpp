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
// A pedal card's width scales with its knob count (the phaser's one-knob face is
// genuinely narrower — its morphology). Cards now ALWAYS take this width: they never
// squeeze, because a crowded board scrolls instead of grinding its pedals into
// slivers. That is the whole point of the viewport.
constexpr int kPedalMaxW = 212;
constexpr int kPedalMinW = 150;
constexpr int kKnobCellW = 66;
constexpr int kKnobCellH = 92;
constexpr int kKnobCellMinH = 46;
constexpr int kMinWidth = 1040;
constexpr int kMinHeight = 560;

// The scrolled content's own margins, so the rail's rounded ends and the first
// card's cast shadow have somewhere to sit instead of being clipped at x=0.
constexpr int kRailPad = 20;
// How much rail shows BELOW the cards — the lip a real pedalboard leaves in front.
constexpr int kRailLip = 26;
// How far the rail rides UP behind the cards. Only the slivers between enclosures
// and this lip are ever visible, which is exactly how a loaded board looks.
constexpr int kRailRise = 84;
constexpr int kScrollBarH = 11;
// Drag auto-scroll: how close to the viewport edge starts the pump, and how far each
// tick slides the board.
constexpr int kAutoScrollEdge = 56;
constexpr int kAutoScrollStep = 22;
constexpr int kAutoScrollMs = 24;
// The edge veil's width (the "more board that way" affordance).
constexpr int kFadeW = 34;
// The cab/IR picker chip under the amp's Cab lever: caption line, chip, and the
// note line that only appears when something needs saying.
constexpr int kCabChipH = 26;
constexpr int kCabCaptionH = 13;
constexpr int kCabNoteH = 26;
constexpr int kCabChipMinW = 148;
const juce::String kCabMenuClean = juce::String::fromUTF8("Clean 2\xc3\x97" "12");
const juce::String kCabMenuBrit = juce::String::fromUTF8("Brit 4\xc3\x97" "12");
const juce::String kCabMenuOrange = juce::String::fromUTF8("Orange 4\xc3\x97" "12");

int preferredPedalWidth(int type) {
    const int knobs = (int)pedalFace(type).knobs.size();
    // The GOLD box's engraved nameplate needs its tracking to breathe, so the plate
    // face is the one card that asks for more than the knob count implies.
    if (pedalFace(type).layout == PedalFace::Layout::Plate) return 236;
    return juce::jlimit(kPedalMinW, kPedalMaxW, 46 + 56 * knobs);
}
}  // namespace

void BoardEdgeFade::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    const float w = juce::jmin((float)kFadeW, r.getWidth() * 0.25f);
    // Fades to the bench, not to white: the board reads as passing UNDER the edge of
    // the working area rather than being wiped out by a gradient. Theme-resolved, so
    // a dark bench veils to charcoal rather than to porcelain.
    const juce::Colour bench = skin::ground();
    if (left_) {
        juce::ColourGradient grad(bench.withAlpha(0.72f), r.getX(), 0.0f,
                                  bench.withAlpha(0.0f), r.getX() + w, 0.0f, false);
        g.setGradientFill(grad);
        g.fillRect(r.withWidth(w));
    }
    if (right_) {
        juce::ColourGradient grad(bench.withAlpha(0.72f), r.getRight(), 0.0f,
                                  bench.withAlpha(0.0f), r.getRight() - w, 0.0f, false);
        g.setGradientFill(grad);
        g.fillRect(r.withTrimmedLeft((int)(r.getWidth() - w)));
    }
}

ClipperAudioProcessorEditor::ClipperAudioProcessorEditor(ClipperAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), proc_(p) {
    setLookAndFeel(&lnf_);

    // ---- THEME (visual pass 3) -------------------------------------------------
    // Resolve before ANY widget is constructed with an accent or a scheme: the
    // saved mode, then the OS state Auto may defer to.
    themeMode_ = skin::loadThemeMode();
    skin::setThemeMode(themeMode_);
    skin::setSystemDark(juce::Desktop::getInstance().isDarkModeActive());
    juce::Desktop::getInstance().addDarkModeSettingListener(this);

    // ---- knob helper: name, accent, visible + APVTS slider attachment ----------
    auto knob = [this](NeuKnob& k, std::unique_ptr<SliderAttach>& at, const char* id,
                       const juce::String& nm, juce::Colour ac) {
        k.setName(nm);
        k.setAccent(ac);
        addAndMakeVisible(k);
        at = std::make_unique<SliderAttach>(proc_.apvts, id, k.slider());
    };

    // ---- INPUT ----------------------------------------------------------------
    knob(inputTrim_, inputTrimAttach_, pid::inputTrim, "Trim",
         skin::accent(skin::AccentId::Twin));

    // ---- THE BOARD ------------------------------------------------------------
    // Cards are built from the processor's chain (see rebuildBoard); the gear tray
    // adds whatever is not on the board yet. Both live inside the SCROLLED content,
    // so the board can be any length without the window growing.
    boardContent_.onPaint = [this](juce::Graphics& g) { paintBoardContent(g); };
    boardView_.setViewedComponent(&boardContent_, false);
    // Horizontal only. The vertical axis is the card's own business; letting the
    // board scroll vertically would just let a user lose the pedals off the top.
    boardView_.setScrollBarsShown(false, true);
    boardView_.setScrollBarThickness(kScrollBarH);
    // A plain wheel with no horizontal component still scrolls the board: JUCE routes
    // deltaY to the x axis when x is the only scrollable one, which is what makes a
    // mouse (not just a trackpad) able to reach the far end of a long chain.
    boardView_.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    boardView_.onScroll = [this] {
        // The boundary cables are drawn by the EDITOR but end inside the content, so
        // they have to be re-struck every time the content moves.
        repaint();
        const int hidden = boardOverflow();
        const int at = boardView_.getViewPositionX();
        boardFade_.setEdges(hidden > 0 && at > 0, hidden > 0 && at < hidden);
    };
    addAndMakeVisible(boardView_);
    addAndMakeVisible(boardFade_);  // added AFTER the viewport, so it veils it

    trayAdd_.setTint(skin::benchInkDim());
    boardContent_.addAndMakeVisible(trayAdd_);
    trayAdd_.onClick = [this] { showTrayMenu(); };

    autoScroll_.onTick = [this] {
        if (draggingCard_ < 0) {
            autoScroll_.stopTimer();
            return;
        }
        const int visW = boardView_.getMaximumVisibleWidth();
        int dir = 0;
        if (dragViewX_ < kAutoScrollEdge) dir = -1;
        else if (dragViewX_ > visW - kAutoScrollEdge) dir = 1;
        if (dir == 0) {
            autoScroll_.stopTimer();
            return;
        }
        const int was = boardView_.getViewPositionX();
        const int want = juce::jlimit(0, juce::jmax(0, boardOverflow()),
                                      was + dir * kAutoScrollStep);
        if (want == was) {  // already against the end — nothing left to reveal
            autoScroll_.stopTimer();
            return;
        }
        boardView_.setViewPosition(want, 0);
        // The pointer has not moved, but the board under it has — so the card the
        // drag is hovering over has changed. Re-run the reorder from the new content x.
        reorderUnderPointer(want + dragViewX_);
    };

    // ---- AMP knobs (superset; per-voice visibility set in updateAmpFace) -------
    // Web parity (visual pass 2): the amp's controls resolve the LIGHT token
    // context on the web (the dark pinning is .pedal-scoped) — amp knobs paint as
    // porcelain on the light amp panel.
    knob(volume_, volumeAttach_, pid::volume, "Vol", skin::accent(skin::AccentId::Clean));
    knob(bass_, bassAttach_, pid::bass, "Bass", skin::accent(skin::AccentId::Clean));
    knob(middle_, middleAttach_, pid::middle, "Mid", skin::accent(skin::AccentId::Clean));
    knob(treble_, trebleAttach_, pid::treble, "Treble", skin::accent(skin::AccentId::Clean));
    knob(presence_, presenceAttach_, pid::jcmPresence, "Presence", skin::accent(skin::AccentId::Jcm));
    knob(master_, masterAttach_, pid::jcmMaster, "Master", skin::accent(skin::AccentId::Jcm));
    knob(gain_, gainAttach_, pid::jcmGain, "Gain", skin::accent(skin::AccentId::Jcm));
    knob(reverb_, reverbAttach_, pid::reverb, "Reverb", skin::accent(skin::AccentId::Clean));
    knob(modSpeed_, modSpeedAttach_, pid::chorusSpeed, "Speed", skin::accent(skin::AccentId::Clean));
    knob(modDepth_, modDepthAttach_, pid::chorusDepth, "Depth", skin::accent(skin::AccentId::Clean));
    knob(fac_, facAttach_, pid::orangeFac, "F.A.C.", skin::accent(skin::AccentId::Orange));
    knob(mesaMode_, mesaModeAttach_, pid::mesaMode, "Mode", skin::accent(skin::AccentId::Mesa));
    knob(champVol_, champVolAttach_, pid::champVolume, "Vol", skin::accent(skin::AccentId::Champ));
    // The DISCRETE amp controls are selectors, not pots: the core quantizes each
    // of them, and drawing them as 0-100 dials meant the readout named nothing and
    // the pointer could sit visually between two clicks. Labels and detent order
    // are the ABI's own (docs §57 for the F.A.C., §69 for the Mesa's three).
    // NOTE the Champ's knob is NOT in this list: it is a real continuous pot (the
    // 5F1's 1 MOhm audio-taper volume), not a selector.
    fac_.setPositions({"1", "2", "3", "4", "5", "6"});
    mesaMode_.setPositions({"CLEAN", "VNTG", "MODRN", "R-VNT", "R-MOD"});
    for (NeuKnob* k : {&volume_, &bass_, &middle_, &treble_, &presence_, &master_, &gain_,
                       &reverb_, &modSpeed_, &modDepth_, &fac_, &mesaMode_, &champVol_})
        k->setScheme(skin::benchScheme());

    // The Mesa's two two-state switches. Bound to the same 0..1 parameters the
    // dials used to drive, so automation, saved sessions and the C ABI are
    // untouched — only the WIDGET changed.
    for (auto* sw : {&mesaRectSw_, &mesaPowerSw_}) addChildComponent(*sw);
    mesaRectSw_.setLabels({"Silicon", "5U4"});
    mesaPowerSw_.setLabels({"Bold", "Spongy"});
    mesaRectSwAttach_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::mesaRectifier),
        [this](float v) { mesaRectSw_.setSelected(v >= 0.5f ? 1 : 0); }, nullptr);
    mesaPowerSwAttach_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::mesaPowerMode),
        [this](float v) { mesaPowerSw_.setSelected(v >= 0.5f ? 1 : 0); }, nullptr);
    mesaRectSw_.onSelect = [this](int idx) {
        mesaRectSwAttach_->setValueAsCompleteGesture(idx >= 1 ? 1.0f : 0.0f);
    };
    mesaPowerSw_.onSelect = [this](int idx) {
        mesaPowerSwAttach_->setValueAsCompleteGesture(idx >= 1 ? 1.0f : 0.0f);
    };

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
    // The CAB / IR picker chip. Its label is the CURRENT cab (or the loaded IR's
    // file name), so the amp card always says which cabinet is in the room.
    cabChip_.setTint(skin::inkDim);
    addAndMakeVisible(cabChip_);
    cabChip_.onClick = [this] { showCabMenu(); };

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
    // Built from the PARAMETER's own label array, never a second hardcoded list.
    // This used to carry five entries against a seven-choice `ampModel` parameter,
    // so Rocker Verb and Dual Rectifier were unselectable and the ComboBox's item
    // index no longer matched the choice index — the owner's "amp selector is
    // dogfooded, missing amps, don't line up". A ComboBoxAttachment maps item i to
    // choice i, so the two lists agreeing is not cosmetic, it is correctness.
    ampVoiceBox_.addItemList(ClipperAudioProcessor::ampModelChoices(), 1);
    jassert(ampVoiceBox_.getNumItems() == clipper::native::AMP_MODEL_COUNT);
    addAndMakeVisible(ampVoiceBox_);
    ampVoiceAttach_ = std::make_unique<ComboAttach>(proc_.apvts, pid::ampModel, ampVoiceBox_);
    oversampleBox_.addItemList({juce::String::fromUTF8("1×"), juce::String::fromUTF8("2×"),
                               juce::String::fromUTF8("4×"), juce::String::fromUTF8("8×")},
                              1);
    addAndMakeVisible(oversampleBox_);
    oversampleAttach_ =
        std::make_unique<ComboAttach>(proc_.apvts, pid::oversampling, oversampleBox_);

    // The THEME override chip: Auto -> Light -> Dark -> Auto. Auto is the default
    // and follows the OS; the other two pin it. Not a parameter — see PluginEditor.h.
    themeChip_.setTint(skin::inkDim);
    themeChip_.setText(skin::themeModeName(themeMode_).toUpperCase());
    themeChip_.onClick = [this] { cycleTheme(); };
    addAndMakeVisible(themeChip_);

    // Re-layout the amp face whenever the voice parameter changes (user or host).
    ampModelListen_ = std::make_unique<ParamAttach>(
        *proc_.apvts.getParameter(pid::ampModel), [this](float) { updateAmpFace(); },
        nullptr);

    // ---- build stamp ----------------------------------------------------------
    buildStamp_.setText(juce::String("build ") + CLIPPER_GIT_HASH, juce::dontSendNotification);
    buildStamp_.setJustificationType(juce::Justification::centredRight);
    buildStamp_.setColour(juce::Label::textColourId, skin::benchFaint());
    buildStamp_.setFont(skin::monoFont(11.0f));
    addAndMakeVisible(buildStamp_);

    setResizable(true, true);
    // A FIXED minimum again. The board no longer votes on how wide the window has to
    // be — it scrolls. 1040x560 fits the input card, a couple of pedals, and a full
    // amp face; everything past that is the user's choice of desk space.
    setResizeLimits(kMinWidth, kMinHeight, 2400, 1200);
    setSize(1360, 640);

    rebuildBoard();
    refreshFromState();  // pull face + toggle + dim states from the APVTS
    startTimer(250);     // watch for board changes made outside this editor
}

ClipperAudioProcessorEditor::~ClipperAudioProcessorEditor() {
    stopTimer();
    autoScroll_.stopTimer();
    juce::Desktop::getInstance().removeDarkModeSettingListener(this);
    setLookAndFeel(nullptr);
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
void ClipperAudioProcessorEditor::darkModeSettingChanged() {
    skin::setSystemDark(juce::Desktop::getInstance().isDarkModeActive());
    if (themeMode_ == skin::ThemeMode::Auto) applyTheme();  // Light/Dark pin it
}

void ClipperAudioProcessorEditor::cycleTheme() {
    switch (themeMode_) {
        case skin::ThemeMode::Auto:  setThemeMode(skin::ThemeMode::Light); break;
        case skin::ThemeMode::Light: setThemeMode(skin::ThemeMode::Dark); break;
        default:                     setThemeMode(skin::ThemeMode::Auto); break;
    }
}

void ClipperAudioProcessorEditor::setThemeMode(skin::ThemeMode m, bool persist) {
    themeMode_ = m;
    skin::setThemeMode(m);
    if (persist) skin::saveThemeMode(m);
    applyTheme();
}

// Every theme-dependent colour a widget CACHED (accents set once, label colours,
// knob schemes) is re-resolved here; everything painted from skin:: at paint time
// follows for free. Only ever runs on a flip, so the cost does not matter.
void ClipperAudioProcessorEditor::applyTheme() {
    themeChip_.setText(skin::themeModeName(themeMode_).toUpperCase());
    themeChip_.setTint(skin::inkDim);
    trayAdd_.setTint(skin::benchInkDim());
    buildStamp_.setColour(juce::Label::textColourId, skin::benchFaint());

    cabChip_.setTint(skin::inkDim);
    inputTrim_.setAccent(skin::accent(skin::AccentId::Twin));
    for (NeuKnob* k : {&volume_, &bass_, &middle_, &treble_, &presence_, &master_, &gain_,
                       &reverb_, &modSpeed_, &modDepth_})
        k->setScheme(skin::benchScheme());
    // The INPUT card is a dark island in both themes (like the pedals), so its knob
    // keeps the pinned-dark scheme; only its accent follows the theme.
    inputTrim_.setScheme(skin::darkIsland());

    for (auto* card : cards_) card->applyTheme();

    updateAmpFace();  // re-resolves ampAccent_ + re-applies it to every amp widget
    repaint();
}

void ClipperAudioProcessorEditor::refreshFromState() {
    if (boardVersion_ != proc_.chainVersion()) rebuildBoard();
    brightAttach_->sendInitialUpdate();
    cabAttach_->sendInitialUpdate();
    ampOnAttach_->sendInitialUpdate();
    chorusModeAttach_->sendInitialUpdate();
    refreshCab();
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
    // ...and so can the cab: host automation of cabModel, a session load, or the
    // missing-IR fallback firing on the message thread. Same version-counter trick.
    if (cabVersion_ != proc_.cabVersion()) {
        refreshCab();
        resized();
        repaint();
    }
    // Hand the engine's retired convolver pair back on the MESSAGE thread. Nothing
    // depends on this being prompt (the next prepare reclaims it anyway) — it just
    // means the memory of a cab you switched away from goes back at human speed
    // instead of waiting for the next switch.
    proc_.retireCab();
}

// ---------------------------------------------------------------------------
// The cab / IR picker
// ---------------------------------------------------------------------------
void ClipperAudioProcessorEditor::refreshCab() {
    cabVersion_ = proc_.cabVersion();
    cabChip_.setText(proc_.cabLabel().toUpperCase());
    const juce::String note = proc_.cabNote();
    if (note != cabNote_) {
        cabNote_ = note;
        resized();  // the note takes a line, so the chip's box moves
    }
    repaint();
}

void ClipperAudioProcessorEditor::showCabMenu() {
    const int choice = proc_.cabChoice();
    const juce::String custom = proc_.customIrLabel();
    juce::PopupMenu m;
    m.addSectionHeader("Cabinet");
    m.addItem(1, kCabMenuClean, true, choice == CAB_CLEAN212);
    m.addItem(2, kCabMenuBrit, true, choice == CAB_BRIT412);
    // M10.3. The MENU ids are 1-based popup ids, not CabChoice values (the popup
    // reserves 0 for "dismissed"), so the Orange takes menu id 5 while its
    // CabChoice is 3 — no arithmetic relationship, mapped in the callback.
    m.addItem(5, kCabMenuOrange, true, choice == CAB_ORANGE412);
    if (custom.isNotEmpty())
        m.addItem(3, juce::String("Custom: ") + custom, true, choice == CAB_CUSTOM);
    m.addSeparator();
    m.addItem(4, custom.isNotEmpty()
                     ? juce::String::fromUTF8("Load another IR\xe2\x80\xa6")
                     : juce::String::fromUTF8("Load IR\xe2\x80\xa6"));
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&cabChip_),
                    [this](int result) {
                        if (result == 1) proc_.setCabChoice(CAB_CLEAN212);
                        else if (result == 2) proc_.setCabChoice(CAB_BRIT412);
                        else if (result == 3) proc_.setCabChoice(CAB_CUSTOM);
                        else if (result == 5) proc_.setCabChoice(CAB_ORANGE412);
                        else if (result == 4) { chooseIrFile(); return; }
                        else return;
                        refreshCab();
                    });
}

void ClipperAudioProcessorEditor::chooseIrFile() {
    // Async, and the chooser OUTLIVES this call — hence the member. Cancelling
    // leaves the current cab exactly as it was; an unreadable file leaves it too,
    // and puts the reason in the note under the chip (proc_.cabNote()).
    irChooser_ = std::make_unique<juce::FileChooser>(
        "Load a cabinet impulse response", juce::File(), "*.wav;*.aif;*.aiff");
    irChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            const juce::File f = fc.getResult();
            if (f != juce::File()) proc_.loadCustomIrFile(f);
            refreshCab();
        });
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
        boardContent_.addAndMakeVisible(card);
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
            autoScroll_.stopTimer();
            repaint();
        };
    }
    // Every type already on the board is unavailable to add.
    trayAdd_.setChipEnabled((int)chain.size() < kMaxChain);
    boardVersion_ = proc_.chainVersion();
    layoutBoardContent();
}

int ClipperAudioProcessorEditor::boardOverflow() const {
    return juce::jmax(0, boardContent_.getWidth() - boardView_.getMaximumVisibleWidth());
}

void ClipperAudioProcessorEditor::setBoardScroll(double proportion) {
    const int hidden = boardOverflow();
    if (hidden <= 0) {
        boardView_.setViewPosition(0, 0);
        return;
    }
    boardView_.setViewPosition(
        juce::roundToInt(juce::jlimit(0.0, 1.0, proportion) * hidden), 0);
}

// Lay the chain out INSIDE the scrolled content, at each card's full width — no
// squeeze. The content is as wide as the chain needs (or the viewport, whichever is
// larger, so the rail always reaches both edges) and the viewport does the rest.
void ClipperAudioProcessorEditor::layoutBoardContent() {
    const int visW = juce::jmax(0, boardView_.getWidth());
    int need = 2 * kRailPad + kTrayW + kChainGap * cards_.size();
    for (auto* c : cards_) need += preferredPedalWidth(c->type());

    // Reserve the scrollbar strip only when it will actually be there, so a board
    // that fits gets the full height for its cards.
    const bool overflows = need > visW;
    const int visH = juce::jmax(0, boardView_.getHeight() - (overflows ? kScrollBarH : 0));
    boardContent_.setSize(juce::jmax(need, visW), visH);

    const int cardH = juce::jmax(40, visH - kRailLip);
    auto row = juce::Rectangle<int>(kRailPad, 0, boardContent_.getWidth() - 2 * kRailPad,
                                    cardH);
    for (auto* card : cards_) {
        card->setBounds(row.removeFromLeft(preferredPedalWidth(card->type())));
        row.removeFromLeft(kChainGap);
    }
    // The tray sits in the chain but ABOVE the jack line, so the cable running from
    // the last pedal to the amp passes cleanly beneath it.
    auto tray = row.removeFromLeft(kTrayW);
    trayAdd_.setBounds(tray.getX(), tray.getY() + (int)(cardH * 0.42) - 74,
                       tray.getWidth(), 42);

    // Keep the scroll position legal after a remove (the content may have shrunk out
    // from under it) and refresh the edge veils.
    const int hidden = boardOverflow();
    if (boardView_.getViewPositionX() > hidden) boardView_.setViewPosition(hidden, 0);
    boardFade_.setEdges(hidden > 0 && boardView_.getViewPositionX() > 0,
                        hidden > 0 && boardView_.getViewPositionX() < hidden);
    boardContent_.repaint();
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
        // Skip a RESERVED type whose slice has not landed: PEDAL_TYPE_COUNT was
        // widened to 11 ahead of three parallel slices, and offering a pedal with
        // no face would be dead UI at best (docs §61.10).
        if (!pedalHasFace(t)) continue;
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

// A card's grip reports the pointer in CONTENT coordinates (the card's parent is now
// the scrolled content). Two things follow from a drag: the live reorder, and — if
// the pointer has reached a viewport edge — the auto-scroll pump that lets a pedal be
// dragged somewhere that is currently off screen.
void ClipperAudioProcessorEditor::dragCardTo(int cardIndex, int contentX) {
    if (cardIndex < 0 || cardIndex >= cards_.size()) return;
    draggingCard_ = cardIndex;
    dragViewX_ = contentX - boardView_.getViewPositionX();
    reorderUnderPointer(contentX);
    updateAutoScroll();
}

void ClipperAudioProcessorEditor::reorderUnderPointer(int contentX) {
    // The same rule the web board uses: the drag lands BEFORE the first card whose
    // horizontal centre is right of the pointer.
    if (draggingCard_ < 0 || draggingCard_ >= cards_.size()) return;
    int target = cards_.size() - 1;
    for (int i = 0; i < cards_.size(); ++i) {
        if (contentX < cards_[i]->getBounds().getCentreX()) {
            target = i;
            break;
        }
    }
    if (target != draggingCard_) {
        const int from = draggingCard_;
        draggingCard_ = target;  // the card travels with the pointer
        movePedal(from, target);
    }
}

void ClipperAudioProcessorEditor::updateAutoScroll() {
    const int visW = boardView_.getMaximumVisibleWidth();
    const bool wantsScroll = boardOverflow() > 0 && draggingCard_ >= 0 &&
                             (dragViewX_ < kAutoScrollEdge ||
                              dragViewX_ > visW - kAutoScrollEdge);
    if (wantsScroll && !autoScroll_.isTimerRunning())
        autoScroll_.startTimer(kAutoScrollMs);
    else if (!wantsScroll && autoScroll_.isTimerRunning())
        autoScroll_.stopTimer();
}

void ClipperAudioProcessorEditor::updateAmpFace() {
    auto* mp = proc_.apvts.getParameter(pid::ampModel);
    ampModel_ = juce::jlimit(0, clipper::native::AMP_MODEL_COUNT - 1,
                             (int)(mp->convertFrom0to1(mp->getValue()) + 0.5f));

    // Hide the whole superset, then re-show per voice.
    for (NeuKnob* k : {&volume_, &bass_, &middle_, &treble_, &presence_, &master_, &gain_,
                       &reverb_, &modSpeed_, &modDepth_, &fac_, &mesaMode_, &champVol_})
        k->setVisible(false);
    for (ModeSwitch* sw : {&mesaRectSw_, &mesaPowerSw_}) sw->setVisible(false);
    ampPrimarySwitches_.clear();
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
            ampAccentId_ = skin::AccentId::Jcm;
            ampAccent_ = skin::accent(ampAccentId_);
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
            ampAccentId_ = skin::AccentId::Twin;
            ampAccent_ = skin::accent(ampAccentId_);
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
            // The trem ON/OFF switch (docs §20 amendment — the field-requested switch;
            // native parity 2026-07-30). Same shared chorusMode param the engine already
            // maps to TwinAmp::PARAM_TREMOLO_ENABLE (index >= 1 == on, declick-bracketed).
            showMode_ = true;
            chorusMode_.setLabels({"Off", "On"});
            chorusMode_.setAccent(ampAccent_);
            break;
        case 4:  // Orange OR120 "Overdrive" — orange. Gain Bass Treble F.A.C. H.F. Boost Reverb.
            // What is ABSENT is the point (docs §57): no Master (GAIN is the whole
            // amp), no Mid (a James stack is bass + treble), no Bright (H.F. BOOST is
            // the brightness control, and it lives at the driver's cathode).
            // PANEL NAMES per the Field Guide (docs §57.11): Input - F.A.C. - Bass -
            // Treble - H.F.Boost - Gain - Reverb. The labels below are the OR120's;
            // the PARAM IDS underneath are the shared amp slots every voice uses, so
            // renaming a label never touches automation, saved state or the C ABI.
            ampWordmark_ = "Overdrive";
            ampEyebrow_ = juce::String::fromUTF8("Head Nº5 · One-Twenty");
            ampAccentId_ = skin::AccentId::Orange;
            ampAccent_ = skin::accent(ampAccentId_);
            showBright_ = false;
            show(volume_, "Gain", ampAccent_);   // pid::volume — printed GAIN here
            show(bass_, "Bass", ampAccent_);
            show(treble_, "Treble", ampAccent_);
            show(fac_, "F.A.C.", ampAccent_);
            show(presence_, "H.F. Boost", ampAccent_);  // pid::presence, printed H.F. BOOST
            show(reverb_, "Reverb", ampAccent_);
            ampPrimaryKnobs_ = {&volume_, &bass_, &treble_, &fac_, &presence_, &reverb_};
            break;
        case 5:  // Rockerverb "Rocker Verb" — orange. Gain Bass Mid Treble Volume Reverb.
            // The MODERN Orange (docs §63), and every difference from case 4 above is
            // a CIRCUIT difference: it HAS a Mid (a Marshall-lineage FMV stack, where
            // the OR120's James network has none) and it HAS a master — the knob
            // printed VOLUME is pid::master, because it sits AFTER the tone stack and
            // is a master by function. It has no F.A.C., no presence of any kind and
            // no bright switch. The PARAM IDS are the shared amp slots, so the panel
            // wording never touches automation or saved state.
            ampWordmark_ = "Rocker Verb";
            ampEyebrow_ = juce::String::fromUTF8("Head Nº6 · One-Hundred");
            ampAccentId_ = skin::AccentId::Orange;
            ampAccent_ = skin::accent(ampAccentId_);
            showBright_ = false;
            show(gain_, "Gain", ampAccent_);
            show(bass_, "Bass", ampAccent_);
            show(middle_, "Mid", ampAccent_);
            show(treble_, "Treble", ampAccent_);
            show(master_, "Volume", ampAccent_);  // pid::master — printed VOLUME here
            show(reverb_, "Reverb", ampAccent_);
            ampPrimaryKnobs_ = {&gain_, &bass_, &middle_, &treble_, &master_, &reverb_};
            break;
        case clipper::native::AMP_MESA:
            // M10.4 the Dual Rectifier (docs §69) — crimson, and the busiest panel
            // on the board at NINE knobs. GAIN rides pid::jcmGain and the MASTER
            // pid::jcmMaster, the house reuse; the three switches take their own
            // ids because no other voice has them.
            //
            // NO REVERB and NO BRIGHT, and both are circuit facts rather than
            // omissions: a reverb tank makes it a Trem-O-Verb, which is a
            // different amp, and the Recto has no bright switch.
            //
            // PRESENCE is shown although it does NOTHING in the two MODERN modes —
            // sheet mbdr7 opens the power amp's feedback loop there, measured at
            // exactly zero loop depth (§69), and presence works through that loop.
            // Hiding it per mode would be worse: the knob is real, the amp is what
            // makes it inert, and the assistant coaches that rather than apologising
            // for it.
            ampWordmark_ = "Dual Rectifier";
            ampEyebrow_ = juce::String::fromUTF8("Head Nº7 · Rectifier");
            ampAccentId_ = skin::AccentId::Mesa;
            ampAccent_ = skin::accent(ampAccentId_);
            showBright_ = false;
            show(gain_, "Gain", ampAccent_);
            show(bass_, "Bass", ampAccent_);
            show(middle_, "Mid", ampAccent_);
            show(treble_, "Treble", ampAccent_);
            show(master_, "Master", ampAccent_);
            show(presence_, "Presence", ampAccent_);
            show(mesaMode_, "Mode", ampAccent_);
            // RECT and POWER are genuine two-position switches on the amp, so they
            // are drawn as switches. They used to be 0-100 dials.
            mesaRectSw_.setAccent(ampAccent_);
            mesaPowerSw_.setAccent(ampAccent_);
            mesaRectSw_.setCaption("Rect");
            mesaPowerSw_.setCaption("Power");
            mesaRectSw_.setVisible(true);
            mesaPowerSw_.setVisible(true);
            ampPrimaryKnobs_ = {&gain_, &bass_, &middle_, &treble_, &master_, &presence_,
                                &mesaMode_};
            ampPrimarySwitches_ = {&mesaRectSw_, &mesaPowerSw_};
            break;
        case 3:  // AC30 "Thirty" — copper. Vol Bass Treble Cut Reverb
            ampWordmark_ = "Thirty";
            ampEyebrow_ = juce::String::fromUTF8("Combo Nº4 · Top-Boost");
            ampAccentId_ = skin::AccentId::Ac30;
            ampAccent_ = skin::accent(ampAccentId_);
            showBright_ = false;
            show(volume_, "Vol", ampAccent_);
            show(bass_, "Bass", ampAccent_);
            show(treble_, "Treble", ampAccent_);
            show(presence_, "Cut", ampAccent_);  // presence param reused as TOP CUT
            show(reverb_, "Reverb", ampAccent_);
            ampPrimaryKnobs_ = {&volume_, &bass_, &treble_, &presence_, &reverb_};
            break;
        case 7:  // M10.10 Champ "Cadet" — lacquered tweed wheat. TWO knobs, and the
                 // sparsest panel in the app by a wide margin.
            //
            // WHAT IS ABSENT IS THE POINT and is listed so it does not read as an
            // unfinished face: a tweed 5F1 has NO TONE STACK AT ALL — no bass, no
            // middle, no treble — because Fender did not put a tone control on a
            // Champ until the 1964 blackface AA764. It has no gain, no master, no
            // presence, no bright switch and no chorus either. The one knob sits
            // BETWEEN the two preamp triodes with nothing downstream to trim it,
            // so how far up it is IS how much distortion you get.
            //
            // The knob rides pid::champVolume, its OWN id rather than the shared
            // pid::volume, because it needs its own DEFAULT: the shared 0.40 opens
            // this amp at ~50 % THD, which is exactly §63.14's "the amp opens at
            // the wall". REVERB is the §19 usability convenience (a real 5F1 has
            // no tank), same as the JCM's.
            ampWordmark_ = "Cadet";
            ampEyebrow_ = juce::String::fromUTF8("Combo Nº8 · Tweed");
            ampAccentId_ = skin::AccentId::Champ;
            ampAccent_ = skin::accent(ampAccentId_);
            showBright_ = false;
            show(champVol_, "Vol", ampAccent_);
            show(reverb_, "Reverb", ampAccent_);
            ampPrimaryKnobs_ = {&champVol_, &reverb_};
            break;
        default:  // Clean 120 — red. Vol Bass Mid Treble Reverb + chorus
            ampModel_ = 0;
            ampWordmark_ = "Clean 120";
            ampEyebrow_ = juce::String::fromUTF8("Solid State · Stereo");
            ampAccentId_ = skin::AccentId::Clean;
            ampAccent_ = skin::accent(ampAccentId_);
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
            // Restore the three chorus labels + accent — the Twin panel re-labels the
            // shared switch to its two-state trem ON/OFF.
            chorusMode_.setLabels({"Off", "Chorus", "Vibrato"});
            chorusMode_.setAccent(ampAccent_);
            break;
    }

    bright_.setAccent(ampAccent_);
    cab_.setAccent(ampAccent_);
    power_.setAccent(ampAccent_);
    bright_.setVisible(showBright_);
    chorusMode_.setVisible(showMode_);
    // Re-labeling clamps the displayed index (Twin shows 2 states, Clean 120 three),
    // and switching amps does not fire the param attachment — re-sync the display
    // from the parameter so e.g. Vibrato (2) survives a Twin round-trip intact.
    if (chorusModeAttach_) chorusModeAttach_->sendInitialUpdate();

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
    auto r = getLocalBounds().reduced(kMargin);

    // Top bar: title left; theme chip + amp-voice + oversample pills right.
    auto top = r.removeFromTop(kTopBar);
    {
        auto rightPills = top.removeFromRight(juce::jmin(470, top.getWidth()));
        auto theme = rightPills.removeFromLeft(102);
        auto voice = rightPills.removeFromLeft(184);
        auto os = rightPills;
        themeChip_.setBounds(theme.withTrimmedTop(22).withHeight(34).reduced(4, 0));
        ampVoiceBox_.setBounds(voice.withTrimmedTop(22).withHeight(34).reduced(4, 0));
        oversampleBox_.setBounds(os.withTrimmedTop(22).withHeight(34).reduced(4, 0));
    }

    r.removeFromTop(6);
    buildStamp_.setBounds(getWidth() - 240 - kMargin, getHeight() - 20, 240, 16);
    auto row = r;
    row.removeFromBottom(18);  // reserve for the build-stamp line

    // ---- the LEFT-TO-RIGHT signal chain --------------------------------------
    // INPUT (fixed) · [ the scrolling board: pedal cards · gear tray ] · AMP (fixed).
    // The two fixed cards are carved off first and the viewport simply takes what is
    // left, so the ends of the signal path never move however long the chain gets.
    cardInput_ = row.removeFromLeft(kInputW);
    row.removeFromLeft(kChainGap);
    cardAmp_ = row.removeFromRight(juce::jmax(kAmpMinW, row.getWidth() / 3));
    row.removeFromRight(kChainGap);

    boardView_.setBounds(row);
    boardFade_.setBounds(row);
    layoutBoardContent();

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

    // The Bright/Cab/Power cluster. Its width is no longer a guess: each widget
    // reports what its own SHADOWS need (visual pass 3 — pass 2 fixed the cluster's
    // height and left the width at a third of the card, which gave the power control
    // ~50 px for a 46 px rocker and sliced its cast shadow and the jewel's halo off
    // at the component's right edge). It still gives ground on a narrow card, but
    // proportionally, so no one widget is the one that gets clipped.
    const int leverW = LeverToggle::preferredWidth();
    const int powerW = PowerControl::preferredWidth();
    const int clusterWant = (showBright_ ? 2 * leverW : leverW) + powerW;
    // The cluster's HEIGHT comes from the power control's real anatomy (glow
    // head-room + jewel + full 46x64 rocker + caption) — the old max(cellH, 84)
    // squished the rocker to a sliver and clipped the jewel's halo square.
    const int clusterH = PowerControl::preferredHeight();
    auto cluster = in.removeFromRight(
        juce::jlimit(powerW, clusterWant, juce::jmax(powerW, in.getWidth() / 2)));
    in.removeFromRight(kGap);
    auto knobArea = in;
    const int nPrimary = (int)ampPrimaryKnobs_.size();
    // Switches get their OWN row beneath the knobs rather than a cell in the knob
    // grid: a segment needs the web's 78 px to print "Silicon" or "Spongy", where a
    // knob cell can be as little as 52 px. Squeezed into the grid they truncated to
    // "Silic…", wrapped one-per-row and collided with the cab chip.
    const bool hasSwitchRow = !ampPrimarySwitches_.empty();
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
    const int totalRows = rows + (hasMod ? 1 : 0) + (hasSwitchRow ? 1 : 0);
    const int spacing = (rows - 1) * 6 + (hasMod ? modGap : 0) + (hasSwitchRow ? 8 : 0);
    const int cellH = juce::jlimit(kKnobCellMinH, kKnobCellH,
                                   (knobArea.getHeight() - spacing) /
                                       juce::jmax(1, totalRows));
    int blockH = rows * cellH + (rows - 1) * 6;
    if (hasSwitchRow) blockH += 8 + cellH;
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

    // The switch row: left-aligned under the knobs, each switch at its own
    // preferred width so its labels are never clipped.
    if (hasSwitchRow) {
        const int swY = primaryBottom + 8;
        int sx = knobArea.getX();
        for (ModeSwitch* sw : ampPrimarySwitches_) {
            const int w = ModeSwitch::preferredWidth();
            sw->setBounds(sx, swY, w, cellH);
            sx += w + 8;
        }
        primaryBottom = swY + cellH;
    }

    // Right cluster (Bright?/Cab/Power) aligned with the FIRST primary knob row.
    // The levers' visual weight sits in their top 70 px; the power control needs
    // its full anatomy height (see clusterH above).
    const juce::Rectangle<int> clusterBox = cluster.withY(startY).withHeight(clusterH);
    {
        auto c = cluster.withY(startY).withHeight(clusterH);
        // Each slot is exactly its widget's preferred width, scaled together if the
        // card could not give the cluster all of it — so a squeeze never lands
        // entirely on the power control the way a fixed third-of-the-card did.
        const float k = juce::jmin(1.0f, (float)c.getWidth() / (float)clusterWant);
        const int lw = juce::jmax(24, (int)(leverW * k));
        // The power jewel sits glowSpread(16) below its bounds top (halo head-room);
        // the web aligns the toggle slots' tops with the JEWEL, so the levers drop
        // by the same amount.
        const int leverDrop = (int)skin::glowSpread(16.0f);
        if (showBright_) bright_.setBounds(c.removeFromLeft(lw).withTrimmedTop(leverDrop));
        cab_.setBounds(c.removeFromLeft(lw).withTrimmedTop(leverDrop));
        power_.setBounds(c);
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
        // taller than the row, so it cannot reach back over the divider. Its width
        // is the web segment plus the active segment's cast-shadow head-room.
        if (showMode_)
            chorusMode_.setBounds(cx + 10, rowY, ModeSwitch::preferredWidth(), cellH);
    }

    // ---- the CAB / IR picker, directly under the Cab lever -------------------
    // It sits in the cluster's column (that is where the CAB lever is), but it is
    // allowed to reach LEFT for a legible width, because a chip that says "TWEED
    // 1X12 CONE B" is useless clipped to the 70 px a squeezed cluster gets. It can
    // never cross the modulation divider: on a small window that line is the only
    // thing below the cluster, and the chip going under it would read as belonging
    // to the tremolo row.
    {
        const bool hasNote = cabNote_.isNotEmpty();
        const int need = kCabCaptionH + kCabChipH + (hasNote ? kCabNoteH : 0);
        const int limit = (hasMod && ampModDividerY_ > 0) ? ampModDividerY_ - 8
                                                          : knobArea.getBottom();
        int top = clusterBox.getBottom() + 12;
        if (top + need > limit) top = juce::jmax(clusterBox.getBottom() + 4, limit - need);

        const int chipW = juce::jmax(clusterBox.getWidth(), kCabChipMinW);
        const int chipX = juce::jmax(knobArea.getX(), clusterBox.getRight() - chipW);
        const int w = clusterBox.getRight() - chipX;

        cabChipCaption_ = {chipX, top, w, kCabCaptionH};
        cabChip_.setBounds(chipX, top + kCabCaptionH, w, kCabChipH);
        cabNoteBox_ = hasNote ? juce::Rectangle<int>(chipX, top + kCabCaptionH + kCabChipH + 2,
                                                     w, kCabNoteH)
                              : juce::Rectangle<int>();
        // Below the card entirely (a pathologically short window) — hide rather
        // than paint the chip over the build stamp.
        cabChip_.setVisible(cabChip_.getBounds().getBottom() <= knobArea.getBottom() + 6);
    }
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void ClipperAudioProcessorEditor::paint(juce::Graphics& g) {
    // The light porcelain bench (a soft vertical lift).
    juce::ColourGradient bench(skin::ground().brighter(0.035f), getWidth() * 0.5f, 0.0f,
                               skin::groundDeep(), getWidth() * 0.5f, (float)getHeight(),
                               false);
    g.setGradientFill(bench);
    g.fillRect(getLocalBounds());

    // Title wordmark.
    g.setColour(skin::benchInkDim());
    g.setFont(skin::wordmarkFont(30.0f));
    g.drawText("CLIPPER", kMargin, kMargin - 2, 320, 40, juce::Justification::centredLeft);
    g.setColour(skin::benchFaint());
    g.setFont(skin::monoFont(10.5f));
    g.drawText(juce::String::fromUTF8("GUITAR RIG · NATIVE"), kMargin, kMargin + 34, 320,
               14, juce::Justification::centredLeft);

    // Pill captions.
    auto pillCaption = [&](juce::Component& box, const juce::String& text) {
        g.setColour(skin::benchInkDim());
        g.setFont(skin::monoFont(10.0f));
        g.drawText(text, box.getX(), box.getY() - 16, box.getWidth(), 14,
                   juce::Justification::centredLeft);
    };
    pillCaption(themeChip_, "THEME");
    pillCaption(ampVoiceBox_, "AMP VOICE");
    pillCaption(oversampleBox_, "OVERSAMPLE");

    // ---- THE TWO BOUNDARY PATCH CABLES ----------------------------------------
    // Cables between pedals live inside the viewport and scroll with them (see
    // paintBoardContent). These two do not: input.out -> card[0].in and
    // card[last].out -> amp.in each have ONE end on the fixed bench and one end
    // inside the scrolling content.
    //
    // They TRACK the scroll. The board-side end is the real jack position pushed
    // through the viewport transform, so as the board slides the cable's span and
    // sag follow it. When that jack scrolls out past the viewport's edge the end is
    // CLAMPED to the edge and a jack plate is drawn there: the cable then reads as
    // entering a grommet on the side of the board, which is what a real board does
    // with a lead that leaves it. No cable is ever drawn running backwards over the
    // input card, which is what an unclamped transform would do.
    //
    // Drawn BEFORE the child components (JUCE paints the parent first), so each end
    // disappears under a chassis and into its socket — the web's cable z-order.
    {
        const float jackY = (float)cardInput_.getY() + cardInput_.getHeight() * 0.42f;
        const float ampJackY = (float)cardAmp_.getY() + cardAmp_.getHeight() * 0.42f;
        const juce::Point<float> inputOut{(float)cardInput_.getRight(), jackY};
        const juce::Point<float> ampIn{(float)cardAmp_.getX(), ampJackY};

        if (cards_.isEmpty()) {
            skin::drawCable(g, inputOut, ampIn);  // an EMPTY board is valid
        } else {
            const float viewL = (float)boardView_.getX();
            const float viewR = viewL + (float)boardView_.getMaximumVisibleWidth();
            const float dx = viewL - (float)boardView_.getViewPositionX();
            const float dy = (float)boardView_.getY();

            auto toBench = [&](juce::Point<float> p, bool& clamped) {
                juce::Point<float> q{p.x + dx, p.y + dy};
                clamped = q.x < viewL || q.x > viewR;
                q.x = juce::jlimit(viewL, viewR, q.x);
                return q;
            };

            bool clampedIn = false, clampedOut = false;
            const juce::Point<float> firstIn = toBench(cards_.getFirst()->inJack(), clampedIn);
            const juce::Point<float> lastOut = toBench(cards_.getLast()->outJack(), clampedOut);

            skin::drawCable(g, inputOut, firstIn);
            skin::drawCable(g, lastOut, ampIn);
            // The grommet only appears when the cable actually terminates at the
            // board's edge; when the pedal is on screen it already draws its own jack.
            if (clampedIn) skin::drawJack(g, firstIn, 16.0f);
            if (clampedOut) skin::drawJack(g, lastOut, 16.0f);
        }
    }

    // The AMP is a LIGHT bench-style panel (web parity, visual pass 2): amp.css
    // resolves the light token context — the dark pinning is .pedal-scoped. The
    // wordmark takes the amp accent (the JCM/Twin/AC30 winks); Clean 120's
    // wordmark is plain ink, exactly like the web default .amp-name.
    auto drawCard = [&](juce::Rectangle<int> card, const juce::String& eyebrow,
                        const juce::String& wordmark, juce::Colour accent, float wordSize) {
        if (card.isEmpty()) return;
        const skin::Scheme& sc = skin::benchScheme();
        skin::drawBenchCard(g, card.toFloat(), 26.0f);
        auto head = card.reduced(18, 14);
        g.setColour(sc.inkFaint);
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

    drawCard(cardAmp_, ampEyebrow_, ampWordmark_,
             ampModel_ == 0 ? skin::benchScheme().ink : ampAccent_, 26.0f);
    if (!cardAmp_.isEmpty())
        skin::drawJack(g, {(float)cardAmp_.getX(),
                           (float)cardAmp_.getY() + cardAmp_.getHeight() * 0.42f},
                       16.0f);

    // The CAB / IR picker's caption and its note. The chip itself is a child
    // component; these two are the editor's, so they resolve the amp panel's own
    // token scheme (light porcelain or dark bench) rather than the chip's chassis.
    if (cabChip_.isVisible() && !cabChipCaption_.isEmpty()) {
        const skin::Scheme& sc = skin::benchScheme();
        g.setColour(sc.inkFaint);
        g.setFont(skin::monoFont(10.0f));
        g.drawText("CAB IR", cabChipCaption_, juce::Justification::centredLeft);
        if (!cabNoteBox_.isEmpty() && cabNote_.isNotEmpty()) {
            // The error / fallback surface: a missing IR, an unreadable file, a
            // truncated upload. Amber-ish (the amp accent reads as "this is about
            // your amp"), two lines, never a dialog.
            g.setColour(ampAccent_.withAlpha(0.92f));
            g.setFont(skin::monoFont(9.5f));
            g.drawFittedText(cabNote_, cabNoteBox_, juce::Justification::topLeft, 2);
        }
    }

    // Modulation row divider + caption inside the amp card. The line lives in the
    // MIDDLE of the gap between the tone rows and the mod row, so neither the mod
    // knobs' floating value arcs nor the mode switch can touch it.
    if (!ampModRowCaption_.isEmpty()) {
        auto cap = ampModRowCaption_;
        const skin::Scheme& sc = skin::benchScheme();
        g.setColour(sc.shDarker);  // .amp-chorus border-top on the light panel
        g.drawLine((float)cap.getX(), (float)ampModDividerY_,
                   (float)cardAmp_.getRight() - 24, (float)ampModDividerY_, 1.0f);
        g.setColour(sc.inkFaint);
        g.setFont(skin::wordmarkFont(15.0f));
        g.drawText(modCaption_.toUpperCase(), cap.getX(), cap.getCentreY() - 10,
                   cap.getWidth(), 20, juce::Justification::centredLeft);
    }
}

// ---------------------------------------------------------------------------
// The scrolled content: the RAIL the pedals stand on, the cables BETWEEN them, and
// the gear-tray caption. Everything here is in content coordinates and slides with
// the board; the editor draws it through BoardContent::onPaint so the whole board's
// visual language stays in one file.
// ---------------------------------------------------------------------------
void ClipperAudioProcessorEditor::paintBoardContent(juce::Graphics& g) {
    auto area = boardContent_.getLocalBounds();
    if (area.isEmpty()) return;

    // The RAIL. It spans the full content width — so on an overflowing board it runs
    // off both edges of the viewport, which is precisely the cue that there is more
    // board out there. Its top rides up BEHIND the enclosures: only the slivers
    // between them and the lip in front are ever seen, exactly as on a loaded board.
    {
        const int cardH = juce::jmax(40, area.getHeight() - kRailLip);
        auto rail = juce::Rectangle<int>(0, juce::jmax(0, cardH - kRailRise),
                                         area.getWidth(), 0)
                        .withBottom(area.getHeight() - 2);
        skin::drawBoardRail(g, rail.toFloat());
    }

    // The pedal cards' CAST SHADOWS, drawn here — by the parent, onto the rail —
    // rather than by each card. A JUCE component's paint is clipped to its own
    // bounds, so a card painting its own shadow lands the whole thing underneath
    // itself and reads flat; that is why the board's pedals had no shadow while the
    // input and amp cards (which the EDITOR paints) did (visual pass 3, the owner's
    // "the pedals don't have shadows matching the other skeuomorphic things").
    for (auto* card : cards_)
        skin::drawIslandCastShadow(g, card->getBounds().toFloat(), 24.0f);

    // The cables BETWEEN pedals, in content space — they scroll for free, because
    // they are drawn by the same component the cards live in. Drawn after the rail
    // and before the cards (children paint last), so a plug tucks into its socket.
    for (int i = 1; i < cards_.size(); ++i)
        skin::drawCable(g, cards_[i - 1]->outJack(), cards_[i]->inJack());

    // Gear-tray caption, above the add button and clear of the cable line.
    g.setColour(skin::benchFaint());
    g.setFont(skin::monoFont(9.5f));
    g.drawText(cards_.isEmpty() ? juce::String("EMPTY CHAIN") : juce::String("GEAR TRAY"),
               trayAdd_.getX(), trayAdd_.getY() - 18, trayAdd_.getWidth(), 14,
               juce::Justification::centred);
}

}  // namespace clipper::native
