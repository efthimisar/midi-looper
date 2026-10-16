#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include "LooperEngine.h"

class LooperProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kNumCommands = 5;   // Main, Stop, Undo, Redo, Clear

    LooperProcessor();
    ~LooperProcessor() override = default;

    // --- AudioProcessor ---
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return JucePlugin_IsMidiEffect != 0; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- για το περιβάλλον (message thread) ---
    void postCommand (ml::Command c);                 // κουμπιά της οθόνης
    void beginLearn (int commandIndex) { learnTarget.store (commandIndex); }
    void cancelLearn()                 { learnTarget.store (-1); }
    int  getLearnTarget() const        { return learnTarget.load(); }
    void clearMapping (int commandIndex) { mapping[(size_t) commandIndex].store (-1); }
    juce::String describeMapping (int commandIndex) const;

    juce::AudioProcessorValueTreeState apvts;

    // κατάσταση για εμφάνιση (γράφεται από το audio thread)
    std::atomic<int> uiState { 0 }, uiLayers { 0 }, uiRedo { 0 }, uiPaused { 0 };
    std::atomic<int64_t> uiPos { 0 }, uiLength { 0 };
    std::atomic<double> uiSampleRate { 44100.0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    ml::LooperEngine engine;
    std::vector<ml::InEvent> inEvents;
    std::vector<ml::OutEvent> outEvents;
    juce::MidiBuffer outBuffer;

    // εντολές από το UI → audio thread
    juce::AbstractFifo cmdFifo { 32 };
    std::array<int, 32> cmdSlots {};

    // MIDI learn: κωδικός = είδος*1000 + αριθμός (είδος 1 = CC, 2 = νότα), -1 = τίποτα
    std::array<std::atomic<int>, kNumCommands> mapping;
    std::atomic<int> learnTarget { -1 };

    // παράμετροι
    std::atomic<float>* pQuantize = nullptr;
    std::atomic<float>* pBeatsPerBar = nullptr;
    std::atomic<float>* pFallbackBpm = nullptr;
    std::atomic<float>* pAfterRecord = nullptr;
    std::atomic<float>* pSyncHost = nullptr;
    std::atomic<float>* pPassThrough = nullptr;
    std::atomic<float>* pConsume = nullptr;
    std::array<std::atomic<float>*, kNumCommands> pTrigger {};
    std::array<bool, kNumCommands> lastTrigger {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperProcessor)
};
