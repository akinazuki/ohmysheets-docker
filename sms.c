/*
 * Sheet Music Scanner CLI - calls native analyze() engine
 * Usage: sms <image.png> [output.mid]
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

/* Leptonica */
extern void *pixRead(const char *);
extern void *pixConvertTo32(void *);
extern int pixSetResolution(void *, int, int);
extern int pixGetWidth(void *);
extern int pixGetHeight(void *);
extern int pixGetDepth(void *);
extern int pixGetWpl(void *);
extern unsigned int *pixGetData(void *);

/* OCR models */
extern int ocr_initCharModel(const char *);
extern int ocr_initTimeSignaturesCModel(const char *);
extern int ocr_initTimeSignaturesDigitModel(const char *);

/* Template loading */
extern void *loadTemplatesPriv(int);

/* --- Reverse-engineered structs (arm64 offsets) --- */

typedef struct {
    int type;       /* 0=flats, 1=none, 2=sharps */
    int count;
} KeySig;

typedef struct {
    int id;
    int pitch;
    int pitchNoAcc;  /* pitchWithoutAccidentals */
    int isRest;
    int accType;     /* accidental type, -1=none */
    int _pad;
    double length;   /* duration in ms (+24) */
    double shortLen;
    double dispLen;
    int volume;
    int type;
    void *pix;
    void *box;       /* {x, y, w, h} relative to staff */
} Sound;

typedef struct {
    double startTime;  /* absolute within group, ms */
    double timeToNext;
} TimePoint;

typedef struct {
    double offset;      /* bar start time in ms (+0) */
    double length;      /* bar duration in ms (+8) */
    void *box;          /* bounding box (+16) */
    int isWholeRest;    /* (+24) */
    int clef;           /* 1=treble, 2=bass (+28) */
    void *_pad;         /* (+32) */
    KeySig *keySig;     /* (+40) */
    void *timeSig;      /* (+48) */
} Bar;

typedef struct {
    void *singleBaraa;  /* per-staff bar arrays */
    void *groupBaraa;   /* merged treble+bass bar arrays */
} Score;

typedef struct {
    Score *score;
    void *overlay;      /* PIX* 1bpp */
    void *background;   /* PIX* 32bpp */
    long resultCode;    /* 0 = success */
} AnalysisResult;

typedef struct {
    void *score;
    Bar *bar;
} PlayerListNode;

/* Score access */
extern int baraaGetBarCount(void *, int);
extern Bar *baraaGetBar(void *, int, int);
extern TimePoint *barGetTP(Bar *, int);
extern Sound *tpGetSound(TimePoint *, int);
extern int pitchToMidi(int);

/* MusicXML export: 8 args, last is voiceIndex (-1 = all voices) */
extern int exportToMusicXml(const char *, void *, const char *, const char *, int, int, int, int);

/* analyze():
 * x0: PIX* pix            - 32bpp RGB image
 * x1: loadTemplatesCb     - void* cb(int staffHeight)
 * x2: testDataDir         - const char* (NULL)
 * x3: insertAtIndex       - int (0)
 * x4: pageIndex           - int (0)
 * x5: userData            - transparent context pointer, forwarded to x6/x7 callbacks
 * x6: isCancelledCb       - int cb(void* userData), NULL = default
 * x7: progressCb          - void cb(void* userData, int stage), NULL = default
 * d0: screenMinWidth      - double (1080.0)
 */
typedef AnalysisResult *(*analyze_fn)(void*, void*, void*, int, int, void*, void*, void*, double);

/* --- Bionic compat (also in stubs.so, but needed for link) --- */

/* Template callback: receives staffHeight as int in w0 */
void *tpl_cb(int h) {
    if (h <= 0) h = 136;
    return loadTemplatesPriv(h);
}

