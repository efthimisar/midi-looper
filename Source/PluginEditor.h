#pragma once

#include "PluginProcessor.h"

// Σύμβολα που ζωγραφίζονται με διανύσματα (χωρίς εικόνες ή γραμματοσειρές εικονιδίων).
enum class Symbol { PadRecord, PadOverdub, Play, Stop, Cancel, Undo,
                    StartQ, LengthQ, Snap, Loop, Sync, Tempo, Thru };

void drawSymbol (juce::Graphics&, Symbol, juce::Point<float> centre, float size, juce::Colour);

// Εμφάνιση: κρεμ σώμα, σκούρο πάνελ, τρία λευκά πετάλια με φωτισμό μέσα τους, παστέλ χρώματα.
class LooperLookAndFeel : public juce::LookAndFeel_V4
{
public:
    LooperLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool isHighlighted, bool isDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isHighlighted, bool isDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isHighlighted, bool isDown) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
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
    void timerCallback() override;
    void drawLanes (juce::Graphics&) const;

    LooperLookAndFeel laf;   // πρέπει να δηλωθεί πρώτο: καταστρέφεται τελευταίο
    LooperProcessor& proc;
    juce::TooltipWindow tooltip { this, 700 };

    juce::Label statusLabel;
    std::array<juce::TextButton, LooperProcessor::kNumCommands> pads;   // REC, UNDO, STOP

    juce::ComboBox startBox, quantizeBox, snapBox, afterBox;
    juce::ToggleButton syncToggle, tempoToggle, thruToggle;

    using Combo  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using Button = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<Combo> startAtt, quantizeAtt, snapAtt, afterAtt;
    std::unique_ptr<Button> syncAtt, tempoAtt, thruAtt;

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
