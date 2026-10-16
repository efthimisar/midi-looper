#include "PluginEditor.h"

namespace
{
    const char* const kCmdLabels[LooperProcessor::kNumCommands] = { "REC", "STOP", "UNDO", "REDO", "CLEAR" };
    const ml::Command kCmds[LooperProcessor::kNumCommands] = { ml::Command::Main, ml::Command::Stop, ml::Command::Undo,
                                                               ml::Command::Redo, ml::Command::Clear };
}

LooperEditor::LooperEditor (LooperProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (600, 330);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    addAndMakeVisible (statusLabel);

    for (int i = 0; i < LooperProcessor::kNumCommands; ++i)
    {
        auto& b = cmdButtons[(size_t) i];
        b.setButtonText (kCmdLabels[i]);
        b.onClick = [this, i] { proc.postCommand (kCmds[i]); };
        addAndMakeVisible (b);

        auto& lb = learnButtons[(size_t) i];
        lb.setButtonText ("Learn");
        lb.onClick = [this, i]
        {
            if (proc.getLearnTarget() == i) proc.cancelLearn();
            else                            proc.beginLearn (i);
        };
        addAndMakeVisible (lb);

        auto& cb = clearButtons[(size_t) i];
        cb.setButtonText ("x");
        cb.setTooltip ("Remove MIDI mapping");
        cb.onClick = [this, i] { proc.clearMapping (i); };
        addAndMakeVisible (cb);

        auto& ml = mapLabels[(size_t) i];
        ml.setJustificationType (juce::Justification::centred);
        ml.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (ml);
    }

    quantizeLabel.setText ("Length quantize", juce::dontSendNotification);
    afterLabel.setText ("After first loop", juce::dontSendNotification);
    bpbLabel.setText ("Beats per bar (if host has none)", juce::dontSendNotification);
    bpmLabel.setText ("Fallback BPM (if host has none)", juce::dontSendNotification);
    for (auto* l : { &quantizeLabel, &afterLabel, &bpbLabel, &bpmLabel }) addAndMakeVisible (l);

    quantizeBox.addItemList ({ "Off", "Beat", "Bar" }, 1);
    afterBox.addItemList ({ "Play", "Overdub" }, 1);
    addAndMakeVisible (quantizeBox);
    addAndMakeVisible (afterBox);

    for (auto* s : { &bpbSlider, &bpmSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
        addAndMakeVisible (s);
    }

    syncToggle.setButtonText ("Sync to host transport");
    thruToggle.setButtonText ("MIDI thru");
    consumeToggle.setButtonText ("Swallow mapped controls");
    for (auto* t : { &syncToggle, &thruToggle, &consumeToggle }) addAndMakeVisible (t);

    quantizeAtt = std::make_unique<Combo> (proc.apvts, "quantize", quantizeBox);
    afterAtt    = std::make_unique<Combo> (proc.apvts, "afterRecord", afterBox);
    bpbAtt      = std::make_unique<Slide> (proc.apvts, "beatsPerBar", bpbSlider);
    bpmAtt      = std::make_unique<Slide> (proc.apvts, "fallbackBpm", bpmSlider);
    syncAtt     = std::make_unique<Button> (proc.apvts, "syncHost", syncToggle);
    thruAtt     = std::make_unique<Button> (proc.apvts, "passThrough", thruToggle);
    consumeAtt  = std::make_unique<Button> (proc.apvts, "consume", consumeToggle);

    startTimerHz (30);
    timerCallback();
}

void LooperEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e1f24));

    g.setColour (juce::Colour (0xff3a3c44));
    g.fillRoundedRectangle (progressArea.toFloat(), 4.0f);

    const auto state = (ml::State) shownState;
    g.setColour (state == ml::State::Recording   ? juce::Colour (0xffe5484d)
               : state == ml::State::Overdubbing ? juce::Colour (0xfff5a524)
               : juce::Colour (0xff3dd68c));
    g.fillRoundedRectangle (progressArea.toFloat().withWidth (progressArea.getWidth() * progress), 4.0f);
}

