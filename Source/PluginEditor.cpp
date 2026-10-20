#include "PluginEditor.h"
#include "BinaryData.h"
#include <cmath>

namespace
{
    // Σειρά πεταλιών: REC, UNDO, STOP
    const ml::Command kCmds[LooperProcessor::kNumCommands] = { ml::Command::Main, ml::Command::Undo, ml::Command::Stop };

    // Θέσεις (λογικά pixel) — τετράγωνο παράθυρο 480 x 480
    constexpr int kSize = 480;
    constexpr int kPadCentreX[3] = { 90, 240, 390 };
    constexpr int kPadCentreY = 402, kPadBox = 100;
    constexpr int kTileX[7] = { 34, 94, 154, 214, 274, 334, 394 };
    constexpr int kTileY = 267, kTileSize = 52;

    constexpr float kLaneX = 30.0f, kLaneW = 420.0f, kLaneTop = 84.0f, kLaneH = 30.0f, kLanePitch = 40.0f;
    constexpr float kGridTop = 80.0f, kGridH = 156.0f;

    // χρώματα (θερμά παστέλ)
    const juce::Colour kBodyTop     { 0xfff6f1ea };
    const juce::Colour kBodyBottom  { 0xffe8e0d6 };
    const juce::Colour kScreen      { 0xff262220 };
    const juce::Colour kStripScreen { 0xff3a3531 };
    const juce::Colour kPanel       { 0xff2b2724 };
    const juce::Colour kPanelLine   { 0xff3a3430 };
    const juce::Colour kText        { 0xfff3f4f6 };
    const juce::Colour kInk         { 0xff4a4038 };
    const juce::Colour kIconGrey    { 0xffb7bbc6 };
    const juce::Colour kOn          { 0xff7fcf98 };
    const juce::Colour kOff         { 0xfff0766f };
    const juce::Colour kSage        { 0xff9ad4a8 };
    const juce::Colour kLilac       { 0xffc4aef0 };
    const juce::Colour kPeach       { 0xfff5b88a };
    const juce::Colour kCoral       { 0xffff8f84 };
    const juce::Colour kRed         { 0xfff26b6b };
    const juce::Colour kButter      { 0xfff5de8a };
    const juce::Colour kBlue        { 0xff8fc8f0 };
    const juce::Colour kRingOff     { 0xffddd4c8 };
    const juce::Colour kMuted       { 0xff3a3d46 };

    const juce::Colour kLayerColour[4] = { kSage, kLilac, juce::Colour (0xfff5de8a), juce::Colour (0xfff4a7b9) };

    enum PadSymbol { symNone = -1 };

    juce::Colour mix (juce::Colour a, juce::Colour b, float t)   // t = 0 → a, t = 1 → b
    {
        return a.interpolatedWith (b, juce::jlimit (0.0f, 1.0f, t));
    }

    // 1 στην αρχή του παλμού, μαλακή πτώση, χαμηλά στο υπόλοιπο
    float pulse (double phase)
    {
        const double p = phase - std::floor (phase);
        return (float) (0.5 + 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * p));
    }
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
            fill (dot, col);
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

    // επίπεδο πετάλι: φωτεινός δακτύλιος 92 px και λευκό πρόσωπο 80 px, χωρίς σκιές
    const auto centre = button.getLocalBounds().toFloat().getCentre();
    const auto ringArgb = (juce::uint32) (int) button.getProperties().getWithDefault ("ring", (int) kRingOff.getARGB());

    g.setColour (juce::Colour (ringArgb));
    g.fillEllipse (juce::Rectangle<float> (92.0f, 92.0f).withCentre (centre));
    g.setColour (juce::Colour (isDown ? 0xfff1ece5 : 0xfffbf9f5));
    g.fillEllipse (juce::Rectangle<float> (80.0f, 80.0f).withCentre (centre));
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
    drawSymbol (g, (Symbol) sym, button.getLocalBounds().toFloat().getCentre(), 21.0f, kInk);
}

