#pragma once

#include "PluginProcessor.h"

class LooperEditor : public juce::AudioProcessorEditor,
                     private juce::Timer
{
public:
    explicit LooperEditor (LooperProcessor&);
    ~LooperEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    LooperProcessor& proc;

    juce::Label statusLabel;
    std::array<juce::TextButton, LooperProcessor::kNumCommands> cmdButtons;
    std::array<juce::TextButton, LooperProcessor::kNumCommands> learnButtons;
    std::array<juce::TextButton, LooperProcessor::kNumCommands> clearButtons;
    std::array<juce::Label, LooperProcessor::kNumCommands> mapLabels;

    juce::Label startLabel, quantizeLabel, bpbLabel, bpmLabel, afterLabel;
    juce::ComboBox startBox, quantizeBox, afterBox;
    juce::Slider bpbSlider, bpmSlider;
    juce::ToggleButton syncToggle, thruToggle, consumeToggle;

    using Combo  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using Slide  = juce::AudioProcessorValueTreeState::SliderAttachment;
    using Button = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<Combo> startAtt, quantizeAtt, afterAtt;
    std::unique_ptr<Slide> bpbAtt, bpmAtt;
    std::unique_ptr<Button> syncAtt, thruAtt, consumeAtt;

    juce::Rectangle<int> progressArea;
    float progress = 0.0f;
    int shownState = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperEditor)
};
