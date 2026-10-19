// LooperEngine.h
// Η μηχανή του MIDI looper. Δεν εξαρτάται από το JUCE (μόνο standard C++),
// ώστε να δοκιμάζεται αυτόνομα. Δεν κάνει δέσμευση μνήμης στο process().
//
// Λογική (έκδοση 2):
//  - Έως kMaxLayers (4) layers. Κάθε layer έχει δικό του μήκος `len`. Το μήκος του loop
//    είναι το μήκος του νεότερου layer. Τα παλαιότερα (πιο κοντά) layers επαναλαμβάνονται
//    κυκλικά για να γεμίσουν το loop, και κόβονται στο τέλος του loop αν δεν χωράνε ακέραιες φορές.
//  - Το πάτημα του REC για overdub ξαναρχίζει όλα τα layers από την αρχή (μετά από αναμονή του Start quantize)
//    και το νέο layer ηχογραφείται από εκεί. Αν το overdub ξεπεράσει το τέλος του loop, το loop μεγαλώνει
//    (στρογγυλοποίηση με το Length quantize).
//  - UNDO: σβήνει το νεότερο layer· το μήκος γυρίζει στο μήκος του προηγούμενου.
//  - STOP: N γρήγορα πατήματα (παράθυρο tapWindowMs μετά το τελευταίο) σιγάζουν/ενεργοποιούν το layer N.
//  - Follow tempo: οι θέσεις αποθηκεύονται σε "samples αναφοράς" (στο tempo της ηχογράφησης)
//    και προχωρούν με ρυθμό bpmΤώρα / bpmΑναφοράς.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ml
{
enum class Command { Main, Undo, Stop };
enum class State { Empty, Recording, Playing, Overdubbing, Armed };   // Armed = περιμένει το πλέγμα για να ξεκινήσει
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
    bool followTempo = false;       // το loop ακολουθεί τις αλλαγές tempo του host
    int snapDiv = 0;                // 0 = όχι, 8 = 1/8, 16 = 1/16: στρογγυλοποίηση της αρχής των νοτών
    bool passThrough = true;
    double tapWindowMs = 500.0;     // παράθυρο πολλαπλών πατημάτων του STOP
};

class LooperEngine
{
public:
    static constexpr int kMaxLayers = 4;
    static constexpr int kEventsPerLayer = 8192;
    static constexpr int kKeys = 16 * 128;
    static constexpr size_t kEventBytes = 12;
    static constexpr size_t kMaxStateBytes = 128 + (size_t) kMaxLayers * (32 + (size_t) kEventsPerLayer * kEventBytes);

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
    bool isPausedByHost() const { return pausedByHost; }
    int64_t getLayerLength (int i) const { return layers[(size_t) i].len; }
    bool isLayerMuted (int i) const { return layers[(size_t) i].muted; }
    int64_t getOverdubStart() const { return odStart; }
    double getRefBpm() const { return refBpm; }
    double getRate() const { return rate; }
    int getPendingTaps() const { return tapCount; }
    bool isOverdubArmed() const { return odArmed; }

    int64_t layerEventCount (int i) const { return (int64_t) layers[(size_t) i].events.size(); }

    // Έχει αλλάξει κάτι που αποθηκεύεται στο project (layers, σίγαση);
    bool takeDirty() { const bool d = dirty; dirty = false; return d; }

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
        curInfo = info;
        curBpm = info.bpm > 0.0 ? info.bpm : settings.fallbackBpm;
        curBarQN = info.barLengthQN > 0.0 ? info.barLengthQN : (double) std::max (1, settings.beatsPerBar);

        // Όσο δεν υπάρχει layer, το "tempo αναφοράς" ακολουθεί το tempo του host.
        if (layerCount == 0) refBpm = curBpm;

        // Ρυθμός προχώρησης του loop σε σχέση με το χρόνο του host.
        rate = 1.0;
        if (settings.followTempo && layerCount > 0 && info.bpm > 0.0 && info.ppqValid && refBpm > 0.0
            && (! settings.syncToHost || anchorPpqValid))
        {
            rate = info.bpm / refBpm;
            if (std::fabs (rate - 1.0) < 1.0e-6) rate = 1.0;
        }

        syncAtBlockStart (info, out);