// ---------------------------------------------------------- τετράγωνο κουμπί τιμής
void ValueTile::paintButton (juce::Graphics& g, bool, bool isDown)
{
    const float dy = isDown ? 1.0f : 0.0f;
    const auto r = getLocalBounds().toFloat().reduced (0.5f);

    g.setGradientFill (juce::ColourGradient (juce::Colour (isDown ? 0xff3d3733 : 0xff46403b), 0.0f, r.getY(),
                                             juce::Colour (isDown ? 0xff24211f : 0xff2a2623), 0.0f, r.getBottom(), false));
    g.fillRoundedRectangle (r, 9.0f);
    g.setColour (juce::Colour (0xff1c1917));
    g.drawRoundedRectangle (r, 9.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (isDown ? 0.08f : 0.18f));
    g.drawLine (r.getX() + 7.0f, r.getY() + 1.5f, r.getRight() - 7.0f, r.getY() + 1.5f, 1.0f);

    juce::Colour iconColour = kIconGrey;
    if (isOnOff) iconColour = index == 1 ? kOn : kOff;
    drawSymbol (g, symbol, { r.getCentreX(), 17.0f + dy }, 22.0f, iconColour);

    const juce::Font f (typeface != nullptr ? juce::FontOptions (typeface).withHeight (14.0f)
                                            : juce::FontOptions (14.0f, juce::Font::bold));
    g.setFont (f);
    g.setColour (juce::Colour (0xffeceef2));
    g.drawFittedText (labels[juce::jlimit (0, juce::jmax (0, labels.size() - 1), index)],
                      juce::Rectangle<int> (1, 32 + (int) dy, getWidth() - 2, 14), juce::Justification::centred, 1, 0.7f);
}

// ------------------------------------------------------------------ παράθυρο
LooperEditor::LooperEditor (LooperProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    typeface = juce::Typeface::createSystemTypefaceFor (BinaryData::CarlitoBold_ttf, (size_t) BinaryData::CarlitoBold_ttfSize);

    setLookAndFeel (&laf);
    setSize (kSize, kSize);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (font (19.0f));
    statusLabel.setColour (juce::Label::textColourId, kText);
    statusLabel.setInterceptsMouseClicks (false, false);
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

    struct Spec { const char* id; Symbol sym; juce::StringArray values; bool onOff; const char* tip; };
    const Spec specs[kNumTiles] =
    {
        { "startQuantize", Symbol::StartQ,  { "Off", "Beat", "Bar" },  false, "Start quantize: recording starts on the next beat / bar (needs host playing). Click to change." },
        { "quantize",      Symbol::LengthQ, { "Off", "Beat", "Bar" },  false, "Length quantize: loop length snaps to beats / bars. Click to change." },
        { "snap",          Symbol::Snap,    { "Off", "1/8", "1/16" },  false, "Snap notes: the start of each recorded note snaps to the grid. Click to change." },
        { "afterRecord",   Symbol::Loop,    { "Play", "Overdub" },     false, "After the first loop: play, or go straight into overdub. Click to change." },
        { "syncHost",      Symbol::Sync,    { "Off", "On" },           true,  "Sync to host transport. Click to switch On / Off." },
        { "followTempo",   Symbol::Tempo,   { "Off", "On" },           true,  "Follow tempo: the loop follows tempo changes of the host. Click to switch On / Off." },
        { "passThrough",   Symbol::Thru,    { "Off", "On" },           true,  "MIDI thru: pass live notes to the instrument. Click to switch On / Off." }
    };
    for (int i = 0; i < kNumTiles; ++i)
    {
        auto t = std::make_unique<ValueTile> (specs[i].sym, typeface);
        t->setValues (specs[i].values, specs[i].onOff);
        t->setTooltip (specs[i].tip);
        t->onClick = [this, i] { cycleTile (i); };
        addAndMakeVisible (*t);
        tiles[(size_t) i] = std::move (t);
        tileParams[(size_t) i] = proc.apvts.getParameter (specs[i].id);
    }

    lastUndoCount = proc.uiUndoCount.load();
    lastStopCount = proc.uiStopCount.load();

    startTimerHz (30);
    timerCallback();
}

