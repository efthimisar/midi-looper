#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Η σειρά ταιριάζει με τα τρία πετάλια: REC, UNDO, STOP.
    const char* const kTriggerIds[LooperProcessor::kNumCommands]   = { "trgMain", "trgUndo", "trgStop" };
    const char* const kTriggerNames[LooperProcessor::kNumCommands] = { "Rec/Play/Overdub", "Undo", "Stop" };
    const ml::Command kCommands[LooperProcessor::kNumCommands]     = { ml::Command::Main, ml::Command::Undo, ml::Command::Stop };
}

LooperProcessor::LooperProcessor()
    : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)   // σιωπηλή έξοδος για την παραλλαγή "Inst"
                     #endif
                      ),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pQuantize      = apvts.getRawParameterValue ("quantize");
    pStartQuantize = apvts.getRawParameterValue ("startQuantize");
    pBeatsPerBar   = apvts.getRawParameterValue ("beatsPerBar");
    pFallbackBpm   = apvts.getRawParameterValue ("fallbackBpm");
    pAfterRecord   = apvts.getRawParameterValue ("afterRecord");
    pSyncHost      = apvts.getRawParameterValue ("syncHost");
    pFollowTempo   = apvts.getRawParameterValue ("followTempo");
    pSnap          = apvts.getRawParameterValue ("snap");
    pPassThrough   = apvts.getRawParameterValue ("passThrough");
    for (int i = 0; i < kNumCommands; ++i)
        pTrigger[(size_t) i] = apvts.getRawParameterValue (kTriggerIds[i]);

    for (auto& l : uiLayerLen) l.store (0);
    snapshot.assign (ml::LooperEngine::kMaxStateBytes, 0);
    loadBuf.assign (ml::LooperEngine::kMaxStateBytes, 0);
    restoreBuf.assign (ml::LooperEngine::kMaxStateBytes, 0);
}

