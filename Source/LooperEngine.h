// LooperEngine.h
// Η μηχανή του MIDI looper. Δεν εξαρτάται από το JUCE (μόνο standard C++),
// ώστε να δοκιμάζεται αυτόνομα. Δεν κάνει δέσμευση μνήμης στο process().
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ml
{
enum class Command { Main, Stop, Undo, Redo, Clear };
enum class State { Empty, Recording, Playing, Overdubbing, Stopped, Armed };   // Armed = περιμένει το πλέγμα για να ξεκινήσει
enum class Quantize { Off, Beat, Bar };
enum class AfterRecord { Play, Overdub };

struct Msg
{
    uint8_t b[3] { 0, 0, 0 };
    uint8_t size = 0;
};

struct OutEvent
{
    int offset = 0;
    Msg msg;
};

struct InEvent
{
    int offset = 0;
    bool isCommand = false;
    Command cmd = Command::Main;
    Msg msg;
};

struct BlockInfo
{
    int numSamples = 0;
    bool hostPlaying = false;
    bool hostPositionValid = false;
    int64_t hostSample = 0;   // θέση του host στην αρχή του block (samples)
    double bpm = 0.0;         // 0 = άγνωστο
    double barLengthQN = 0.0; // μήκος μέτρου σε τέταρτα (4/4 → 4, 6/8 → 3), 0 = άγνωστο
    bool ppqValid = false;
    double ppq = 0.0;         // θέση σε τέταρτα στην αρχή του block
    bool barStartValid = false;
    double barStartPpq = 0.0; // θέση σε τέταρτα της αρχής του τρέχοντος μέτρου
};

struct Settings
{
    Quantize quantize = Quantize::Off;        // στρογγυλοποίηση του ΜΗΚΟΥΣ
    Quantize startQuantize = Quantize::Off;   // εκκίνηση ηχογράφησης στο επόμενο beat/μέτρο
    int beatsPerBar = 4;
    double fallbackBpm = 120.0;
    AfterRecord afterRecord = AfterRecord::Play;
    bool syncToHost = false;
    bool passThrough = true;
};

class LooperEngine
{
public:
    static constexpr int kMaxLayers = 32;
    static constexpr int kEventsPerLayer = 8192;
    static constexpr int kKeys = 16 * 128;

    // Καλείται εκτός audio thread (εδώ γίνεται η μόνη δέσμευση μνήμης).
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& l : layers)
            l.events.reserve ((size_t) kEventsPerLayer);
        hardReset();
    }

    // Για τα tests: η δυναμική χωρητικότητα δεν πρέπει ποτέ να αλλάζει.
    size_t layerCapacity (int i) const { return layers[(size_t) i].events.capacity(); }

    State getState() const { return state; }
    int64_t getLength() const { return length; }
    int64_t getPosition() const { return pos; }
    int getLayerCount() const { return layerCount; }
    int getRedoCount() const { return redoCount; }
    bool isPausedByHost() const { return pausedByHost; }

    int64_t layerEventCount (int i) const { return (int64_t) layers[(size_t) i].events.size(); }

    // in: ταξινομημένα κατά offset (τα ταξινομεί η process με insertion sort).
    void process (const BlockInfo& info, const Settings& newSettings,
                  InEvent* in, int numIn, std::vector<OutEvent>& out)
    {
        applySettings (newSettings, out);
        settings = newSettings;

        for (int i = 1; i < numIn; ++i) // σταθερή insertion sort
        {
            InEvent e = in[i];
            int j = i - 1;
            while (j >= 0 && in[j].offset > e.offset) { in[j + 1] = in[j]; --j; }
            in[j + 1] = e;
        }

        blockHostStart = info.hostSample;
        syncAtBlockStart (info, out);

        int cursor = 0;
        for (int i = 0; i < numIn; ++i)
        {
            const int off = std::min (std::max (in[i].offset, 0), std::max (info.numSamples - 1, 0));
            advance (cursor, off, out);
            cursor = off;

            if (in[i].isCommand) doCommand (in[i].cmd, off, info, out);
            else                 handleMidi (in[i].msg, off, out);
        }
        advance (cursor, info.numSamples, out);

        lastHostEnd = info.hostSample + info.numSamples;
        lastHostValid = info.hostPlaying && info.hostPositionValid;
    }