LooperEditor::~LooperEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

juce::Font LooperEditor::font (float height) const
{
    if (typeface != nullptr) return juce::Font (juce::FontOptions (typeface).withHeight (height));
    return juce::Font (juce::FontOptions (height, juce::Font::bold));
}

void LooperEditor::cycleTile (int i)
{
    auto* prm = tileParams[(size_t) i];
    auto& tile = *tiles[(size_t) i];
    if (prm == nullptr || tile.getNumValues() < 2) return;

    const int next = (tile.getIndex() + 1) % tile.getNumValues();   // μετά την τελευταία τιμή ξανά η πρώτη
    prm->beginChangeGesture();
    prm->setValueNotifyingHost (prm->convertTo0to1 ((float) next));
    prm->endChangeGesture();
    tile.setIndex (next);
}

void LooperEditor::resized()
{
    statusLabel.setBounds (30, 44, 420, 28);

    for (int i = 0; i < kNumTiles; ++i)
        tiles[(size_t) i]->setBounds (kTileX[i], kTileY, kTileSize, kTileSize);

    for (int i = 0; i < 3; ++i)
        pads[(size_t) i].setBounds (kPadCentreX[i] - kPadBox / 2, kPadCentreY - kPadBox / 2, kPadBox, kPadBox);
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
        g.fillRect (xOf (t), kGridTop, 1.0f, kGridH);

    // μπάρες των layers (επαναλαμβάνονται όσο χρειάζεται για να γεμίσουν το loop)
    const int layers = shown.layers;
    g.setFont (font (12.0f));
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
            g.fillRoundedRectangle (x1 + 1.0f, y, juce::jmax (2.0f, x2 - x1 - 2.0f), kLaneH, 6.0f);
        }
        if (! muted)
        {
            g.setColour (kPanel);
            g.drawText (juce::String (i + 1), juce::Rectangle<int> (34, (int) y, 18, (int) kLaneH), juce::Justification::centred);
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
                g.setColour (juce::Colour (0xffff4a4f).withAlpha (0.28f));
                g.fillRoundedRectangle (x1 - 3.0f, y - 3.0f, x2 - x1 + 6.0f, kLaneH + 6.0f, 9.0f);
                g.setColour (kCoral);
                g.fillRoundedRectangle (x1 + 1.0f, y, juce::jmax (2.0f, x2 - x1 - 2.0f), kLaneH, 6.0f);
            }
            g.setColour (kPanel);
            g.setFont (font (12.0f));
            g.drawText (juce::String (idx + 1), juce::Rectangle<int> (34 + (int) juce::jmax (0.0f, x1 - kLaneX), (int) y, 18, (int) kLaneH),
                        juce::Justification::centred);
        }
    }

    // γραμμή θέσης
    if (state == ml::State::Playing || state == ml::State::Overdubbing || state == ml::State::Recording)
    {
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (juce::jlimit (kLaneX, kLaneX + kLaneW, xOf ((double) shown.pos)) - 1.0f, kGridTop, 2.0f, 152.0f, 1.0f);
    }
}

