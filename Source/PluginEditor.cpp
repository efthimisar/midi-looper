#include "PluginEditor.h"

namespace
{
    const char* const kCmdLabels[LooperProcessor::kNumCommands] = { "REC", "STOP", "UNDO", "REDO", "CLEAR" };
    const ml::Command kCmds[LooperProcessor::kNumCommands] = { ml::Command::Main, ml::Command::Stop, ml::Command::Undo,
                                                               ml::Command::Redo, ml::Command::Clear };

    // Θέσεις (λογικά pixel)
    constexpr int kWidth = 600, kHeight = 410;
    constexpr int kFootSize = 92, kFootY = 262;
    constexpr int kFootX[3] = { 54, 254, 454 };               // REC, STOP, REDO
    constexpr int kFootCmd[3] = { 0, 1, 3 };                  // δείκτες μέσα στον πίνακα εντολών
    constexpr int kLedY = 251;

    const juce::Colour kPanelColour { 0xff0e0f12 };
    const juce::Colour kLabelColour { 0xffb7bbc6 };
    const juce::Colour kGreen       { 0xff3dd68c };
}

// ---------------------------------------------------------------- εμφάνιση
LooperLookAndFeel::LooperLookAndFeel()
{
    setColour (juce::ComboBox::textColourId, juce::Colour (0xffeceef2));
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1b1d22));
    setColour (juce::PopupMenu::textColourId, juce::Colour (0xffeceef2));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kGreen);
    setColour (juce::PopupMenu::highlightedTextColourId, kPanelColour);
}

void LooperLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& colour,
                                              bool isHighlighted, bool isDown)
{
    if (button.getComponentID() != "footswitch")
    {
        LookAndFeel_V4::drawButtonBackground (g, button, colour, isHighlighted, isDown);
        return;
    }

    const auto area = button.getLocalBounds().toFloat().reduced (3.0f);
    const float d = juce::jmin (area.getWidth(), area.getHeight());
    auto circle = juce::Rectangle<float> (d, d).withCentre (area.getCentre());
    if (isDown) circle = circle.translated (0.0f, 2.0f);

    juce::Path p;
    p.addEllipse (circle);
    juce::DropShadow (juce::Colours::black.withAlpha (isDown ? 0.18f : 0.30f), isDown ? 4 : 8,
                      { 0, isDown ? 1 : 4 }).drawForPath (g, p);

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffdfdfe), circle.getX() + d * 0.35f, circle.getY() + d * 0.30f,
                                             juce::Colour (0xff9ea2ab), circle.getRight(), circle.getBottom(), true));
    g.fillEllipse (circle);

    const auto inner = circle.reduced (d * 0.15f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (isDown ? 0xffc9ccd2 : 0xffeceef1),
                                             inner.getX() + inner.getWidth() * 0.4f, inner.getY() + inner.getHeight() * 0.35f,
                                             juce::Colour (0xffaeb2ba), inner.getRight(), inner.getBottom(), true));
    g.fillEllipse (inner);
    g.setColour (juce::Colours::black.withAlpha (0.15f));
    g.drawEllipse (inner, 1.0f);
}

void LooperLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool isHighlighted, bool isDown)
{
    if (button.getComponentID() == "footswitch") return;   // τα κείμενα ζωγραφίζονται από το παράθυρο
    LookAndFeel_V4::drawButtonText (g, button, isHighlighted, isDown);
}

void LooperLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool, bool)
{
    const auto b = button.getLocalBounds().toFloat();
    const float y = b.getCentreY() - 9.0f;
    const juce::Rectangle<float> pill (b.getX(), y, 34.0f, 18.0f);
    const bool on = button.getToggleState();

    g.setColour (on ? kGreen : juce::Colour (0xff2b2e36));
    g.fillRoundedRectangle (pill, 9.0f);
    g.setColour (on ? kPanelColour : juce::Colour (0xff8d92a0));
    g.fillEllipse (pill.getX() + (on ? 18.0f : 2.0f), pill.getY() + 2.0f, 14.0f, 14.0f);

    g.setColour (kLabelColour);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (button.getButtonText(), juce::Rectangle<int> ((int) pill.getRight() + 8, 0,
                                                              button.getWidth() - 42, button.getHeight()),
                juce::Justification::centredLeft);
}

void LooperLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox&)
{
    const auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    g.setColour (juce::Colour (0xff1b1d22));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (juce::Colour (0xff3a3d46));
    g.drawRoundedRectangle (b, 6.0f, 1.0f);

    const float cx = (float) width - 18.0f, cy = (float) height * 0.5f;
    juce::Path arrow;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (juce::Colour (0xff8d92a0));
    g.strokePath (arrow, juce::PathStrokeType (1.6f));
}

// ------------------------------------------------------------------ παράθυρο
LooperEditor::LooperEditor (LooperProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&laf);
    setSize (kWidth, kHeight);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff3f4f6));
    addAndMakeVisible (statusLabel);

    for (int i = 0; i < LooperProcessor::kNumCommands; ++i)
    {
        auto& b = cmdButtons[(size_t) i];
        b.setButtonText (kCmdLabels[i]);
        b.onClick = [this, i] { proc.postCommand (kCmds[i]); };
        // Εμφανίζονται μόνο REC, STOP, REDO (τα UNDO και CLEAR βγαίνουν από διπλό πάτημα)
        if (i == 0 || i == 1 || i == 3)
        {
            b.setComponentID ("footswitch");
            addAndMakeVisible (b);
        }
    }

    startLabel.setText ("Start quantize (needs host playing)", juce::dontSendNotification);
    quantizeLabel.setText ("Length quantize", juce::dontSendNotification);
    afterLabel.setText ("After first loop", juce::dontSendNotification);
    for (auto* l : { &startLabel, &quantizeLabel, &afterLabel })
    {
        l->setColour (juce::Label::textColourId, kLabelColour);
        l->setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (l);
    }

    startBox.addItemList ({ "Off", "Beat", "Bar" }, 1);
    quantizeBox.addItemList ({ "Off", "Beat", "Bar" }, 1);
    afterBox.addItemList ({ "Play", "Overdub" }, 1);
    addAndMakeVisible (startBox);
    addAndMakeVisible (quantizeBox);
    addAndMakeVisible (afterBox);

    syncToggle.setButtonText ("Sync to host transport");
    thruToggle.setButtonText ("MIDI thru");
    for (auto* t : { &syncToggle, &thruToggle }) addAndMakeVisible (t);

    startAtt    = std::make_unique<Combo> (proc.apvts, "startQuantize", startBox);
    quantizeAtt = std::make_unique<Combo> (proc.apvts, "quantize", quantizeBox);
    afterAtt    = std::make_unique<Combo> (proc.apvts, "afterRecord", afterBox);
    syncAtt     = std::make_unique<Button> (proc.apvts, "syncHost", syncToggle);
    thruAtt     = std::make_unique<Button> (proc.apvts, "passThrough", thruToggle);

    startTimerHz (30);
    timerCallback();
}

