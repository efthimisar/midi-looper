#pragma once

#include "PluginProcessor.h"

// Σύμβολα που ζωγραφίζονται με διανύσματα (χωρίς εικόνες ή γραμματοσειρές εικονιδίων).
enum class Symbol { PadRecord, PadOverdub, Play, Stop, Cancel, Undo,
                    StartQ, LengthQ, Snap, Loop, Sync, Tempo, Thru };

void drawSymbol (juce::Graphics&, Symbol, juce::Point<float> centre, float size, juce::Colour);

// Εμφάνιση: κρεμ σώμα, σκούρα οθόνη, τετράγωνα κουμπιά, τρία επίπεδα στρογγυλά πετάλια με φωτεινό δαχτυλίδι.
class LooperLookAndFeel : public juce::LookAndFeel_V4
{
public:
    LooperLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool isHighlighted, bool isDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isHighlighted, bool isDown) override;
};

// Τετράγωνο κουμπί χωρίς μενού: κάθε πάτημα προχωρά στην επόμενη τιμή και μετά την τελευταία γυρνά στην πρώτη.
class ValueTile : public juce::Button
{
public:
    ValueTile (Symbol s, juce::Typeface::Ptr face) : juce::Button ({}), symbol (s), typeface (face) {}

    void setValues (const juce::StringArray& v, bool onOff) { labels = v; isOnOff = onOff; repaint(); }
    void setIndex (int i) { if (i != index) { index = i; repaint(); } }
    int getIndex() const { return index; }
    int getNumValues() const { return labels.size(); }

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    Symbol symbol;
    juce::Typeface::Ptr typeface;
    juce::StringArray labels;
    bool isOnOff = false;
    int index = 0;
};

class LooperEditor : public juce::AudioProcessorEditor,
                     private juce::Timer
{
public:
    explicit LooperEditor (LooperProcessor&);
    ~LooperEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int kNumTiles = 7;

    void timerCallback() override;
    void drawLanes (juce::Graphics&) const;
    void cycleTile (int i);
    juce::Font font (float height) const;

    LooperLookAndFeel laf;   // πρέπει να δηλωθεί πρώτο: καταστρέφεται τελευταίο
    LooperProcessor& proc;
    juce::TooltipWindow tooltip { this, 700 };
    juce::Typeface::Ptr typeface;

    juce::Label statusLabel;
    std::array<juce::TextButton, LooperProcessor::kNumCommands> pads;   // REC, UNDO, STOP

    std::array<std::unique_ptr<ValueTile>, kNumTiles> tiles;
    std::array<juce::RangedAudioParameter*, kNumTiles> tileParams {};

    // animation
    int lastUndoCount = 0, lastStopCount = 0;
    double undoFlashStart = -1000.0, stopFlashStart = -1000.0;

    // στιγμιότυπο της κατάστασης για ζωγράφισμα
    struct Shown
    {
        int state = 0, layers = 0, muteMask = 0;
        std::array<int64_t, LooperProcessor::kMaxLayers> layerLen {};
        int64_t length = 0, pos = 0, odStart = 0;
        double barSamples = 96000.0;
    } shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperEditor)
};