/* Progress callback: stage progresses during analysis */
static const char *stage_names[] = {
    [0] = NULL, [1] = NULL,
    [2] = "Preprocessing",
    [3] = "Detecting staves",
    [4] = "Splitting staves",
    [5] = "Analyzing staves",
    [6] = "Building score",
    [7] = "Completed",
};
void progress_cb(void *scanner, int stage) {
    char buf[64];
    const char *name = (stage >= 2 && stage <= 7) ? stage_names[stage] : NULL;
    int n;
    if (name)
        n = snprintf(buf, 64, "  [%d/7] %s\n", stage, name);
    else
        n = snprintf(buf, 64, "  [%d/7] ...\n", stage);
    write(2, buf, n);
}

/* Cancel callback: never cancel */
int cancelled_cb(void *scanner) { return 0; }

/* Key signature application */
int apply_keysig(int midi, int ks_type, int ks_count) {
    if (ks_count <= 0) return midi;
    int n = midi % 12;
    if (ks_type == 0) { /* flats */
        int f[] = {11, 4, 9, 2, 7, 0, 5};
        for (int i = 0; i < ks_count && i < 7; i++)
            if (n == f[i]) return midi - 1;
    }
    return midi;
}

/* --- Simple MIDI writer --- */
typedef struct { unsigned char *d; int len, cap; } MB;
void mb_init(MB *m) { m->cap = 65536; m->d = malloc(m->cap); m->len = 0; }
void mb_u8(MB *m, unsigned char v) {
    if (m->len >= m->cap) { m->cap *= 2; m->d = realloc(m->d, m->cap); }
    m->d[m->len++] = v;
}
void mb_var(MB *m, int v) {
    unsigned char t[5]; int n = 0;
    t[n++] = v & 0x7f; v >>= 7;
    while (v) { t[n++] = 0x80 | (v & 0x7f); v >>= 7; }
    for (int i = n - 1; i >= 0; i--) mb_u8(m, t[i]);
}
void mb_note(MB *m, int dt, int on, int ch, int note, int vel) {
    mb_var(m, dt);
    mb_u8(m, (on ? 0x90 : 0x80) | (ch & 0xf));
    mb_u8(m, note & 0x7f);
    mb_u8(m, vel);
}

typedef struct { int tick; int midi; int on; } MidiEvt;
int cmp_evt(const void *a, const void *b) {
    int d = ((MidiEvt *)a)->tick - ((MidiEvt *)b)->tick;
    return d ? d : ((MidiEvt *)a)->on - ((MidiEvt *)b)->on;
}