void LooperEditor::resized()
{
    auto r = getLocalBounds().reduced (14);

    statusLabel.setBounds (r.removeFromTop (28));
    r.removeFromTop (4);
    progressArea = r.removeFromTop (14);
    r.removeFromTop (12);

    auto row = r.removeFromTop (54);
    const int w = row.getWidth() / LooperProcessor::kNumCommands;
    auto learnRow = r.removeFromTop (26);
    auto mapRow = r.removeFromTop (20);
    for (int i = 0; i < LooperProcessor::kNumCommands; ++i)
    {
        cmdButtons[(size_t) i].setBounds (row.removeFromLeft (w).reduced (4));
        auto l = learnRow.removeFromLeft (w).reduced (4, 0);
        clearButtons[(size_t) i].setBounds (l.removeFromRight (24));
        l.removeFromRight (4);
        learnButtons[(size_t) i].setBounds (l);
        mapLabels[(size_t) i].setBounds (mapRow.removeFromLeft (w));
    }

    r.removeFromTop (14);
    const int labelW = 230;
    auto line = [&] (juce::Component& label, juce::Component& control)
    {
        auto a = r.removeFromTop (28);
        label.setBounds (a.removeFromLeft (labelW));
        control.setBounds (a.reduced (0, 2));
        r.removeFromTop (4);
    };
    line (quantizeLabel, quantizeBox);
    line (afterLabel, afterBox);
    line (bpbLabel, bpbSlider);
    line (bpmLabel, bpmSlider);

    auto t = r.removeFromTop (28);
    const int tw = t.getWidth() / 3;
    syncToggle.setBounds (t.removeFromLeft (tw));
    thruToggle.setBounds (t.removeFromLeft (tw));
    consumeToggle.setBounds (t);
}

void LooperEditor::timerCallback()
{
    const auto state = (ml::State) proc.uiState.load();
    const auto len = proc.uiLength.load();
    const auto pos = proc.uiPos.load();
    const double sr = proc.uiSampleRate.load();

    juce::String text;
    switch (state)
    {
        case ml::State::Empty:       text = "Empty"; break;
        case ml::State::Recording:   text = "RECORDING  " + juce::String ((double) pos / sr, 2) + " s"; break;
        case ml::State::Playing:     text = "PLAYING"; break;
        case ml::State::Overdubbing: text = "OVERDUBBING"; break;
        case ml::State::Stopped:     text = "Stopped"; break;
    }
    if (len > 0)
        text += "   |   loop " + juce::String ((double) len / sr, 2) + " s   |   layers " + juce::String (proc.uiLayers.load());
    if (proc.uiPaused.load() != 0) text += "   (waiting for host transport)";
    statusLabel.setText (text, juce::dontSendNotification);

    progress = (state == ml::State::Recording || len <= 0) ? 0.0f
             : juce::jlimit (0.0f, 1.0f, (float) pos / (float) len);
    if (state == ml::State::Recording) progress = 1.0f;

    if (shownState != (int) state)
    {
        shownState = (int) state;
        const char* mainText = state == ml::State::Empty       ? "REC"
                             : state == ml::State::Recording   ? "PLAY"
                             : state == ml::State::Playing     ? "OVERDUB"
                             : state == ml::State::Overdubbing ? "PLAY"
                             : "PLAY";
        cmdButtons[0].setButtonText (mainText);
        cmdButtons[0].setColour (juce::TextButton::buttonColourId,
                                 state == ml::State::Recording   ? juce::Colour (0xffe5484d)
                               : state == ml::State::Overdubbing ? juce::Colour (0xfff5a524)
                               : getLookAndFeel().findColour (juce::TextButton::buttonColourId));
    }

    for (int i = 0; i < LooperProcessor::kNumCommands; ++i)
    {
        const bool learning = proc.getLearnTarget() == i;
        learnButtons[(size_t) i].setButtonText (learning ? "Press..." : "Learn");
        learnButtons[(size_t) i].setToggleState (learning, juce::dontSendNotification);
        mapLabels[(size_t) i].setText (proc.describeMapping (i), juce::dontSendNotification);
    }
    repaint (progressArea);
}
