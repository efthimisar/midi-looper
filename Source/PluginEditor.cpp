#include "PluginEditor.h"
#include <cmath>

namespace
{
    // Σειρά πεταλιών: REC, UNDO, STOP
    const ml::Command kCmds[LooperProcessor::kNumCommands] = { ml::Command::Main, ml::Command::Undo, ml::Command::Stop };

    // Θέσεις (λογικά pixel)
    constexpr int kWidth = 600, kHeight = 414;
    constexpr int kPadSize = 92, kPadY = 266;
    constexpr int kPadX[3] = { 54, 254, 454 };

    constexpr int kRowX = 70, kRowW = 84, kRowH = 26;
    constexpr int kRowY[4] = { 62, 92, 122, 152 };
    constexpr int kToggleY = 186;
    constexpr int kToggleX[3] = { 34, 124, 214 };

    constexpr float kLaneX = 196.0f, kLaneW = 370.0f, kLaneTop = 68.0f, kLaneH = 22.0f, kLanePitch = 32.0f;

    // χρώματα (θερμά παστέλ)
    const juce::Colour kBodyTop     { 0xfff6f1ea };
    const juce::Colour kBodyBottom  { 0xffe8e0d6 };
    const juce::Colour kPanel       { 0xff2b2724 };
    const juce::Colour kPanelLine   { 0xff3a3430 };
    const juce::Colour kLabel       { 0xffc9bfb6 };
    const juce::Colour kText        { 0xfff6f1ea };
    const juce::Colour kInk         { 0xff4a4038 };
    const juce::Colour kSage        { 0xff9ad4a8 };
    const juce::Colour kLilac       { 0xffc4aef0 };
    const juce::Colour kPeach       { 0xffffb98a };
    const juce::Colour kCoral       { 0xffff8f84 };
    const juce::Colour kButter      { 0xffffe08a };
    const juce::Colour kRecDot      { 0xffe8845a };
    const juce::Colour kMuted       { 0xff4a433e };

    const juce::Colour kLayerColour[4] = { kSage, kLilac, juce::Colour (0xfff5de8a), juce::Colour (0xfff4a7b9) };

    enum PadSymbol { symNone = -1 };
}

