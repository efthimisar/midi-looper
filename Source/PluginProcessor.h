#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <vector>
#include "LooperEngine.h"

class LooperProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kNumCommands = 3;   // Rec/Play/Overdub, Undo, Stop (πολλαπλά πατήματα = layer)
    static constexpr int kMaxLayers = ml::LooperEngine::kMaxLayers;

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

    juce::AudioProcessorValueTreeState apvts;

    // κατάσταση για εμφάνιση (γράφεται από το audio thread)
    std::atomic<int> uiState { 0 }, uiLayers { 0 }, uiPaused { 0 }, uiMuteMask { 0 }, uiPendingTaps { 0 }, uiOdArmed { 0 }, uiUndoCount { 0 }, uiStopCount { 0 };
    std::atomic<int64_t> uiPos { 0 }, uiLength { 0 }, uiOdStart { 0 };
    std::array<std::atomic<int64_t>, kMaxLayers> uiLayerLen;
    std::atomic<double> uiBarSamples { 96000.0 };   // μήκος μέτρου σε samples αναφοράς
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

    // αποθήκευση των layers στο project: το audio thread γράφει snapshot όταν κάτι αλλάξει,
    // και παραλαμβάνει φορτώσεις που ζητά το message thread (χωρίς αναμονή σε κανένα από τα δύο)
    juce::SpinLock snapLock;
    std::vector<uint8_t> snapshot;
    size_t snapshotSize = 0;
    bool snapshotPending = false;           // μόνο audio thread
    juce::SpinLock loadLock;
    std::vector<uint8_t> loadBuf;
    size_t loadSize = 0;
    std::atomic<bool> loadPending { false };
    std::vector<uint8_t> restoreBuf;        // τελευταία κατάσταση που φορτώθηκε από το project (message thread)
    size_t restoreSize = 0;

    // παράμετροι
    std::atomic<float>* pQuantize = nullptr;
    std::atomic<float>* pStartQuantize = nullptr;
    std::atomic<float>* pBeatsPerBar = nullptr;
    std::atomic<float>* pFallbackBpm = nullptr;
    std::atomic<float>* pAfterRecord = nullptr;
    std::atomic<float>* pSyncHost = nullptr;
    std::atomic<float>* pFollowTempo = nullptr;
    std::atomic<float>* pSnap = nullptr;
    std::atomic<float>* pPassThrough = nullptr;
    std::array<std::atomic<float>*, kNumCommands> pTrigger {};
    std::array<bool, kNumCommands> lastTrigger {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperProcessor)
};