        int cursor = 0;
        for (int i = 0; i < numIn; ++i)
        {
            const int off = std::min (std::max (in[i].offset, 0), std::max (info.numSamples - 1, 0));
            cursor = flushTaps (cursor, off, true, out);
            advance (cursor, off, out);
            cursor = off;

            if (in[i].isCommand) doCommand (in[i].cmd, off, info, out);
            else                 handleMidi (in[i].msg, off, out);
        }
        cursor = flushTaps (cursor, info.numSamples, false, out);
        advance (cursor, info.numSamples, out);

        lastHostEnd = info.hostSample + info.numSamples;
        lastHostValid = info.hostPlaying && info.hostPositionValid;
        clock += info.numSamples;
    }

    // ---------- αποθήκευση / φόρτωση (για το project του host) ----------
    // Γράφει την κατάσταση των layers στο buf. Επιστρέφει τα bytes ή 0 αν δεν χωράει / δεν υπάρχει τίποτα.
    size_t serialize (uint8_t* buf, size_t cap) const
    {
        if (layerCount <= 0) return 0;
        size_t need = 8 + 8 + 8 + 8 + 8 + 1;
        for (int i = 0; i < layerCount; ++i)
            need += 8 + 1 + 4 + layers[(size_t) i].events.size() * kEventBytes;
        need += 4;   // magic
        if (need > cap) return 0;

        size_t p = 0;
        auto put = [&] (const void* src, size_t n) { std::memcpy (buf + p, src, n); p += n; };
        const uint32_t magic = 0x4D4C5031u;   // "MLP1"
        put (&magic, 4);
        put (&refBpm, 8);
        put (&length, 8);
        put (&anchor, 8);
        put (&anchorPpq, 8);
        const double ppqFlag = anchorPpqValid ? 1.0 : 0.0; put (&ppqFlag, 8);
        const uint8_t n = (uint8_t) layerCount; put (&n, 1);
        for (int i = 0; i < layerCount; ++i)
        {
            const auto& L = layers[(size_t) i];
            put (&L.len, 8);
            const uint8_t mu = L.muted ? 1 : 0; put (&mu, 1);
            const uint32_t cnt = (uint32_t) L.events.size(); put (&cnt, 4);
            for (const auto& e : L.events)
            {
                put (&e.pos, 8);
                put (&e.msg.size, 1);
                put (e.msg.b, 3);
            }
        }
        return p;
    }

    // Έλεγχος εγκυρότητας, χωρίς καμία αλλαγή και χωρίς δέσμευση μνήμης.
    static bool validateState (const uint8_t* buf, size_t n)
    {
        size_t p = 0;
        auto get = [&] (void* dst, size_t k) -> bool { if (p + k > n) return false; std::memcpy (dst, buf + p, k); p += k; return true; };
        uint32_t magic = 0; double refB = 0, aPpq = 0, pad = 0; int64_t len = 0, anc = 0; uint8_t cnt = 0;
        if (! get (&magic, 4) || magic != 0x4D4C5031u) return false;
        if (! get (&refB, 8) || ! (refB > 1.0 && refB < 1000.0)) return false;
        if (! get (&len, 8) || len <= 0) return false;
        if (! get (&anc, 8) || ! get (&aPpq, 8) || ! get (&pad, 8) || ! get (&cnt, 1)) return false;
        if (cnt < 1 || cnt > kMaxLayers) return false;
        int64_t lastLen = 0;
        for (int i = 0; i < cnt; ++i)
        {
            int64_t l = 0; uint8_t mu = 0; uint32_t c = 0;
            if (! get (&l, 8) || l <= 0 || ! get (&mu, 1) || ! get (&c, 4) || c > (uint32_t) kEventsPerLayer) return false;
            for (uint32_t e = 0; e < c; ++e)
            {
                int64_t ep = 0; uint8_t sz = 0; uint8_t b[3];
                if (! get (&ep, 8) || ! get (&sz, 1) || ! get (b, 3)) return false;
                if (ep < 0 || ep >= l || sz < 1 || sz > 3) return false;
            }
            lastLen = l;
        }
        return lastLen == len;
    }

    // Καλείται από το audio thread. Αντικαθιστά τα πάντα με την αποθηκευμένη κατάσταση.
    bool loadState (const uint8_t* buf, size_t n, std::vector<OutEvent>& out)
    {
        if (! validateState (buf, n)) return false;
        releaseAll (0, out);
        hardReset();

        size_t p = 4;
        auto get = [&] (void* dst, size_t k) { std::memcpy (dst, buf + p, k); p += k; };
        double pad; uint8_t cnt = 0;
        get (&refBpm, 8); get (&length, 8); get (&anchor, 8); get (&anchorPpq, 8); get (&pad, 8); get (&cnt, 1);
        for (int i = 0; i < cnt; ++i)
        {
            auto& L = layers[(size_t) i];
            uint8_t mu = 0; uint32_t c = 0;
            get (&L.len, 8); get (&mu, 1); get (&c, 4);
            L.muted = mu != 0;
            L.events.clear();
            for (uint32_t e = 0; e < c; ++e)
            {
                Ev ev; uint8_t sz = 0;
                get (&ev.pos, 8); get (&sz, 1); get (ev.msg.b, 3);
                ev.msg.size = sz;
                L.events.push_back (ev);
            }
        }
        layerCount = cnt;
        state = State::Playing;
        pos = 0; posFrac = 0.0;
        anchorValid = true;
        anchorPpqValid = pad > 0.5;
        pausedByHost = false;
        dirty = false;
        return true;
    }

