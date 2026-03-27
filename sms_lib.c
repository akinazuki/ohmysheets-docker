/*
 * Sheet Music Scanner Library - extracted from sms.c for cgo integration.
 * No main(), no printf/fprintf/fflush overrides.
 * Logging goes through callbacks instead of stderr.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

#include "sms_lib.h"

/* Leptonica */
extern void *pixRead(const char *);
extern void *pixReadMem(const unsigned char *, size_t);
extern void *pixConvertTo32(void *);
extern int pixSetResolution(void *, int, int);
extern int pixGetWidth(void *);
extern int pixGetHeight(void *);
extern int pixGetDepth(void *);
extern int pixGetWpl(void *);
extern unsigned int *pixGetData(void *);
extern void pixDestroy(void **);

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

typedef AnalysisResult *(*analyze_fn)(void*, void*, void*, int, int, void*, void*, void*, double);

/* Session */
extern void *sessionCreate(int);
extern int sessionAdd(void *, Score *);
extern int sessionAlignVoiceIndexes(void *, int);
extern int sessionStateSetCurrentBar(void *, Score *, Bar *, int);
extern PlayerListNode *sessionStateMoveToNextBar(void *);
extern Score *sessionGet(void *, int);

/* MusicXML export: writes to file path.
 * voiceIndex=-1 exports all voices. */
extern int exportToMusicXml(const char *, void *, const char *, const char *, int, int, int, int);

/* --- Engine state --- */

static int g_initialized = 0;
static analyze_fn g_do_analyze = NULL;

/* --- Internal logging & progress --- */

static sms_log_fn g_log_cb = NULL;
static sms_progress_fn g_progress_cb = NULL;
static sms_page_done_fn g_page_done_cb = NULL;
static void *g_userdata = NULL;
static int g_cur_page = 0;
static int g_total_pages = 0;

static void sms_log(const char *fmt, ...) {
    if (!g_log_cb) return;
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    g_log_cb(buf, g_userdata);
}

/* Template callback */
static void *tpl_cb(int h) {
    if (h <= 0) h = 136;
    return loadTemplatesPriv(h);
}

/* Progress callback bridge */
static const char *stage_names[] = {
    [0] = NULL, [1] = NULL,
    [2] = "PREPROCESSING",
    [3] = "DETECTING_STAVES",
    [4] = "SPLITTING_STAVES",
    [5] = "ANALYZING_STAVES",
    [6] = "BUILDING_SCORE",
    [7] = "COMPLETED",
    [8] = "COMPLETED",
};
#define STAGE_MAX 8

static void internal_progress_cb(void *scanner, int stage) {
    const char *name = (stage >= 2 && stage <= STAGE_MAX) ? stage_names[stage] : NULL;
    if (g_progress_cb) {
        g_progress_cb(g_cur_page, g_total_pages, stage, name, g_userdata);
    }
    if (name)
        sms_log("  Page %d/%d [%d/7] %s", g_cur_page + 1, g_total_pages, stage, name);
    else
        sms_log("  Page %d/%d [%d/7] ...", g_cur_page + 1, g_total_pages, stage);
}

/* Cancel callback: never cancel */
static int cancelled_cb(void *scanner) { return 0; }

/* Key signature application */
static int apply_keysig(int midi, int ks_type, int ks_count) {
    if (ks_count <= 0) return midi;
    int n = midi % 12;
    if (ks_type == 0) { /* flats */
        int f[] = {11, 4, 9, 2, 7, 0, 5};
        for (int i = 0; i < ks_count && i < 7; i++)
            if (n == f[i]) return midi - 1;
    }
    return midi;
}

/* --- Public: sms_init --- */