LooperEditor::~LooperEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void LooperEditor::drawLed (juce::Graphics& g, juce::Point<float> c, juce::Colour colour, bool lit) const
{
    if (lit)
    {
        g.setColour (colour.withAlpha (0.35f));
        g.fillEllipse (c.x - 9.0f, c.y - 9.0f, 18.0f, 18.0f);
        g.setColour (colour);
    }
    else
        g.setColour (juce::Colour (0xffc9ccd2));
    g.fillEllipse (c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.drawEllipse (c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f, 1.0f);
}

void LooperEditor::drawIcon (juce::Graphics& g, Icon icon, juce::Point<float> c, float s, juce::Colour col) const
{
    g.setColour (col);
    const float h = s * 0.5f;

    switch (icon)
    {
        case Icon::Record:
            g.fillEllipse (c.x - h, c.y - h, s, s);
            break;
        case Icon::Play:
        {
            juce::Path p;
            p.addTriangle ({ c.x - h * 0.7f, c.y - h }, { c.x - h * 0.7f, c.y + h }, { c.x + h, c.y });
            g.fillPath (p);
            break;
        }
        case Icon::Stop:
        {
            const float q = s * 0.85f;
            g.fillRoundedRectangle (c.x - q * 0.5f, c.y - q * 0.5f, q, q, 2.0f);
            break;
        }
        case Icon::Overdub:
            g.drawEllipse (c.x - h, c.y - h, s, s, 2.0f);
            g.fillEllipse (c.x - h * 0.45f, c.y - h * 0.45f, h * 0.9f, h * 0.9f);
            break;
        case Icon::Cancel:
        case Icon::Clear:
            g.drawLine (c.x - h * 0.8f, c.y - h * 0.8f, c.x + h * 0.8f, c.y + h * 0.8f, 2.2f);
            g.drawLine (c.x - h * 0.8f, c.y + h * 0.8f, c.x + h * 0.8f, c.y - h * 0.8f, 2.2f);
            break;
        case Icon::Redo:
        case Icon::Undo:
        {
            const float r = h * 0.8f, a0 = 0.9f, a1 = 5.6f;   // γωνίες από τις 12 η ώρα, δεξιόστροφα
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, r, r, 0.0f, a0, a1, true);
            const juce::Point<float> e (c.x + r * std::sin (a1), c.y - r * std::cos (a1));
            const juce::Point<float> d (std::cos (a1), std::sin (a1));
            const juce::Point<float> n (-d.y, d.x);
            juce::Path head;
            head.addTriangle (e + d * (h * 0.75f), e + n * (h * 0.55f), e - n * (h * 0.55f));
            if (icon == Icon::Undo)
            {
                const auto m = juce::AffineTransform::scale (-1.0f, 1.0f, c.x, c.y);
                arc.applyTransform (m);
                head.applyTransform (m);
            }
            g.strokePath (arc, juce::PathStrokeType (2.0f));
            g.fillPath (head);
            break;
        }
    }
}

void LooperEditor::paint (juce::Graphics& g)
{
    // σώμα
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffeceef0), 0.0f, 0.0f,
                                             juce::Colour (0xffd9dbdf), 0.0f, (float) getHeight(), false));
    g.fillAll();

    // μαύρο πάνελ
    g.setColour (juce::Colours::white.withAlpha (0.7f));
    g.fillRoundedRectangle (14.0f, 15.0f, 572.0f, 206.0f, 14.0f);
    g.setColour (kPanelColour);
    g.fillRoundedRectangle (14.0f, 14.0f, 572.0f, 206.0f, 14.0f);

    // μπάρα προόδου
    const auto state = (ml::State) shownState;
    g.setColour (juce::Colour (0xff24262c));
    g.fillRoundedRectangle (progressArea.toFloat(), 5.0f);
    g.setColour (state == ml::State::Recording   ? juce::Colour (0xffe5484d)
               : state == ml::State::Overdubbing ? juce::Colour (0xfff5a524)
               : state == ml::State::Armed       ? juce::Colour (0xfff5d90a)
               : kGreen);
    g.fillRoundedRectangle (progressArea.toFloat().withWidth (progressArea.getWidth() * progress), 5.0f);

    // φωτάκια πάνω από τα πετάλια
    const bool recLit = state == ml::State::Recording || state == ml::State::Overdubbing
                     || state == ml::State::Armed || state == ml::State::Playing;
    const auto recColour = state == ml::State::Recording   ? juce::Colour (0xffff4a4f)
                         : state == ml::State::Overdubbing ? juce::Colour (0xfff5a524)
                         : state == ml::State::Armed       ? juce::Colour (0xfff5d90a)
                         : kGreen;
    const juce::Colour blue (0xff5aa2ff);
    drawLed (g, { (float) kFootX[0] + kFootSize * 0.5f, (float) kLedY }, recColour, recLit);
    drawLed (g, { (float) kFootX[1] + kFootSize * 0.5f, (float) kLedY }, blue, state == ml::State::Stopped);
    drawLed (g, { (float) kFootX[2] + kFootSize * 0.5f, (float) kLedY }, blue, shownRedo > 0);

    // σύμβολα κάτω από τα πετάλια
    const juce::Colour dark (0xff2a2c31), grey (0xff6b6f7a);
    const float cx0 = (float) kFootX[0] + kFootSize * 0.5f;
    const float cx1 = (float) kFootX[1] + kFootSize * 0.5f;
    const float cx2 = (float) kFootX[2] + kFootSize * 0.5f;

    const auto mainColour = mainIcon == Icon::Record  ? juce::Colour (0xffe5484d)
                          : mainIcon == Icon::Overdub ? juce::Colour (0xfff5a524)
                          : dark;
    drawIcon (g, mainIcon, { cx0, 372.0f }, 16.0f, mainColour);
    drawIcon (g, Icon::Stop, { cx1, 372.0f }, 16.0f, dark);
    drawIcon (g, Icon::Redo, { cx2, 372.0f }, 18.0f, dark);

    // υπενθύμιση διπλού πατήματος: 2× και σύμβολο
    g.setColour (grey);
    g.setFont (juce::FontOptions (11.0f));
    for (const auto& hint : { std::pair<float, Icon> { cx1, Icon::Undo }, std::pair<float, Icon> { cx2, Icon::Clear } })
    {
        g.drawText (juce::String (juce::CharPointer_UTF8 ("2\xc3\x97")),
                    juce::Rectangle<int> ((int) hint.first - 24, 384, 22, 14), juce::Justification::centredRight);
        drawIcon (g, hint.second, { hint.first + 10.0f, 391.0f }, 11.0f, grey);
    }
}