private:
    struct Ev { int64_t pos; Msg msg; };
    struct Layer
    {
        std::vector<Ev> events;
        std::array<uint16_t, kKeys> mine {};   // νότες που ηχούν τώρα από αυτό το layer
    };

    static int keyOf (const Msg& m) { return (m.b[0] & 0x0F) * 128 + (m.b[1] & 0x7F); }

    // ---------- βασική κατάσταση ----------
    double sr = 44100.0;
    Settings settings;
    State state = State::Empty;
    int64_t length = 0;          // μήκος loop σε samples (0 = κανένα)
    int64_t redoLength = 0;      // μήκος που θυμόμαστε για redo από άδειο
    int64_t pos = 0;             // θέση μέσα στο loop (ή χρόνος ηχογράφησης)
    int layerCount = 0;          // ολοκληρωμένα layers
    int redoCount = 0;           // layers που μπορούν να γίνουν redo
    std::array<Layer, kMaxLayers> layers;

    // ---------- καταγραφή ----------
    std::array<bool, kKeys> held {};          // νότες που κρατιούνται ΚΑΙ έχουν ηχογραφηθεί
    std::array<int64_t, kKeys> heldPos {};
    int heldCount = 0;
    std::array<bool, 16> sustainRec {};
    std::array<int64_t, 16> sustainRecPos {};

    // ---------- έξοδος ----------
    std::array<uint16_t, kKeys> outCount {};  // πόσα layers κρατούν τη νότα αναμμένη
    std::array<bool, 16> sustainOut {};
    std::array<bool, 16> bendOut {};
    std::array<bool, kKeys> liveHeld {};      // νότες που περνούν live (pass-through)

    // ---------- host sync ----------
    int64_t anchor = 0;
    bool anchorValid = false;
    int64_t lastHostEnd = 0;
    bool lastHostValid = false;
    bool pausedByHost = false;
    int64_t armedBoundary = 0;      // θέση host (samples) όπου ξεκινά η ηχογράφηση
    int64_t blockHostStart = 0;     // θέση host στην αρχή του τρέχοντος block

    void hardReset()
    {
        state = State::Empty; length = 0; redoLength = 0; pos = 0;
        layerCount = 0; redoCount = 0;
        for (auto& l : layers) { l.events.clear(); l.mine.fill (0); }
        held.fill (false); heldCount = 0;
        sustainRec.fill (false);
        outCount.fill (0); sustainOut.fill (false); bendOut.fill (false); liveHeld.fill (false);
        anchorValid = false; pausedByHost = false; lastHostValid = false;
    }

    // ---------- έξοδος ----------
    static void emit (std::vector<OutEvent>& out, int offset, uint8_t s, uint8_t d1, uint8_t d2, uint8_t size = 3)
    {
        if (out.size() >= out.capacity()) return;   // ποτέ δέσμευση στο audio thread
        OutEvent e; e.offset = offset; e.msg.b[0] = s; e.msg.b[1] = d1; e.msg.b[2] = d2; e.msg.size = size;
        out.push_back (e);
    }

    void releaseLayer (int idx, int offset, std::vector<OutEvent>& out)
    {
        auto& L = layers[(size_t) idx];
        for (int k = 0; k < kKeys; ++k)
        {
            if (L.mine[(size_t) k] == 0) continue;
            const auto n = L.mine[(size_t) k];
            L.mine[(size_t) k] = 0;
            outCount[(size_t) k] = (uint16_t) (outCount[(size_t) k] > n ? outCount[(size_t) k] - n : 0);
            if (outCount[(size_t) k] == 0)
                emit (out, offset, (uint8_t) (0x80 | (k / 128)), (uint8_t) (k % 128), 64);
        }
    }

    void releaseControllers (int offset, std::vector<OutEvent>& out)
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            if (sustainOut[(size_t) ch]) { emit (out, offset, (uint8_t) (0xB0 | ch), 64, 0); sustainOut[(size_t) ch] = false; }
            if (bendOut[(size_t) ch])    { emit (out, offset, (uint8_t) (0xE0 | ch), 0x00, 0x40); bendOut[(size_t) ch] = false; }
        }
    }

    void releaseAll (int offset, std::vector<OutEvent>& out)
    {
        for (int i = 0; i < kMaxLayers; ++i) releaseLayer (i, offset, out);
        releaseControllers (offset, out);
    }

    void applySettings (const Settings& s, std::vector<OutEvent>& out)
    {
        if (settings.passThrough && ! s.passThrough)
        {
            for (int k = 0; k < kKeys; ++k)
                if (liveHeld[(size_t) k])
                {
                    emit (out, 0, (uint8_t) (0x80 | (k / 128)), (uint8_t) (k % 128), 64);
                    liveHeld[(size_t) k] = false;
                }
        }
    }

    // ---------- καταγραφή ----------
    Layer& pending() { return layers[(size_t) layerCount]; }

    bool roomFor (const Layer& L) const
    {
        return (int) L.events.size() + heldCount + 4 < kEventsPerLayer;
    }

    static void insertSorted (Layer& L, int64_t p, const Msg& m)
    {
        auto it = std::upper_bound (L.events.begin(), L.events.end(), p,
                                    [] (int64_t v, const Ev& e) { return v < e.pos; });
        L.events.insert (it, Ev { p, m });   // εντός χωρητικότητας: καμία δέσμευση
    }

    static Msg make (uint8_t s, uint8_t a, uint8_t b)
    {
        Msg m; m.b[0] = s; m.b[1] = a; m.b[2] = b; m.size = 3; return m;
    }

    void recordMsg (Msg m, int64_t p)
    {
        Layer& L = pending();
        const int ch = m.b[0] & 0x0F;

        // note-on με velocity 0 = note-off
        if ((m.b[0] & 0xF0) == 0x90 && m.b[2] == 0) { m.b[0] = (uint8_t) (0x80 | ch); m.b[2] = 64; }
        const uint8_t kind = m.b[0] & 0xF0;

        switch (kind)
        {
            case 0x90:
            {
                if (! roomFor (L)) return;
                const int k = keyOf (m);
                if (held[(size_t) k])   // νέο note-on χωρίς off: κλείσε την προηγούμενη
                {
                    insertSorted (L, std::max (p, heldPos[(size_t) k]), make ((uint8_t) (0x80 | ch), m.b[1], 64));
                    held[(size_t) k] = false; --heldCount;
                }
                held[(size_t) k] = true; heldPos[(size_t) k] = p; ++heldCount;
                insertSorted (L, p, m);
                break;
            }
            case 0x80:
            {
                const int k = keyOf (m);
                if (! held[(size_t) k]) return;     // το on δεν ηχογραφήθηκε σε αυτό το layer
                held[(size_t) k] = false; --heldCount;
                insertSorted (L, std::max (p, heldPos[(size_t) k]), m);
                break;
            }
            case 0xB0:
            {
                const uint8_t cc = m.b[1];
                if (cc >= 120) return;              // all-notes-off κ.λπ. δεν ηχογραφούνται
                if (! roomFor (L)) return;
                if (cc == 64)
                {
                    if (m.b[2] >= 64) { sustainRec[(size_t) ch] = true; sustainRecPos[(size_t) ch] = p; }
                    else sustainRec[(size_t) ch] = false;
                }
                insertSorted (L, p, m);
                break;
            }
            case 0xA0: case 0xD0: case 0xE0:
                if (! roomFor (L)) return;
                insertSorted (L, p, m);
                break;
            default: break;   // program change, system: δεν ηχογραφούνται
        }
    }

    // Κλείνει τις νότες/sustain που κρατιούνται, στη θέση closePos (όχι πριν το on).
    void closeHeld (Layer& L, int64_t closePos)
    {
        for (int k = 0; k < kKeys && heldCount > 0; ++k)
            if (held[(size_t) k])
            {
                insertSorted (L, std::max (closePos, heldPos[(size_t) k]),
                              make ((uint8_t) (0x80 | (k / 128)), (uint8_t) (k % 128), 64));
                held[(size_t) k] = false; --heldCount;
            }
        for (int ch = 0; ch < 16; ++ch)
            if (sustainRec[(size_t) ch])
            {
                insertSorted (L, std::max (closePos, sustainRecPos[(size_t) ch]), make ((uint8_t) (0xB0 | ch), 64, 0));
                sustainRec[(size_t) ch] = false;
            }
    }

    void discardHeld()
    {
        held.fill (false); heldCount = 0; sustainRec.fill (false);
    }

    // ---------- προώθηση χρόνου / αναπαραγωγή ----------
    void playRange (int64_t p0, int64_t p1, int offsetBase, std::vector<OutEvent>& out)
    {
        for (int li = 0; li < layerCount; ++li)
        {
            auto& L = layers[(size_t) li];
            auto it = std::lower_bound (L.events.begin(), L.events.end(), p0,
                                        [] (const Ev& e, int64_t v) { return e.pos < v; });
            for (; it != L.events.end() && it->pos < p1; ++it)
            {
                const int off = offsetBase + (int) (it->pos - p0);
                const Msg& m = it->msg;
                const uint8_t kind = m.b[0] & 0xF0;
                const int ch = m.b[0] & 0x0F;

                if (kind == 0x90)
                {
                    const int k = keyOf (m);
                    ++L.mine[(size_t) k]; ++outCount[(size_t) k];
                    emit (out, off, m.b[0], m.b[1], m.b[2]);
                }
                else if (kind == 0x80)
                {
                    const int k = keyOf (m);
                    if (L.mine[(size_t) k] > 0)
                    {
                        --L.mine[(size_t) k];
                        if (--outCount[(size_t) k] == 0) emit (out, off, m.b[0], m.b[1], m.b[2]);
                    }
                }
                else
                {
                    if (kind == 0xB0 && m.b[1] == 64) sustainOut[(size_t) ch] = m.b[2] >= 64;
                    if (kind == 0xE0) bendOut[(size_t) ch] = ! (m.b[1] == 0x00 && m.b[2] == 0x40);
                    emit (out, off, m.b[0], m.b[1], m.b[2], m.size);
                }
            }
        }
    }

    void onWrap (int offset, std::vector<OutEvent>& out)
    {
        if (state == State::Overdubbing && ! pausedByHost)
            closeHeld (pending(), length - 1);
        for (int i = 0; i < layerCount; ++i) releaseLayer (i, offset, out);
        releaseControllers (offset, out);
    }

    // Αν περιμένουμε το πλέγμα και η στιγμή `at` (offset μέσα στο block) το έχει φτάσει, ξεκινά η ηχογράφηση.
    void maybeStartArmed (int at)
    {
        if (state != State::Armed) return;
        const int64_t bOff = armedBoundary - blockHostStart;
        if (bOff <= at) { state = State::Recording; pos = at - bOff; }
    }

    void advance (int from, int to, std::vector<OutEvent>& out)
    {
        if (state == State::Armed) { maybeStartArmed (to); return; }

        int remaining = to - from;
        if (remaining <= 0) return;

        if (state == State::Recording) { pos += remaining; return; }
        if (state != State::Playing && state != State::Overdubbing) return;
        if (pausedByHost || length <= 0) return;

        int off = from;
        while (remaining > 0)
        {
            const int64_t untilWrap = length - pos;
            const int seg = (int) std::min<int64_t> (remaining, untilWrap);
            playRange (pos, pos + seg, off, out);
            pos += seg; off += seg; remaining -= seg;
            if (pos >= length) { onWrap (off, out); pos = 0; }
        }
    }

    // ---------- host sync ----------
    void syncAtBlockStart (const BlockInfo& info, std::vector<OutEvent>& out)
    {
        // Αν ο host σταμάτησε ή έκανε άλμα ενώ περιμέναμε το πλέγμα, ακυρώνεται η αναμονή.
        if (state == State::Armed)
        {
            const bool contiguous = info.hostPlaying && info.hostPositionValid && lastHostValid
                                    && info.hostSample == lastHostEnd;
            if (! contiguous) state = State::Empty;
        }

        const bool active = settings.syncToHost && anchorValid && length > 0
                            && (state == State::Playing || state == State::Overdubbing);
        if (! active) { pausedByHost = false; return; }

        if (! info.hostPlaying || ! info.hostPositionValid)
        {
            if (! pausedByHost)
            {
                if (state == State::Overdubbing) closeHeld (pending(), pos);
                releaseAll (0, out);
                pausedByHost = true;
            }
            return;
        }

        pausedByHost = false;
        int64_t want = (info.hostSample - anchor) % length;
        if (want < 0) want += length;
        if (want != pos)
        {
            if (state == State::Overdubbing) closeHeld (pending(), pos);
            releaseAll (0, out);
            pos = want;
        }
    }

    // ---------- εντολές ----------
    int64_t quantizeLength (int64_t raw, const BlockInfo& info) const
    {
        raw = std::max<int64_t> (1, raw);
        if (settings.quantize == Quantize::Off) return raw;
        const double bpm = info.bpm > 0.0 ? info.bpm : settings.fallbackBpm;
        const double barQN = info.barLengthQN > 0.0 ? info.barLengthQN : (double) std::max (1, settings.beatsPerBar);
        const double unit = 60.0 * sr / bpm * (settings.quantize == Quantize::Bar ? barQN : 1.0);
        const double n = std::max (1.0, std::floor ((double) raw / unit + 0.5));
        return std::max<int64_t> (1, (int64_t) std::llround (n * unit));
    }

    void setAnchor (int offset, const BlockInfo& info)
    {
        anchorValid = info.hostPlaying && info.hostPositionValid;
        anchor = info.hostSample + offset;
        pausedByHost = false;
    }

    // Επόμενο όριο beat/μέτρου του host, σε απόλυτη θέση host (samples).
    // Επιστρέφει false αν ο host δεν παίζει ή δεν ξέρουμε το πλέγμα.
    bool nextGridBoundary (int offset, const BlockInfo& info, int64_t& boundaryHost) const
    {
        if (! (info.hostPlaying && info.hostPositionValid)) return false;

        const double bpm = info.bpm > 0.0 ? info.bpm : settings.fallbackBpm;
        const double samplesPerQN = 60.0 * sr / bpm;
        const double barQN = info.barLengthQN > 0.0 ? info.barLengthQN : (double) std::max (1, settings.beatsPerBar);
        const double unit = settings.startQuantize == Quantize::Bar ? barQN : 1.0;

        double ppqNow, origin = 0.0;
        if (info.ppqValid)
        {
            ppqNow = info.ppq + (double) offset / samplesPerQN;
            if (info.barStartValid) origin = info.barStartPpq;
        }
        else
            ppqNow = (double) (info.hostSample + offset) / samplesPerQN;   // υποθέτει σταθερό tempo από το 0

        const double rel = (ppqNow - origin) / unit;
        const double fl = std::floor (rel);
        const double pastSamples = (rel - fl) * unit * samplesPerQN;      // πόσα samples μετά το προηγούμενο όριο
        const double nextRel = pastSamples < 0.5 ? fl : fl + 1.0;         // <0.5 sample: είμαστε πάνω στο όριο
        const double delta = (origin + nextRel * unit - ppqNow) * samplesPerQN;
        boundaryHost = info.hostSample + offset + (int64_t) std::llround (std::max (0.0, delta));
        return true;
    }

    void beginOverdub()
    {
        if (layerCount >= kMaxLayers) return;
        redoCount = 0;
        auto& L = pending();
        L.events.clear(); L.mine.fill (0);
        discardHeld();
        state = State::Overdubbing;
    }

    void commitOverdub (int offset)
    {
        (void) offset;
        auto& L = pending();
        if (! pausedByHost) closeHeld (L, pos);
        discardHeld();
        if (! L.events.empty()) ++layerCount;
        else L.events.clear();
        state = State::Playing;
    }

    void closeFirstLoop (int offset, const BlockInfo& info)
    {
        const int64_t raw = pos;
        auto& L = layers[0];
        const int64_t len = quantizeLength (raw, info);

        // 1) τύλιξε ό,τι ξεπερνά το νέο μήκος (σταθερή insertion sort, χωρίς δέσμευση)
        size_t firstBeyond = L.events.size();
        for (size_t i = 0; i < L.events.size(); ++i)
            if (L.events[i].pos >= len) { firstBeyond = i; break; }
        for (size_t i = firstBeyond; i < L.events.size(); ++i)
        {
            L.events[i].pos %= len;
            auto ub = std::upper_bound (L.events.begin(), L.events.begin() + (std::ptrdiff_t) i, L.events[i].pos,
                                        [] (int64_t v, const Ev& e) { return v < e.pos; });
            std::rotate (ub, L.events.begin() + (std::ptrdiff_t) i, L.events.begin() + (std::ptrdiff_t) i + 1);
        }

        // 2) κλείσε νότες/sustain που κρατιούνται: διάρκεια = όση κράτησε ως το πάτημα
        for (int k = 0; k < kKeys && heldCount > 0; ++k)
            if (held[(size_t) k])
            {
                const int64_t on = heldPos[(size_t) k] % len;
                const int64_t dur = std::max<int64_t> (0, raw - heldPos[(size_t) k]);
                insertSorted (L, std::min (on + dur, len - 1),
                              make ((uint8_t) (0x80 | (k / 128)), (uint8_t) (k % 128), 64));
                held[(size_t) k] = false; --heldCount;
            }
        for (int ch = 0; ch < 16; ++ch)
            if (sustainRec[(size_t) ch])
            {
                const int64_t on = sustainRecPos[(size_t) ch] % len;
                const int64_t dur = std::max<int64_t> (0, raw - sustainRecPos[(size_t) ch]);
                insertSorted (L, std::min (on + dur, len - 1), make ((uint8_t) (0xB0 | ch), 64, 0));
                sustainRec[(size_t) ch] = false;
            }

        length = len;
        layerCount = 1;
        redoCount = 0;
        // Το loop συνεχίζει από το σημείο που βρισκόμαστε (όχι από την αρχή), ώστε η φάση του
        // να μένει δεμένη με την αρχή της ηχογράφησης και, άρα, με το πλέγμα του host.
        pos = raw % len;
        anchorValid = info.hostPlaying && info.hostPositionValid;
        anchor = info.hostSample + offset - raw;
        pausedByHost = false;
        if (settings.afterRecord == AfterRecord::Overdub) beginOverdub();
        else state = State::Playing;
    }

    void undoLast (int offset, std::vector<OutEvent>& out)
    {
        if (layerCount == 0) return;
        --layerCount;
        releaseLayer (layerCount, offset, out);
        releaseControllers (offset, out);
        ++redoCount;
        if (layerCount == 0)
        {
            redoLength = length;
            length = 0; pos = 0; state = State::Empty; anchorValid = false;
        }
    }

    void doCommand (Command c, int offset, const BlockInfo& info, std::vector<OutEvent>& out)
    {
        switch (c)
        {
            case Command::Main:
                switch (state)
                {
                    case State::Empty:
                        layers[0].events.clear(); layers[0].mine.fill (0);
                        layerCount = 0; redoCount = 0; redoLength = 0; length = 0;
                        discardHeld();
                        pos = 0;
                        state = State::Recording;
                        if (settings.startQuantize != Quantize::Off)
                        {
                            int64_t b = 0;
                            if (nextGridBoundary (offset, info, b) && b > info.hostSample + offset)
                            {
                                armedBoundary = b;
                                state = State::Armed;
                            }
                        }
                        break;
                    case State::Armed:       state = State::Empty; break;   // δεύτερο πάτημα = ακύρωση
                    case State::Recording:
                        if (pos <= 0) { state = State::Empty; layers[0].events.clear(); discardHeld(); }
                        else closeFirstLoop (offset, info);
                        break;
                    case State::Playing:     beginOverdub(); break;
                    case State::Overdubbing: commitOverdub (offset); break;
                    case State::Stopped:
                        pos = 0; setAnchor (offset, info); state = State::Playing; break;
                }
                break;

            case Command::Stop:
                if (state == State::Armed) state = State::Empty;
                else if (state == State::Recording)
                {
                    if (pos <= 0) { state = State::Empty; layers[0].events.clear(); discardHeld(); }
                    else { closeFirstLoop (offset, info);
                           if (state == State::Overdubbing) commitOverdub (offset);
                           releaseAll (offset, out); pos = 0; state = State::Stopped; }
                }
                else if (state == State::Playing || state == State::Overdubbing)
                {
                    if (state == State::Overdubbing) commitOverdub (offset);
                    releaseAll (offset, out);
                    pos = 0; state = State::Stopped;
                }
                break;

            case Command::Undo:
                if (state == State::Armed) state = State::Empty;
                else if (state == State::Recording)
                {
                    state = State::Empty; layers[0].events.clear(); discardHeld(); pos = 0;
                }
                else if (state == State::Overdubbing)
                {
                    pending().events.clear(); discardHeld(); state = State::Playing;
                }
                else if (state == State::Playing || state == State::Stopped)
                    undoLast (offset, out);
                break;

            case Command::Redo:
                if ((state == State::Playing || state == State::Stopped || state == State::Empty)
                    && redoCount > 0 && layerCount < kMaxLayers)
                {
                    if (state == State::Empty)
                    {
                        if (redoLength <= 0) break;
                        length = redoLength; pos = 0; state = State::Playing; setAnchor (offset, info);
                    }
                    ++layerCount; --redoCount;
                }
                break;

            case Command::Clear:
                releaseAll (offset, out);
                for (auto& l : layers) { l.events.clear(); l.mine.fill (0); }
                outCount.fill (0);
                discardHeld();
                layerCount = 0; redoCount = 0; length = 0; redoLength = 0; pos = 0;
                state = State::Empty; anchorValid = false; pausedByHost = false;
                break;
        }
    }

    // ---------- εισερχόμενο MIDI ----------
    void handleMidi (const Msg& m, int offset, std::vector<OutEvent>& out)
    {
        const uint8_t kind = m.b[0] & 0xF0;
        if (settings.passThrough)
        {
            emit (out, offset, m.b[0], m.b[1], m.b[2], m.size);
            if (kind == 0x90 && m.b[2] > 0)           liveHeld[(size_t) keyOf (m)] = true;
            else if (kind == 0x80 || kind == 0x90)    liveHeld[(size_t) keyOf (m)] = false;
        }
        if (state == State::Recording || (state == State::Overdubbing && ! pausedByHost))
            recordMsg (m, pos);
    }
};
} // namespace ml
