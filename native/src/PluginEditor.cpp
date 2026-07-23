#include "PluginEditor.h"

#ifndef CLIPPER_GIT_HASH
#define CLIPPER_GIT_HASH "dev"
#endif

namespace clipper::native {

namespace {
// Flat dark palette (no gradients/images — the tidy v1 look).
const juce::Colour kBg{0xff1a1d21};
const juce::Colour kPanel{0xff23272c};
const juce::Colour kAccent{0xffe0a44c};   // amber, echoing the web build stamp
const juce::Colour kText{0xffd7dde3};
const juce::Colour kMuted{0xff8a929b};

constexpr int kColW = 190;     // column width
constexpr int kPad = 16;
constexpr int kHeaderH = 28;
constexpr int kKnobH = 78;
constexpr int kRowH = 30;
constexpr int kTitleH = 40;

void styleHeader(juce::Label& l, const juce::String& text) {
    l.setText(text, juce::dontSendNotification);
    l.setJustificationType(juce::Justification::centredLeft);
    l.setColour(juce::Label::textColourId, kAccent);
    l.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
}
}  // namespace

void ClipperAudioProcessorEditor::addKnob(std::unique_ptr<Knob>& slot,
                                          const char* paramId,
                                          const juce::String& name) {
    slot = std::make_unique<Knob>();
    auto& s = slot->slider;
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 16);
    s.setColour(juce::Slider::rotarySliderFillColourId, kAccent);
    s.setColour(juce::Slider::rotarySliderOutlineColourId, kPanel);
    s.setColour(juce::Slider::thumbColourId, kText);
    s.setColour(juce::Slider::textBoxTextColourId, kText);
    s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(s);

    auto& l = slot->label;
    l.setText(name, juce::dontSendNotification);
    l.setJustificationType(juce::Justification::centred);
    l.setColour(juce::Label::textColourId, kMuted);
    l.setFont(juce::Font(juce::FontOptions(11.0f)));
    addAndMakeVisible(l);

    slot->attach = std::make_unique<SliderAttach>(proc_.apvts, paramId, s);
}

void ClipperAudioProcessorEditor::addToggle(std::unique_ptr<Toggle>& slot,
                                            const char* paramId,
                                            const juce::String& name) {
    slot = std::make_unique<Toggle>();
    auto& b = slot->button;
    b.setButtonText(name);
    b.setColour(juce::ToggleButton::textColourId, kText);
    b.setColour(juce::ToggleButton::tickColourId, kAccent);
    b.setColour(juce::ToggleButton::tickDisabledColourId, kMuted);
    addAndMakeVisible(b);
    slot->attach = std::make_unique<ButtonAttach>(proc_.apvts, paramId, b);
}

ClipperAudioProcessorEditor::ClipperAudioProcessorEditor(ClipperAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), proc_(p) {
    title_.setText("CLIPPER", juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centredLeft);
    title_.setColour(juce::Label::textColourId, kText);
    title_.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    addAndMakeVisible(title_);

    styleHeader(pedalsHeader_, "PEDALS");
    styleHeader(ampHeader_, "AMP");
    styleHeader(chorusHeader_, "CHORUS");
    styleHeader(outputHeader_, "OUTPUT");
    addAndMakeVisible(pedalsHeader_);
    addAndMakeVisible(ampHeader_);
    addAndMakeVisible(chorusHeader_);
    addAndMakeVisible(outputHeader_);

    // Pedals column.
    addKnob(inputTrim_, pid::inputTrim, "Input Trim");
    addToggle(ratOn_, pid::ratOn, "RAT");
    addKnob(ratDist_, pid::ratDist, "Dist");
    addKnob(ratFilter_, pid::ratFilter, "Filter");
    addKnob(ratLevel_, pid::ratLevel, "Level");
    addToggle(sdOn_, pid::sdOn, "SD-1");
    addKnob(sdDrive_, pid::sdDrive, "Drive");
    addKnob(sdTone_, pid::sdTone, "Tone");
    addKnob(sdLevel_, pid::sdLevel, "Level");

    // Amp column.
    addToggle(ampOn_, pid::ampOn, "Power");
    addKnob(volume_, pid::volume, "Volume");
    addKnob(bass_, pid::bass, "Bass");
    addKnob(middle_, pid::middle, "Middle");
    addKnob(treble_, pid::treble, "Treble");
    addToggle(bright_, pid::bright, "Bright");
    addToggle(cab_, pid::cab, "Cab");

    // Chorus column.
    chorusMode_.addItemList({"Off", "Chorus", "Vibrato"}, 1);
    chorusMode_.setColour(juce::ComboBox::backgroundColourId, kPanel);
    chorusMode_.setColour(juce::ComboBox::textColourId, kText);
    chorusMode_.setColour(juce::ComboBox::outlineColourId, kBg);
    addAndMakeVisible(chorusMode_);
    chorusModeAttach_ =
        std::make_unique<ComboAttach>(proc_.apvts, pid::chorusMode, chorusMode_);
    chorusModeLabel_.setText("Mode", juce::dontSendNotification);
    chorusModeLabel_.setJustificationType(juce::Justification::centred);
    chorusModeLabel_.setColour(juce::Label::textColourId, kMuted);
    chorusModeLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
    addAndMakeVisible(chorusModeLabel_);
    addKnob(chorusSpeed_, pid::chorusSpeed, "Speed");
    addKnob(chorusDepth_, pid::chorusDepth, "Depth");

    // Output column.
    oversampling_.addItemList({"1x", "2x", "4x", "8x"}, 1);
    oversampling_.setColour(juce::ComboBox::backgroundColourId, kPanel);
    oversampling_.setColour(juce::ComboBox::textColourId, kText);
    oversampling_.setColour(juce::ComboBox::outlineColourId, kBg);
    addAndMakeVisible(oversampling_);
    oversamplingAttach_ =
        std::make_unique<ComboAttach>(proc_.apvts, pid::oversampling, oversampling_);
    oversamplingLabel_.setText("Oversampling", juce::dontSendNotification);
    oversamplingLabel_.setJustificationType(juce::Justification::centred);
    oversamplingLabel_.setColour(juce::Label::textColourId, kMuted);
    oversamplingLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
    addAndMakeVisible(oversamplingLabel_);

    buildStamp_.setText(juce::String("build ") + CLIPPER_GIT_HASH,
                        juce::dontSendNotification);
    buildStamp_.setJustificationType(juce::Justification::centredRight);
    buildStamp_.setColour(juce::Label::textColourId, kMuted);
    buildStamp_.setFont(juce::Font(juce::FontOptions(11.0f)));
    addAndMakeVisible(buildStamp_);

    setSize(kPad * 5 + kColW * 4, 560);
}

void ClipperAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    // Four flat column panels.
    for (int c = 0; c < 4; ++c) {
        const int x = kPad + c * (kColW + kPad);
        juce::Rectangle<int> panel(x, kPad + kTitleH, kColW,
                                   getHeight() - (kPad * 2 + kTitleH + 18));
        g.setColour(kPanel.withAlpha(0.45f));
        g.fillRoundedRectangle(panel.toFloat(), 8.0f);
    }
    g.setColour(kAccent);
    g.fillRect(kPad, kPad + kTitleH - 6, getWidth() - kPad * 2, 2);
}

void ClipperAudioProcessorEditor::resized() {
    const int colX0 = kPad;
    auto colX = [&](int c) { return colX0 + c * (kColW + kPad); };
    const int innerPad = 12;

    title_.setBounds(kPad, kPad, 300, kTitleH);
    buildStamp_.setBounds(getWidth() - 220 - kPad, getHeight() - 22, 220, 18);

    const int top = kPad + kTitleH + 8;

    // Helper cursors per column.
    auto layoutKnob = [&](Knob& k, int x, int& y, int w) {
        k.label.setBounds(x, y, w, 14);
        k.slider.setBounds(x, y + 14, w, kKnobH - 14);
        y += kKnobH + 6;
    };
    auto layoutToggle = [&](Toggle& t, int x, int& y, int w) {
        t.button.setBounds(x + 6, y, w - 12, kRowH);
        y += kRowH;
    };

    // Column 0: Pedals.
    {
        int x = colX(0) + innerPad, w = kColW - innerPad * 2, y = top;
        pedalsHeader_.setBounds(x, y, w, kHeaderH); y += kHeaderH;
        layoutKnob(*inputTrim_, x, y, w);
        layoutToggle(*ratOn_, x, y, w);
        layoutKnob(*ratDist_, x, y, w);
        layoutKnob(*ratFilter_, x, y, w);
        layoutKnob(*ratLevel_, x, y, w);
        layoutToggle(*sdOn_, x, y, w);
        layoutKnob(*sdDrive_, x, y, w);
        layoutKnob(*sdTone_, x, y, w);
        layoutKnob(*sdLevel_, x, y, w);
    }
    // Column 1: Amp.
    {
        int x = colX(1) + innerPad, w = kColW - innerPad * 2, y = top;
        ampHeader_.setBounds(x, y, w, kHeaderH); y += kHeaderH;
        layoutToggle(*ampOn_, x, y, w);
        layoutKnob(*volume_, x, y, w);
        layoutKnob(*bass_, x, y, w);
        layoutKnob(*middle_, x, y, w);
        layoutKnob(*treble_, x, y, w);
        layoutToggle(*bright_, x, y, w);
        layoutToggle(*cab_, x, y, w);
    }
    // Column 2: Chorus.
    {
        int x = colX(2) + innerPad, w = kColW - innerPad * 2, y = top;
        chorusHeader_.setBounds(x, y, w, kHeaderH); y += kHeaderH;
        chorusModeLabel_.setBounds(x, y, w, 14); y += 14;
        chorusMode_.setBounds(x, y, w, 26); y += 26 + 10;
        layoutKnob(*chorusSpeed_, x, y, w);
        layoutKnob(*chorusDepth_, x, y, w);
    }
    // Column 3: Output.
    {
        int x = colX(3) + innerPad, w = kColW - innerPad * 2, y = top;
        outputHeader_.setBounds(x, y, w, kHeaderH); y += kHeaderH;
        oversamplingLabel_.setBounds(x, y, w, 14); y += 14;
        oversampling_.setBounds(x, y, w, 26); y += 26;
    }
}

}  // namespace clipper::native
