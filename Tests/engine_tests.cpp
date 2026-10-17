// Αυτόνομα tests για τη μηχανή. Build: g++ -std=c++17 -O1 -fsanitize=address,undefined engine_tests.cpp
#include "../Source/LooperEngine.h"
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

using namespace ml;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Out { int64_t t; uint8_t s, a, b; bool operator== (const Out& o) const { return t == o.t && s == o.s && a == o.a && b == o.b; } };

struct Script   // γεγονότα σε απόλυτο χρόνο (samples)
{
    struct Item { int64_t t; bool cmd; Command c; Msg m; };
    std::vector<Item> items;
    void cmd (int64_t t, Command c) { items.push_back ({ t, true, c, {} }); }
    void midi (int64_t t, uint8_t s, uint8_t a, uint8_t b) { Msg m; m.b[0] = s; m.b[1] = a; m.b[2] = b; m.size = 3; items.push_back ({ t, false, Command::Main, m }); }
    void on  (int64_t t, int n, int v = 100) { midi (t, 0x90, (uint8_t) n, (uint8_t) v); }
    void off (int64_t t, int n)              { midi (t, 0x80, (uint8_t) n, 0); }
};

struct Host { bool playing = false; int64_t origin = 0; double bpm = 0; int bpb = 0; bool jumpAt = false; };

// Εκτελεί το script με μέγεθος block bs. hostFn(t) δίνει θέση host (ή -1 = άγνωστη).
static std::vector<Out> run (const Script& sc, int64_t total, int bs, const Settings& st,
                             std::function<int64_t (int64_t)> hostPos = nullptr, double bpm = 0.0,
                             LooperEngine* engOut = nullptr)
{
    static LooperEngine local;
    LooperEngine& eng = engOut ? *engOut : local;
    eng.prepare (48000.0);
    std::vector<Out> res;
    std::vector<OutEvent> out; out.reserve (4096);
    std::vector<InEvent> in; in.reserve (4096);

    for (int64_t t0 = 0; t0 < total; t0 += bs)
    {
        const int n = (int) std::min<int64_t> (bs, total - t0);
        in.clear(); out.clear();
        for (auto& it : sc.items)
            if (it.t >= t0 && it.t < t0 + n)
            {
                InEvent e; e.offset = (int) (it.t - t0); e.isCommand = it.cmd; e.cmd = it.c; e.msg = it.m;
                in.push_back (e);
            }
        BlockInfo info; info.numSamples = n; info.bpm = bpm;
        if (hostPos)
        {
            const int64_t h = hostPos (t0);
            info.hostPlaying = h >= 0; info.hostPositionValid = h >= 0; info.hostSample = h >= 0 ? h : 0;
            if (h >= 0 && bpm > 0.0) { info.ppqValid = true; info.ppq = (double) h * bpm / (60.0 * 48000.0); }
        }
        eng.process (info, st, in.data(), (int) in.size(), out);
        for (auto& o : out) res.push_back ({ t0 + o.offset, o.msg.b[0], o.msg.b[1], o.msg.b[2] });
    }
    return res;
}

static std::vector<Out> only (const std::vector<Out>& v, int64_t from, bool noPass = false)
{
    std::vector<Out> r;
    for (auto& o : v) if (o.t >= from) r.push_back (o);
    (void) noPass; return r;
}

// κάθε note-on πρέπει να έχει αντίστοιχο off στο τέλος
static bool balanced (const std::vector<Out>& v)
{
    std::map<int, int> c;
    for (auto& o : v)
    {
        const int k = (o.s & 0x0F) * 128 + o.a;
        if ((o.s & 0xF0) == 0x90 && o.b > 0) ++c[k];
        else if ((o.s & 0xF0) == 0x80 || (o.s & 0xF0) == 0x90) --c[k];
    }
    for (auto& kv : c) if (kv.second != 0) return false;
    return true;
}

static Settings noThru() { Settings s; s.passThrough = false; return s; }

static Script basicScript()
{
    Script s;
    s.cmd (1000, Command::Main);         // αρχή ηχογράφησης
    s.on (1100, 60); s.off (1600, 60);
    s.cmd (3000, Command::Main);         // μήκος = 2000
    return s;
}