int sms_init(sms_log_fn log_cb, void *userdata) {
    if (g_initialized) return 0;

    g_log_cb = log_cb;
    g_userdata = userdata;

    /* OCR models */
    ocr_initCharModel("/app/assets/nnModels/ocr_model.json");
    ocr_initTimeSignaturesCModel("/app/assets/nnModels/keySignatures_c_model.json");
    ocr_initTimeSignaturesDigitModel("/app/assets/nnModels/keySignatures_digit_model.json");

    /* Resolve native symbols */
    void *na = dlsym(RTLD_DEFAULT, "nativeAnalyze");
    if (!na) return -1;
    unsigned long base = (unsigned long)na - 0x28927c;
    *(char **)(base + 0x351F08) = "/app/assets/templates/";

    g_do_analyze = (analyze_fn)dlsym(RTLD_DEFAULT, "analyze");
    if (!g_do_analyze) return -2;

    g_initialized = 1;
    sms_log("Engine initialized");
    return 0;
}

/* --- Public: PIX helpers --- */

void *sms_pix_read(const char *path) {
    return pixRead(path);
}

void *sms_pix_read_mem(const unsigned char *data, int len) {
    return pixReadMem(data, (size_t)len);
}

int sms_pix_width(void *pix) {
    return pix ? pixGetWidth(pix) : 0;
}

int sms_pix_height(void *pix) {
    return pix ? pixGetHeight(pix) : 0;
}

void sms_pix_free(void *pix) {
    if (pix) pixDestroy(&pix);
}

/* --- Simple MIDI writer --- */
typedef struct { unsigned char *d; int len, cap; } MB;
static void mb_init(MB *m) { m->cap = 65536; m->d = malloc(m->cap); m->len = 0; }
static void mb_u8(MB *m, unsigned char v) {
    if (m->len >= m->cap) { m->cap *= 2; m->d = realloc(m->d, m->cap); }
    m->d[m->len++] = v;
}
static void mb_var(MB *m, int v) {
    unsigned char t[5]; int n = 0;
    t[n++] = v & 0x7f; v >>= 7;
    while (v) { t[n++] = 0x80 | (v & 0x7f); v >>= 7; }
    for (int i = n - 1; i >= 0; i--) mb_u8(m, t[i]);
}
static void mb_note(MB *m, int dt, int on, int ch, int note, int vel) {
    mb_var(m, dt);
    mb_u8(m, (on ? 0x90 : 0x80) | (ch & 0xf));
    mb_u8(m, note & 0x7f);
    mb_u8(m, vel);
}

typedef struct { int tick; int midi; int on; } MidiEvt;
static int cmp_evt(const void *a, const void *b) {
    int d = ((MidiEvt *)a)->tick - ((MidiEvt *)b)->tick;
    return d ? d : ((MidiEvt *)a)->on - ((MidiEvt *)b)->on;
}

/* Build a complete MIDI buffer from a single Score's singleBaraa.
 * Returns SmsMidi with .data/.len. Caller must free .data.
 * out_notes/out_bars are set if non-NULL. */
