#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const char* const kTriggerIds[LooperProcessor::kNumCommands]   = { "trgMain", "trgStop", "trgUndo", "trgRedo", "trgClear" };
    const char* const kTriggerNames[LooperProcessor::kNumCommands] = { "Rec/Play/Overdub", "Stop", "Undo", "Redo", "Clear" };
    const ml::Command kCommands[LooperProcessor::kNumCommands]     = { ml::Command::Main, ml::Command::Stop, ml::Command::Undo,
                                                                       ml::Command::Redo, ml::Command::Clear };

    int codeFor (const juce::MidiMessage& m)
    {
        if (m.isController())  return 1000 + m.getControllerNumber();
        if (m.isNoteOn())      return 2000 + m.getNoteNumber();
        return -1;
    }
}

LooperProcessor::LooperProcessor()
    : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)   // σιωπηλή έξοδος για την παραλλαγή "Inst"
                     #endif
                      ),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (auto& m : mapping) m.store (-1);

    pQuantize     = apvts.getRawParameterValue ("quantize");
    pStartQuantize = apvts.getRawParameterValue ("startQuantize");
    pBeatsPerBar  = apvts.getRawParameterValue ("beatsPerBar");
    pFallbackBpm  = apvts.getRawParameterValue ("fallbackBpm");
    pAfterRecord  = apvts.getRawParameterValue ("afterRecord");
    pSyncHost     = apvts.getRawParameterValue ("syncHost");
    pPassThrough  = apvts.getRawParameterValue ("passThrough");
    pConsume      = apvts.getRawParameterValue ("consume");
    for (int i = 0; i < kNumCommands; ++i)
        pTrigger[(size_t) i] = apvts.getRawParameterValue (kTriggerIds[i]);
}

juce::AudioProcessorValueTreeState::ParameterLayout LooperProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "startQuantize", 1 }, "Start Quantize",
                                                   StringArray { "Off", "Beat", "Bar" }, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "quantize", 1 }, "Length Quantize",
                                                   StringArray { "Off", "Beat", "Bar" }, 0));
    l.add (std::make_unique<AudioParameterInt> (ParameterID { "beatsPerBar", 1 }, "Beats per Bar", 1, 16, 4));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "fallbackBpm", 1 }, "Fallback BPM",
                                                  NormalisableRange<float> (20.0f, 300.0f, 0.1f), 120.0f));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "afterRecord", 1 }, "After First Loop",
                                                   StringArray { "Play", "Overdub" }, 0));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "syncHost", 1 }, "Sync to Host Transport", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "passThrough", 1 }, "MIDI Thru", true));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "consume", 1 }, "Swallow Mapped Controls", true));

    // Κουμπιά αυτοματισμού: κάθε μετάβαση 0 → 1 εκτελεί την εντολή.
    for (int i = 0; i < kNumCommands; ++i)
        l.add (std::make_unique<AudioParameterBool> (ParameterID { kTriggerIds[i], 1 },
                                                     String ("Trigger: ") + kTriggerNames[i], false));
    return l;
}

void LooperProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    uiSampleRate.store (sampleRate);
    inEvents.clear();   inEvents.reserve (16384);
    outEvents.clear();  outEvents.reserve (32768);
    outBuffer.ensureSize (1 << 16);
    lastTrigger.fill (false);
    cmdFifo.reset();
}

bool LooperProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
   #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
   #else
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
   #endif
}

void LooperProcessor::postCommand (ml::Command c)
{
    const auto scope = cmdFifo.write (1);
    if (scope.blockSize1 > 0)      cmdSlots[(size_t) scope.startIndex1] = (int) c;
    else if (scope.blockSize2 > 0) cmdSlots[(size_t) scope.startIndex2] = (int) c;
}

juce::String LooperProcessor::describeMapping (int i) const
{
    const int code = mapping[(size_t) i].load();
    if (code < 0)      return "-";
    if (code >= 2000)  return "Note " + juce::String (code - 2000);
    return "CC " + juce::String (code - 1000);
}

void LooperProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();

    const int numSamples = audio.getNumSamples();

    // ---- πληροφορίες transport ----
    ml::BlockInfo info;
    info.numSamples = numSamples;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            info.hostPlaying = pos->getIsPlaying();
            if (auto t = pos->getTimeInSamples()) { info.hostSample = *t; info.hostPositionValid = true; }
            if (auto b = pos->getBpm())           info.bpm = *b;
            if (auto ts = pos->getTimeSignature())
                if (ts->denominator > 0)
                    info.barLengthQN = (double) ts->numerator * 4.0 / (double) ts->denominator;
            if (auto q = pos->getPpqPosition())               { info.ppq = *q; info.ppqValid = true; }
            if (auto bs = pos->getPpqPositionOfLastBarStart()) { info.barStartPpq = *bs; info.barStartValid = true; }
        }

    ml::Settings st;
    st.quantize     = (ml::Quantize) juce::jlimit (0, 2, (int) pQuantize->load());
    st.startQuantize = (ml::Quantize) juce::jlimit (0, 2, (int) pStartQuantize->load());
    st.beatsPerBar  = (int) pBeatsPerBar->load();
    st.fallbackBpm  = pFallbackBpm->load();
    st.afterRecord  = (ml::AfterRecord) juce::jlimit (0, 1, (int) pAfterRecord->load());
    st.syncToHost   = pSyncHost->load() >= 0.5f;
    st.passThrough  = pPassThrough->load() >= 0.5f;
    const bool consume = pConsume->load() >= 0.5f;

    inEvents.clear();
    outEvents.clear();
    outBuffer.clear();

    auto pushCommand = [this] (ml::Command c, int offset)
    {
        if (inEvents.size() >= inEvents.capacity()) return;
        ml::InEvent e; e.offset = offset; e.isCommand = true; e.cmd = c;
        inEvents.push_back (e);
    };

    // ---- εντολές από αυτοματισμό παραμέτρων (άνοδος 0 → 1) ----
    for (int i = 0; i < kNumCommands; ++i)
    {
        const bool now = pTrigger[(size_t) i]->load() >= 0.5f;
        if (now && ! lastTrigger[(size_t) i]) pushCommand (kCommands[i], 0);
        lastTrigger[(size_t) i] = now;
    }

    // ---- εντολές από τα κουμπιά της οθόνης ----
    {
        const auto scope = cmdFifo.read (cmdFifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i) pushCommand ((ml::Command) cmdSlots[(size_t) (scope.startIndex1 + i)], 0);
        for (int i = 0; i < scope.blockSize2; ++i) pushCommand ((ml::Command) cmdSlots[(size_t) (scope.startIndex2 + i)], 0);
    }

    // ---- εισερχόμενο MIDI ----
    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        const int sample = juce::jlimit (0, juce::jmax (0, numSamples - 1), metadata.samplePosition);
        const int code = codeFor (msg);

        // MIDI learn
        const int learning = learnTarget.load();
        if (learning >= 0 && code >= 0 && (msg.isNoteOn() || (msg.isController() && msg.getControllerValue() > 0 && msg.getControllerNumber() < 120)))
        {
            for (auto& m : mapping) if (m.load() == code) m.store (-1);   // ένα χειριστήριο = μία εντολή
            mapping[(size_t) learning].store (code);
            learnTarget.store (-1);
            continue;
        }

        // χαρτογραφημένο χειριστήριο;
        bool handled = false;
        if (code >= 0 || msg.isNoteOff())
        {
            const int lookup = msg.isNoteOff() ? 2000 + msg.getNoteNumber() : code;
            for (int i = 0; i < kNumCommands; ++i)
            {
                if (mapping[(size_t) i].load() != lookup) continue;
                const bool pressed = msg.isNoteOn() || (msg.isController() && msg.getControllerValue() >= 64);
                if (pressed) pushCommand (kCommands[i], sample);
                handled = consume;
                break;
            }
        }
        if (handled) continue;

        // sysex και μεγάλα μηνύματα: περνούν απλώς (δεν ηχογραφούνται)
        const int size = msg.getRawDataSize();
        if (size < 1 || size > 3 || msg.isSysEx())
        {
            if (st.passThrough) outBuffer.addEvent (msg, sample);
            continue;
        }

        if (inEvents.size() >= inEvents.capacity()) continue;
        ml::InEvent e;
        e.offset = sample;
        e.msg.size = (uint8_t) size;
        const auto* d = msg.getRawData();
        for (int i = 0; i < size; ++i) e.msg.b[i] = d[i];
        inEvents.push_back (e);
    }

    // ---- μηχανή ----
    engine.process (info, st, inEvents.data(), (int) inEvents.size(), outEvents);

    for (const auto& o : outEvents)
        outBuffer.addEvent (juce::MidiMessage (o.msg.b, (int) o.msg.size, 0.0), o.offset);

    midi.swapWith (outBuffer);

    uiState.store ((int) engine.getState());
    uiLayers.store (engine.getLayerCount());
    uiRedo.store (engine.getRedoCount());
    uiPaused.store (engine.isPausedByHost() ? 1 : 0);
    uiPos.store (engine.getPosition());
    uiLength.store (engine.getLength());
}

juce::AudioProcessorEditor* LooperProcessor::createEditor()
{
    return new LooperEditor (*this);
}

void LooperProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    for (int i = 0; i < kNumCommands; ++i)
        state.setProperty ("map" + juce::String (i), mapping[(size_t) i].load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LooperProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto vt = juce::ValueTree::fromXml (*xml);
            for (int i = 0; i < kNumCommands; ++i)
                mapping[(size_t) i].store ((int) vt.getProperty ("map" + juce::String (i), -1));
            apvts.replaceState (vt);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LooperProcessor();
}
