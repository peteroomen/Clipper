// Clipper native shell — the neumorphic PEDAL BOARD editor (native parity).
//
// A doctrine-compliant (docs §17) board, laid out LEFT TO RIGHT exactly like the
// web's Board.tsx — the arrangement the user asked to keep:
//
//   [INPUT] ~~ | [pedal] ~~ [pedal] ~~ ... ~~ [+ ADD] | ~~ [AMP]
//              ^-------- the SCROLLING pedalboard --------^
//
// joined by neumorphic PATCH CABLES: sagging rubber tubes running jack to jack,
// drawn behind the enclosures so each plug reads as entering its socket.
//
// SCROLLING BOARD (superseding the grow-the-window judgment call). The parity pass
// made the window's minimum width track the board, so adding pedals shoved the
// window wider until a five-pedal rig needed ~1622 px of desk. The user asked for
// the opposite: a fixed window and a board that scrolls, "that way we can have n
// pedals". So the pedal strip now lives in a horizontally scrollable viewport while
// the INPUT card and the AMP face stay pinned outside it — the two things you want
// on screen at all times are exactly the two ends of the signal path. The window
// minimum is a plain 1040x560 again, whatever the board holds.
//
// The pedals stand on a RAIL: a channel milled into the porcelain bench carrying a
// ribbed rubber mat (skin::drawBoardRail). It scrolls with them, and it is what
// makes an overflowing board read as a board running off the edge of the bench
// rather than as cards that ran out of room.
//
// The board is DYNAMIC (this is the parity work). Cards come from the processor's
// chain order, and the user can:
//   * ADD    — the gear tray lists every pedal type not already on the board;
//   * REMOVE — the ✕ chip on each card;
//   * SWAP   — the ⇄ chip, listing the types not on the board;
//   * MOVE   — the ◀ ▶ chips, or by DRAGGING a card's ⠿ grip: the card reorders
//              live under the pointer, exactly as the web board does.
// Every edit goes through the processor's chain state, so it round-trips with the
// session, and the engine applies it through the 6 ms declick fade.
//
// The AMP stays fixed at the end of the chain, a single card whose FACE switches
// with the amp-voice choice (Eight Hundred gold / Twin silver-blue / Thirty copper
// / Clean 120 red), mirroring the web Amp faces.
//
// All controls stay 100% APVTS-attached; the drawing lives in ClipperLookAndFeel
// and PedalCard. Light-bench look only (a dark theme is still future work).

#ifndef CLIPPER_NATIVE_PLUGIN_EDITOR_H
#define CLIPPER_NATIVE_PLUGIN_EDITOR_H

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>
#include <vector>

#include "ClipperLookAndFeel.h"
#include "PedalCard.h"
#include "PluginProcessor.h"

