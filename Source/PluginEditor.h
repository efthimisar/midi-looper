#pragma once

#include "PluginProcessor.h"

// Εμφάνιση: ανοιχτό γκρι σώμα, μαύρο πάνελ, τρία μεγάλα πετάλια.
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
    void drawLed (juce::Graphics&, juce::Point<float> centre, juce::Colour colour, bool lit) const;

    enum class Icon { Record, Play, Stop, Overdub, Cancel, Redo, Undo, Clear };
    void drawIcon (juce::Graphics&, Icon, juce::Point<float> centre, float size, juce::Colour) const;

    LooperLookAndFeel laf;   // πρέπει να δηλωθεί πρώτο: καταστρέφεται τελευταίο
    LooperProcessor& proc;

    juce::Label statusLabel;
    std::array<juce::TextButton, LooperProcessor::kNumCommands> cmdButtons;   // REC, STOP, UNDO, REDO, CLEAR (τα UNDO/CLEAR δεν εμφανίζονται)

    juce::Label startLabel, quantizeLabel, afterLabel;
    juce::ComboBox startBox, quantizeBox, afterBox;
    juce::ToggleButton syncToggle, thruToggle;

    using Combo  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using Button = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<Combo> startAtt, quantizeAtt, afterAtt;
    std::unique_ptr<Button> syncAtt, thruAtt;

    juce::Rectangle<int> progressArea;
    float progress = 0.0f;
    int shownState = -1;
    int shownRedo = -1;
    Icon mainIcon = Icon::Record;   // τι κάνει το επόμενο πάτημα του REC

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperEditor)
};