private:
    struct Ev { int64_t pos; Msg msg; };
    struct Layer
    {
        std::vector<Ev> events;
        std::array<uint16_t, kKeys> mine {};   // νότες που ηχούν τώρα από αυτό το layer
        int64_t len = 0;                       // μήκος του layer (samples αναφοράς)
        bool muted = false;
    };

    static int keyOf (const Msg& m) { return (m.b[0] & 0x0F) * 128 + (m.b[1] & 0x7F); }

    // ---------- βασική κατάσταση ----------
    double sr = 44100.0;
    Settings settings;
    State state = State::Empty;
    int64_t length = 0;          // μήκος loop σε samples αναφοράς (0 = κανένα)
    int64_t pos = 0;             // θέση μέσα στο loop (ή χρόνος ηχογράφησης), σε samples αναφοράς
    double posFrac = 0.0;        // κλασματικό μέρος της θέσης (μόνο όταν rate ≠ 1)
    int layerCount = 0;          // ολοκληρωμένα layers
    std::array<Layer, kMaxLayers> layers;
    int64_t odStart = 0;         // θέση όπου ξεκίνησε το τρέχον overdub
    bool dirty = false;
    bool odArmed = false;        // το overdub περιμένει το πλέγμα για να ξεκινήσει (το loop συνεχίζει να παίζει)
    BlockInfo curInfo;

    double curBpm = 120.0;       // tempo του host στο τρέχον block
    double curBarQN = 4.0;
    double refBpm = 120.0;       // tempo στο οποίο είναι γραμμένες οι θέσεις των layers
    double rate = 1.0;           // samples αναφοράς ανά sample host

    // ---------- καταγραφή ----------
    std::array<bool, kKeys> held {};          // νότες που κρατιούνται ΚΑΙ έχουν ηχογραφηθεί
    std::array<int64_t, kKeys> heldPos {};
    std::array<int64_t, kKeys> snapDelta {};
    int heldCount = 0;
    std::array<bool, 16> sustainRec {};
    std::array<int64_t, 16> sustainRecPos {};

    // ---------- έξοδος ----------
    std::array<uint16_t, kKeys> outCount {};  // πόσα layers κρατούν τη νότα αναμμένη
    std::array<bool, 16> sustainOut {};
    std::array<bool, 16> bendOut {};
    std::array<bool, kKeys> liveHeld {};      // νότες που περνούν live (pass-through)

    // ---------- host sync ----------
    int64_t anchor = 0;            // θέση host (samples) που αντιστοιχεί στη θέση 0 του loop
    double anchorPpq = 0.0;        // ίδιο, σε τέταρτα (για Follow tempo)
    bool anchorValid = false;
    bool anchorPpqValid = false;
    int64_t lastHostEnd = 0;
    bool lastHostValid = false;
    bool pausedByHost = false;
    int64_t armedBoundary = 0;      // θέση host (samples) όπου ξεκινά η ηχογράφηση
    int64_t blockHostStart = 0;     // θέση host στην αρχή του τρέχοντος block

    // ---------- πολλαπλά πατήματα STOP ----------
    int64_t clock = 0;              // samples που έχουν περάσει (δεν εξαρτάται από τον host)
    int tapCount = 0;
    int64_t tapExpiry = 0;

    void hardReset()
    {
        state = State::Empty; length = 0; pos = 0; posFrac = 0.0; layerCount = 0; odStart = 0;
        for (auto& l : layers) { l.events.clear(); l.mine.fill (0); l.len = 0; l.muted = false; }
        held.fill (false); heldCount = 0;
        sustainRec.fill (false);
        outCount.fill (0); sustainOut.fill (false); bendOut.fill (false); liveHeld.fill (false);
        anchorValid = false; anchorPpqValid = false; pausedByHost = false; lastHostValid = false;
        tapCount = 0; odArmed = false;
        dirty = true;
    }

    double spqRef() const { return 60.0 * sr / refBpm; }   // samples αναφοράς ανά τέταρτο

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

    // Στρογγυλοποίηση της αρχής μιας νότας στο πλησιέστερο 1/8 ή 1/16 (σε σχέση με την αρχή του loop).
    int64_t snapPos (int64_t p) const
    {
        if (settings.snapDiv <= 0) return p;
        const double unit = spqRef() * 4.0 / (double) settings.snapDiv;
        if (unit < 1.0) return p;
        const double n = std::floor ((double) p / unit + 0.5);
        return std::max<int64_t> (0, (int64_t) std::llround (n * unit));
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
                const int64_t sp = snapPos (p);
                if (held[(size_t) k])   // νέο note-on χωρίς off: κλείσε την προηγούμενη
                {
                    insertSorted (L, std::max (sp, heldPos[(size_t) k]), make ((uint8_t) (0x80 | ch), m.b[1], 64));
                    held[(size_t) k] = false; --heldCount;
                }
                held[(size_t) k] = true; heldPos[(size_t) k] = sp; snapDelta[(size_t) k] = sp - p; ++heldCount;
                insertSorted (L, sp, m);
                break;
            }
            case 0x80:
            {
                const int k = keyOf (m);
                if (! held[(size_t) k]) return;     // το on δεν ηχογραφήθηκε σε αυτό το layer
                held[(size_t) k] = false; --heldCount;
                insertSorted (L, std::max (p + snapDelta[(size_t) k], heldPos[(size_t) k]), m);
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

    // Τύλιξε ό,τι ξεπερνά το μήκος len (σταθερή insertion sort, χωρίς δέσμευση)
    static void wrapEvents (Layer& L, int64_t len)
    {
        if (len <= 0) return;
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
    }

    // ---------- προώθηση χρόνου / αναπαραγωγή ----------
    // Παίζει τα γεγονότα των layers που πέφτουν στο [start, end) (θέσεις αναφοράς).
    // Κάθε layer επαναλαμβάνεται κυκλικά με το δικό του μήκος.
    void playRange (double start, double end, double rt, int offBase, int seg, std::vector<OutEvent>& out)
    {
        const int maxOff = offBase + seg - 1;
        for (int li = 0; li < layerCount; ++li)
        {
            auto& L = layers[(size_t) li];
            if (L.muted || L.len <= 0 || L.events.empty()) continue;
            const double len = (double) L.len;

            for (double b = std::floor (start / len) * len; b < end; b += len)
            {
                const double lo = std::max (start, b) - b;
                const double hi = std::min (end, b + len) - b;
                auto it = std::lower_bound (L.events.begin(), L.events.end(), lo,
                                            [] (const Ev& e, double v) { return (double) e.pos < v; });
                for (; it != L.events.end() && (double) it->pos < hi; ++it)
                {
                    int off = offBase + (int) std::floor ((b + (double) it->pos - start) / rt + 1.0e-9);
                    off = std::min (std::max (off, offBase), maxOff);
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
    }

    void onWrap (int offset, std::vector<OutEvent>& out)
    {
        for (int i = 0; i < layerCount; ++i) releaseLayer (i, offset, out);
        releaseControllers (offset, out);
    }

    // Αν περιμένουμε το πλέγμα και η στιγμή `at` (offset μέσα στο block) το έχει φτάσει, ξεκινά η ηχογράφηση.
    void maybeStartArmed (int at)
    {
        if (state != State::Armed) return;
        const int64_t bOff = armedBoundary - blockHostStart;
        if (bOff <= at) { state = State::Recording; pos = at - bOff; posFrac = 0.0; }
    }

    void advance (int from, int to, std::vector<OutEvent>& out)
    {
        if (odArmed)
        {
            const int64_t bOff = armedBoundary - blockHostStart;
            if (bOff < to)
            {
                const int at = (int) std::min<int64_t> (std::max<int64_t> (bOff, from), to);
                advanceCore (from, at, out);
                odArmed = false;
                startOverdubNow (at, curInfo, out);
                advanceCore (at, to, out);
                return;
            }
        }
        advanceCore (from, to, out);
    }

    void advanceCore (int from, int to, std::vector<OutEvent>& out)
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
            int seg = remaining;
            if (state == State::Playing)   // μόνο το Playing τυλίγεται· το overdub συνεχίζει γραμμικά
            {
                int64_t hostToWrap;
                if (rate == 1.0 && posFrac == 0.0) hostToWrap = length - pos;
                else hostToWrap = (int64_t) std::ceil (((double) (length - pos) - posFrac) / rate - 1.0e-7);
                if (hostToWrap < 1) hostToWrap = 1;
                seg = (int) std::min<int64_t> (remaining, hostToWrap);
            }

            const double start = (double) pos + posFrac;
            const double end = start + (double) seg * rate;
            playRange (start, end, rate, off, seg, out);
            pos = (int64_t) std::floor (end);
            posFrac = end - (double) pos;
            if (rate == 1.0 && posFrac < 1.0e-9) posFrac = 0.0;
            off += seg; remaining -= seg;
            if (state == State::Playing && pos >= length) { onWrap (off, out); pos %= length; }
        }
    }

    // ---------- host sync ----------
    void setPos (double p)
    {
        if (p < 0.0) p = 0.0;
        pos = (int64_t) std::floor (p);
        posFrac = p - (double) pos;
        if (state == State::Playing && length > 0 && pos >= length) pos %= length;
    }

    void syncAtBlockStart (const BlockInfo& info, std::vector<OutEvent>& out)
    {
        // Αν ο host σταμάτησε ή έκανε άλμα ενώ περιμέναμε το πλέγμα, ακυρώνεται η αναμονή.
        if (state == State::Armed)
        {
            const bool contiguous = info.hostPlaying && info.hostPositionValid && lastHostValid
                                    && info.hostSample == lastHostEnd;
            if (! contiguous) state = State::Empty;
        }

        if (odArmed)
        {
            const bool contiguous = info.hostPlaying && info.hostPositionValid && lastHostValid
                                    && info.hostSample == lastHostEnd;
            if (! contiguous) odArmed = false;
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
        const bool ppqPath = (rate != 1.0);
        auto wantNow = [&] () -> double
        {
            double w = ppqPath ? (info.ppq - anchorPpq) * spqRef() : (double) (info.hostSample - anchor);
            if (state == State::Playing)
            {
                w = std::fmod (w, (double) length);
                if (w < 0.0) w += (double) length;
            }
            return w;
        };
        auto diffOf = [&] (double w) -> double
        {
            double d = w - ((double) pos + posFrac);
            if (state == State::Playing)
            {
                const double half = (double) length * 0.5;
                if (d > half) d -= (double) length;
                else if (d < -half) d += (double) length;
            }
            return d;
        };
        const double tol = ppqPath ? 0.02 * sr : 1.0;   // μικρές αποκλίσεις διορθώνονται αθόρυβα

        double w = wantNow();
        double d = diffOf (w);
        if (std::fabs (d) >= tol && state == State::Overdubbing)
        {
            // άλμα του host μέσα σε overdub: ολοκληρώνεται το layer και συνεχίζουμε ως Playing
            commitOverdub (0, info, out, false);
            w = wantNow();
            d = diffOf (w);
        }

        if (std::fabs (d) >= tol)
        {
            releaseAll (0, out);
            setPos (w);
        }
        else if (d != 0.0)
            setPos (w);

        if (ppqPath)   // κράτα και το πλέγμα σε samples ενημερωμένο, για όταν το tempo επανέλθει
            anchor = info.hostSample - (int64_t) std::llround ((double) pos + posFrac);
    }

    // ---------- εντολές ----------
    int64_t quantizeLength (int64_t raw, const BlockInfo& info) const
    {
        (void) info;
        raw = std::max<int64_t> (1, raw);
        if (settings.quantize == Quantize::Off) return raw;
        const double unit = spqRef() * (settings.quantize == Quantize::Bar ? curBarQN : 1.0);
        const double n = std::max (1.0, std::floor ((double) raw / unit + 0.5));
        return std::max<int64_t> (1, (int64_t) std::llround (n * unit));
    }

    // Δένει τη θέση αναφοράς refPos του loop με τη στιγμή `offset` του τρέχοντος block του host.
    void setAnchors (int offset, const BlockInfo& info, double refPos)
    {
        anchorValid = info.hostPlaying && info.hostPositionValid;
        anchor = info.hostSample + offset - (int64_t) std::llround (refPos);
        anchorPpqValid = info.ppqValid;
        if (info.ppqValid)
        {
            const double spqHost = 60.0 * sr / curBpm;
            anchorPpq = info.ppq + (double) offset / spqHost - refPos / spqRef();
        }
        pausedByHost = false;
    }

    // Αν υπάρχει ήδη δεμένο πλέγμα και ο host παίζει, ξαναδέσε το (π.χ. μετά από αλλαγή μήκους).
    void reanchorIfValid (int offset, const BlockInfo& info, double refPos)
    {
        if (anchorValid && info.hostPlaying && info.hostPositionValid)
            setAnchors (offset, info, refPos);
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

    void beginOverdub (int offset, const BlockInfo& info)
    {
        if (layerCount >= kMaxLayers) return;
        auto& L = pending();
        L.events.clear(); L.mine.fill (0); L.len = 0; L.muted = false;
        discardHeld();
        tapCount = 0;
        odStart = pos;
        // Από εδώ η θέση προχωρά γραμμικά (χωρίς τύλιγμα) μέχρι το τέλος του overdub.
        reanchorIfValid (offset, info, (double) pos + posFrac);
        state = State::Overdubbing;
    }

    // Το πάτημα του REC για overdub: όλα τα layers ξαναρχίζουν από την αρχή και το νέο layer ηχογραφείται από εκεί.
    void startOverdubNow (int offset, const BlockInfo& info, std::vector<OutEvent>& out)
    {
        if (layerCount >= kMaxLayers) return;
        releaseAll (offset, out);
        pos = 0; posFrac = 0.0;
        beginOverdub (offset, info);
    }

    // Ολοκληρώνει το overdub. Αν πέρασε το τέλος του loop, το loop μεγαλώνει.
    void commitOverdub (int offset, const BlockInfo& info, std::vector<OutEvent>& out, bool doReanchor)
    {
        auto& L = pending();
        const int64_t tEnd = pos;
        if (! pausedByHost) closeHeld (L, tEnd);
        discardHeld();

        if (! L.events.empty())
        {
            int64_t newLen = length;
            if (tEnd > length) newLen = std::max (length, quantizeLength (tEnd, info));
            wrapEvents (L, newLen);
            L.len = newLen; L.muted = false;
            ++layerCount;
            length = newLen;
            dirty = true;
        }
        else
            L.events.clear();

        state = State::Playing;
        if (pos >= length)
        {
            onWrap (offset, out);   // το loop γύρισε: κόψε ό,τι ηχεί
            pos %= length;
        }
        if (doReanchor) reanchorIfValid (offset, info, (double) tEnd + posFrac);
    }

    void closeFirstLoop (int offset, const BlockInfo& info)
    {
        const int64_t raw = pos;
        auto& L = layers[0];
        const int64_t len = quantizeLength (raw, info);

        wrapEvents (L, len);

        // κλείσε νότες/sustain που κρατιούνται: διάρκεια = όση κράτησε ως το πάτημα
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

        refBpm = curBpm;
        L.len = len; L.muted = false;
        length = len;
        layerCount = 1;
        dirty = true;
        // Το loop συνεχίζει από το σημείο που βρισκόμαστε (όχι από την αρχή), ώστε η φάση του
        // να μένει δεμένη με την αρχή της ηχογράφησης και, άρα, με το πλέγμα του host.
        pos = raw % len; posFrac = 0.0;
        setAnchors (offset, info, (double) raw);
        if (settings.afterRecord == AfterRecord::Overdub)
        {
            pos = 0;   // το πρώτο loop ξαναρχίζει από την αρχή και το overdub ηχογραφείται από εκεί
            setAnchors (offset, info, 0.0);
            beginOverdub (offset, info);
        }
        else state = State::Playing;
    }

    void undoLast (int offset, const BlockInfo& info, std::vector<OutEvent>& out)
    {
        if (layerCount == 0) return;
        --layerCount;
        releaseLayer (layerCount, offset, out);
        releaseControllers (offset, out);
        auto& L = layers[(size_t) layerCount];
        L.events.clear(); L.len = 0; L.muted = false;
        tapCount = 0; odArmed = false;
        dirty = true;
        if (layerCount == 0)
        {
            length = 0; pos = 0; posFrac = 0.0; state = State::Empty;
            anchorValid = false; anchorPpqValid = false; pausedByHost = false;
        }
        else
        {
            length = layers[(size_t) layerCount - 1].len;
            pos %= length;
            reanchorIfValid (offset, info, (double) pos + posFrac);
        }
    }

    void toggleLayer (int idx, int offset, std::vector<OutEvent>& out)
    {
        auto& L = layers[(size_t) idx];
        L.muted = ! L.muted;
        dirty = true;
        if (L.muted)
        {
            releaseLayer (idx, offset, out);
            releaseControllers (offset, out);
        }
    }

    // Πολλαπλά πατήματα STOP: όταν περάσει το παράθυρο, ο αριθμός των πατημάτων δείχνει το layer.
    int flushTaps (int cursor, int limit, bool inclusive, std::vector<OutEvent>& out)
    {
        if (tapCount <= 0) return cursor;
        const int64_t e = tapExpiry - clock;
        if (inclusive ? (e <= limit) : (e < limit))
        {
            const int eo = (int) std::min<int64_t> (std::max<int64_t> (e, cursor), limit);
            advance (cursor, eo, out);
            const int n = tapCount;
            tapCount = 0;
            if (n >= 1 && n <= layerCount && (state == State::Playing || state == State::Overdubbing))
                toggleLayer (n - 1, eo, out);
            return eo;
        }
        return cursor;
    }

    void doCommand (Command c, int offset, const BlockInfo& info, std::vector<OutEvent>& out)
    {
        switch (c)
        {
            case Command::Main:
                switch (state)
                {
                    case State::Empty:
                        for (auto& l : layers) { l.events.clear(); l.mine.fill (0); l.len = 0; l.muted = false; }
                        layerCount = 0; length = 0;
                        discardHeld();
                        tapCount = 0;
                        pos = 0; posFrac = 0.0;
                        dirty = true;
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
                    case State::Playing:
                        if (odArmed) { odArmed = false; break; }                 // δεύτερο πάτημα = ακύρωση της αναμονής
                        if (layerCount >= kMaxLayers) break;
                        if (settings.startQuantize != Quantize::Off)
                        {
                            int64_t b = 0;
                            if (nextGridBoundary (offset, info, b) && b > info.hostSample + offset)
                            {
                                armedBoundary = b;                               // το loop συνεχίζει μέχρι το πλέγμα
                                odArmed = true;
                                break;
                            }
                        }
                        startOverdubNow (offset, info, out);
                        break;
                    case State::Overdubbing: commitOverdub (offset, info, out, true); break;
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
                    // ακύρωση του τρέχοντος overdub (δεν έχει γίνει ακόμα layer)
                    pending().events.clear(); discardHeld();
                    tapCount = 0;
                    state = State::Playing;
                    if (pos >= length) { onWrap (offset, out); pos %= length; }
                    reanchorIfValid (offset, info, (double) pos + posFrac);
                }
                else if (state == State::Playing)
                {
                    if (odArmed) odArmed = false;   // το UNDO ακυρώνει πρώτα την αναμονή
                    else undoLast (offset, info, out);
                }
                break;

            case Command::Stop:
                if (layerCount > 0 && (state == State::Playing || state == State::Overdubbing))
                {
                    if (tapCount < 99) ++tapCount;
                    tapExpiry = clock + offset + (int64_t) (settings.tapWindowMs * 0.001 * sr);
                }
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