namespace clipper::native {

// The scrolling half of the bench. `onScroll` fires whenever the visible window
// moves, so the editor can redraw the two BOUNDARY cables (input→first pedal and
// last pedal→amp), whose far ends live in this component's coordinate space.
class BoardViewport : public juce::Viewport {
public:
    std::function<void()> onScroll;
    void visibleAreaChanged(const juce::Rectangle<int>&) override {
        if (onScroll) onScroll();
    }
};

// The scrolled content: the rail, the pedal cards, the gear tray, and the cables
// BETWEEN cards. It owns no drawing of its own — the editor paints it through this
// callback, so all the board's visual language stays in one file.
class BoardContent : public juce::Component {
public:
    std::function<void(juce::Graphics&)> onPaint;
    void paint(juce::Graphics& g) override {
        if (onPaint) onPaint(g);
    }
};

// A mouse-transparent veil over the viewport's left/right edges, shown only on the
// side that has content hidden past it. This is the "there is more board that way"
// affordance — cheaper to read than a scrollbar and always visible, where the
// scrollbar is thin and sits at the very bottom.
class BoardEdgeFade : public juce::Component {
public:
    BoardEdgeFade() { setInterceptsMouseClicks(false, false); }
    void setEdges(bool left, bool right) {
        if (left != left_ || right != right_) {
            left_ = left;
            right_ = right;
            repaint();
        }
    }
    void paint(juce::Graphics&) override;

private:
    bool left_ = false, right_ = false;
};

// A repeating timer that just runs a callback — the drag-to-edge auto-scroll pump.
// (The editor's own Timer base is the once-a-quarter-second board-version watch; the
// auto-scroll needs ~30 ms and a completely different lifetime.)
class TickTimer : public juce::Timer {
public:
    std::function<void()> onTick;
    void timerCallback() override {
        if (onTick) onTick();
    }
};

class ClipperAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer {
public:
    explicit ClipperAudioProcessorEditor(ClipperAudioProcessor&);
    ~ClipperAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Re-sync every widget (the board cards, amp face + labels/visibility, dim
    // states, toggle positions, combo selections) from the current APVTS state.
    // Called on construction and whenever a bound parameter changes; also used by
    // the headless snapshot tool to force a synchronous refresh after setting
    // params.
    void refreshFromState();

    // --- board editing (also the snapshot tool's hooks) ----------------------
    void addPedal(int type);           // append before the amp
    void removePedalAt(int index);
    void swapPedalAt(int index, int newType);
    void movePedal(int from, int to);  // live-reorder, declicked by the engine

    // Scroll the board to `proportion` of its overflow (0 = hard left, 1 = hard
    // right). A no-op when the board fits. The headless snapshot tool uses this to
    // photograph a mid-scroll board; nothing in the shipped UI calls it.
    void setBoardScroll(double proportion);
    // How much board is hidden right now (0 when it fits) — the snapshot tool's
    // assertion that a scene really does overflow.
    int boardOverflow() const;

private:
    using ComboAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ParamAttach = juce::ParameterAttachment;

    void timerCallback() override;  // notices an EXTERNAL board change (host load)

    void rebuildBoard();          // recreate the cards from the processor's chain
    void scheduleBoardRefresh();  // ...but never from inside a card's own callback
    void layoutBoardContent();    // size + place the cards inside the scrolled content
    void updateAutoScroll();      // start/stop the drag-to-edge scroll pump
    void reorderUnderPointer(int contentX);  // the live-reorder rule, given a content x
    void paintBoardContent(juce::Graphics&); // the rail + the cables between cards
    void showTrayMenu();      // the gear tray's "add a pedal" popup
    void updateAmpFace();     // rebuild the visible amp control set for the voice
    void updateEnablement();  // dim bypassed sections' knobs
    void layoutAmpCard(juce::Rectangle<int>);
    void dragCardTo(int cardIndex, int parentX);  // pointer-driven live reorder

    ClipperAudioProcessor& proc_;
    ClipperLookAndFeel lnf_;

    // Top-bar selectors.
    juce::ComboBox ampVoiceBox_, oversampleBox_;
    std::unique_ptr<ComboAttach> ampVoiceAttach_, oversampleAttach_;

    // INPUT card.
    NeuKnob inputTrim_;
    std::unique_ptr<SliderAttach> inputTrimAttach_;

    // THE BOARD: one card per chain entry, plus the gear tray — all children of the
    // scrolled content, not of the editor. The INPUT card and the AMP face stay
    // outside the viewport, pinned to the window's two ends.
    BoardViewport boardView_;
    BoardContent boardContent_;
    BoardEdgeFade boardFade_;
    juce::OwnedArray<PedalCard> cards_;
    ChipButton trayAdd_{"+ ADD PEDAL"};
    int boardVersion_ = -1;
    int draggingCard_ = -1;
    // Where the dragging pointer sits in VIEWPORT space. Auto-scroll moves the
    // content under a stationary pointer, so the reorder must be re-evaluated from
    // this rather than from the content-space x the card last reported.
    int dragViewX_ = 0;
    TickTimer autoScroll_;

    // AMP card — the full superset of knobs; visibility/labels set per voice.
    NeuKnob volume_, bass_, middle_, treble_, presence_, master_, gain_, reverb_;
    NeuKnob modSpeed_, modDepth_;  // chorus/tremolo speed + depth/intensity
    std::unique_ptr<SliderAttach> volumeAttach_, bassAttach_, middleAttach_, trebleAttach_,
        presenceAttach_, masterAttach_, gainAttach_, reverbAttach_, modSpeedAttach_,
        modDepthAttach_;
    LeverToggle bright_, cab_;
    PowerControl power_;
    ModeSwitch chorusMode_;
    std::unique_ptr<ParamAttach> brightAttach_, cabAttach_, ampOnAttach_, chorusModeAttach_,
        ampModelListen_;

    // Card rectangles (computed in resized(), painted in paint()).
    juce::Rectangle<int> cardInput_, cardAmp_;
    juce::Rectangle<int> ampModRowCaption_;  // caption box for the chorus/tremolo row
    int ampModDividerY_ = 0;                 // the divider's OWN line, in its own gap

    // Per-voice presentation state.
    int ampModel_ = 0;
    juce::String ampWordmark_{"Clean 120"}, ampEyebrow_{"Solid State · Stereo"};
    juce::Colour ampAccent_{skin::accentClean};
    std::vector<NeuKnob*> ampPrimaryKnobs_;  // ordered tone knobs for this voice
    std::vector<NeuKnob*> ampModKnobs_;      // speed/depth (chorus/tremolo), if any
    juce::String modCaption_;                // "Chorus" / "Tremolo" / ""
    bool showBright_ = true;
    bool showMode_ = true;

    juce::Label buildStamp_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipperAudioProcessorEditor)
};

}  // namespace clipper::native

#endif  // CLIPPER_NATIVE_PLUGIN_EDITOR_H