void LooperEditor::resized()
{
    statusLabel.setBounds (34, 26, 532, 30);
    progressArea = { 34, 66, 532, 10 };

    startLabel.setBounds (34, 94, 230, 26);
    startBox.setBounds (272, 94, 294, 26);
    quantizeLabel.setBounds (34, 126, 230, 26);
    quantizeBox.setBounds (272, 126, 294, 26);
    afterLabel.setBounds (34, 158, 230, 26);
    afterBox.setBounds (272, 158, 294, 26);

    syncToggle.setBounds (34, 188, 240, 26);
    thruToggle.setBounds (330, 188, 200, 26);

    for (int i = 0; i < 3; ++i)
        cmdButtons[(size_t) kFootCmd[i]].setBounds (kFootX[i], kFootY, kFootSize, kFootSize);
}

void LooperEditor::timerCallback()
{
    const auto state = (ml::State) proc.uiState.load();
    const auto len = proc.uiLength.load();
    const auto pos = proc.uiPos.load();
    const double sr = proc.uiSampleRate.load();
    const int redo = proc.uiRedo.load();

    juce::String text;
    switch (state)
    {
        case ml::State::Empty:       text = "Empty"; break;
        case ml::State::Recording:   text = "RECORDING  " + juce::String ((double) pos / sr, 2) + " s"; break;
        case ml::State::Playing:     text = "PLAYING"; break;
        case ml::State::Overdubbing: text = "OVERDUBBING"; break;
        case ml::State::Stopped:     text = "Stopped"; break;
        case ml::State::Armed:       text = "ARMED: waiting for beat/bar"; break;
    }
    if (len > 0)
        text += "  |  loop " + juce::String ((double) len / sr, 2) + " s  |  layers " + juce::String (proc.uiLayers.load());
    if (proc.uiPaused.load() != 0) text += "  (waiting for host)";
    statusLabel.setText (text, juce::dontSendNotification);

    progress = (state == ml::State::Recording || len <= 0) ? 0.0f
             : juce::jlimit (0.0f, 1.0f, (float) pos / (float) len);
    if (state == ml::State::Recording) progress = 1.0f;
    if (state == ml::State::Armed) progress = 0.0f;

    if (shownState != (int) state || shownRedo != redo)
    {
        shownState = (int) state;
        shownRedo = redo;
        mainIcon = state == ml::State::Empty       ? Icon::Record
                 : state == ml::State::Recording   ? Icon::Play
                 : state == ml::State::Playing     ? Icon::Overdub
                 : state == ml::State::Overdubbing ? Icon::Play
                 : state == ml::State::Armed       ? Icon::Cancel
                 : Icon::Play;
        repaint();
    }
    else
        repaint (progressArea);
}