namespace
{
    // ανάγλυφο πλαίσιο με βαθουλωμένη οθόνη μέσα του
    void drawBezel (juce::Graphics& g, juce::Rectangle<float> bezel, juce::Rectangle<float> screen,
                    juce::Colour screenColour, float innerShadow)
    {
        juce::Path bp;
        bp.addRoundedRectangle (bezel, 17.0f);
        juce::DropShadow (juce::Colour (0xff503c28).withAlpha (0.28f), 6, { 0, 3 }).drawForPath (g, bp);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffbf8f3), bezel.getX(), bezel.getY(),
                                                 juce::Colour (0xffcfc3b4), bezel.getRight(), bezel.getBottom(), false));
        g.fillPath (bp);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawLine (bezel.getX() + 12.0f, bezel.getY() + 0.5f, bezel.getRight() - 12.0f, bezel.getY() + 0.5f, 1.0f);

        // λευκή άκρη κάτω από την οθόνη, μετά η οθόνη και η εσωτερική σκιά της
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.fillRoundedRectangle (screen.translated (0.0f, 1.0f), 12.0f);
        g.setColour (screenColour);
        g.fillRoundedRectangle (screen, 12.0f);

        juce::Path sp;
        sp.addRoundedRectangle (screen, 12.0f);
        g.saveState();
        g.reduceClipRegion (sp);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.8f), 0.0f, screen.getY(),
                                                 juce::Colours::transparentBlack, 0.0f, screen.getY() + innerShadow, false));
        g.fillRect (screen);
        g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, 0.0f, screen.getBottom() - 6.0f,
                                                 juce::Colours::white.withAlpha (0.08f), 0.0f, screen.getBottom(), false));
        g.fillRect (screen);
        g.restoreState();
    }
}

void LooperEditor::paint (juce::Graphics& g)
{
    // σώμα
    g.setGradientFill (juce::ColourGradient (kBodyTop, 0.0f, 0.0f, kBodyBottom, 0.0f, (float) getHeight(), false));
    g.fillAll();

    // π/- πάνω στο κέντρο (ανάγλυφο, πολύ ελαφριά σκιά)
    {
        const auto f = font (15.0f);
        const juce::String chars[3] = { juce::String (juce::CharPointer_UTF8 ("\xcf\x80")), "/", "-" };
        const float gap = 6.0f;
        float w[3], total = gap * 2.0f;
        for (int i = 0; i < 3; ++i) { { juce::GlyphArrangement ga; ga.addLineOfText (f, chars[i], 0.0f, 0.0f); w[i] = ga.getBoundingBox (0, -1, true).getWidth(); } total += w[i]; }
        g.setFont (f);
        for (int pass = 0; pass < 3; ++pass)
        {
            const float off = pass == 0 ? -1.0f : 1.0f;
            float x = ((float) kSize - total) * 0.5f;
            for (int i = 0; i < 3; ++i)
            {
                const juce::Rectangle<float> r (x + (pass == 2 ? 0.0f : off), 9.0f + (pass == 2 ? 0.0f : off), w[i] + 2.0f, 16.0f);
                g.setColour (pass == 0 ? juce::Colours::white.withAlpha (0.75f)
                                       : pass == 1 ? juce::Colour (0xffddd3c6) : juce::Colour (0xfff1ebe2));
                g.drawText (chars[i], r, juce::Justification::centredLeft, false);
                x += w[i] + gap;
            }
        }
    }

    // οθόνη με τις μπάρες
    drawBezel (g, { 11.0f, 33.0f, 458.0f, 218.0f }, { 16.0f, 38.0f, 448.0f, 208.0f }, kScreen, 18.0f);
    drawLanes (g);

    // λωρίδα με τα τετράγωνα κουμπιά
    drawBezel (g, { 11.0f, 257.0f, 458.0f, 72.0f }, { 16.0f, 262.0f, 448.0f, 62.0f }, kStripScreen, 15.0f);
    for (int i = 0; i < kNumTiles; ++i)
    {
        juce::Path tp;
        tp.addRoundedRectangle (juce::Rectangle<float> ((float) kTileX[i], (float) kTileY, (float) kTileSize, (float) kTileSize), 9.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 4, { 0, 3 }).drawForPath (g, tp);
    }
}

