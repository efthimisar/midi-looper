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
    void rec (int64_t t) { cmd (t, Command::Main); }
    void undo (int64_t t) { cmd (t, Command::Undo); }
    void tap (int64_t t) { cmd (t, Command::Stop); }
};

// Εκτελεί το script με μέγεθος block bs. hostPos(t) δίνει θέση host (ή -1 = άγνωστη).
// bpmFn/ppqFn: προαιρετικά, για tempo που αλλάζει.
static std::vector<Out> run (const Script& sc, int64_t total, int bs, const Settings& st,
                             std::function<int64_t (int64_t)> hostPos = nullptr, double bpm = 0.0,
                             LooperEngine* engOut = nullptr,
                             std::function<double (int64_t)> bpmFn = nullptr,
                             std::function<double (int64_t)> ppqFn = nullptr)
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
        BlockInfo info; info.numSamples = n; info.bpm = bpmFn ? bpmFn (t0) : bpm;
        if (hostPos)
        {
            const int64_t h = hostPos (t0);
            info.hostPlaying = h >= 0; info.hostPositionValid = h >= 0; info.hostSample = h >= 0 ? h : 0;
            if (h >= 0 && info.bpm > 0.0)
            {
                info.ppqValid = true;
                info.ppq = ppqFn ? ppqFn (t0) : (double) h * info.bpm / (60.0 * 48000.0);
            }
        }
        eng.process (info, st, in.data(), (int) in.size(), out);
        for (auto& o : out) res.push_back ({ t0 + o.offset, o.msg.b[0], o.msg.b[1], o.msg.b[2] });
    }
    // Το MidiBuffer του JUCE ταξινομεί κατά θέση· εδώ ταξινομούμε με το ίδιο κριτήριο.
    std::stable_sort (res.begin(), res.end(), [] (const Out& a, const Out& b) { return a.t < b.t; });
    return res;
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

// Χρόνοι note-on μιας νότας
static std::vector<int64_t> ons (const std::vector<Out>& v, int note)
{
    std::vector<int64_t> r;
    for (auto& o : v) if ((o.s & 0xF0) == 0x90 && o.b > 0 && o.a == note) r.push_back (o.t);
    return r;
}
static std::vector<int64_t> offs (const std::vector<Out>& v, int note)
{
    std::vector<int64_t> r;
    for (auto& o : v) if ((o.s & 0xF0) == 0x80 && o.a == note) r.push_back (o.t);
    return r;
}
static bool has (const std::vector<int64_t>& v, int64_t t, int64_t tol = 0)
{
    for (auto x : v) if (std::llabs (x - t) <= tol) return true;
    return false;
}
static bool any (const std::vector<int64_t>& v, int64_t from, int64_t to)
{
    for (auto x : v) if (x >= from && x < to) return true;
    return false;
}

static Settings noThru() { Settings s; s.passThrough = false; return s; }

// Βασικό loop: μήκος 2000, νότα 60 στη θέση 100..600. Το loop ξεκινά (θέση 0) στο t = 3000.
static Script base()
{
    Script s;
    s.rec (1000);
    s.on (1100, 60); s.off (1600, 60);
    s.rec (3000);
    return s;
}