static SmsMidi build_midi_from_score(Score *sc, int tpq, int bpm,
                                     int *out_notes, int *out_bars) {
    SmsMidi result = {0};
    int nStaves = 0;
    int gks_type = 1, gks_count = 0;

    for (int i = 0; i < 20; i++) {
        if (baraaGetBarCount(sc->singleBaraa, i) <= 0) break;
        nStaves++;
    }
    for (int si = 0; si < nStaves && gks_count == 0; si++) {
        int nb = baraaGetBarCount(sc->singleBaraa, si);
        for (int bi = 0; bi < nb; bi++) {
            Bar *bar = baraaGetBar(sc->singleBaraa, si, bi);
            if (!bar) continue;
            if (bar->keySig && bar->keySig->count > 0) {
                gks_type = bar->keySig->type;
                gks_count = bar->keySig->count;
                break;
            }
        }
    }

    /* Collect events from all staves/bars */
    MidiEvt *evts = NULL;
    int nevt = 0, ecap = 0;
    int total_notes = 0, total_bars = 0;
    double cumul = 0;
    double std_bar_ms = 4000.0;

    for (int si = 0; si < nStaves; si++) {
        int nb = baraaGetBarCount(sc->singleBaraa, si);
        double staff_cumul = 0;
        double staff_bar_ms = 4000.0;

        for (int bi = 0; bi < nb; bi++) {
            Bar *cur = baraaGetBar(sc->singleBaraa, si, bi);
            if (!cur || cur->length <= 0) continue;

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
                    int tick = (int)((staff_cumul + (tp->startTime - cur->offset)) * tpq / 1000.0);
                    int dur = (int)(dur_ms * tpq / 1000.0);
                    if (dur < 20) dur = 20;

                    if (nevt + 2 > ecap) {
                        ecap = ecap ? ecap * 2 : 4096;
                        evts = realloc(evts, ecap * sizeof(MidiEvt));
                    }
                    evts[nevt++] = (MidiEvt){tick, midi, 1};
                    evts[nevt++] = (MidiEvt){tick + dur - 10, midi, 0};
                    total_notes++;
                }
            }

            if (cur->timeSig) {
                int num = ((int *)cur->timeSig)[1];
                int den = ((int *)cur->timeSig)[2];
                if (num > 0 && den > 0)
                    staff_bar_ms = num * (4.0 / den) * 1000.0;
            }
            staff_cumul += staff_bar_ms;
            if (si == 0) total_bars++;
        }
    }

    if (out_notes) *out_notes = total_notes;
    if (out_bars) *out_bars = total_bars;

    qsort(evts, nevt, sizeof(MidiEvt), cmp_evt);

    /* Build MIDI buffer */
    int uspqn = 60000000 / bpm;
    MB tempo; mb_init(&tempo);
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

    MB trk; mb_init(&trk);
    const char *tname = "Piano";
    mb_var(&trk, 0); mb_u8(&trk, 0xff); mb_u8(&trk, 0x03); mb_u8(&trk, 5);
    for (int i = 0; i < 5; i++) mb_u8(&trk, tname[i]);
    mb_var(&trk, 0); mb_u8(&trk, 0xc0); mb_u8(&trk, 0);

    int last = 0;
    for (int i = 0; i < nevt; i++) {
        int dt = evts[i].tick - last;
        if (dt < 0) dt = 0;
        mb_note(&trk, dt, evts[i].on, 0, evts[i].midi, evts[i].on ? 80 : 0);
        last = evts[i].tick;
    }
    mb_var(&trk, 0); mb_u8(&trk, 0xff); mb_u8(&trk, 0x2f); mb_u8(&trk, 0);

    int total_len = 14 + 8 + tempo.len + 8 + trk.len;
    unsigned char *midi = malloc(total_len);
    int pos = 0;
    unsigned char mthd[14] = {'M','T','h','d', 0,0,0,6, 0,1, 0,2,
                              (tpq >> 8) & 0xff, tpq & 0xff};
    memcpy(midi + pos, mthd, 14); pos += 14;
    unsigned char th[8] = {'M','T','r','k',
        (tempo.len >> 24) & 0xff, (tempo.len >> 16) & 0xff,
        (tempo.len >> 8) & 0xff, tempo.len & 0xff};
    memcpy(midi + pos, th, 8); pos += 8;
    memcpy(midi + pos, tempo.d, tempo.len); pos += tempo.len;
    unsigned char h[8] = {'M','T','r','k',
        (trk.len >> 24) & 0xff, (trk.len >> 16) & 0xff,
        (trk.len >> 8) & 0xff, trk.len & 0xff};
    memcpy(midi + pos, h, 8); pos += 8;
    memcpy(midi + pos, trk.d, trk.len); pos += trk.len;

    result.data = midi;
    result.len = total_len;

    free(evts);
    free(tempo.d);
    free(trk.d);
    return result;
}