// ---------------------------------------------------------------- σύμβολα
void drawSymbol (juce::Graphics& g, Symbol sym, juce::Point<float> c, float size, juce::Colour col)
{
    const float k = size / 22.0f;
    const auto tf = juce::AffineTransform::scale (k).translated (c.x, c.y);

    auto stroke = [&] (juce::Path p, float w, const juce::AffineTransform& extra = {})
    {
        p.applyTransform (extra);
        p.applyTransform (tf);
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (w * k, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    auto fill = [&] (juce::Path p, juce::Colour colour)
    {
        p.applyTransform (tf);
        g.setColour (colour);
        g.fillPath (p);
    };
    auto line = [] (float x1, float y1, float x2, float y2) { juce::Path p; p.startNewSubPath (x1, y1); p.lineTo (x2, y2); return p; };

    switch (sym)
    {
        case Symbol::PadRecord:
        {
            juce::Path p; p.addEllipse (-8.0f, -8.0f, 16.0f, 16.0f);
            fill (p, col);
            break;
        }
        case Symbol::PadOverdub:
        {
            juce::Path ring; ring.addEllipse (-9.0f, -9.0f, 18.0f, 18.0f);
            stroke (ring, 2.4f);
            juce::Path dot; dot.addEllipse (-4.2f, -4.2f, 8.4f, 8.4f);
            fill (dot, kRecDot);
            break;
        }
        case Symbol::Play:
        {
            juce::Path p; p.addTriangle (-6.0f, -8.0f, -6.0f, 8.0f, 8.0f, 0.0f);
            fill (p, col);
            break;
        }
        case Symbol::Stop:
        {
            juce::Path p; p.addRoundedRectangle (-8.0f, -8.0f, 16.0f, 16.0f, 2.5f);
            fill (p, col);
            break;
        }
        case Symbol::Cancel:
        {
            juce::Path p;
            p.startNewSubPath (-7.0f, -7.0f); p.lineTo (7.0f, 7.0f);
            p.startNewSubPath (-7.0f, 7.0f);  p.lineTo (7.0f, -7.0f);
            stroke (p, 2.4f);
            break;
        }
        case Symbol::Undo:
        {
            const float h = 11.0f, r = h * 0.8f, a0 = 0.9f, a1 = 5.6f;   // γωνίες από τις 12 η ώρα, δεξιόστροφα
            juce::Path arc;
            arc.addCentredArc (0.0f, 0.0f, r, r, 0.0f, a0, a1, true);
            const juce::Point<float> e (r * std::sin (a1), -r * std::cos (a1));
            const juce::Point<float> d (std::cos (a1), std::sin (a1));
            const juce::Point<float> n (-d.y, d.x);
            juce::Path head;
            head.addTriangle (e + d * (h * 0.75f), e + n * (h * 0.55f), e - n * (h * 0.55f));
            const auto mirror = juce::AffineTransform::scale (-1.0f, 1.0f);
            head.applyTransform (mirror);
            stroke (arc, 2.2f, mirror);
            fill (head, col);
            break;
        }
        case Symbol::StartQ:
        {
            stroke (line (-8.0f, -7.0f, -8.0f, 7.0f), 1.7f);
            juce::Path p; p.addTriangle (-3.0f, -6.0f, -3.0f, 6.0f, 8.0f, 0.0f);
            fill (p, col);
            break;
        }
        case Symbol::LengthQ:
        {
            juce::Path p;
            p.startNewSubPath (-9.0f, -6.0f); p.lineTo (-9.0f, 6.0f);
            p.startNewSubPath (9.0f, -6.0f);  p.lineTo (9.0f, 6.0f);
            p.startNewSubPath (-6.0f, 0.0f);  p.lineTo (6.0f, 0.0f);
            p.startNewSubPath (-3.0f, -3.0f); p.lineTo (-6.0f, 0.0f); p.lineTo (-3.0f, 3.0f);
            p.startNewSubPath (3.0f, -3.0f);  p.lineTo (6.0f, 0.0f);  p.lineTo (3.0f, 3.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Snap:
        {
            juce::Path p;
            p.startNewSubPath (-9.0f, -3.0f); p.lineTo (9.0f, -3.0f);
            p.startNewSubPath (-9.0f, 3.0f);  p.lineTo (9.0f, 3.0f);
            p.startNewSubPath (-3.0f, -9.0f); p.lineTo (-3.0f, 9.0f);
            p.startNewSubPath (3.0f, -9.0f);  p.lineTo (3.0f, 9.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Loop:
        {
            juce::Path p;
            p.addCentredArc (0.0f, 0.0f, 7.0f, 7.0f, 0.0f, 1.816f, 7.449f, true);
            p.startNewSubPath (8.0f, -9.0f); p.lineTo (8.0f, -3.0f); p.lineTo (2.0f, -3.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Sync:
        {
            juce::Path p;
            p.addRoundedRectangle (-10.0f, -4.0f, 11.0f, 8.0f, 4.0f);
            p.addRoundedRectangle (-1.0f, -4.0f, 11.0f, 8.0f, 4.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Tempo:
        {
            juce::Path p;
            p.startNewSubPath (-5.0f, 9.0f); p.lineTo (-3.0f, -8.0f); p.lineTo (3.0f, -8.0f); p.lineTo (5.0f, 9.0f); p.closeSubPath();
            p.startNewSubPath (0.0f, 4.0f);  p.lineTo (5.0f, -6.0f);
            stroke (p, 1.7f);
            break;
        }
        case Symbol::Thru:
        {
            juce::Path p;
            p.startNewSubPath (-10.0f, 0.0f); p.lineTo (-4.0f, 0.0f);
            p.startNewSubPath (4.0f, 0.0f);   p.lineTo (10.0f, 0.0f);
            p.startNewSubPath (7.0f, -3.0f);  p.lineTo (10.0f, 0.0f); p.lineTo (7.0f, 3.0f);
            p.addRoundedRectangle (-4.0f, -5.0f, 8.0f, 10.0f, 2.0f);
            stroke (p, 1.7f);
            break;
        }
    }
}

// ---------------------------------------------------------------- εμφάνιση
LooperLookAndFeel::LooperLookAndFeel()
{
    setColour (juce::ComboBox::textColourId, kText);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff38322e));
    setColour (juce::PopupMenu::textColourId, kText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kSage);
    setColour (juce::PopupMenu::highlightedTextColourId, kPanel);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff38322e));
    setColour (juce::TooltipWindow::textColourId, kText);
    setColour (juce::TooltipWindow::outlineColourId, juce::Colour (0xff50473f));
}

void LooperLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& colour,
                                              bool isHighlighted, bool isDown)
{
    if (button.getComponentID() != "footswitch")
    {
        LookAndFeel_V4::drawButtonBackground (g, button, colour, isHighlighted, isDown);
        return;
    }

    const auto area = button.getLocalBounds().toFloat().reduced (4.0f);
    const float d = juce::jmin (area.getWidth(), area.getHeight());
    auto circle = juce::Rectangle<float> (d, d).withCentre (area.getCentre());
    if (isDown) circle = circle.translated (0.0f, 1.5f);

    juce::Path p;
    p.addEllipse (circle);
    juce::DropShadow (juce::Colour (0xff504030).withAlpha (isDown ? 0.14f : 0.22f), isDown ? 3 : 8,
                      { 0, isDown ? 1 : 3 }).drawForPath (g, p);

    g.setColour (juce::Colour (isDown ? 0xfff1ece5 : 0xffffffff));
    g.fillEllipse (circle);

    // φωτισμός μέσα στο κουμπί
    const auto glowArgb = (juce::uint32) (int) button.getProperties().getWithDefault ("glow", 0);
    if (glowArgb != 0)
    {
        const auto glow = juce::Colour (glowArgb);
        const auto c = circle.getCentre();
        juce::ColourGradient grad (glow.withAlpha (0.90f), c.x, c.y, juce::Colours::white, c.x + d * 0.5f, c.y, true);
        grad.addColour (0.55, glow.withAlpha (0.50f));
        g.setGradientFill (grad);
        g.fillEllipse (circle);
    }

    g.setColour (juce::Colour (0xffdcd6cf));
    g.drawEllipse (circle.reduced (1.0f), 2.0f);
}

void LooperLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool isHighlighted, bool isDown)
{
    if (button.getComponentID() != "footswitch")
    {
        LookAndFeel_V4::drawButtonText (g, button, isHighlighted, isDown);
        return;
    }

    const int sym = (int) button.getProperties().getWithDefault ("symbol", (int) symNone);
    if (sym == symNone) return;
    auto centre = button.getLocalBounds().toFloat().getCentre();
    if (isDown) centre.y += 1.5f;
    const auto colour = (Symbol) sym == Symbol::PadRecord ? kRecDot : kInk;
    drawSymbol (g, (Symbol) sym, centre, 24.0f, colour);
}

void LooperLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool, bool)
{
    const auto b = button.getLocalBounds().toFloat();
    const float y = b.getCentreY() - 9.0f;
    const juce::Rectangle<float> pill (b.getX(), y, 34.0f, 18.0f);
    const bool on = button.getToggleState();

    g.setColour (on ? kSage : juce::Colour (0xff4a433e));
    g.fillRoundedRectangle (pill, 9.0f);
    g.setColour (on ? kPanel : juce::Colour (0xffa39a92));
    g.fillEllipse (pill.getX() + (on ? 18.0f : 2.0f), pill.getY() + 2.0f, 14.0f, 14.0f);

    const auto id = button.getComponentID();
    const Symbol sym = id == "sync" ? Symbol::Sync : (id == "tempo" ? Symbol::Tempo : Symbol::Thru);
    drawSymbol (g, sym, { pill.getRight() + 20.0f, b.getCentreY() }, 20.0f, on ? kLabel : juce::Colour (0xff8b8179));
}

void LooperLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox&)
{
    const auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    g.setColour (juce::Colour (0xff38322e));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (juce::Colour (0xff50473f));
    g.drawRoundedRectangle (b, 6.0f, 1.0f);

    const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
    juce::Path arrow;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (juce::Colour (0xffa39a92));
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
    statusLabel.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (statusLabel);

    const char* const padTips[3] = { "REC / PLAY / OVERDUB", "UNDO: removes the newest layer", "STOP: tap N times = silence / restore layer N" };
    for (int i = 0; i < LooperProcessor::kNumCommands; ++i)
    {
        auto& b = pads[(size_t) i];
        b.setComponentID ("footswitch");
        b.setTooltip (padTips[i]);
        b.onClick = [this, i] { proc.postCommand (kCmds[i]); };
        addAndMakeVisible (b);
    }
    pads[1].getProperties().set ("symbol", (int) Symbol::Undo);
    pads[2].getProperties().set ("symbol", (int) Symbol::Stop);

    startBox.addItemList ({ "Off", "Beat", "Bar" }, 1);
    quantizeBox.addItemList ({ "Off", "Beat", "Bar" }, 1);
    snapBox.addItemList ({ "Off", "1/8", "1/16" }, 1);
    afterBox.addItemList ({ "Play", "Overdub" }, 1);
    startBox.setTooltip ("Start quantize: recording starts on the next beat / bar (needs host playing)");
    quantizeBox.setTooltip ("Length quantize: loop length snaps to beats / bars");
    snapBox.setTooltip ("Snap notes: the start of each recorded note snaps to the grid");
    afterBox.setTooltip ("After the first loop: play, or go straight into overdub");
    for (auto* c : { &startBox, &quantizeBox, &snapBox, &afterBox })
    {
        c->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (c);
    }

    syncToggle.setComponentID ("sync");
    tempoToggle.setComponentID ("tempo");
    thruToggle.setComponentID ("thru");
    syncToggle.setTooltip ("Sync to host transport");
    tempoToggle.setTooltip ("Follow tempo: the loop follows tempo changes of the host");
    thruToggle.setTooltip ("MIDI thru: pass live notes to the instrument");
    for (auto* t : { &syncToggle, &tempoToggle, &thruToggle }) addAndMakeVisible (t);

    startAtt    = std::make_unique<Combo> (proc.apvts, "startQuantize", startBox);
    quantizeAtt = std::make_unique<Combo> (proc.apvts, "quantize", quantizeBox);
    snapAtt     = std::make_unique<Combo> (proc.apvts, "snap", snapBox);
    afterAtt    = std::make_unique<Combo> (proc.apvts, "afterRecord", afterBox);
    syncAtt     = std::make_unique<Button> (proc.apvts, "syncHost", syncToggle);
    tempoAtt    = std::make_unique<Button> (proc.apvts, "followTempo", tempoToggle);
    thruAtt     = std::make_unique<Button> (proc.apvts, "passThrough", thruToggle);

    startTimerHz (30);
    timerCallback();
}

LooperEditor::~LooperEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void LooperEditor::resized()
{
    statusLabel.setBounds (34, 26, 532, 30);

    startBox.setBounds    (kRowX, kRowY[0], kRowW, kRowH);
    quantizeBox.setBounds (kRowX, kRowY[1], kRowW, kRowH);
    snapBox.setBounds     (kRowX, kRowY[2], kRowW, kRowH);
    afterBox.setBounds    (kRowX, kRowY[3], kRowW, kRowH);

    syncToggle.setBounds  (kToggleX[0], kToggleY, 66, kRowH);
    tempoToggle.setBounds (kToggleX[1], kToggleY, 66, kRowH);
    thruToggle.setBounds  (kToggleX[2], kToggleY, 66, kRowH);

    for (int i = 0; i < 3; ++i)
        pads[(size_t) i].setBounds (kPadX[i], kPadY, kPadSize, kPadSize);
}

void LooperEditor::drawLanes (juce::Graphics& g) const
{
    const auto state = (ml::State) shown.state;
    const double bar = juce::jmax (1.0, shown.barSamples);

    // Άξονας χρόνου (σε samples αναφοράς)
    double span = 4.0 * bar;
    if (state == ml::State::Recording)
        span = bar * juce::jmax (4.0, std::ceil ((double) shown.pos / bar / 4.0) * 4.0);
    else if (shown.length > 0)
        span = (double) juce::jmax (shown.length, state == ml::State::Overdubbing ? shown.pos : (int64_t) 0);
    if (span <= 0.0) span = 4.0 * bar;

    auto xOf = [&] (double t) { return kLaneX + (float) (t / span) * kLaneW; };

    // γραμμές πλέγματος (το πολύ 8)
    double stepBars = 1.0;
    while (span / (stepBars * bar) > 8.5) stepBars *= 2.0;
    g.setColour (kPanelLine);
    for (double t = 0.0; t <= span + 1.0; t += stepBars * bar)
        g.fillRect (xOf (t), 62.0f, 1.0f, 136.0f);

    // μπάρες των layers (επαναλαμβάνονται όσο χρειάζεται για να γεμίσουν το loop)
    const int layers = shown.layers;
    for (int i = 0; i < layers && i < LooperProcessor::kMaxLayers; ++i)
    {
        const int64_t len = shown.layerLen[(size_t) i];
        if (len <= 0) continue;
        const bool muted = (shown.muteMask & (1 << i)) != 0;
        const float y = kLaneTop + kLanePitch * (float) i;
        g.setColour (muted ? kMuted : kLayerColour[i]);
        for (double t0 = 0.0; t0 < span - 0.5; t0 += (double) len)
        {
            const float x1 = xOf (t0), x2 = xOf (juce::jmin (span, t0 + (double) len));
            g.fillRoundedRectangle (x1 + 1.0f, y, juce::jmax (2.0f, x2 - x1 - 2.0f), kLaneH, 5.0f);
        }
        if (! muted)
        {
            g.setColour (kPanel);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (juce::String (i + 1), juce::Rectangle<int> ((int) kLaneX + 8, (int) y, 20, (int) kLaneH),
                        juce::Justification::centredLeft);
        }
    }

    // το layer που ηχογραφείται τώρα
    if (state == ml::State::Recording || state == ml::State::Overdubbing)
    {
        const int idx = state == ml::State::Recording ? 0 : layers;
        if (idx < LooperProcessor::kMaxLayers)
        {
            const double a = state == ml::State::Recording ? 0.0 : (double) shown.odStart;
            const float y = kLaneTop + kLanePitch * (float) idx;
            const float x1 = xOf (a), x2 = xOf ((double) shown.pos);
            if (x2 > x1)
            {
                g.setColour (kCoral.withAlpha (0.35f));
                g.fillRoundedRectangle (x1 - 2.0f, y - 2.0f, x2 - x1 + 4.0f, kLaneH + 4.0f, 7.0f);
                g.setColour (kCoral);
                g.fillRoundedRectangle (x1 + 1.0f, y, juce::jmax (2.0f, x2 - x1 - 2.0f), kLaneH, 5.0f);
            }
            g.setColour (kPanel);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (juce::String (idx + 1), juce::Rectangle<int> ((int) kLaneX + 8 + (int) juce::jmax (0.0f, x1 - kLaneX), (int) y, 20, (int) kLaneH),
                        juce::Justification::centredLeft);
        }
    }

    // γραμμή θέσης
    if (state == ml::State::Playing || state == ml::State::Overdubbing || state == ml::State::Recording)
    {
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (juce::jlimit (kLaneX, kLaneX + kLaneW, xOf ((double) shown.pos)) - 1.0f, 62.0f, 2.0f, 128.0f, 1.0f);
    }
}

void LooperEditor::paint (juce::Graphics& g)
{
    // σώμα
    g.setGradientFill (juce::ColourGradient (kBodyTop, 0.0f, 0.0f, kBodyBottom, 0.0f, (float) getHeight(), false));
    g.fillAll();

    // πάνελ
    g.setColour (juce::Colours::white.withAlpha (0.7f));
    g.fillRoundedRectangle (14.0f, 15.0f, 572.0f, 210.0f, 14.0f);
    g.setColour (kPanel);
    g.fillRoundedRectangle (14.0f, 14.0f, 572.0f, 210.0f, 14.0f);

    // σύμβολα δίπλα στα μενού
    const Symbol rowSymbols[4] = { Symbol::StartQ, Symbol::LengthQ, Symbol::Snap, Symbol::Loop };
    for (int i = 0; i < 4; ++i)
        drawSymbol (g, rowSymbols[i], { 45.0f, (float) kRowY[i] + kRowH * 0.5f }, 20.0f, kLabel);

    drawLanes (g);
}

void LooperEditor::timerCallback()
{
    const auto state = (ml::State) proc.uiState.load();
    const double sr = proc.uiSampleRate.load();

    shown.state = (int) state;
    shown.layers = proc.uiLayers.load();
    shown.muteMask = proc.uiMuteMask.load();
    for (int i = 0; i < LooperProcessor::kMaxLayers; ++i) shown.layerLen[(size_t) i] = proc.uiLayerLen[(size_t) i].load();
    shown.length = proc.uiLength.load();
    shown.pos = proc.uiPos.load();
    shown.odStart = proc.uiOdStart.load();
    shown.barSamples = proc.uiBarSamples.load();

    auto barsText = [&] (int64_t samples)
    {
        const double bars = (double) samples / juce::jmax (1.0, shown.barSamples);
        const double r = std::round (bars);
        const juce::String n = std::fabs (bars - r) < 0.02 ? juce::String ((int) r) : juce::String (bars, 1);
        return n + (std::fabs (bars - 1.0) < 0.02 ? " bar" : " bars");
    };
    juce::ignoreUnused (sr);

    juce::String text;
    switch (state)
    {
        case ml::State::Empty:       text = "EMPTY"; break;
        case ml::State::Recording:   text = "RECORDING  |  " + barsText (shown.pos); break;
        case ml::State::Playing:     text = "PLAYING"; break;
        case ml::State::Overdubbing: text = "OVERDUB"; break;
        case ml::State::Armed:       text = "ARMED  |  waiting for the grid"; break;
    }
    if (shown.length > 0 && state != ml::State::Recording)
        text += "  |  loop " + barsText (shown.length);
    if (proc.uiPaused.load() != 0) text += "  (waiting for host)";
    if (proc.uiPendingTaps.load() > 0) text += "  |  STOP x" + juce::String (proc.uiPendingTaps.load());
    statusLabel.setText (text, juce::dontSendNotification);

    // φωτισμός και σύμβολο του πρώτου πεταλιού
    juce::Colour recGlow;   // διαφανές = σβηστό
    Symbol recSymbol = Symbol::PadRecord;
    switch (state)
    {
        case ml::State::Empty:       recSymbol = Symbol::PadRecord; break;
        case ml::State::Recording:   recGlow = kCoral;  recSymbol = Symbol::Play; break;
        case ml::State::Playing:     recGlow = kSage;   recSymbol = Symbol::PadOverdub; break;
        case ml::State::Overdubbing: recGlow = kPeach;  recSymbol = Symbol::Play; break;
        case ml::State::Armed:       recGlow = kButter; recSymbol = Symbol::Cancel; break;
    }
    auto setPad = [] (juce::TextButton& b, juce::Colour glow)
    {
        const int newGlow = glow.isTransparent() ? 0 : (int) glow.getARGB();
        if ((int) b.getProperties().getWithDefault ("glow", 0) != newGlow)
        {
            b.getProperties().set ("glow", newGlow);
            b.repaint();
        }
    };
    setPad (pads[0], recGlow);
    setPad (pads[1], juce::Colour());
    setPad (pads[2], shown.muteMask != 0 ? kLilac : juce::Colour());
    if ((int) pads[0].getProperties().getWithDefault ("symbol", -1) != (int) recSymbol)
    {
        pads[0].getProperties().set ("symbol", (int) recSymbol);
        pads[0].repaint();
    }

    repaint (juce::Rectangle<int> (14, 14, 572, 210));
}