void LooperEditor::timerCallback()
{
    auto state = (ml::State) proc.uiState.load();
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

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

    if (state == ml::State::Playing && proc.uiOdArmed.load() != 0) state = ml::State::Armed;

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

    // τιμές των τετράγωνων κουμπιών (ακολουθούν την παράμετρο, π.χ. από αυτοματισμό ή φόρτωση project)
    for (int i = 0; i < kNumTiles; ++i)
    {
        auto* prm = tileParams[(size_t) i];
        if (prm == nullptr) continue;
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (prm)) tiles[(size_t) i]->setIndex (c->getIndex());
        else if (auto* b = dynamic_cast<juce::AudioParameterBool*> (prm)) tiles[(size_t) i]->setIndex (b->get() ? 1 : 0);
    }

    // φάση του αναβοσβησίματος: στο beat του host όταν παίζει, αλλιώς ρολόι ~1 Hz
    const bool hostPlaying = proc.uiPaused.load() == 0
                          && (state == ml::State::Recording || state == ml::State::Playing
                              || state == ml::State::Overdubbing || state == ml::State::Armed);
    const float bpbRaw = proc.apvts.getRawParameterValue ("beatsPerBar")->load();
    const double beatLen = shown.barSamples / juce::jmax (1.0f, bpbRaw);
    const double phase = hostPlaying && beatLen > 1.0 ? (double) shown.pos / beatLen : now;

    // πρώτο πετάλι: REC
    Symbol recSymbol = Symbol::PadRecord;
    juce::Colour recRing = kRingOff;
    switch (state)
    {
        case ml::State::Empty:       recSymbol = Symbol::PadRecord; break;
        case ml::State::Recording:   recSymbol = Symbol::Play;       recRing = mix (kRingOff, kRed,    pulse (phase));       break;
        case ml::State::Playing:     recSymbol = Symbol::PadOverdub; recRing = kSage;                                        break;
        case ml::State::Overdubbing: recSymbol = Symbol::Play;       recRing = mix (kRingOff, kPeach,  pulse (phase));       break;
        case ml::State::Armed:       recSymbol = Symbol::Cancel;     recRing = mix (kRingOff, kButter, pulse (phase * 2.0)); break;
    }

    // UNDO: γαλάζιος παλμός ~0.25 s για κάθε undo
    const int undoNow = proc.uiUndoCount.load();
    if (undoNow != lastUndoCount) { lastUndoCount = undoNow; undoFlashStart = now; }
    const float undoLevel = juce::jlimit (0.0f, 1.0f, 1.0f - (float) ((now - undoFlashStart) / 0.25));

    // STOP: λιλά αργό αναβόσβημα όσο υπάρχει σιγασμένο layer + γρήγορος παλμός σε κάθε πάτημα
    const int stopNow = proc.uiStopCount.load();
    if (stopNow != lastStopCount) { lastStopCount = stopNow; stopFlashStart = now; }
    const float stopFlash = juce::jlimit (0.0f, 1.0f, 1.0f - (float) ((now - stopFlashStart) / 0.25));
    const float stopSlow = shown.muteMask != 0 ? 0.25f + 0.75f * pulse (now * 0.5) : 0.0f;

    auto setPad = [] (juce::TextButton& b, juce::Colour ring)
    {
        const int argb = (int) ring.getARGB();
        if ((int) b.getProperties().getWithDefault ("ring", 0) != argb)
        {
            b.getProperties().set ("ring", argb);
            b.repaint();
        }
    };
    setPad (pads[0], recRing);
    setPad (pads[1], mix (kRingOff, kBlue, undoLevel));
    setPad (pads[2], mix (kRingOff, kLilac, juce::jmax (stopSlow, stopFlash)));
    if ((int) pads[0].getProperties().getWithDefault ("symbol", -1) != (int) recSymbol)
    {
        pads[0].getProperties().set ("symbol", (int) recSymbol);
        pads[0].repaint();
    }

    repaint (juce::Rectangle<int> (11, 33, 458, 218));
}