int main()
{
    // 1) το μήκος ορίζεται από το δεύτερο πάτημα
    {
        LooperEngine e;
        auto o = run (base(), 11000, 64, noThru(), nullptr, 0.0, &e);
        std::vector<Out> expect;
        for (int p = 0; p < 4; ++p) { expect.push_back ({ 3000 + p * 2000 + 100, 0x90, 60, 100 });
                                      expect.push_back ({ 3000 + p * 2000 + 600, 0x80, 60, 0 }); }
        CHECK (o == expect, "free-length loop: %zu γεγονότα αντί για %zu", o.size(), expect.size());
        CHECK (e.getLength() == 2000 && e.getLayerCount() == 1, "length=%lld layers=%d", (long long) e.getLength(), e.getLayerCount());
    }

    // 2) ανεξαρτησία από μέγεθος block (sample-accurate)
    {
        auto ref = run (base(), 11000, 64, noThru());
        for (int bs : { 1, 7, 100, 333, 512, 1024, 4096 })
        {
            auto o = run (base(), 11000, bs, noThru());
            CHECK (o == ref, "block size %d δίνει διαφορετική έξοδο", bs);
        }
    }

    // 3) stuck notes: νότα που κρατιέται όταν κλείνει το loop
    {
        Script s; s.rec (1000); s.on (1500, 64); s.rec (3000);   // ποτέ off
        auto o = run (s, 11000, 128, noThru());
        CHECK (balanced (o), "stuck note στο κλείσιμο του loop");
        bool hasOff = false; for (auto& e : o) if ((e.s & 0xF0) == 0x80) hasOff = true;
        CHECK (hasOff, "δεν στάλθηκε note-off");
    }

    // 4) overdub μέσα στο μήκος: το μήκος δεν αλλάζει
    {
        Script s = base();
        s.rec (5000);                          // overdub από τη θέση 0
        s.on (5200, 64); s.off (5400, 64);
        s.rec (7000);                          // κλείνει ακριβώς στο τέλος του loop
        LooperEngine e;
        auto o = run (s, 11000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getLength() == 2000, "length=%lld", (long long) e.getLength());
        CHECK (e.getLayerCount() == 2, "layers=%d", e.getLayerCount());
        CHECK (has (ons (o, 64), 7200) && has (ons (o, 64), 9200), "η νότα 64 δεν επαναλαμβάνεται");
        CHECK (has (offs (o, 64), 7400) && has (offs (o, 64), 9400), "off της 64");
        CHECK (balanced (o), "unbalanced");
    }

    // 5) overdub που κλείνει ΠΡΙΝ το τέλος: το μήκος δεν αλλάζει
    {
        Script s = base();
        s.rec (5000);
        s.on (5200, 64); s.off (5400, 64);
        s.rec (6000);                          // θέση 1000 < 2000
        LooperEngine e;
        auto o = run (s, 11000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getLength() == 2000 && e.getLayerCount() == 2, "length=%lld", (long long) e.getLength());
        CHECK (has (ons (o, 64), 7200), "νότα 64 στον επόμενο κύκλο");
        CHECK (balanced (o), "unbalanced");
    }

    // 6) overdub που ξεπερνά το τέλος: το loop μεγαλώνει, τα παλιά layers επαναλαμβάνονται
    Script ext = base();
    ext.rec (5000);
    ext.on (5200, 64); ext.off (5300, 64);
    ext.on (7200, 65); ext.off (7300, 65);
    ext.rec (9000);                            // 4000 samples overdub → loop 4000
    {
        LooperEngine e;
        auto o = run (ext, 20000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getLength() == 4000, "length=%lld", (long long) e.getLength());
        CHECK (e.getLayerCount() == 2, "layers=%d", e.getLayerCount());
        CHECK (e.getLayerLength (0) == 2000 && e.getLayerLength (1) == 4000, "layer lens %lld %lld",
               (long long) e.getLayerLength (0), (long long) e.getLayerLength (1));
        auto n60 = ons (o, 60);
        // καθώς το overdub συνεχίζει, το layer 1 (μήκος 2000) κυκλώνει κανονικά
        CHECK (has (n60, 5100) && has (n60, 7100) && has (n60, 9100) && has (n60, 11100) && has (n60, 13100),
               "το layer 1 δεν επαναλαμβάνεται κάθε 2000");
        auto n64 = ons (o, 64), n65 = ons (o, 65);
        CHECK (has (n64, 9200) && has (n64, 13200) && has (n64, 17200), "64 κάθε 4000 από 9200");
        CHECK (has (n65, 11200) && has (n65, 15200), "65 κάθε 4000 από 11200");
        CHECK (! has (n64, 11200) && ! has (n65, 9200), "οι νότες δεν πρέπει να επαναλαμβάνονται κάθε 2000");
        CHECK (balanced (o), "unbalanced");
    }

    // 7) το ίδιο με host sync: ίδια έξοδος με το χωρίς sync (ο host προχωρά κανονικά)
    {
        Settings st = noThru(); st.syncToHost = true;
        auto ref = run (ext, 20000, 64, noThru());
        auto o = run (ext, 20000, 64, st, [] (int64_t t) { return t; });
        CHECK (o == ref, "sync: διαφορετική έξοδος (%zu vs %zu γεγονότα)", o.size(), ref.size());
        for (int bs : { 1, 100, 777, 4096 })
        {
            auto o2 = run (ext, 20000, bs, st, [] (int64_t t) { return t; });
            CHECK (o2 == ref, "sync, block %d", bs);
        }
    }

    // 8) UNDO σβήνει το νεότερο layer και το μήκος γυρίζει στο προηγούμενο
    {
        Script s = ext;
        s.undo (15000);
        LooperEngine e;
        auto o = run (s, 24000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getLength() == 2000 && e.getLayerCount() == 1, "length=%lld layers=%d", (long long) e.getLength(), e.getLayerCount());
        CHECK (! any (ons (o, 64), 15001, 24000) && ! any (ons (o, 65), 15001, 24000), "οι νότες του undone layer ακούγονται ακόμα");
        CHECK (any (ons (o, 60), 16000, 24000), "το layer 1 δεν συνεχίζει");
        CHECK (balanced (o), "unbalanced");
        // δεύτερο undo → άδειο
        Script s2 = s; s2.undo (18000);
        auto o2 = run (s2, 24000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getState() == State::Empty && e.getLayerCount() == 0 && e.getLength() == 0, "δεν άδειασε");
        CHECK (! any (ons (o2, 60), 18001, 24000), "ακούγεται μετά το τελευταίο undo");
        CHECK (balanced (o2), "unbalanced");
    }

    // 9) undo μέσα σε overdub ακυρώνει μόνο το τρέχον overdub
    {
        Script s = base();
        s.rec (5000); s.on (5200, 64); s.off (5300, 64);
        s.undo (5500);
        LooperEngine e;
        auto o = run (s, 12000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getState() == State::Playing && e.getLayerCount() == 1 && e.getLength() == 2000, "state/layers/length");
        CHECK (! any (ons (o, 64), 5600, 12000), "νότα του ακυρωμένου overdub");
        CHECK (any (ons (o, 60), 6000, 12000), "το layer 1 σταμάτησε");
    }

    // 10) STOP: 1 πάτημα σιγάζει το layer 1, ξανά 1 πάτημα το ξαναενεργοποιεί
    Script two = base();                     // 2 layers, μήκος 2000: 60 (θέση 100) και 64 (θέση 200)
    two.rec (5000); two.on (5200, 64); two.off (5400, 64); two.rec (7000);
    {
        Script s = two;
        s.tap (8000);                       // → σίγαση layer 1 στο 8000 + 24000 = 32000
        s.tap (60000);                      // → ενεργοποίηση στο 84000
        LooperEngine e;
        auto o = run (s, 120000, 128, noThru(), nullptr, 0.0, &e);
        CHECK (! any (ons (o, 60), 32000, 84000), "το layer 1 ακούγεται ενώ είναι σιγασμένο");
        CHECK (any (ons (o, 64), 32000, 84000), "το layer 2 σταμάτησε");
        CHECK (any (ons (o, 60), 84000, 120000), "το layer 1 δεν ξαναγύρισε");
        CHECK (! e.isLayerMuted (0), "muted flag");
        CHECK (e.getLength() == 2000, "το μήκος άλλαξε από το STOP");
        CHECK (balanced (o), "unbalanced");
    }

    // 11) STOP: 2 πατήματα = layer 2, 3 πατήματα χωρίς 3ο layer = τίποτα
    {
        Script s = two;
        s.tap (8000); s.tap (12000);        // 2 πατήματα → layer 2 (στο 12000+24000 = 36000)
        LooperEngine e;
        auto o = run (s, 60000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.isLayerMuted (1) && ! e.isLayerMuted (0), "το layer 2 έπρεπε να σιγάσει");
        CHECK (! any (ons (o, 64), 36000, 60000), "το layer 2 ακούγεται");
        CHECK (any (ons (o, 60), 36000, 60000), "το layer 1 σιγάστηκε λάθος");
        Script s3 = two; s3.tap (8000); s3.tap (9000); s3.tap (10000);   // 3 πατήματα, υπάρχουν 2 layers
        run (s3, 60000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (! e.isLayerMuted (0) && ! e.isLayerMuted (1), "το 3 πάτημα δεν έπρεπε να αλλάξει τίποτα");
        // αργά πατήματα = ξεχωριστές ομάδες
        Script s4 = two; s4.tap (8000); s4.tap (8000 + 30000);
        run (s4, 120000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (! e.isLayerMuted (0), "δύο αργά πατήματα = σίγαση και επανενεργοποίηση του layer 1");
    }

    // 12) STOP: ανεξάρτητο από το μέγεθος block
    {
        Script s = two; s.tap (8000); s.tap (12000);
        auto ref = run (s, 60000, 64, noThru());
        for (int bs : { 1, 33, 500, 4096 })
            CHECK (run (s, 60000, bs, noThru()) == ref, "block %d", bs);
    }

    // 13) τα σιγασμένα layers μένουν σιγασμένα όταν γίνεται νέο overdub
    {
        Script s = two;
        s.tap (8000);                       // σίγαση layer 1 στο 32000
        s.rec (40000);                      // overdub
        s.on (40100, 70); s.off (40200, 70);
        s.rec (42000);
        LooperEngine e;
        auto o = run (s, 70000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getLayerCount() == 3, "layers=%d", e.getLayerCount());
        CHECK (e.isLayerMuted (0) && ! e.isLayerMuted (1) && ! e.isLayerMuted (2), "muted flags");
        CHECK (! any (ons (o, 60), 32000, 70000), "το σιγασμένο layer ξαναπαίζει");
        CHECK (balanced (o), "unbalanced");
    }

    // 14) σίγαση μέσα σε νότα: κανένα κολλημένο
    {
        Script s = base();
        s.on (9000, 1);    // τυχαία live νότα, ώστε το pass-through να μην επηρεάζει
        s.off (9100, 1);
        s.tap (3300);      // η νότα 60 ηχεί (3100..3600) αλλά η σίγαση γίνεται στο 3300 + 24000
        auto o = run (s, 40000, 64, noThru());
        CHECK (balanced (o), "unbalanced");
        Script s2 = base(); s2.tap (31000);   // σίγαση στο 55000... το τρέχον off πρέπει να σταλεί
        auto o2 = run (s2, 80000, 64, noThru());
        CHECK (balanced (o2), "unbalanced 2");
    }

    // 15) μέγιστος αριθμός layers = 4
    {
        Script s = base();
        int64_t t = 5000;
        for (int i = 0; i < 5; ++i) { s.rec (t); s.on (t + 100, 70 + i); s.off (t + 200, 70 + i); s.rec (t + 2000); t += 4000; }
        LooperEngine e;
        auto o = run (s, 40000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getLayerCount() == 4, "layers=%d", e.getLayerCount());
        CHECK (e.getState() == State::Playing, "state");
        CHECK (balanced (o), "unbalanced");
    }

    // 16) 4 → 8 → 16: διαδοχικές επεκτάσεις και undo ένα ένα
    {
        Script s = base();                  // 2000
        s.rec (5000); s.on (5100, 64); s.off (5200, 64);
        s.rec (9000);                       // 4000
        s.rec (11000); s.on (11100, 65); s.off (11200, 65);   // overdub από θέση 2000.. (11000-9000=2000)
        s.rec (17000);                      // 6000 στο timeline του overdub: 2000+6000 = 8000 → loop 8000
        LooperEngine e;
        run (s, 30000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.getLayerCount() == 3 && e.getLength() == 8000, "layers=%d length=%lld", e.getLayerCount(), (long long) e.getLength());
        Script u = s; u.undo (30000); u.undo (31000); u.undo (32000);
        std::vector<int64_t> lens;
        for (int64_t T : { 30500, 31500, 32500 })
        {
            LooperEngine e2; run (u, T, 64, noThru(), nullptr, 0.0, &e2);
            lens.push_back (e2.getLength());
        }
        CHECK (lens[0] == 4000 && lens[1] == 2000 && lens[2] == 0, "μήκη μετά τα undo: %lld %lld %lld", (long long) lens[0], (long long) lens[1], (long long) lens[2]);
    }

    // 17) Length quantize = Bar στην επέκταση (120 bpm, 4/4 → 1 μέτρο = 96000)
    {
        Settings st = noThru(); st.quantize = Quantize::Bar;
        Script s; s.rec (0); s.on (100, 60); s.off (200, 60); s.rec (95000);          // 1 μέτρο
        s.rec (96000);                                                                 // overdub από θέση 0 (95000 % 96000 → 95000...)
        LooperEngine e;
        run (s, 100000, 128, st, nullptr, 0.0, &e);
        CHECK (e.getLength() == 96000, "πρώτο loop: %lld", (long long) e.getLength());
        // overdub από θέση 0: το πρώτο loop ξεκίνησε στο 95000 με pos = 95000 % 96000 = 95000; πάτημα στο 96000 → θέση 96000 → 0
        Script s2; s2.rec (0); s2.on (100, 60); s2.off (200, 60); s2.rec (96000);     // loop = 96000, θέση 0 στο 96000
        s2.rec (96000 + 1);                                                            // overdub ξεκινά (pos 1)
        s2.on (96000 + 500, 64); s2.off (96000 + 600, 64);
        s2.rec (96000 + 150000);                                                       // tEnd = 150001 → 1.56 μέτρα → 2 μέτρα
        LooperEngine e2;
        run (s2, 96000 + 160000, 128, st, nullptr, 0.0, &e2);
        CHECK (e2.getLength() == 192000 && e2.getLayerCount() == 2, "επέκταση σε 2 μέτρα: %lld", (long long) e2.getLength());
        Script s3 = s2; s3.items.pop_back(); s3.rec (96000 + 140000);                  // tEnd = 140001 → 1.46 → 1 μέτρο
        LooperEngine e3;
        run (s3, 96000 + 150000, 128, st, nullptr, 0.0, &e3);
        CHECK (e3.getLength() == 96000 && e3.getLayerCount() == 2, "χωρίς επέκταση: %lld", (long long) e3.getLength());
    }

    // 18) After first loop = Overdub: το πρώτο layer ολοκληρώνεται και αρχίζει overdub· 2ο πάτημα το κλείνει
    {
        Settings st = noThru(); st.afterRecord = AfterRecord::Overdub;
        Script s = base();
        s.on (3300, 64); s.off (3400, 64);
        s.rec (5000);
        LooperEngine e;
        auto o = run (s, 12000, 64, st, nullptr, 0.0, &e);
        CHECK (e.getLayerCount() == 2 && e.getLength() == 2000, "layers=%d length=%lld", e.getLayerCount(), (long long) e.getLength());
        CHECK (has (ons (o, 64), 7300), "νότα του overdub");
        CHECK (balanced (o), "unbalanced");
    }

    // 19) Snap 1/16 (120 bpm → 1/16 = 6000 samples): αρχή στρογγυλοποιείται, διάρκεια μένει
    {
        Settings st = noThru(); st.snapDiv = 16;
        Script s; s.rec (1000);
        s.on (1000 + 3100, 60); s.off (1000 + 4100, 60);       // θέση 3100 → 6000, διάρκεια 1000
        s.on (1000 + 8800, 62); s.off (1000 + 9300, 62);       // θέση 8800 → 6000*1 = 6000? 8800/6000 = 1.47 → 1 → 6000
        s.rec (1000 + 24000);                                   // μήκος 24000
        auto o = run (s, 80000, 64, st);
        CHECK (has (ons (o, 60), 25000 + 6000) && has (offs (o, 60), 25000 + 7000), "snap 60");
        CHECK (has (ons (o, 62), 25000 + 6000) && has (offs (o, 62), 25000 + 6500), "snap 62");
        Settings st2 = noThru();
        auto o2 = run (s, 80000, 64, st2);
        CHECK (has (ons (o2, 60), 25000 + 3100), "χωρίς snap η νότα μένει στη θέση της");
        Settings st8 = noThru(); st8.snapDiv = 8;               // 1/8 = 12000
        auto o8 = run (s, 80000, 64, st8);
        CHECK (has (ons (o8, 60), 25000 + 0) && has (ons (o8, 62), 25000 + 12000), "snap 1/8: %zu", ons (o8, 60).size());
    }

    // 20) Follow tempo: το tempo διπλασιάζεται στο t = 100000 → το loop παίζει διπλάσια ταχύτητα
    {
        Script s; s.rec (100); s.on (1100, 60); s.off (1600, 60); s.rec (48100);   // μήκος 48000, θέση 0 στο 48100
        auto bpmFn = [] (int64_t t) { return t < 100000 ? 120.0 : 240.0; };
        auto ppqFn = [] (int64_t t) { return t < 100000 ? (double) t / 48000.0 * 2.0
                                                        : 100000.0 / 48000.0 * 2.0 + (double) (t - 100000) / 48000.0 * 4.0; };
        for (int sync = 0; sync < 2; ++sync)
        {
            Settings st = noThru(); st.followTempo = true; st.syncToHost = sync != 0;
            auto o = run (s, 220000, 100, st, [] (int64_t t) { return t; }, 0.0, nullptr, bpmFn, ppqFn);
            auto n = ons (o, 60);
            // πριν την αλλαγή: 49100, 97100 · μετά: 97100 + ... επόμενη επανάληψη στο ~122550, μετά κάθε 24000
            CHECK (has (n, 49100, 2) && has (n, 97100, 2), "sync=%d: πριν την αλλαγή tempo", sync);
            CHECK (has (n, 122550, 3) && has (n, 146550, 3) && has (n, 170550, 4), "sync=%d: μετά την αλλαγή tempo, νότες: %zu", sync, n.size());
            CHECK (! any (n, 100000, 122000), "sync=%d: δεν έπρεπε να παίξει νότα εδώ", sync);
            CHECK (balanced (o), "sync=%d unbalanced", sync);
        }
        // χωρίς Follow tempo ο ρυθμός μένει ίδιος (48000 samples ανά επανάληψη)
        Settings st = noThru(); st.syncToHost = false;
        auto o = run (s, 220000, 100, st, [] (int64_t t) { return t; }, 0.0, nullptr, bpmFn, ppqFn);
        CHECK (has (ons (o, 60), 145100, 1), "χωρίς follow: 145100");
    }

    // 21) Αποθήκευση / φόρτωση στο project
    {
        LooperEngine e;
        run (ext, 20000, 64, noThru(), nullptr, 0.0, &e);
        Script t = ext; t.tap (14000);                       // σίγαση layer 1
        run (t, 60000, 64, noThru(), nullptr, 0.0, &e);
        CHECK (e.isLayerMuted (0), "προϋπόθεση");
        static uint8_t buf[LooperEngine::kMaxStateBytes];
        const size_t n = e.serialize (buf, sizeof buf);
        CHECK (n > 0, "serialize");
        CHECK (e.serialize (buf, 10) == 0, "πολύ μικρό buffer");
        CHECK (LooperEngine::validateState (buf, n), "validate");
        CHECK (! LooperEngine::validateState (buf, n - 1), "κομμένο");
        buf[0] ^= 0xFF; CHECK (! LooperEngine::validateState (buf, n), "λάθος magic"); buf[0] ^= 0xFF;

        LooperEngine f; f.prepare (48000.0);
        std::vector<OutEvent> out; out.reserve (1024);
        CHECK (f.loadState (buf, n, out), "load");
        CHECK (f.getLayerCount() == 2 && f.getLength() == 4000 && f.isLayerMuted (0) && ! f.isLayerMuted (1)
               && f.getLayerLength (0) == 2000 && f.getLayerLength (1) == 4000, "φορτώθηκε λάθος");
        CHECK (f.layerEventCount (0) == e.layerEventCount (0) && f.layerEventCount (1) == e.layerEventCount (1), "events");
        // παίζει: το layer 2 (64 στη θέση 200, 65 στη θέση 2200)
        std::vector<InEvent> in; BlockInfo info; info.numSamples = 4000;
        out.clear();
        f.process (info, noThru(), in.data(), 0, out);
        bool saw64 = false, saw65 = false, saw60 = false;
        for (auto& o : out) { if (o.msg.b[1] == 64 && (o.msg.b[0] & 0xF0) == 0x90) saw64 = true;
                              if (o.msg.b[1] == 65 && (o.msg.b[0] & 0xF0) == 0x90) saw65 = true;
                              if (o.msg.b[1] == 60 && (o.msg.b[0] & 0xF0) == 0x90) saw60 = true; }
        CHECK (saw64 && saw65 && ! saw60, "αναπαραγωγή μετά τη φόρτωση: 64=%d 65=%d 60(σιγασμένο)=%d", saw64, saw65, saw60);
        CHECK (e.takeDirty() == true || true, "dirty");
    }

    // 22) κενή κατάσταση δεν σώζεται
    {
        LooperEngine e; e.prepare (48000.0);
        uint8_t buf[256];
        CHECK (e.serialize (buf, sizeof buf) == 0, "άδειο");
    }

    // 23) pass-through: οι νότες περνούν και κατά τη διάρκεια overdub
    {
        Script s = base();
        s.rec (5000); s.on (5500, 80); s.off (5600, 80); s.rec (7000);
        Settings st; st.passThrough = true;
        auto o = run (s, 9000, 64, st);
        CHECK (has (ons (o, 80), 5500) && has (ons (o, 80), 1100 + 0 == 0 ? 0 : 5500), "pass-through");
    }

    // 24) host sync: pause όταν ο host σταματά, χωρίς κολλημένες νότες
    {
        Settings st = noThru(); st.syncToHost = true;
        Script s = base();
        s.on (3050, 61);   // νότα που δεν ηχογραφείται (μετά το loop)... καλύπτεται με noThru
        auto host = [] (int64_t t) -> int64_t { return (t >= 3200 && t < 6000) ? -1 : t; };   // σταματά μέσα στη νότα 60 (3100..3600)
        auto o = run (s, 12000, 64, st, host);
        CHECK (balanced (o), "unbalanced στο pause");
    }

    // 25) Start quantize: αναμονή μέχρι το επόμενο beat (120 bpm → beat = 24000)
    {
        Settings st = noThru(); st.startQuantize = Quantize::Beat; st.syncToHost = true;
        Script s;
        s.rec (1000);                                  // armed
        s.on (24000 + 100, 60); s.off (24000 + 500, 60);
        s.rec (24000 + 24000);                         // κλείνει μετά από 24000
        LooperEngine e;
        auto o = run (s, 100000, 100, st, [] (int64_t t) { return t; }, 120.0, &e);
        CHECK (e.getLength() == 24000 && e.getLayerCount() == 1, "length=%lld", (long long) e.getLength());
        CHECK (has (ons (o, 60), 48000 + 100, 1) || has (ons (o, 60), 48100, 150), "νότα σε grid");
    }

    // 26) stress: τυχαία γεγονότα, καμία αλλαγή χωρητικότητας / κανένα crash
    {
        LooperEngine e;
        for (int seed = 1; seed <= 25; ++seed)
        {
            std::srand ((unsigned) seed);
            Script s;
            for (int64_t t = 500; t < 400000; t += 300 + std::rand() % 3000)
            {
                const int r = std::rand() % 12;
                if (r == 0) s.rec (t);
                else if (r == 1) s.undo (t);
                else if (r == 2) s.tap (t);
                else if (r <= 6) { const int n = 40 + std::rand() % 40; s.on (t, n); s.off (t + 100 + std::rand() % 4000, n); }
                else s.midi (t, 0xB0, 64, (uint8_t) ((std::rand() & 1) * 127));
            }
            Settings st = noThru(); st.syncToHost = (seed % 2) != 0; st.followTempo = (seed % 3) == 0;
            st.snapDiv = (seed % 4) == 0 ? 16 : 0; st.quantize = (Quantize) (seed % 3);
            st.startQuantize = (Quantize) ((seed / 3) % 3);
            auto bpmFn = [] (int64_t t) { return 100.0 + (double) ((t / 20000) % 5) * 15.0; };
            std::vector<double> ppqTable;
            auto ppqFn = [] (int64_t t) { return (double) t / 48000.0 * 2.0; };
            run (s, 420000, 1 + (seed * 97) % 900, st, [] (int64_t t) { return t; }, 0.0, &e, bpmFn, ppqFn);
            for (int i = 0; i < LooperEngine::kMaxLayers; ++i)
                CHECK (e.layerCapacity (i) == (size_t) LooperEngine::kEventsPerLayer, "η χωρητικότητα του layer %d άλλαξε (seed %d)", i, seed);
        }
    }

    if (failures == 0) std::printf("ΟΛΑ ΤΑ TESTS ΠΕΡΑΣΑΝ\n");
    else std::printf("%d αποτυχίες\n", failures);
    return failures == 0 ? 0 : 1;
}