juce::AudioProcessorValueTreeState::ParameterLayout LooperProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "startQuantize", 1 }, "Start Quantize",
                                                   StringArray { "Off", "Beat", "Bar" }, 1));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "quantize", 1 }, "Length Quantize",
                                                   StringArray { "Off", "Beat", "Bar" }, 2));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "snap", 1 }, "Snap Notes to Grid",
                                                   StringArray { "Off", "1/8", "1/16" }, 2));
    l.add (std::make_unique<AudioParameterInt> (ParameterID { "beatsPerBar", 1 }, "Beats per Bar", 1, 16, 4));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "fallbackBpm", 1 }, "Fallback BPM",
                                                  NormalisableRange<float> (20.0f, 300.0f, 0.1f), 120.0f));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "afterRecord", 1 }, "After First Loop",
                                                   StringArray { "Play", "Overdub" }, 0));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "syncHost", 1 }, "Sync to Host Transport", true));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "followTempo", 1 }, "Follow Tempo", true));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "passThrough", 1 }, "MIDI Thru", true));

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

    // Το engine άδειασε: ξαναφόρτωσε τα layers (πρώτα το τελευταίο snapshot, αλλιώς ό,τι ήρθε από το project).
    {
        const juce::SpinLock::ScopedLockType a (snapLock);
        const juce::SpinLock::ScopedLockType b (loadLock);
        if (snapshotSize > 0)      { std::copy (snapshot.begin(), snapshot.begin() + (std::ptrdiff_t) snapshotSize, loadBuf.begin()); loadSize = snapshotSize; }
        else if (restoreSize > 0)  { std::copy (restoreBuf.begin(), restoreBuf.begin() + (std::ptrdiff_t) restoreSize, loadBuf.begin()); loadSize = restoreSize; }
        else                       loadSize = 0;
        loadPending.store (loadSize > 0);
    }
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
    st.quantize      = (ml::Quantize) juce::jlimit (0, 2, (int) pQuantize->load());
    st.startQuantize = (ml::Quantize) juce::jlimit (0, 2, (int) pStartQuantize->load());
    st.beatsPerBar   = (int) pBeatsPerBar->load();
    st.fallbackBpm   = pFallbackBpm->load();
    st.afterRecord   = (ml::AfterRecord) juce::jlimit (0, 1, (int) pAfterRecord->load());
    st.syncToHost    = pSyncHost->load() >= 0.5f;
    st.followTempo   = pFollowTempo->load() >= 0.5f;
    {
        const int snapIdx = juce::jlimit (0, 2, (int) pSnap->load());
        st.snapDiv = snapIdx == 1 ? 8 : (snapIdx == 2 ? 16 : 0);
    }
    st.passThrough   = pPassThrough->load() >= 0.5f;

    inEvents.clear();
    outEvents.clear();
    outBuffer.clear();

    // ---- φόρτωση layers από το project (αν ζητήθηκε) ----
    if (loadPending.load() && loadLock.tryEnter())
    {
        if (loadSize > 0) engine.loadState (loadBuf.data(), loadSize, outEvents);
        loadPending.store (false);
        loadLock.exit();
    }

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

    // ---- snapshot των layers για το project ----
    if (engine.takeDirty()) snapshotPending = true;
    if (snapshotPending && snapLock.tryEnter())
    {
        snapshotSize = engine.serialize (snapshot.data(), snapshot.size());
        snapLock.exit();
        snapshotPending = false;
    }

    // ---- κατάσταση για το περιβάλλον ----
    uiState.store ((int) engine.getState());
    const int layers = engine.getLayerCount();
    uiLayers.store (layers);
    int mask = 0;
    for (int i = 0; i < kMaxLayers; ++i)
    {
        if (i < layers)
        {
            if (engine.isLayerMuted (i)) mask |= (1 << i);
            uiLayerLen[(size_t) i].store (engine.getLayerLength (i));
        }
        else
            uiLayerLen[(size_t) i].store (0);
    }
    uiMuteMask.store (mask);
    uiPendingTaps.store (engine.getPendingTaps());
    uiPaused.store (engine.isPausedByHost() ? 1 : 0);
    uiPos.store (engine.getPosition());
    uiLength.store (engine.getLength());
    uiOdStart.store (engine.getOverdubStart());
    {
        const double bpm = engine.getRefBpm() > 1.0 ? engine.getRefBpm() : 120.0;
        const double barQN = info.barLengthQN > 0.0 ? info.barLengthQN : (double) juce::jmax (1, st.beatsPerBar);
        uiBarSamples.store (60.0 * getSampleRate() / bpm * barQN);
    }
}

juce::AudioProcessorEditor* LooperProcessor::createEditor()
{
    return new LooperEditor (*this);
}

void LooperProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    {
        const juce::SpinLock::ScopedLockType lock (snapLock);
        if (snapshotSize > 0)
        {
            juce::MemoryBlock mb (snapshot.data(), snapshotSize);
            state.setProperty ("layers", mb.toBase64Encoding(), nullptr);
        }
        else
            state.removeProperty ("layers", nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LooperProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto vt = juce::ValueTree::fromXml (*xml);
            const juce::String layersText = vt.getProperty ("layers").toString();
            vt.removeProperty ("layers", nullptr);
            apvts.replaceState (vt);

            if (layersText.isNotEmpty())
            {
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding (layersText)
                    && mb.getSize() <= ml::LooperEngine::kMaxStateBytes
                    && ml::LooperEngine::validateState (static_cast<const uint8_t*> (mb.getData()), mb.getSize()))
                {
                    const juce::SpinLock::ScopedLockType a (snapLock);
                    const juce::SpinLock::ScopedLockType b (loadLock);
                    std::memcpy (restoreBuf.data(), mb.getData(), mb.getSize());
                    restoreSize = mb.getSize();
                    std::memcpy (loadBuf.data(), mb.getData(), mb.getSize());
                    loadSize = mb.getSize();
                    // το snapshot γίνεται αυτό που φορτώσαμε, ώστε να μη χαθεί αν ξανασωθεί πριν αλλάξει κάτι
                    std::memcpy (snapshot.data(), mb.getData(), mb.getSize());
                    snapshotSize = mb.getSize();
                    loadPending.store (true);
                }
            }
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LooperProcessor();
}