/* --- Main --- */
int main(int argc, char **argv) {
    if (argc < 2) {
        write(2, "Usage: sms <image> [image2 ...] [output.mid|.musicxml]\n"
              "  Single page:  sms page.png out.mid\n"
              "  Multi-page:   sms p0.png p1.png p2.png out.mid\n"
              "  MusicXML:     sms page.png out.musicxml\n", 170);
        return 1;
    }

    /* Parse args: last arg ending in .mid/.musicxml/.xml is the output */
    int nPages = argc - 1;
    const char *out_path = "/app/docker_out/output.mid";
    int out_musicxml = 0;
    int last_len = strlen(argv[argc - 1]);
    if (last_len > 4 && strcmp(argv[argc - 1] + last_len - 4, ".mid") == 0) {
        out_path = argv[argc - 1];
        nPages = argc - 2;
    } else if (last_len > 4 && strcmp(argv[argc - 1] + last_len - 4, ".xml") == 0) {
        out_path = argv[argc - 1]; nPages = argc - 2; out_musicxml = 1;
    } else if (last_len > 9 && strcmp(argv[argc - 1] + last_len - 9, ".musicxml") == 0) {
        out_path = argv[argc - 1]; nPages = argc - 2; out_musicxml = 1;
    }
    if (nPages < 1) { write(2, "Error: no input images\n", 23); return 1; }

    int tpq = 480, bpm = 160;

    char buf[256];
    #define LOG(f, ...) do { \
        int n = snprintf(buf, 256, f "\n", ##__VA_ARGS__); \
        write(2, buf, n); \
    } while(0)

    /* 1. Init OCR models */
    ocr_initCharModel("/app/assets/nnModels/ocr_model.json");
    ocr_initTimeSignaturesCModel("/app/assets/nnModels/keySignatures_c_model.json");
    ocr_initTimeSignaturesDigitModel("/app/assets/nnModels/keySignatures_digit_model.json");

    /* 2. Set global template path */
    void *na = dlsym(RTLD_DEFAULT, "nativeAnalyze");
    unsigned long base = (unsigned long)na - 0x28927c;
    *(char **)(base + 0x351F08) = "/app/assets/templates/";

    analyze_fn do_analyze = (analyze_fn)dlsym(RTLD_DEFAULT, "analyze");

    /* 3. Session-based multi-page analysis (matching App behavior) */
    extern void *sessionCreate(int);
    extern int sessionAdd(void *, Score *);
    extern int sessionStateSetCurrentBar(void *, Score *, Bar *, int);
    extern PlayerListNode *sessionStateMoveToNextBar(void *);
    extern int sessionAlignVoiceIndexes(void *, int);

    void *session = NULL;
    Score *score = NULL;

    for (int page = 0; page < nPages; page++) {
        const char *img_path = argv[1 + page];

        void *pix = pixRead(img_path);
        if (!pix) { LOG("Error: cannot read %s", img_path); return 1; }
        void *p32 = pixConvertTo32(pix);
        pixSetResolution(p32, 300, 300);
        LOG("Page %d/%d: %s (%dx%d)", page + 1, nPages, img_path,
            pixGetWidth(p32), pixGetHeight(p32));

        LOG("Analyzing...");
        AnalysisResult *result = do_analyze(p32, (void *)tpl_cb,
                                            page > 0 ? session : NULL,
                                            page, 0,
                                            NULL, (void *)cancelled_cb,
                                            (void *)progress_cb, 1080.0);

        if (!result || result->resultCode != 0) {
            LOG("Error: page %d analysis failed (code=%ld)",
                page, result ? result->resultCode : -1);
            return 1;
        }

        score = result->score;

        if (page == 0) {
            session = sessionCreate(1);
        }
        sessionAdd(session, score);

        {
            /* Count notes in this page's score */
            int pn = 0;
            for (int si = 0; si < 20; si++) {
                int nb = baraaGetBarCount(score->singleBaraa, si);
                if (nb <= 0) break;
                for (int bi = 0; bi < nb; bi++) {
                    Bar *b = baraaGetBar(score->singleBaraa, si, bi);
                    if (!b) continue;
                    for (int ti = 0; ti < 200; ti++) {
                        TimePoint *tp = barGetTP(b, ti);
                        if (!tp) break;
                        for (int sni = 0; sni < 20; sni++) {
                            Sound *s = tpGetSound(tp, sni);
                            if (!s) break;
                            if (!s->isRest) pn++;
                        }
                    }
                }
            }
            LOG("  Page %d: %d notes", page + 1, pn);
        }
    }

    if (!score) { LOG("Error: no score"); return 1; }

    /* MusicXML export: use the native function directly */
    if (out_musicxml) {
        sessionAlignVoiceIndexes(session, 0);
        LOG("Exporting MusicXML...");
        int ret = exportToMusicXml(out_path, session, "Score", "Piano", 1, 0, bpm, -1);
        if (ret != 0) { LOG("MusicXML export failed (%d)", ret); return 1; }
        LOG("MusicXML: %s", out_path);
        _exit(0);
    }

    /* 5. Collect MIDI events using unified session (matching App behavior)
       App uses one session with all scores, traversed by sessionStateMoveToNextBar
       which handles cross-page transitions and repeats automatically. */
    extern Score *sessionGet(void *, int);

    /* Get global keySig from first page */
    int gks_type = 1, gks_count = 0, nStaves = 0;
    Score *sc0 = sessionGet(session, 0);
    if (sc0) {
        for (int i = 0; i < 20; i++) { if (baraaGetBarCount(sc0->singleBaraa, i) <= 0) break; nStaves++; }
        for (int si = 0; si < nStaves && gks_count == 0; si++) {
            int nb = baraaGetBarCount(sc0->singleBaraa, si);
            for (int bi = 0; bi < nb; bi++) {
                Bar *bar = baraaGetBar(sc0->singleBaraa, si, bi);
                if (!bar) continue;
                if (bar->keySig && bar->keySig->count > 0) {
                    gks_type = bar->keySig->type;
                    gks_count = bar->keySig->count;
                    break;
                }
            }
        }
    }

    /* Find first non-empty group bar from first score (like App's playFromScore) */
    Bar *firstBar = NULL;
    for (int gi = 0; gi < 20 && !firstBar; gi++) {
        int nb = baraaGetBarCount(sc0->groupBaraa, gi);
        if (nb <= 0) break;
        for (int bi = 0; bi < nb; bi++) {
            Bar *bar = baraaGetBar(sc0->groupBaraa, gi, bi);
            if (bar && bar->length > 0) { firstBar = bar; break; }
        }
    }

    MidiEvt *evts = NULL;
    int nevt = 0, ecap = 0;
    int total_notes = 0;
    int total_bars = 0;

    if (firstBar) {
        /* Start from first bar of first score, voice=-1 for group mode */
        sessionStateSetCurrentBar(session, sc0, firstBar, -1);

        Bar *cur = firstBar;
        double cumul = 0;

        while (cur && total_bars < 500) {
            if (cur->length <= 0) {
                PlayerListNode *node = sessionStateMoveToNextBar(session);
                if (!node) break;
                cur = node->bar;
                continue;
            }

            /* Per-bar key signature */
            int kt = gks_type, kc = gks_count;
            if (cur->keySig && cur->keySig->count > 0) {
                kt = cur->keySig->type;
                kc = cur->keySig->count;
            }

            for (int ti = 0; ti < 200; ti++) {
                TimePoint *tp = barGetTP(cur, ti);
                if (!tp) break;

                for (int sni = 0; sni < 20; sni++) {
                    Sound *snd = tpGetSound(tp, sni);
                    if (!snd) break;
                    if (snd->isRest) continue;

                    int midi = pitchToMidi(snd->pitch);
                    if (snd->pitch == snd->pitchNoAcc)
                        midi = apply_keysig(midi, kt, kc);
                    if (midi < 21 || midi > 108) continue;

                    double dur_ms = snd->length > 0 ? snd->length : 500;

                    int tick = (int)((cumul + (tp->startTime - cur->offset)) * tpq / 1000.0);
                    int dur = (int)(dur_ms * tpq / 1000.0);
                    if (dur < 20) dur = 20;

                    if (nevt + 2 > ecap) {
                        ecap = ecap ? ecap * 2 : 8192;
                        evts = realloc(evts, ecap * sizeof(MidiEvt));
                    }
                    evts[nevt++] = (MidiEvt){tick, midi, 1};
                    evts[nevt++] = (MidiEvt){tick + dur - 10, midi, 0};
                    total_notes++;
                }
            }

            /* Use time-signature-derived bar duration instead of raw bar->length
               which can be inaccurate for transition bars.
               timeSig struct: [0]=type/symbol, [1]=numerator, [2]=denominator, [3]=?
               Standard bar duration = numerator * (4.0/denominator) * 1000ms */
            static double std_bar_ms = 4000.0; /* default 4/4 */
            if (cur->timeSig) {
                int num = ((int *)cur->timeSig)[1];
                int den = ((int *)cur->timeSig)[2];
                if (num > 0 && den > 0)
                    std_bar_ms = num * (4.0 / den) * 1000.0;
            }
            cumul += std_bar_ms;
            total_bars++;
            PlayerListNode *node = sessionStateMoveToNextBar(session);
            if (!node) break;
            cur = node->bar;
        }
    }
    LOG("Bars: %d (with repeats)", total_bars);

    qsort(evts, nevt, sizeof(MidiEvt), cmp_evt);

    /* 7. Write MIDI file */
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { LOG("Error: cannot write %s", out_path); return 1; }

    unsigned char mthd[14] = {'M','T','h','d', 0,0,0,6, 0,1, 0,2,
                              (tpq >> 8) & 0xff, tpq & 0xff};
    write(fd, mthd, 14);

    /* Tempo track */
    MB tempo; mb_init(&tempo);
    int uspqn = 60000000 / bpm;
    mb_var(&tempo, 0); mb_u8(&tempo, 0xff); mb_u8(&tempo, 0x51); mb_u8(&tempo, 3);
    mb_u8(&tempo, (uspqn >> 16) & 0xff); mb_u8(&tempo, (uspqn >> 8) & 0xff); mb_u8(&tempo, uspqn & 0xff);
    if (gks_count > 0) {
        mb_var(&tempo, 0); mb_u8(&tempo, 0xff); mb_u8(&tempo, 0x59); mb_u8(&tempo, 2);
        mb_u8(&tempo, (unsigned char)(gks_type == 0 ? -gks_count : gks_count));
        mb_u8(&tempo, 0);
    }
    mb_var(&tempo, 0); mb_u8(&tempo, 0xff); mb_u8(&tempo, 0x58); mb_u8(&tempo, 4);
    mb_u8(&tempo, 4); mb_u8(&tempo, 2); mb_u8(&tempo, 24); mb_u8(&tempo, 8);
    mb_var(&tempo, 0); mb_u8(&tempo, 0xff); mb_u8(&tempo, 0x2f); mb_u8(&tempo, 0);
    unsigned char th[8] = {'M','T','r','k',
        (tempo.len >> 24) & 0xff, (tempo.len >> 16) & 0xff,
        (tempo.len >> 8) & 0xff, tempo.len & 0xff};
    write(fd, th, 8); write(fd, tempo.d, tempo.len);

    /* Single note track (both hands on channel 0) */
    MB trk; mb_init(&trk);
    const char *tname = "Piano";
    mb_var(&trk, 0); mb_u8(&trk, 0xff); mb_u8(&trk, 0x03); mb_u8(&trk, 5);
    for (int i = 0; i < 5; i++) mb_u8(&trk, tname[i]);
    mb_var(&trk, 0); mb_u8(&trk, 0xc0); mb_u8(&trk, 0); /* piano */

    int last = 0;
    for (int i = 0; i < nevt; i++) {
        int dt = evts[i].tick - last;
        if (dt < 0) dt = 0;
        mb_note(&trk, dt, evts[i].on, 0, evts[i].midi, evts[i].on ? 80 : 0);
        last = evts[i].tick;
    }
    mb_var(&trk, 0); mb_u8(&trk, 0xff); mb_u8(&trk, 0x2f); mb_u8(&trk, 0);

    unsigned char h[8] = {'M','T','r','k',
        (trk.len >> 24) & 0xff, (trk.len >> 16) & 0xff,
        (trk.len >> 8) & 0xff, trk.len & 0xff};
    write(fd, h, 8); write(fd, trk.d, trk.len);
    free(trk.d);
    close(fd);

    /* 8. Summary */
    const char *ks_names[] = {"flats", "none", "sharps"};
    LOG("Done: %d notes, %d staves, keySig=%d %s",
        total_notes, nStaves, gks_count,
        gks_count > 0 ? ks_names[gks_type < 3 ? gks_type : 1] : "");
    LOG("MIDI: %s", out_path);

    free(evts); free(tempo.d);
    _exit(0);
}