/* --- Main analysis function --- */
SmsResult sms_analyze(void **pix_pages, int num_pages,
                      sms_log_fn log_cb, sms_progress_fn progress_cb,
                      sms_page_done_fn page_done_cb,
                      void *userdata) {
    SmsResult res = {0};

    g_log_cb = log_cb;
    g_progress_cb = progress_cb;
    g_page_done_cb = page_done_cb;
    g_userdata = userdata;

    if (num_pages < 1 || !pix_pages) {
        res.result_code = 1;
        snprintf(res.error_msg, sizeof(res.error_msg), "invalid arguments");
        return res;
    }

    if (!g_initialized || !g_do_analyze) {
        res.result_code = 2;
        snprintf(res.error_msg, sizeof(res.error_msg), "engine not initialized, call sms_init first");
        return res;
    }

    int tpq = 480, bpm = 160;

    /* Session-based multi-page analysis */
    void *session = NULL;
    Score *score = NULL;
    Score **scores = calloc(num_pages, sizeof(Score *));
    g_total_pages = num_pages;

    /* Allocate per-page MIDI array */
    res.page_midis = calloc(num_pages, sizeof(SmsMidi));
    res.num_pages = num_pages;

    for (int page = 0; page < num_pages; page++) {
        g_cur_page = page;
        void *pix = pix_pages[page];
        if (!pix) {
            res.result_code = 3;
            snprintf(res.error_msg, sizeof(res.error_msg), "page %d: null PIX", page);
            return res;
        }
        void *p32 = pixConvertTo32(pix);
        pixSetResolution(p32, 300, 300);
        sms_log("Page %d/%d: %dx%d", page + 1, num_pages,
                pixGetWidth(p32), pixGetHeight(p32));

        sms_log("Analyzing...");
        AnalysisResult *result = g_do_analyze(p32, (void *)tpl_cb,
                                            page > 0 ? session : NULL,
                                            page, 0,
                                            NULL, (void *)cancelled_cb,
                                            (void *)internal_progress_cb, 1080.0);

        if (!result || result->resultCode != 0) {
            res.result_code = 4;
            snprintf(res.error_msg, sizeof(res.error_msg),
                     "page %d analysis failed (code=%ld)",
                     page, result ? result->resultCode : -1);
            return res;
        }

        score = result->score;

        if (page == 0) {
            session = sessionCreate(1);
        }
        sessionAdd(session, score);
        sessionAlignVoiceIndexes(session, page > 0 ? page : 0);
        scores[page] = score;

        /* Build per-page MIDI */
        int pn = 0;
        res.page_midis[page] = build_midi_from_score(score, tpq, bpm, &pn, NULL);
        sms_log("  Page %d: %d notes, %d bytes MIDI", page + 1, pn, res.page_midis[page].len);

        /* Notify caller immediately with this page's MIDI */
        if (g_page_done_cb) {
            g_page_done_cb(page, num_pages,
                           res.page_midis[page].data, res.page_midis[page].len,
                           pn, g_userdata);
        }
    }

    if (!score) {
        res.result_code = 5;
        snprintf(res.error_msg, sizeof(res.error_msg), "no score produced");
        free(scores);
        return res;
    }

    /* Build merged MIDI using session traversal (handles repeats & cross-page links).
     * This matches the original sms.c logic using groupBaraa + sessionStateMoveToNextBar. */
    {
        Score *sc0 = sessionGet(session, 0);
        int gks_type = 1, gks_count = 0, nStaves = 0;

        if (sc0) {
            for (int i = 0; i < 20; i++) {
                if (baraaGetBarCount(sc0->singleBaraa, i) <= 0) break;
                nStaves++;
            }
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
        res.num_staves = nStaves;

        /* Find first non-empty group bar */
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

        if (firstBar) {
            sessionStateSetCurrentBar(session, sc0, firstBar, -1);
            Bar *cur = firstBar;
            double cumul = 0;
            double std_bar_ms = 4000.0;

            while (cur && res.total_bars < 500) {
                if (cur->length <= 0) {
                    PlayerListNode *node = sessionStateMoveToNextBar(session);
                    if (!node) break;
                    cur = node->bar;
                    continue;
                }

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
                        res.total_notes++;
                    }
                }

                if (cur->timeSig) {
                    int num = ((int *)cur->timeSig)[1];
                    int den = ((int *)cur->timeSig)[2];
                    if (num > 0 && den > 0)
                        std_bar_ms = num * (4.0 / den) * 1000.0;
                }
                cumul += std_bar_ms;
                res.total_bars++;

                PlayerListNode *node = sessionStateMoveToNextBar(session);
                if (!node) break;
                cur = node->bar;
            }
        }

        sms_log("Bars: %d, Notes: %d", res.total_bars, res.total_notes);
        qsort(evts, nevt, sizeof(MidiEvt), cmp_evt);

        /* Build merged MIDI buffer */
        int uspqn = 60000000 / bpm;
        MB tempo; mb_init(&tempo);
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

        MB trk; mb_init(&trk);
        const char *tname = "Piano";
        mb_var(&trk, 0); mb_u8(&trk, 0xff); mb_u8(&trk, 0x03); mb_u8(&trk, 5);
        for (int i = 0; i < 5; i++) mb_u8(&trk, tname[i]);
        mb_var(&trk, 0); mb_u8(&trk, 0xc0); mb_u8(&trk, 0);

        int last = 0;
        for (int i = 0; i < nevt; i++) {
            int dt = evts[i].tick - last;
            if (dt < 0) dt = 0;
            mb_note(&trk, dt, evts[i].on, 0, evts[i].midi, evts[i].on ? 80 : 0);
            last = evts[i].tick;
        }
        mb_var(&trk, 0); mb_u8(&trk, 0xff); mb_u8(&trk, 0x2f); mb_u8(&trk, 0);

        int total_len = 14 + 8 + tempo.len + 8 + trk.len;
        unsigned char *midi = malloc(total_len);
        int pos = 0;
        unsigned char mthd[14] = {'M','T','h','d', 0,0,0,6, 0,1, 0,2,
                                  (tpq >> 8) & 0xff, tpq & 0xff};
        memcpy(midi + pos, mthd, 14); pos += 14;
        unsigned char th[8] = {'M','T','r','k',
            (tempo.len >> 24) & 0xff, (tempo.len >> 16) & 0xff,
            (tempo.len >> 8) & 0xff, tempo.len & 0xff};
        memcpy(midi + pos, th, 8); pos += 8;
        memcpy(midi + pos, tempo.d, tempo.len); pos += tempo.len;
        unsigned char h[8] = {'M','T','r','k',
            (trk.len >> 24) & 0xff, (trk.len >> 16) & 0xff,
            (trk.len >> 8) & 0xff, trk.len & 0xff};
        memcpy(midi + pos, h, 8); pos += 8;
        memcpy(midi + pos, trk.d, trk.len); pos += trk.len;

        res.midi_data = midi;
        res.midi_len = total_len;

        free(evts);
        free(tempo.d);
        free(trk.d);
    }

    /* Build MusicXML via the native export function (writes to tmpfile, read back) */
    {
        char tmppath[] = "/tmp/sms_xml_XXXXXX";
        int tmpfd = mkstemp(tmppath);
        if (tmpfd >= 0) {
            close(tmpfd);
            int ret = exportToMusicXml(tmppath, session, "Score", "Piano", 1, 0, bpm, -1);
            if (ret == 0) {
                FILE *f = fopen(tmppath, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    if (sz > 0) {
                        res.musicxml_data = malloc(sz);
                        res.musicxml_len = (int)fread(res.musicxml_data, 1, sz, f);
                    }
                    fclose(f);
                }
            }
            unlink(tmppath);
        }
    }

    sms_log("Done: %d notes, %d bars, %d staves, %d bytes MusicXML",
            res.total_notes, res.total_bars, res.num_staves, res.musicxml_len);
    free(scores);
    return res;
}