int main()
{
    // 1) free-length: το μήκος ορίζεται από το δεύτερο πάτημα
    {
        auto o = run (basicScript(), 11000, 64, noThru());
        std::vector<Out> expect;
        for (int p = 0; p < 4; ++p) { expect.push_back ({ 3000 + p * 2000 + 100, 0x90, 60, 100 });
                                      expect.push_back ({ 3000 + p * 2000 + 600, 0x80, 60, 0 }); }
        CHECK (o == expect, "free-length loop: %zu γεγονότα αντί για %zu", o.size(), expect.size());
    }

    // 2) ανεξαρτησία από μέγεθος block (sample-accurate)
    {
        auto ref = run (basicScript(), 11000, 64, noThru());
        for (int bs : { 1, 7, 100, 333, 512, 1024, 4096 })
        {
            auto o = run (basicScript(), 11000, bs, noThru());
            CHECK (o == ref, "block size %d δίνει διαφορετική έξοδο", bs);
        }
    }

    // 3) stuck notes: νότα που κρατιέται όταν κλείνει το loop
    {
        Script s; s.cmd (1000, Command::Main); s.on (1500, 64); s.cmd (3000, Command::Main); // ποτέ off
        auto o = run (s, 11000, 128, noThru());   // τελειώνει ανάμεσα σε περάσματα
        CHECK (balanced (o), "stuck note στο κλείσιμο του loop");
        bool hasOff = false; for (auto& e : o) if ((e.s & 0xF0) == 0x80) hasOff = true;
        CHECK (hasOff, "δεν στάλθηκε note-off");
    }

    // 4) stuck notes: stop στη μέση νότας
    {
        Script s = basicScript(); s.cmd (3000 + 300, Command::Stop); // νότα ηχεί 100..600
        s.items.clear(); s.cmd (1000, Command::Main); s.on (1100, 60); s.off (1900, 60); s.cmd (3000, Command::Main);
        s.cmd (3000 + 500, Command::Stop);   // η νότα (100..900) ηχεί
        auto o = run (s, 9000, 100, noThru());
        CHECK (balanced (o), "stuck note μετά το Stop");
        int64_t lastOff = -1; for (auto& e : o) if ((e.s & 0xF0) == 0x80) lastOff = e.t;
        CHECK (lastOff == 3500, "το off του Stop έπρεπε να είναι στο 3500, είναι %lld", (long long) lastOff);
    }

    // 5) overdub + undo, με νότα που ηχεί την ώρα του undo
    {
        Script s = basicScript();
        s.cmd (5000, Command::Main);                       // overdub (θέση 0 στο loop στα 5000)
        s.on (5200, 72); s.off (5400, 72);                 // layer 2: 72 στη θέση 200..400
        s.cmd (6000, Command::Main);                       // τέλος overdub → Playing
        s.cmd (7300, Command::Undo);                       // undo (θέση 1300 → 7300 δεν ηχεί 72)
        auto o = run (s, 13000, 97, noThru());
        int cnt72before = 0, cnt72after = 0;
        for (auto& e : o) if (e.a == 72 && (e.s & 0xF0) == 0x90) { if (e.t < 7300) ++cnt72before; else ++cnt72after; }
        CHECK (cnt72before >= 1, "το overdub δεν παίχτηκε");
        CHECK (cnt72after == 0, "το undo δεν αφαίρεσε το layer (%d)", cnt72after);
        CHECK (balanced (o), "stuck notes μετά το undo");
        int base = 0; for (auto& e : o) if (e.a == 60 && (e.s & 0xF0) == 0x90 && e.t > 7300) ++base;
        CHECK (base >= 2, "το πρώτο layer έπρεπε να συνεχίσει (%d)", base);
    }

    // 6) undo την ώρα που ηχεί νότα του layer → note-off
    {
        Script s;
        s.cmd (0 + 1, Command::Main);
        s.cmd (4001, Command::Main);                       // length 4000, κενό layer 0
        s.cmd (4001 + 100, Command::Main);                 // overdub από θέση 100
        s.on (4001 + 200, 65); s.off (4001 + 3000, 65);    // 65 ηχεί 200..3000
        s.cmd (4001 + 3500, Command::Main);                // τέλος overdub
        s.cmd (4001 + 4000 + 1000, Command::Undo);         // undo ενώ ηχεί η 65 (στο 2ο πέρασμα, θέση 1000)
        auto o = run (s, 20000, 64, noThru());
        CHECK (balanced (o), "νότα έμεινε αναμμένη μετά το undo");
        int64_t offT = -1; for (auto& e : o) if (e.a == 65 && (e.s & 0xF0) == 0x80 && e.t >= 4001 + 4000 + 1000) { offT = e.t; break; }
        CHECK (offT == 4001 + 4000 + 1000, "το off του undo έπρεπε να είναι ακριβώς στο undo (%lld)", (long long) offT);
    }

    // 7) νότα που κρατιέται κατά το wrap σε overdub δεν κολλάει
    {
        Script s;
        s.cmd (0, Command::Main); s.cmd (4000, Command::Main);   // length 4000
        s.cmd (4000 + 100, Command::Main);                       // overdub
        s.on (4000 + 3900, 70);                                  // κρατιέται πάνω από το wrap
        s.off (4000 + 4100, 70);
        s.cmd (4000 + 9000, Command::Main);
        auto o = run (s, 30000, 256, noThru());
        CHECK (balanced (o), "stuck note στο wrap του overdub");
    }

    // 8) quantize
    {
        Settings st = noThru(); st.quantize = Quantize::Bar;   // 120 bpm, 48k → beat 24000, bar 96000
        Script s; s.cmd (0, Command::Main); s.cmd (100000, Command::Main);
        LooperEngine e; run (s, 100001, 512, st, nullptr, 120.0, &e);
        CHECK (e.getLength() == 96000, "bar quantize 100000 → %lld", (long long) e.getLength());

        Script s2; s2.cmd (0, Command::Main); s2.cmd (200000, Command::Main);
        LooperEngine e2; run (s2, 200001, 512, st, nullptr, 120.0, &e2);
        CHECK (e2.getLength() == 192000, "bar quantize 200000 → %lld", (long long) e2.getLength());

        Script s3; s3.cmd (0, Command::Main); s3.cmd (10000, Command::Main);
        LooperEngine e3; run (s3, 10001, 512, st, nullptr, 120.0, &e3);
        CHECK (e3.getLength() == 96000, "ελάχιστο 1 μέτρο → %lld", (long long) e3.getLength());

        Settings sb = noThru(); sb.quantize = Quantize::Beat;
        Script s4; s4.cmd (0, Command::Main); s4.cmd (60000, Command::Main);
        LooperEngine e4; run (s4, 60001, 512, sb, nullptr, 120.0, &e4);
        CHECK (e4.getLength() == 72000, "beat quantize 60000 → %lld", (long long) e4.getLength());
    }

    // 9) quantize προς τα κάτω: τυλίγει τα γεγονότα, χωρίς stuck note
    {
        Settings st = noThru(); st.quantize = Quantize::Bar;
        Script s; s.cmd (0, Command::Main);
        s.on (50000, 60); s.off (60000, 60);
        s.on (97000, 62); s.off (98500, 62);   // μετά το νέο μήκος 96000 → τυλίγεται στο 1000..2500
        s.on (99000, 64);                      // κρατιέται όταν πατηθεί το 2ο Main
        s.cmd (100000, Command::Main);
        auto o = run (s, 500000, 480, st, nullptr, 120.0);
        CHECK (balanced (o), "stuck note με quantize προς τα κάτω");
        // Πάτημα στο 100000 ⇒ θέση loop 4000. Το γεγονός στη θέση 1000 παίζει στο επόμενο πέρασμα.
        bool saw62 = false; for (auto& e : o) if (e.a == 62 && e.t == 100000 + (96000 - 4000) + 1000) saw62 = true;
        CHECK (saw62, "το τυλιγμένο γεγονός δεν έπεσε στη σωστή θέση");
    }

    // 10) redo / clear / όριο layers
    {
        Script s = basicScript();
        s.cmd (5000, Command::Main); s.on (5100, 80); s.off (5300, 80); s.cmd (5500, Command::Main);
        s.cmd (6000, Command::Undo);
        s.cmd (6500, Command::Redo);
        LooperEngine e; run (s, 7000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getLayerCount() == 2 && e.getRedoCount() == 0, "redo: layers=%d redo=%d", e.getLayerCount(), e.getRedoCount());

        Script s2 = basicScript(); s2.cmd (4000, Command::Undo); s2.cmd (4500, Command::Redo);
        LooperEngine e2; run (s2, 6000, 64, noThru(), nullptr, 0.0, &e2);
        CHECK (e2.getState() == State::Playing && e2.getLength() == 2000, "redo από άδειο loop");

        Script s3 = basicScript(); s3.cmd (4000, Command::Clear);
        LooperEngine e3; auto o3 = run (s3, 9000, 64, noThru(), nullptr, 0.0, &e3);
        CHECK (e3.getState() == State::Empty && e3.getLength() == 0, "clear");
        CHECK (only (o3, 4001).empty(), "έξοδος μετά το clear");
        CHECK (balanced (o3), "stuck note μετά το clear");
    }

    // 11) καμία δέσμευση μνήμης: η χωρητικότητα δεν αλλάζει σε μεγάλη ηχογράφηση
    {
        Script s; s.cmd (0, Command::Main);
        for (int i = 0; i < 20000; ++i) { s.on (10 + i * 5LL, 60 + (i % 12)); s.off (13 + i * 5LL, 60 + (i % 12)); }
        s.cmd (120000, Command::Main);
        LooperEngine e; auto o = run (s, 130000, 256, noThru(), nullptr, 0.0, &e);
        CHECK (e.layerCapacity (0) == (size_t) LooperEngine::kEventsPerLayer, "η χωρητικότητα άλλαξε: %zu", e.layerCapacity (0));
        CHECK (e.layerEventCount (0) <= LooperEngine::kEventsPerLayer, "υπερχείλιση layer");
        CHECK (balanced (o), "stuck notes σε γεμάτο layer");
    }

    // 12) pass-through και απενεργοποίησή του με νότα που κρατιέται
    {
        Script s; s.on (100, 60);
        Settings thru; thru.passThrough = true;
        auto o = run (s, 1000, 100, thru);
        CHECK (o.size() == 1 && o[0].t == 100, "pass-through");

        // αλλαγή ρύθμισης στη μέση: με δύο runs δεν γίνεται, οπότε δοκιμάζουμε απευθείας
        LooperEngine e; e.prepare (48000.0);
        std::vector<OutEvent> out; out.reserve (64);
        InEvent ev; ev.offset = 10; ev.msg.b[0] = 0x90; ev.msg.b[1] = 60; ev.msg.b[2] = 100; ev.msg.size = 3;
        BlockInfo bi; bi.numSamples = 64;
        e.process (bi, thru, &ev, 1, out);
        out.clear();
        Settings off = thru; off.passThrough = false;
        e.process (bi, off, nullptr, 0, out);
        CHECK (out.size() == 1 && (out[0].msg.b[0] & 0xF0) == 0x80, "το pass-through έκλεισε με νότα κολλημένη");
    }

    // 13) sustain pedal δεν μένει πατημένο
    {
        Script s; s.cmd (0, Command::Main); s.midi (1000, 0xB0, 64, 127); s.cmd (3000, Command::Main);
        auto o = run (s, 21000, 100, noThru());
        int down = 0, up = 0; for (auto& e : o) if ((e.s & 0xF0) == 0xB0 && e.a == 64) { if (e.b >= 64) ++down; else ++up; }
        CHECK (down == up && down > 0, "sustain: down=%d up=%d", down, up);
    }

    // 14) host sync: μετά από άλματα του transport η θέση του loop ακολουθεί το host
    {
        Settings st = noThru(); st.syncToHost = true;
        Script s;
        s.cmd (1000, Command::Main);   // anchor = host 1000
        s.on (1100, 60); s.off (1600, 60);
        s.cmd (3000, Command::Main);   // length 2000
        // host: παίζει κανονικά ως 9000, μετά άλμα στο 100000 (+ 7000 μετά)
        auto hostPos = [] (int64_t t) -> int64_t { return t < 9000 ? t : 100000 + (t - 9000); };
        auto o = run (s, 14000, 64, st, hostPos);
        // μετά το άλμα: host=100000+k. θέση loop = (host-1000) % 2000. Το 60 παίζει όταν θέση=100.
        // host τιμές: (h-1000)%2000==100 → h = 1100 + 2000n → 101100 είναι ο πρώτος μετά το 100000
        bool ok = false; for (auto& e : o) if ((e.s & 0xF0) == 0x90 && e.t == 9000 + (101100 - 100000)) ok = true;
        CHECK (ok, "μετά το άλμα του host το loop δεν ευθυγραμμίστηκε");
        CHECK (balanced (o), "stuck notes μετά το άλμα");

        // παύση transport στη μέση νότας → off
        Script s2 = s;
        auto hostStop = [] (int64_t t) -> int64_t { return t < 3300 ? t : -1; };   // η 60 ηχεί 3100..3600
        auto o2 = run (s2, 8000, 64, st, hostStop);
        CHECK (balanced (o2), "stuck note όταν σταματά το transport");
    }

    // 15) δεύτερο πάτημα στο ίδιο sample → ακύρωση (όχι μήκος 0)
    {
        Script s; s.cmd (500, Command::Main); s.cmd (500, Command::Main);
        LooperEngine e; run (s, 2000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getState() == State::Empty, "διπλό πάτημα στο ίδιο sample");
    }

    // 16) afterRecord = Overdub
    {
        Settings st = noThru(); st.afterRecord = AfterRecord::Overdub;
        LooperEngine e; run (basicScript(), 3500, 64, st, nullptr, 0.0, &e);
        CHECK (e.getState() == State::Overdubbing, "afterRecord=Overdub");
    }


    // ---------- Start quantize ----------
    auto idHost = [] (int64_t t) -> int64_t { return t; };

    // 17) ηχογράφηση που ξεκινά στο επόμενο μέτρο του host (120 bpm: μέτρο = 96000 samples)
    {
        Settings st = noThru(); st.startQuantize = Quantize::Bar; st.quantize = Quantize::Bar; st.syncToHost = true;
        Script s;
        s.cmd (30000, Command::Main);                     // πάτημα μέσα στο μέτρο 0 → περιμένει
        s.on (50000, 61); s.off (55000, 61);              // πριν το πλέγμα: ΔΕΝ ηχογραφείται
        s.on (97000, 60); s.off (98000, 60);              // 1000 samples μετά την αρχή του μέτρου 1
        s.cmd (197000, Command::Main);                    // κλείσιμο μέσα στο μέτρο 2 → μήκος 1 μέτρο
        std::vector<Out> ref;
        for (int bs : { 64, 480, 1000, 1024, 7, 96000 })
        {
            LooperEngine e;
            auto o = run (s, 400000, bs, st, idHost, 120.0, &e);
            if (bs == 64) ref = o;
            CHECK (o == ref, "start quantize: διαφορετική έξοδος με block %d", bs);
            std::vector<Out> expect;
            // το loop ξεκίνησε στο 96000. Περάσματα: 192000 (ήδη στη θέση 5000), 288000, 384000
            for (int64_t base : { 288000, 384000 })
            { expect.push_back ({ base + 1000, 0x90, 60, 100 }); expect.push_back ({ base + 2000, 0x80, 60, 0 }); }
            CHECK (o == expect, "start quantize (block %d): %zu γεγονότα, πρώτο στο %lld", bs, o.size(),
                   o.empty() ? -1LL : (long long) o[0].t);
            CHECK (e.getLength() == 96000, "μήκος μετά το start quantize: %lld", (long long) e.getLength());
        }
    }

    // 18) πάτημα ακριβώς πάνω στο όριο → ξεκινά αμέσως
    {
        Settings st = noThru(); st.startQuantize = Quantize::Bar;
        Script s; s.cmd (96000, Command::Main);
        LooperEngine e; run (s, 96100, 512, st, idHost, 120.0, &e);
        CHECK (e.getState() == State::Recording && e.getPosition() == 100, "πάτημα πάνω στο όριο: state=%d pos=%lld",
               (int) e.getState(), (long long) e.getPosition());
    }

    // 19) ο host σταματά όσο περιμένουμε → ακύρωση
    {
        Settings st = noThru(); st.startQuantize = Quantize::Bar;
        Script s; s.cmd (30000, Command::Main);
        auto hostStop = [] (int64_t t) -> int64_t { return t < 40000 ? t : -1; };
        LooperEngine e; run (s, 120000, 256, st, hostStop, 120.0, &e);
        CHECK (e.getState() == State::Empty, "Armed δεν ακυρώθηκε όταν σταμάτησε ο host (state=%d)", (int) e.getState());
    }

    // 20) δεύτερο πάτημα ενώ περιμένει → ακύρωση, και το Stop/Undo επίσης
    {
        Settings st = noThru(); st.startQuantize = Quantize::Bar;
        Script s; s.cmd (30000, Command::Main); s.cmd (40000, Command::Main);
        LooperEngine e; run (s, 50000, 256, st, idHost, 120.0, &e);
        CHECK (e.getState() == State::Empty, "δεύτερο πάτημα δεν ακύρωσε");
        Script s2; s2.cmd (30000, Command::Main); s2.cmd (40000, Command::Stop);
        LooperEngine e2; run (s2, 50000, 256, st, idHost, 120.0, &e2);
        CHECK (e2.getState() == State::Empty, "Stop δεν ακύρωσε");
        Script s3; s3.cmd (30000, Command::Main); s3.cmd (40000, Command::Undo);
        LooperEngine e3; run (s3, 50000, 256, st, idHost, 120.0, &e3);
        CHECK (e3.getState() == State::Empty, "Undo δεν ακύρωσε");
    }

    // 21) νότα ακριβώς στο sample του ορίου ηχογραφείται στη θέση 0
    {
        Settings st = noThru(); st.startQuantize = Quantize::Bar; st.quantize = Quantize::Bar;
        Script s; s.cmd (30000, Command::Main); s.on (96000, 62); s.off (96500, 62); s.cmd (192000, Command::Main);
        for (int bs : { 1000, 1024, 96000 })
        {
            auto o = run (s, 200000, bs, st, idHost, 120.0);
            CHECK (!o.empty() && o[0].t == 192000 && o[0].a == 62, "νότα στο όριο (block %d): πρώτο γεγονός στο %lld", bs,
                   o.empty() ? -1LL : (long long) o[0].t);
        }
    }

    // 22) start quantize σε beat (24000 samples) + length quantize σε beat
    {
        Settings st = noThru(); st.startQuantize = Quantize::Beat; st.quantize = Quantize::Beat; st.syncToHost = true;
        Script s;
        s.cmd (30000, Command::Main);                    // επόμενο beat: 48000
        s.on (49000, 60); s.off (50000, 60);             // θέση 1000..2000 του loop
        s.cmd (75000, Command::Main);                    // raw = 27000 → μήκος 24000, θέση 3000
        LooperEngine e;
        auto o = run (s, 130000, 333, st, idHost, 120.0, &e);
        CHECK (e.getLength() == 24000, "beat start/length quantize: μήκος %lld", (long long) e.getLength());
        bool ok = false; for (auto& x : o) if (x.t == 96000 + 1000 && x.a == 60 && (x.s & 0xF0) == 0x90) ok = true;
        CHECK (ok, "το loop δεν είναι δεμένο στο πλέγμα του beat");
        CHECK (balanced (o), "stuck note με beat quantize");
    }

    // 23) χωρίς host που παίζει, το start quantize δεν μπλοκάρει: ξεκινά αμέσως
    {
        Settings st = noThru(); st.startQuantize = Quantize::Bar;
        Script s; s.cmd (30000, Command::Main);
        LooperEngine e; run (s, 31000, 256, st, nullptr, 0.0, &e);
        CHECK (e.getState() == State::Recording, "χωρίς host δεν ξεκίνησε η ηχογράφηση");
    }

    if (failures == 0) std::printf("ΟΛΑ ΤΑ TESTS ΠΕΡΑΣΑΝ\n");
    else std::printf("%d αποτυχίες\n", failures);
    return failures == 0 ? 0 : 1;
}
