/* Potential1200 (PotentialBreaks): a 12-bit vintage sampler for MPC OS standalone devices, in the spirit of the classic
 * late-80s 12-bit drum samplers. Not affiliated with any maker of them.
 *
 * The sound:
 *  - Every sound is "sampled" when it loads: mixed to mono, read at 26.04 kHz with NO anti-alias filter (the high end folds
 *    back as grit), and kept as 12-bit values. SPEED 45 -> 33 is the old record trick: the source is taken 1.35x fast and
 *    played back slowed, so it keeps its pitch and length but has fewer samples (darker and crunchier).
 *  - Playing: one DAC clock at 26.04 kHz for everything. Pitch is drop-sample: a sound steps through its samples faster or
 *    slower (repeating or skipping them), so pitch changes its length, as on the old machines. STRETCH (per pad) keeps the
 *    length instead: two overlapping grains, still drop-sampled.
 *  - The 26.04 kHz steps reach the 44.1 kHz output as steps (sample and hold), then each pad's filter: DYNAMIC (a 4-pole
 *    low-pass with a little resonance, its CUTOFF on the FILTER faders), FIXED (a gentle low-pass) or OFF. By default pads
 *    1-2 are DYNAMIC, 3-6 FIXED and 7-8 OFF, like the old machine's outputs.
 *  - CHARACTER: CLEAN skips the 12-bit and 26 kHz parts (44.1 kHz, smooth pitch) for comparison.
 *  - 8 pads, one voice each (a pad cuts itself off). CHOKE 1-4: pads in the same group cut each other.
 *  - PADS 9-16 MULTI PITCH: the selected pad's sound on pads 9-16, at 0 2 3 5 7 8 10 12 semitones.
 * The screen: 8 faders that follow the mode (TUNE, DECAY, LEVEL or FILTER) for pads 1-8; the selected pad on PROGRAM; the
 * sounds on KIT, from ONE-SHOTS (a folder per pad type, e.g. DRUM ONE SHOTS from MPC TOOLS) or a BREAK chopped across the
 * 8 pads; SETUP.
 * 44.1 kHz, interleaved int16 stereo; render() may be asked for 1..128 frames (SAMPLE_ACCURATE). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "engine.h"
#include "../shared/looks/potential_theme.h"   /* the LOOK switch (parameter "theme") */
#include "../shared/potential_manual.h"        /* the MANUAL tab's page */
#include "kit_writer.h"                         /* SAVE KIT: an MPC drum program */

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO_WIDE
#include "vendor/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_WCHAR
#include "vendor/dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3.h"

#define FS 44100
#define SP_RATE 26040.0          /* the old machines' sample rate */
#define NP8 8                    /* pads */
#define ONE_SHOT_SEC 10.0        /* the longest one-shot kept */
#define MAX_FILE_SEC 180.0       /* never decode more than the first 3 minutes of a file */
#define LONG_FILE_SEC 24.0       /* longer than this: a track, a loop is cut from it */
#define HOP 441                  /* 10 ms analysis hop */
#define SPEED45 1.35             /* 45 rpm played at 33 1/3 */
#define WCOLS 64                 /* KIT (BREAK): waveform columns */
#define WMARKS 19                /* waveform pictures: plain, played, playhead, pad 1-8 start, selected pad 1-8 start */
#define WFRAMES (WMARKS * 9)     /* x 9 bar heights */
#define QUIET_SEC 3              /* a project opening sets every value: taps and fader moves are ignored meanwhile */

/* ---------------- parameters (same order as vst/params.json) ---------------- */
enum {
    P_DEAD, P_MODE,
    P_F1, P_F8 = P_F1 + 7,
    P_PAD1, P_PAD8 = P_PAD1 + 7,
    P_DISP, P_DISP2,
    P_SEL_NAME, P_SEL_FILE, P_SEL_INFO,
    P_TUNE, P_DECAY, P_LEVEL, P_CUTOFF, P_FILTER, P_STRETCH, P_REVERSE, P_CHOKE,
    P_START, P_START_PREV, P_START_NEXT, P_END, P_END_PREV, P_END_NEXT,
    P_SOURCE,
    P_TYPE1, P_TYPE8 = P_TYPE1 + 7,
    P_FILE1, P_FILE8 = P_FILE1 + 7,
    P_PREV1, P_PREV8 = P_PREV1 + 7,
    P_NEXT1, P_NEXT8 = P_NEXT1 + 7,
    P_LOCK1, P_LOCK8 = P_LOCK1 + 7,
    P_LIB_INFO, P_NEW_KIT, P_ROLL, P_WILD, P_UNLOCK,
    P_BREAK_NAME, P_BREAK_INFO, P_LIB_NAME, P_RANDOM_BREAK, P_PREV_BREAK, P_NEXT_BREAK, P_LIB_NEXT,
    P_CHOP, P_TEMPO, P_PLAY_BREAK, P_SHUFFLE,
    P_SPEED, P_CHARACTER, P_PADS916, P_SAVE_KIT, P_RESCAN, P_STATUS,
    P_W0, P_W63 = P_W0 + 63,          /* KIT (BREAK): the waveform, 64 columns (display only) */
    NP
};
static char PKEY[NP][24];
static void init_keys(void) {
    static int done;
    if (done) return;
    static const char *fixed[NP] = {
        [P_DEAD] = "unused", [P_MODE] = "mode", [P_DISP] = "disp", [P_DISP2] = "disp2",
        [P_SEL_NAME] = "sel_name", [P_SEL_FILE] = "sel_file", [P_SEL_INFO] = "sel_info",
        [P_TUNE] = "tune", [P_DECAY] = "decay", [P_LEVEL] = "level", [P_CUTOFF] = "cutoff", [P_FILTER] = "filter",
        [P_STRETCH] = "stretch", [P_REVERSE] = "reverse", [P_CHOKE] = "choke",
        [P_START] = "start", [P_START_PREV] = "start_prev", [P_START_NEXT] = "start_next",
        [P_END] = "end", [P_END_PREV] = "end_prev", [P_END_NEXT] = "end_next",
        [P_SOURCE] = "source", [P_LIB_INFO] = "lib_info", [P_NEW_KIT] = "new_kit", [P_ROLL] = "roll", [P_WILD] = "wild",
        [P_UNLOCK] = "unlock_all", [P_BREAK_NAME] = "break_name", [P_BREAK_INFO] = "break_info", [P_LIB_NAME] = "lib_name",
        [P_RANDOM_BREAK] = "random_break", [P_PREV_BREAK] = "prev_break", [P_NEXT_BREAK] = "next_break",
        [P_LIB_NEXT] = "lib_next", [P_CHOP] = "chop", [P_TEMPO] = "tempo", [P_PLAY_BREAK] = "play_break",
        [P_SHUFFLE] = "shuffle", [P_SPEED] = "speed", [P_CHARACTER] = "character", [P_PADS916] = "pads916",
        [P_SAVE_KIT] = "save_kit", [P_RESCAN] = "rescan", [P_STATUS] = "status",
    };
    for (int i = 0; i < NP; i++) if (fixed[i]) snprintf(PKEY[i], sizeof PKEY[i], "%s", fixed[i]);
    for (int c = 0; c < WCOLS; c++) snprintf(PKEY[P_W0 + c], 24, "w_%02d", c);
    for (int k = 0; k < NP8; k++) {
        snprintf(PKEY[P_F1 + k], 24, "fader_%d", k + 1);
        snprintf(PKEY[P_PAD1 + k], 24, "pad_%d", k + 1);
        snprintf(PKEY[P_TYPE1 + k], 24, "type_%d", k + 1);
        snprintf(PKEY[P_FILE1 + k], 24, "file_%d", k + 1);
        snprintf(PKEY[P_PREV1 + k], 24, "prev_%d", k + 1);
        snprintf(PKEY[P_NEXT1 + k], 24, "next_%d", k + 1);
        snprintf(PKEY[P_LOCK1 + k], 24, "lock_%d", k + 1);
    }
    done = 1;
}

enum { M_TUNE, M_DECAY, M_LEVEL, M_FILTER };
enum { F_DYN, F_FIXED, F_OFF };
enum { SRC_SHOTS, SRC_BREAK };
enum { CHOP_BEAT, CHOP_HITS };
enum { TM_ASIS, TM_TUNE, TM_STRETCH };
enum { T_KICK, T_SNARE, T_CLAP, T_HAT, T_OPENHAT, T_PERC, T_TOM, T_CYMBAL, T_OTHER, NTYPES };
static const char *const TYPE_NAME[NTYPES] = {"KICK", "SNARE", "CLAP", "HAT", "OPEN HAT", "PERC", "TOM", "CYMBAL", "OTHER"};
static const int ROLE[NP8] = {T_KICK, T_SNARE, T_HAT, T_OPENHAT, T_CLAP, T_PERC, T_TOM, T_CYMBAL};
static const int MULTI[8] = {0, 2, 3, 5, 7, 8, 10, 12};

/* ================================================================ files ================================================ */
static int has_ext(const char *n, const char *ext) {
    size_t a = strlen(n), b = strlen(ext);
    return a > b && !strcasecmp(n + a - b, ext);
}
static int audio_file(const char *n) {
    return n[0] != '.' && (has_ext(n, ".wav") || has_ext(n, ".flac") || has_ext(n, ".mp3") || has_ext(n, ".aif") || has_ext(n, ".aiff"));
}
static int is_internal(const char *d) { return !strncmp(d, "az01", 4) || !strncmp(d, "acvs", 4); }

typedef struct { float *x; long n; unsigned rate; } mono_t;   /* mono, at the file's own rate */

/* decode at most max_sec seconds, mixed to mono */
static int decode_mono(const char *path, double max_sec, mono_t *m) {
    unsigned ch = 0, rate = 0;
    uint64_t want = 0, got = 0;
    float *raw = NULL;
    if (has_ext(path, ".mp3")) {
        drmp3 d;
        if (!drmp3_init_file(&d, path, NULL)) return -1;
        ch = d.channels; rate = d.sampleRate;
        want = (uint64_t)(max_sec * rate);
        if (ch && rate) raw = malloc((size_t)want * ch * sizeof(float));
        if (raw) got = drmp3_read_pcm_frames_f32(&d, want, raw);
        drmp3_uninit(&d);
    } else if (has_ext(path, ".flac")) {
        drflac *f = drflac_open_file(path, NULL);
        if (!f) return -1;
        ch = f->channels; rate = f->sampleRate;
        want = (uint64_t)(max_sec * rate);
        if (f->totalPCMFrameCount && f->totalPCMFrameCount < want) want = f->totalPCMFrameCount;
        if (ch && rate) raw = malloc((size_t)want * ch * sizeof(float));
        if (raw) got = drflac_read_pcm_frames_f32(f, want, raw);
        drflac_close(f);
    } else {
        drwav w;
        if (!drwav_init_file(&w, path, NULL)) return -1;
        ch = w.channels; rate = w.sampleRate;
        want = (uint64_t)(max_sec * rate);
        if (w.totalPCMFrameCount && w.totalPCMFrameCount < want) want = w.totalPCMFrameCount;
        if (ch && rate) raw = malloc((size_t)want * ch * sizeof(float));
        if (raw) got = drwav_read_pcm_frames_f32(&w, want, raw);
        drwav_uninit(&w);
    }
    if (!raw || !got || rate < 4000 || rate > 384000) { free(raw); return -1; }
    float *x = malloc((size_t)got * sizeof(float));
    if (!x) { free(raw); return -1; }
    for (uint64_t i = 0; i < got; i++) {
        float s = 0;
        for (unsigned c = 0; c < ch; c++) s += raw[i * ch + c];
        x[i] = s / (float)ch;
    }
    free(raw);
    m->x = x; m->n = (long)got; m->rate = rate;
    return 0;
}

/* ---------------- words: what a sample is, from its name and folders ---------------- */
#define MAXTOK 40
static int tokens(const char *s, char tok[MAXTOK][24]) {
    int n = 0, k = 0;
    char cur[24];
    for (const char *p = s;; p++) {
        unsigned char c = (unsigned char)*p;
        int al = c && isalnum(c);
        if (al && k < 23) cur[k++] = (char)tolower(c);
        if ((!al || !c) && k) {
            cur[k] = 0;
            if (n < MAXTOK) snprintf(tok[n++], 24, "%s", cur);
            int d = 0;
            while (cur[d] && isdigit((unsigned char)cur[d])) d++;
            if (d && cur[d] && n < MAXTOK) snprintf(tok[n++], 24, "%s", cur + d);
            int e = k;
            while (e > 0 && isdigit((unsigned char)cur[e - 1])) e--;
            if (e < k && e > 0 && n < MAXTOK) { cur[e] = 0; snprintf(tok[n++], 24, "%s", cur); }
            k = 0;
        }
        if (!c) break;
    }
    return n;
}
static int word_type(const char *t) {
    static const struct { const char *w; int type; } W[] = {
        {"kick", T_KICK}, {"kicks", T_KICK}, {"kik", T_KICK}, {"kck", T_KICK}, {"bd", T_KICK}, {"bassdrum", T_KICK},
        {"snare", T_SNARE}, {"snares", T_SNARE}, {"snr", T_SNARE}, {"sd", T_SNARE}, {"sn", T_SNARE}, {"rimshot", T_SNARE},
        {"clap", T_CLAP}, {"claps", T_CLAP}, {"clp", T_CLAP}, {"cp", T_CLAP}, {"snap", T_CLAP}, {"snaps", T_CLAP},
        {"hat", T_HAT}, {"hats", T_HAT}, {"hihat", T_HAT}, {"hihats", T_HAT}, {"hh", T_HAT}, {"chh", T_HAT}, {"ch", T_HAT},
        {"closedhat", T_HAT}, {"oh", T_OPENHAT}, {"ohh", T_OPENHAT}, {"openhat", T_OPENHAT},
        {"tom", T_TOM}, {"toms", T_TOM},
        {"cymbal", T_CYMBAL}, {"cymbals", T_CYMBAL}, {"crash", T_CYMBAL}, {"ride", T_CYMBAL}, {"china", T_CYMBAL},
        {"splash", T_CYMBAL}, {"cym", T_CYMBAL},
        {"perc", T_PERC}, {"percs", T_PERC}, {"percussion", T_PERC}, {"conga", T_PERC}, {"bongo", T_PERC},
        {"shaker", T_PERC}, {"tamb", T_PERC}, {"tambourine", T_PERC}, {"cowbell", T_PERC}, {"clave", T_PERC},
        {"rim", T_PERC}, {"woodblock", T_PERC}, {"triangle", T_PERC}, {"tabla", T_PERC}, {"timbale", T_PERC},
    };
    for (size_t i = 0; i < sizeof W / sizeof *W; i++) if (!strcmp(t, W[i].w)) return W[i].type;
    static const struct { const char *w; int type; } S[] = {   /* inside longer words ("popkick", "realclaps") */
        {"kick", T_KICK}, {"snare", T_SNARE}, {"clap", T_CLAP}, {"hihat", T_HAT}, {"crash", T_CYMBAL},
        {"cymbal", T_CYMBAL}, {"conga", T_PERC}, {"shaker", T_PERC}, {"cowbell", T_PERC}, {"bongo", T_PERC},
    };
    for (size_t i = 0; i < sizeof S / sizeof *S; i++) if (strstr(t, S[i].w)) return S[i].type;
    return -1;
}
/* the type of a sample: its file name first, then its folders from the nearest up */
static int sample_type(const char *rel) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", rel);
    char *dot = strrchr(tmp, '.');
    char *slash = strrchr(tmp, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    for (;;) {
        char *seg = strrchr(tmp, '/');
        const char *name = seg ? seg + 1 : tmp;
        char tok[MAXTOK][24];
        int n = tokens(name, tok), type = -1, open = 0;
        for (int i = 0; i < n; i++) {
            if (!strcmp(tok[i], "open")) open = 1;
            if (i + 1 < n && !strcmp(tok[i], "bass") && !strcmp(tok[i + 1], "drum")) type = T_KICK;
            if (i + 1 < n && !strcmp(tok[i], "hi") && !strncmp(tok[i + 1], "hat", 3)) type = T_HAT;
            int t = word_type(tok[i]);
            if (t >= 0) type = t;
        }
        if (type == T_HAT && open) type = T_OPENHAT;
        if (type >= 0) return type;
        if (!seg) return T_OTHER;
        *seg = 0;
    }
}

/* ---------------- the one-shot library ---------------- */
typedef struct { char *path; const char *name; int type; } shot_t;
typedef struct {
    shot_t *e; int n, cap;
    int *by[NTYPES]; int nby[NTYPES];
    char where[160];                    /* shown on KIT */
    char drive[256];                    /* SAVE KIT writes here */
} shots_t;

#define MAX_SHOTS 60000
static void shots_add(shots_t *L, const char *path, const char *rel, int drums_only) {
    if (L->n >= MAX_SHOTS) return;
    int type = sample_type(rel);
    if (drums_only && type == T_OTHER) return;
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 2048;
        shot_t *ne = realloc(L->e, (size_t)nc * sizeof *ne);
        if (!ne) return;
        L->e = ne; L->cap = nc;
    }
    shot_t *x = &L->e[L->n++];
    x->path = strdup(path);
    x->type = type;
    const char *s = strrchr(x->path, '/');
    x->name = s ? s + 1 : x->path;
}
static void shots_walk(shots_t *L, const char *dir, size_t root_len, int depth, int drums_only) {
    if (depth > 6 || L->n >= MAX_SHOTS) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    char p[1024];
    while ((de = readdir(d)) && L->n < MAX_SHOTS) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, ".drumsort")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) shots_walk(L, p, root_len, depth + 1, drums_only);
        else if (S_ISREG(st.st_mode) && audio_file(de->d_name) && st.st_size > 500 && st.st_size < 8 * 1024 * 1024)
            shots_add(L, p, p + root_len, drums_only);
    }
    closedir(d);
}
static int cmp_shot(const void *a, const void *b) { return strcasecmp(((const shot_t *)a)->path, ((const shot_t *)b)->path); }

static void read_drives(char drives[][256], int *nd) {
    *nd = 0;
    DIR *m = opendir("/media");
    struct dirent *de;
    if (!m) return;
    while ((de = readdir(m)) && *nd < 24) if (de->d_name[0] != '.') snprintf(drives[(*nd)++], 256, "%s", de->d_name);
    closedir(m);
    qsort(drives, *nd, 256, (int (*)(const void *, const void *))strcasecmp);
}

/* One-shots come from:
 *   1. a folder called "DRUM ONE SHOTS" at the top of any drive (MPC TOOLS' MAKE DRUM FOLDER makes one on the SD card)
 *   2. folders listed in /media/<drive>/Potential1200/folders.txt, one per line (a full path or a folder on that drive)
 *   3. when neither has anything: the Expansions and Samples folders on every drive, drum sounds only */
static void shots_scan(shots_t *L) {
    memset(L, 0, sizeof *L);
    char drives[24][256]; int nd = 0;
    read_drives(drives, &nd);
    int found = 0;
    for (int i = 0; i < nd; i++) {
        char p[600];
        snprintf(p, sizeof p, "/media/%s/DRUM ONE SHOTS", drives[i]);
        struct stat st;
        if (stat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        int before = L->n;
        shots_walk(L, p, strlen(p) + 1, 0, 0);
        if (L->n > before) {
            if (!found) snprintf(L->where, sizeof L->where, "DRUM ONE SHOTS");
            if (!L->drive[0] && !is_internal(drives[i])) snprintf(L->drive, sizeof L->drive, "/media/%s", drives[i]);
            found = 1;
        }
    }
    for (int i = 0; i < nd; i++) {
        char p[600];
        snprintf(p, sizeof p, "/media/%s/Potential1200/folders.txt", drives[i]);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char line[600];
        while (fgets(line, sizeof line, f)) {
            char *t = line;
            while (*t == ' ' || *t == '\t' || *t == '"') t++;
            size_t n = strlen(t);
            while (n && (t[n - 1] == '\n' || t[n - 1] == '\r' || t[n - 1] == ' ' || t[n - 1] == '"' || t[n - 1] == '/')) t[--n] = 0;
            if (!n || *t == '#') continue;
            char q[700];
            if (*t == '/') snprintf(q, sizeof q, "%s", t);
            else snprintf(q, sizeof q, "/media/%s/%s", drives[i], t);
            int before = L->n;
            shots_walk(L, q, strlen(q) + 1, 0, 0);
            if (L->n > before && !found) { snprintf(L->where, sizeof L->where, "YOUR FOLDERS (folders.txt)"); found = 1; }
        }
        fclose(f);
    }
    if (!found) {
        for (int i = 0; i < nd; i++) {
            static const char *const SUB[] = {"Expansions", "Samples"};
            for (int k = 0; k < 2; k++) {
                char p[600];
                snprintf(p, sizeof p, "/media/%s/%s", drives[i], SUB[k]);
                shots_walk(L, p, strlen(p) + 1, 0, 1);
            }
        }
        snprintf(L->where, sizeof L->where, "EXPANSIONS AND SAMPLES");
    }
    if (!L->drive[0]) for (int i = 0; i < nd; i++) if (!is_internal(drives[i])) { snprintf(L->drive, sizeof L->drive, "/media/%s", drives[i]); break; }
    if (L->n) qsort(L->e, L->n, sizeof *L->e, cmp_shot);
    for (int t = 0; t < NTYPES; t++) L->by[t] = malloc(sizeof(int) * (size_t)(L->n ? L->n : 1));
    for (int i = 0; i < L->n; i++) { int t = L->e[i].type; L->by[t][L->nby[t]++] = i; }
}
static void shots_free(shots_t *L) {
    for (int i = 0; i < L->n; i++) free(L->e[i].path);
    free(L->e);
    for (int t = 0; t < NTYPES; t++) free(L->by[t]);
    memset(L, 0, sizeof *L);
}
/* the type a pad draws from, or the nearest one that has sounds */
static int usable_type(const shots_t *L, int t) {
    static const int NEXT[NTYPES] = {T_OTHER, T_CLAP, T_SNARE, T_OPENHAT, T_HAT, T_TOM, T_PERC, T_OPENHAT, T_PERC};
    for (int k = 0; k < NTYPES; k++) { if (L->nby[t]) return t; t = NEXT[t]; }
    for (t = 0; t < NTYPES; t++) if (L->nby[t]) return t;
    return -1;
}

/* ---------------- the break library (as PotentialBreaks) ---------------- */
typedef struct { char *path; const char *name; int lib; } entry_t;
typedef struct { char name[96]; int count; } lib_t;
#define MAX_LIBS 48
typedef struct { entry_t *e; int n, cap; lib_t lib[MAX_LIBS]; int nlib; } breaks_t;

static void blib_add(breaks_t *L, const char *path, int lib) {
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 512;
        entry_t *ne = realloc(L->e, (size_t)nc * sizeof *ne);
        if (!ne) return;
        L->e = ne; L->cap = nc;
    }
    entry_t *x = &L->e[L->n++];
    x->path = strdup(path);
    x->lib = lib;
    const char *s = strrchr(x->path, '/');
    x->name = s ? s + 1 : x->path;
}
static void blib_walk(breaks_t *L, const char *dir, int lib, int depth) {
    if (depth > 4) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    char p[1024];
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) blib_walk(L, p, lib, depth + 1);
        else if (S_ISREG(st.st_mode) && audio_file(de->d_name) && st.st_size > 1000) blib_add(L, p, lib);
    }
    closedir(d);
}
static int cmp_entry(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->lib != y->lib) return x->lib - y->lib;
    return strcasecmp(x->path, y->path);
}
static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* a "DRUM BREAKS LIBRARY" folder at the top of any drive, and top-level folders with "break" in their name */
static void breaks_scan(breaks_t *L) {
    memset(L, 0, sizeof *L);
    snprintf(L->lib[0].name, sizeof L->lib[0].name, "ALL BREAKS");
    L->nlib = 1;
    char drives[24][256]; int nd = 0;
    read_drives(drives, &nd);
    char roots[32][600]; int nr = 0;
    for (int i = 0; i < nd && nr < 32; i++) {
        snprintf(roots[nr], 600, "/media/%s/DRUM BREAKS LIBRARY", drives[i]);
        struct stat st;
        if (!stat(roots[nr], &st) && S_ISDIR(st.st_mode)) nr++;
    }
    for (int i = 0; i < nd && nr < 32; i++) {
        if (is_internal(drives[i])) continue;
        char p[300];
        snprintf(p, sizeof p, "/media/%s", drives[i]);
        DIR *d = opendir(p);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && nr < 32) {
            if (de->d_name[0] == '.' || !strcasestr(de->d_name, "break") || !strcasecmp(de->d_name, "DRUM BREAKS LIBRARY")) continue;
            snprintf(roots[nr], 600, "%s/%s", p, de->d_name);
            struct stat st;
            if (!stat(roots[nr], &st) && S_ISDIR(st.st_mode)) nr++;
        }
        closedir(d);
    }
    for (int r = 0; r < nr; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        char *subs[MAX_LIBS]; int ns = 0, loose = -1;
        const char *rname = strrchr(roots[r], '/');
        rname = rname ? rname + 1 : roots[r];
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char q[1024];
            snprintf(q, sizeof q, "%s/%s", roots[r], de->d_name);
            struct stat st;
            if (stat(q, &st)) continue;
            if (S_ISDIR(st.st_mode)) { if (ns < MAX_LIBS) subs[ns++] = strdup(de->d_name); }
            else if (S_ISREG(st.st_mode) && audio_file(de->d_name) && st.st_size > 1000) {
                if (loose < 0 && L->nlib < MAX_LIBS) { loose = L->nlib++; snprintf(L->lib[loose].name, sizeof L->lib[loose].name, "%s", rname); }
                if (loose >= 0) blib_add(L, q, loose);
            }
        }
        closedir(d);
        qsort(subs, ns, sizeof *subs, cmp_str);
        for (int i = 0; i < ns; i++) {
            if (L->nlib < MAX_LIBS) {
                int li = L->nlib++;
                snprintf(L->lib[li].name, sizeof L->lib[li].name, "%s", subs[i]);
                char q[1024];
                snprintf(q, sizeof q, "%s/%s", roots[r], subs[i]);
                blib_walk(L, q, li, 1);
            }
            free(subs[i]);
        }
    }
    if (L->n) qsort(L->e, L->n, sizeof *L->e, cmp_entry);
    for (int i = 0; i < L->n; i++) L->lib[L->e[i].lib].count++;
    L->lib[0].count = L->n;
    int map[MAX_LIBS], k = 1;
    map[0] = 0;
    for (int i = 1; i < L->nlib; i++) {
        if (L->lib[i].count) { map[i] = k; if (k != i) L->lib[k] = L->lib[i]; k++; }
        else map[i] = -1;
    }
    L->nlib = k;
    for (int i = 0; i < L->n; i++) L->e[i].lib = map[L->e[i].lib];
}
static void breaks_free(breaks_t *L) {
    for (int i = 0; i < L->n; i++) free(L->e[i].path);
    free(L->e);
    memset(L, 0, sizeof *L);
}

/* ---------------- a break: tempo and hits (as PotentialBreaks) ---------------- */
static float *onset_env(const float *x, long n, int *nh) {   /* x mono at 44.1 kHz */
    int H = (int)(n / HOP);
    *nh = H;
    if (H < 4) return NULL;
    float *e = calloc((size_t)H, sizeof *e), *o = calloc((size_t)H, sizeof *o);
    if (!e || !o) { free(e); free(o); return NULL; }
    float prev = 0;
    for (int h = 0; h < H; h++) {
        double acc = 0;
        for (int i = 0; i < HOP; i++) {
            float m = x[(long)h * HOP + i];
            float d = m - prev; prev = m;
            acc += (double)d * d + 0.1 * (double)m * m;
        }
        e[h] = logf((float)(acc / HOP) + 1e-9f);
    }
    for (int h = 1; h < H; h++) { float d = e[h] - e[h - 1]; o[h] = d > 0 ? d : 0; }
    free(e);
    return o;
}
static float bpm_from_name(const char *name) {
    for (const char *p = name; *p; p++) {
        if (!strncasecmp(p, "bpm", 3) && p > name) {
            const char *q = p;
            while (q > name && q[-1] == ' ') q--;
            const char *s = q;
            while (s > name && isdigit((unsigned char)s[-1])) s--;
            if (q - s >= 2 && q - s <= 3) { float b = (float)atof(s); if (b >= 50 && b <= 220) return b; }
        }
    }
    return 0;
}
static float detect_bpm(const float *o, int H) {
    if (!o || H < 200) return 0;
    double best = -1; float bb = 0;
    for (float bpm = 80.0f; bpm <= 160.0f; bpm += 0.25f) {
        double lag = 6000.0 / bpm, s = 0;
        for (int m = 1; m <= 4; m++) {
            double Lg = lag * m;
            int l0 = (int)Lg; double fr = Lg - l0, a = 0;
            for (int h = l0 + 1; h < H; h++) a += o[h] * ((1 - fr) * o[h - l0] + fr * o[h - l0 - 1]);
            s += a / (m == 1 ? 1.0 : 1.5);
        }
        if (s > best) { best = s; bb = bpm; }
    }
    return bb;
}

typedef struct {
    float *x; long n;               /* the loop, mono, 44.1 kHz */
    float bpm; int bars;
    long start[NP8], len[NP8]; int nslices;
    char name[160];
} loop_t;
static int col_of(double f) { int c = (int)(f * WCOLS); return c < 0 ? 0 : c >= WCOLS ? WCOLS - 1 : c; }
static void loop_free(loop_t *l) { if (l) { free(l->x); free(l); } }

static void chop(loop_t *L, int mode) {
    int H = 0;
    float *o = onset_env(L->x, L->n, &H);
    long st[NP8]; int n = 0;
    if (mode == CHOP_HITS && o) {   /* the 8 strongest hits, at least a 1/8 note apart, the first slice at the start */
        float strength[NP8]; long pos[NP8]; int k = 0;
        int min_gap = (int)(6000.0f / L->bpm / 2.0f * 0.9f);
        if (min_gap < 6) min_gap = 6;
        for (int h = 1; h + 1 < H; h++) {
            if (o[h] < o[h - 1] || o[h] < o[h + 1] || o[h] < 0.15f) continue;
            int clash = -1;
            for (int j = 0; j < k; j++) if (labs(pos[j] - h) < min_gap) { clash = j; break; }
            if (clash >= 0) { if (o[h] > strength[clash]) { strength[clash] = o[h]; pos[clash] = h; } continue; }
            if (k < NP8 - 1) { strength[k] = o[h]; pos[k] = h; k++; continue; }
            int w = 0;
            for (int j = 1; j < k; j++) if (strength[j] < strength[w]) w = j;
            if (o[h] > strength[w]) { strength[w] = o[h]; pos[w] = h; }
        }
        for (int a = 0; a < k; a++) for (int b = a + 1; b < k; b++) if (pos[b] < pos[a]) { long t = pos[a]; pos[a] = pos[b]; pos[b] = t; }
        st[n++] = 0;
        for (int j = 0; j < k && n < NP8; j++) {
            long s = pos[j] * HOP - 88;
            if (s > st[n - 1] + HOP * 3) st[n++] = s;
        }
    } else {   /* on the beat: 8 equal parts, each snapped to the strongest hit within 30 ms */
        for (int i = 0; i < NP8; i++) {
            long s = (long)((double)L->n * i / NP8);
            if (o && i > 0) {
                int h0 = (int)(s / HOP), bh = h0; float bv = 0;
                for (int h = h0 - 3; h <= h0 + 3; h++) if (h > 0 && h < H && o[h] > bv) { bv = o[h]; bh = h; }
                if (bv > 0.3f) s = (long)bh * HOP - 88;
            }
            if (s < 0) s = 0;
            st[n++] = s;
        }
    }
    free(o);
    L->nslices = n;
    for (int i = 0; i < n; i++) {
        L->start[i] = st[i];
        long e = i + 1 < n ? st[i + 1] : L->n;
        L->len[i] = e > st[i] ? e - st[i] : 1;
    }
}

/* the loop from a decoded file (resampled to 44.1 kHz mono): whole for a loop, a random 2-bar window of a track */
static loop_t *make_loop(const mono_t *src, const char *name, unsigned *seed, int mode) {
    long n44 = (long)((double)src->n * FS / src->rate);
    if (n44 < FS / 4) return NULL;
    float *x = malloc((size_t)n44 * sizeof *x);
    if (!x) return NULL;
    double step = (double)src->rate / FS;
    for (long i = 0; i < n44; i++) {
        double p = i * step; long i0 = (long)p; double fr = p - i0;
        float a = src->x[i0], b = i0 + 1 < src->n ? src->x[i0 + 1] : a;
        x[i] = a + (b - a) * (float)fr;
    }
    loop_t *L = calloc(1, sizeof *L);
    if (!L) { free(x); return NULL; }
    snprintf(L->name, sizeof L->name, "%s", name);
    char *dot = strrchr(L->name, '.');
    if (dot) *dot = 0;
    int H = 0;
    float *o = onset_env(x, n44, &H);
    float bpm = bpm_from_name(name);
    long start = 0, len = n44;
    if ((double)n44 / FS <= LONG_FILE_SEC) {
        for (long i = 0; i < n44 && i < 3 * FS; i++) if (fabsf(x[i]) > 0.0056f) { start = i - 88; break; }
        if (start < 0) start = 0;
        len = n44 - start;
        if (!bpm) bpm = detect_bpm(o, H);
        if (bpm > 0) {
            double beats = (double)len / FS * bpm / 60.0;
            int b4 = (int)lround(beats / 4.0);
            if (b4 >= 1) {
                double exact = b4 * 4.0 * 60.0 * FS / len;
                if (fabs(exact - bpm) / bpm < 0.04) bpm = (float)exact;
                L->bars = b4;
            }
        }
        if (bpm <= 0) bpm = 95;
        if (!L->bars) L->bars = 1;
    } else {
        if (!bpm) bpm = detect_bpm(o, H);
        if (bpm <= 0) bpm = 95;
        long want = (long)(2 * 4 * 60.0 / bpm * FS);
        if (want > n44) want = n44;
        long last = n44 - want;
        for (int tries = 0; o && tries < 400; tries++) {
            *seed = *seed * 1664525u + 1013904223u;
            long cand = (long)((*seed >> 8) % (unsigned)(last / HOP + 1));
            if (cand < H && o[cand] > 1.0f) { start = cand * HOP - 88; break; }
        }
        if (start < 0) start = 0;
        if (start > last) start = last;
        len = want;
        L->bars = 2;
    }
    free(o);
    L->bpm = bpm;
    L->n = len;
    L->x = malloc((size_t)len * sizeof(float));
    if (!L->x) { free(x); free(L); return NULL; }
    memcpy(L->x, x + start, (size_t)len * sizeof(float));
    free(x);
    for (int i = 0; i < 88 && i < len; i++) { float g = i / 88.0f; L->x[i] *= g; L->x[len - 1 - i] *= g; }
    chop(L, mode);
    return L;
}

/* ================================================================ the sound =========================================== */
/* A pad's sound, ready to play. Sampled: 12-bit values at 26.04 kHz (int16, steps of 16). Clean: floats at 44.1 kHz. */
typedef struct { int16_t *d; float *c; long n; int clean; double speed; } snd_t;
typedef struct { snd_t s[NP8 + 1]; float bpm; int is_break; float f0[NP8 + 1], f1[NP8 + 1]; } bank_t;   /* s[8]: the whole break; f0..f1: where each pad's sound sits in the break (0..1) */

static void bank_free(bank_t *b) {
    if (!b) return;
    for (int i = 0; i <= NP8; i++) { free(b->s[i].d); free(b->s[i].c); }
    free(b);
}

/* "sample" a source the old way: point-sampled at 26.04 kHz (no filter in front: the highs fold back), 12 bits */
static int sample_sp(const float *x, long n, double rate, double speed, snd_t *o) {
    double step = rate * speed / SP_RATE;
    long m = (long)((double)n / step);
    if (m < 2) return -1;
    o->d = malloc((size_t)m * sizeof(int16_t));
    if (!o->d) return -1;
    for (long k = 0; k < m; k++) {
        long i = (long)(k * step);
        float v = x[i < n ? i : n - 1] * 1.12f;   /* a little hot into the converter, as people sampled */
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        int q = (int)lrintf(v * 2047.0f);         /* 12 bits */
        o->d[k] = (int16_t)(q * 16);
    }
    o->n = m; o->clean = 0; o->speed = speed;
    return 0;
}
static int sample_clean(const float *x, long n, double rate, snd_t *o) {
    double step = rate / FS;
    long m = (long)((double)n / step);
    if (m < 2) return -1;
    o->c = malloc((size_t)m * sizeof(float));
    if (!o->c) return -1;
    for (long k = 0; k < m; k++) {
        double p = k * step; long i0 = (long)p; double fr = p - i0;
        float a = x[i0 < n ? i0 : n - 1], b = i0 + 1 < n ? x[i0 + 1] : a;
        o->c[k] = a + (b - a) * (float)fr;
    }
    o->n = m; o->clean = 1; o->speed = 1;
    return 0;
}

/* ---------------- pads and voices ---------------- */
typedef struct {
    /* settings (the screen's values) */
    float tune;      /* semitones -12..12 */
    float decay;     /* 0..100 % (100 = the whole sound) */
    float level;     /* dB -40..6 (-40 = off) */
    float cutoff;    /* 0..100 % (DYNAMIC filter) */
    int filter, stretch, reverse, choke, lock;
    float start, end;   /* 0..1 of the sound */
    /* where its sound comes from */
    int type;           /* one-shots: the type it draws from */
    char path[512];     /* one-shots: the file */
    int slice;          /* break: the slice it plays */
} pad_t;

typedef struct {
    int on, pad, stop;
    const snd_t *snd;
    double ph, inc;                  /* position and step, in the sound's own samples */
    long a, b;                       /* start and end in the sound */
    int rev;
    double gpos, trate, off, offb; double ga, gb; int st;   /* STRETCH: grain anchors */
    float held, env, envmul, gain, fade;
    float z[4], bq[2];               /* filter state */
    int filter; float g1, k, b0, b1, b2, a1, a2;
} voice_t;

typedef struct {
    float p[NP];
    pad_t pad[NP8 + 1];   /* pad[8]: the whole break (PLAY WHOLE BREAK, note 52) */
    int sel;
    voice_t v[NP8 + 1];
    double clk;                      /* the 26.04 kHz clock against 44.1 kHz */
    float host_bpm;
    /* text */
    pthread_mutex_t text_mx;
    char disp2[128], lib_info[160], break_name[160], break_info[96], lib_name[128], status[128];
    char file_label[NP8][128];
    unsigned display_rev;
    /* the sounds: the audio thread owns cur; the worker hands a new bank over through next */
    bank_t *cur;
    pthread_mutex_t xfer_mx;
    bank_t *next, *garbage;
    /* the worker */
    pthread_t th; int th_ok;
    pthread_mutex_t mx; pthread_cond_t cv;
    int quit, rq_rebuild, rq_load, rq_rescan, rq_new_kit, rq_roll, rq_break, rq_save, rq_restore;
    unsigned rq_pads;                 /* one-shots: pads whose file changed (bit per pad) */
    int rq_step[NP8];                 /* one-shots: PREV (-1) / NEXT (+1) / random (2) per pad */
    char restore_break[512];
    pthread_mutex_t lib_mx;
    shots_t shots; breaks_t blib; int scanned;
    mono_t src[NP8];                  /* worker: each pad's decoded one-shot */
    loop_t *loop;                     /* worker: the break */
    int cur_lib, cur_entry;
    char break_path[512];
    unsigned seed;
    uint8_t pending[NP];             /* values the engine changed, for the screen (mpc_engine_changed) */
    time_t quiet_until;              /* after a project's state: its values, not moves (faders, knobs) */
    time_t tap_quiet_until;          /* while the plugin opens: no taps (pads, buttons) */
    /* KIT (BREAK) waveform: heights and pad starts (worker, under text_mx); playhead (audio thread) */
    uint8_t wave_h[WCOLS];
    int pad_col[NP8];
    int ph_col, ph_lo, ph_hi, ph_tick, ph_ch;
    uint64_t ph_dirty;               /* columns whose picture changed: told to the host (mpc_engine_changed) */
    pt_theme_t theme;
    pm_manual_t manual;
} sp_t;

static void set_text(sp_t *s, char *dst, size_t n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void set_text(sp_t *s, char *dst, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    pthread_mutex_lock(&s->text_mx);
    vsnprintf(dst, n, fmt, ap);
    s->display_rev++;
    pthread_mutex_unlock(&s->text_mx);
    va_end(ap);
}
static void bump(sp_t *s) { pthread_mutex_lock(&s->text_mx); s->display_rev++; pthread_mutex_unlock(&s->text_mx); }
static unsigned rnd(sp_t *s) { s->seed = s->seed * 1664525u + 1013904223u; return s->seed >> 8; }

/* ---------------- faders: the mode's value of each pad, as 0..1 ---------------- */
static float pad_value(const pad_t *p, int mode) {
    switch (mode) {
    case M_TUNE: return (p->tune + 12.0f) / 24.0f;
    case M_DECAY: return p->decay / 100.0f;
    case M_LEVEL: return (p->level + 40.0f) / 46.0f;
    default: return p->cutoff / 100.0f;
    }
}
static void set_pad_value(pad_t *p, int mode, float v) {
    if (v < 0) v = 0; else if (v > 1) v = 1;
    switch (mode) {
    case M_TUNE: p->tune = roundf(v * 24.0f - 12.0f); break;
    case M_DECAY: p->decay = roundf(v * 100.0f); break;
    case M_LEVEL: p->level = roundf((v * 46.0f - 40.0f) * 2.0f) / 2.0f; break;
    default: p->cutoff = roundf(v * 100.0f); break;
    }
}
static int value_text(const pad_t *p, int mode, char *buf, int n) {
    switch (mode) {
    case M_TUNE: return snprintf(buf, n, "%+d", (int)p->tune);
    case M_DECAY: return p->decay >= 100 ? snprintf(buf, n, "FULL") : snprintf(buf, n, "%d%%", (int)p->decay);
    case M_LEVEL: return p->level <= -40 ? snprintf(buf, n, "OFF") : snprintf(buf, n, "%+.1f dB", (double)p->level);
    default:
        if (p->filter == F_FIXED) return snprintf(buf, n, "FIXED");
        if (p->filter == F_OFF) return snprintf(buf, n, "OFF");
        return snprintf(buf, n, "%d%%", (int)p->cutoff);
    }
}
static const char *const MODE_NAME[4] = {"TUNE", "DECAY", "LEVEL", "FILTER"};

static void mark(sp_t *s, int i) { s->pending[i] = 1; }
static void sync_faders(sp_t *s) {
    int mode = (int)s->p[P_MODE];
    for (int k = 0; k < NP8; k++) { s->p[P_F1 + k] = pad_value(&s->pad[k], mode); mark(s, P_F1 + k); }
}
static void sync_sel(sp_t *s) {   /* PROGRAM's controls show the selected pad */
    const pad_t *p = &s->pad[s->sel];
    s->p[P_TUNE] = p->tune; s->p[P_DECAY] = p->decay; s->p[P_LEVEL] = p->level; s->p[P_CUTOFF] = p->cutoff;
    s->p[P_FILTER] = (float)p->filter; s->p[P_STRETCH] = (float)p->stretch; s->p[P_REVERSE] = (float)p->reverse;
    s->p[P_CHOKE] = (float)p->choke;
    static const int K[] = {P_TUNE, P_DECAY, P_LEVEL, P_CUTOFF, P_FILTER, P_STRETCH, P_REVERSE, P_CHOKE};
    for (size_t i = 0; i < sizeof K / sizeof *K; i++) mark(s, K[i]);
    bump(s);
}
static void select_pad(sp_t *s, int k) {
    if (k == s->sel) return;
    s->sel = k;
    sync_sel(s);
    __atomic_or_fetch(&s->ph_dirty, ~0ull, __ATOMIC_RELEASE);   /* the lit pad start moves */
}

static void default_pads(sp_t *s) {
    for (int k = 0; k < NP8; k++) {
        pad_t *p = &s->pad[k];
        memset(p, 0, sizeof *p);
        p->decay = 100; p->level = 0; p->cutoff = 70;
        p->filter = k < 2 ? F_DYN : k < 6 ? F_FIXED : F_OFF;   /* like the old machine's outputs */
        p->start = 0; p->end = 1;
        p->type = ROLE[k];
        p->slice = k;
    }
    pad_t *w = &s->pad[NP8];
    memset(w, 0, sizeof *w);
    w->decay = 100; w->level = 0; w->cutoff = 70; w->filter = F_FIXED; w->end = 1; w->type = T_OTHER;
}

/* ---------------- playing ---------------- */
static void voice_filter_setup(voice_t *v, const pad_t *p) {
    v->filter = p->filter;
    if (p->filter == F_DYN) {
        double fc = 80.0 * pow(2.0, p->cutoff / 100.0 * 7.5);   /* 80 Hz .. 14.5 kHz */
        if (fc > 15000) fc = 15000;
        v->g1 = (float)(1.0 - exp(-2.0 * M_PI * fc / FS));
        v->k = 0.9f;   /* a little resonance */
    } else if (p->filter == F_FIXED) {   /* 2-pole low-pass at 9.5 kHz, Q 0.6 */
        double w0 = 2 * M_PI * 9500.0 / FS, al = sin(w0) / (2 * 0.6), c = cos(w0), a0 = 1 + al;
        v->b0 = (float)((1 - c) / 2 / a0); v->b1 = (float)((1 - c) / a0); v->b2 = v->b0;
        v->a1 = (float)(-2 * c / a0); v->a2 = (float)((1 - al) / a0);
    }
}

#define GRAIN 1024.0   /* STRETCH grain, in the sound's samples */
static float vel_gain(int vel) { float x = vel / 127.0f; return 0.2f + 0.8f * x * x; }

static void start_voice(sp_t *s, int padk, int ch, int vel, int semis) {
    bank_t *B = s->cur;
    if (!B) return;
    const snd_t *sd = &B->s[padk];
    if (!sd->n) return;
    pad_t *p = &s->pad[padk];
    if (p->choke)   /* pads in the same group cut each other */
        for (int i = 0; i <= NP8; i++) if (i != ch && s->v[i].on && s->pad[s->v[i].pad].choke == p->choke) s->v[i].stop = 1;
    voice_t *v = &s->v[ch];
    float held = v->on ? v->held : 0;
    memset(v, 0, sizeof *v);
    v->held = held;   /* the channel carries on from where it was: no click */
    v->on = 1; v->pad = padk; v->snd = sd;
    long a = (long)(p->start * sd->n), b = (long)(p->end * sd->n);
    if (b > sd->n) b = sd->n;
    if (b - a < 8) { a = 0; b = sd->n; }
    v->a = a; v->b = b; v->rev = p->reverse;
    double pitch = pow(2.0, (p->tune + semis) / 12.0);
    double trate = 1.0;
    int stretch = p->stretch;
    if (B->is_break && B->bpm > 0 && s->host_bpm > 0) {
        int tm = (int)s->p[P_TEMPO];
        if (tm == TM_TUNE) pitch *= s->host_bpm / B->bpm;            /* the old way: faster is higher */
        else if (tm == TM_STRETCH) { trate = s->host_bpm / B->bpm; stretch = 1; }
    }
    v->inc = pitch / sd->speed;      /* 45 -> 33: slowed back down */
    v->trate = trate / sd->speed;
    v->st = stretch;
    v->ph = (double)a; v->gpos = (double)a; v->ga = (double)a; v->gb = (double)a; v->off = 0; v->offb = GRAIN / 2;
    v->env = 1;
    float d = p->decay;
    v->envmul = d >= 100 ? 1.0f : (float)exp(-1.0 / (0.02 * pow(150.0, d / 100.0) * FS));
    v->gain = vel_gain(vel) * (p->level <= -40 ? 0.0f : powf(10.0f, p->level / 20.0f));
    v->fade = 1;
    voice_filter_setup(v, p);
    if (B->is_break) s->ph_ch = ch;   /* the waveform's playhead follows the newest hit */
}

static inline float rd(const snd_t *sd, const voice_t *v, long i) {
    if (i < v->a) i = v->a;
    if (i >= v->b) i = v->b - 1;
    if (v->rev) i = v->a + v->b - 1 - i;
    return sd->clean ? sd->c[i] : sd->d[i] * (1.0f / 32768.0f);
}
static inline float rd_lin(const snd_t *sd, const voice_t *v, double p) {   /* CLEAN: smooth */
    long i = (long)p; float fr = (float)(p - i);
    float a = rd(sd, v, i), b = rd(sd, v, i + 1);
    return a + (b - a) * fr;
}

/* the voice's next value at its own rate (one tick of the 26.04 kHz clock, or one 44.1 kHz sample when clean) */
static void voice_step(voice_t *v) {
    const snd_t *sd = v->snd;
    if (!v->st) {
        if (v->ph >= v->b) { v->on = 0; return; }
        v->held = sd->clean ? rd_lin(sd, v, v->ph) : rd(sd, v, (long)v->ph);   /* drop-sample: no interpolation */
        v->ph += v->inc;
        return;
    }
    /* STRETCH: two heads half a grain apart, each reading at the pitch from an anchor that moves at the tempo */
    if (v->gpos >= v->b) { v->on = 0; return; }
    float wa = (float)(1.0 - fabs(2.0 * v->off / GRAIN - 1.0)), wb = (float)(1.0 - fabs(2.0 * v->offb / GRAIN - 1.0));
    float a = sd->clean ? rd_lin(sd, v, v->ga + v->off * v->inc) : rd(sd, v, (long)(v->ga + v->off * v->inc));
    float b = sd->clean ? rd_lin(sd, v, v->gb + v->offb * v->inc) : rd(sd, v, (long)(v->gb + v->offb * v->inc));
    v->held = a * wa + b * wb;   /* the two triangles add up to 1 */
    v->off += 1.0; v->offb += 1.0;
    v->gpos += v->trate;
    if (v->off >= GRAIN) { v->off -= GRAIN; v->ga = v->gpos; }
    if (v->offb >= GRAIN) { v->offb -= GRAIN; v->gb = v->gpos; }
}

static inline float voice_out(voice_t *v) {   /* one 44.1 kHz sample: the held value, the envelope, the filter */
    float x = v->held * v->gain * v->env;
    v->env *= v->envmul;
    if (v->stop) { v->fade -= 1.0f / 88.0f; if (v->fade <= 0) { v->on = 0; return 0; } x *= v->fade; }
    if (v->env < 0.0005f && v->envmul < 1.0f) { v->on = 0; return 0; }
    if (v->filter == F_DYN) {
        float u = x - v->k * v->z[3];
        u = tanhf(u);
        v->z[0] += v->g1 * (u - v->z[0]);
        v->z[1] += v->g1 * (v->z[0] - v->z[1]);
        v->z[2] += v->g1 * (v->z[1] - v->z[2]);
        v->z[3] += v->g1 * (v->z[2] - v->z[3]);
        return v->z[3] * (1.0f + v->k * 0.5f);
    }
    if (v->filter == F_FIXED) {
        float y = v->b0 * x + v->bq[0];
        v->bq[0] = v->b1 * x - v->a1 * y + v->bq[1];
        v->bq[1] = v->b2 * x - v->a2 * y;
        return y;
    }
    return x;
}

/* take a new bank when one is ready (audio thread: before a note starts and before each block) */
static void take_bank(sp_t *s) {
    if (pthread_mutex_trylock(&s->xfer_mx) == 0) {   /* take a new bank when one is ready */
        if (s->next && !s->garbage) {
            s->garbage = s->cur;
            s->cur = s->next;
            s->next = NULL;
            for (int i = 0; i <= NP8; i++) s->v[i].on = 0;
            s->ph_col = s->ph_lo = s->ph_hi = -1;
            __atomic_or_fetch(&s->ph_dirty, ~0ull, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&s->xfer_mx);
            pthread_mutex_lock(&s->mx); pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mx);
        } else pthread_mutex_unlock(&s->xfer_mx);
    }
}

/* where the newest break hit is in the break, 0..1 (-1: nothing playing); with the played run's start in *from */
static double play_pos(const sp_t *s, double *from) {
    const bank_t *B = s->cur;
    if (!B || !B->is_break) return -1;
    const voice_t *v = &s->v[s->ph_ch];
    if (!v->on || !v->snd || v->snd->n <= 0) return -1;
    int k = v->pad;
    double n = (double)v->snd->n, x = v->st ? v->gpos : v->ph;
    if (x < v->a) x = v->a;
    if (x > v->b) x = v->b;
    double a = v->a, b = v->b;
    if (v->rev) { x = a + b - x; double t = a; a = b; b = t; }
    double f0 = B->f0[k], w = B->f1[k] - B->f0[k];
    *from = f0 + a / n * w;
    return f0 + x / n * w;
}
/* about 43 times a second: when the playhead moves, only the columns whose picture changed are reported */
static void wave_tick(sp_t *s, int frames) {
    if ((s->ph_tick += frames) < 1024) return;
    s->ph_tick = 0;
    double from = 0, x = play_pos(s, &from);
    int col = -1, lo = -1, hi = -1;
    if (x >= 0) { col = col_of(x); int fc = col_of(from); lo = fc < col ? fc : col; hi = fc < col ? col : fc; }
    if (col == s->ph_col && lo == s->ph_lo && hi == s->ph_hi) return;
    uint64_t d = 0;
    for (int c = 0; c < WCOLS; c++) {
        int was = c == s->ph_col ? 2 : (s->ph_lo >= 0 && c >= s->ph_lo && c <= s->ph_hi) ? 1 : 0;
        int now = c == col ? 2 : (lo >= 0 && c >= lo && c <= hi) ? 1 : 0;
        if (was != now) d |= 1ull << c;
    }
    __atomic_store_n(&s->ph_col, col, __ATOMIC_RELAXED);
    __atomic_store_n(&s->ph_lo, lo, __ATOMIC_RELAXED);
    __atomic_store_n(&s->ph_hi, hi, __ATOMIC_RELAXED);
    __atomic_or_fetch(&s->ph_dirty, d, __ATOMIC_RELEASE);
}

static void render(void *inst, int16_t *out, int frames) {
    sp_t *s = inst;
    take_bank(s);
    wave_tick(s, frames);
    const double tick = SP_RATE / FS;
    for (int n = 0; n < frames; n++) {
        s->clk += tick;
        int ticks = 0;
        while (s->clk >= 1.0) { s->clk -= 1.0; ticks++; }
        float acc = 0;
        for (int i = 0; i <= NP8; i++) {
            voice_t *v = &s->v[i];
            if (!v->on) continue;
            if (v->snd->clean) voice_step(v);
            else for (int t = 0; t < ticks && v->on; t++) voice_step(v);
            if (v->on) acc += voice_out(v);
        }
        float y = acc;
        if (y > 0.98f) y = 0.98f + 0.02f * tanhf((y - 0.98f) / 0.02f);
        else if (y < -0.98f) y = -0.98f - 0.02f * tanhf((-y - 0.98f) / 0.02f);
        int16_t q = (int16_t)lrintf(y * 32767.0f);
        out[2 * n] = q; out[2 * n + 1] = q;   /* mono, as the old mix out */
    }
}

static int note_pad(int note) {   /* 0..7 pads 1-8, 8..15 pads 9-16, -1 none */
    if (note >= 36 && note < 52) return note - 36;
    if (note >= 0 && note < 16) return note;
    return -1;
}
static void hit(sp_t *s, int k, int vel) {
    if (k < 8) { start_voice(s, k, k, vel, 0); select_pad(s, k); return; }
    if ((int)s->p[P_PADS916] == 0) start_voice(s, s->sel, s->sel, vel, MULTI[k - 8]);   /* MULTI PITCH */
}
static void midi(void *inst, const uint8_t *msg, int len) {
    sp_t *s = inst;
    if (len < 3) return;
    int st = msg[0] & 0xF0, note = msg[1], vel = msg[2];
    if (st != 0x90 || vel == 0) return;   /* one-shots: note-offs are ignored */
    take_bank(s);                         /* a fresh kit plays from its very first hit */
    if (note == 52 && (int)s->p[P_SOURCE] == SRC_BREAK) { start_voice(s, NP8, NP8, vel, 0); return; }   /* the pad above: the whole break */
    int k = note_pad(note);
    if (k >= 0) hit(s, k, vel);
}

/* ================================================================ the worker ========================================== */
static void publish(sp_t *s, bank_t *b) {
    pthread_mutex_lock(&s->xfer_mx);
    if (s->next) bank_free(s->next);
    s->next = b;
    pthread_mutex_unlock(&s->xfer_mx);
}

/* KIT (BREAK): the waveform (64 column heights, 0..8) and where each pad's slice starts; and in the bank, where each pad's
 * sound sits in the break, so the audio thread can draw the playhead */
static void wave_build(sp_t *s, bank_t *b) {
    uint8_t h[WCOLS] = {0}; int pc[NP8];
    for (int k = 0; k < NP8; k++) pc[k] = -1;
    b->f0[NP8] = 0; b->f1[NP8] = 1;
    const loop_t *L = s->loop;
    if (b->is_break && L && L->n > 0 && L->nslices) {
        float pk[WCOLS] = {0}, mx = 0;
        for (int c = 0; c < WCOLS; c++) {
            long i0 = (long)((double)L->n * c / WCOLS), i1 = (long)((double)L->n * (c + 1) / WCOLS);
            for (long i = i0; i < i1; i++) { float a = fabsf(L->x[i]); if (a > pk[c]) pk[c] = a; }
            if (pk[c] > mx) mx = pk[c];
        }
        for (int c = 0; c < WCOLS; c++)
            if (mx > 0 && pk[c] > 0.002f) { int v = (int)ceilf(8.0f * powf(pk[c] / mx, 0.75f)); h[c] = v < 1 ? 1 : v > 8 ? 8 : v; }
        for (int k = 0; k < NP8; k++) {
            int sl = s->pad[k].slice % L->nslices;
            b->f0[k] = (float)((double)L->start[sl] / L->n);
            b->f1[k] = (float)((double)(L->start[sl] + L->len[sl]) / L->n);
            pc[k] = col_of(b->f0[k]);
        }
    } else for (int k = 0; k < NP8; k++) { b->f0[k] = 0; b->f1[k] = 1; }
    pthread_mutex_lock(&s->text_mx);
    memcpy(s->wave_h, h, sizeof h); memcpy(s->pad_col, pc, sizeof pc);
    pthread_mutex_unlock(&s->text_mx);
    __atomic_or_fetch(&s->ph_dirty, ~0ull, __ATOMIC_RELEASE);
}
/* a waveform column's picture: mark * 9 + height (see WMARKS) */
static int wave_frame(sp_t *s, int c) {
    pthread_mutex_lock(&s->text_mx);
    int h = s->wave_h[c], mk = 0;
    for (int k = 0; k < NP8; k++) if (s->pad_col[k] == c) { if (k == s->sel) { mk = 11 + k; break; } if (!mk) mk = 3 + k; }
    pthread_mutex_unlock(&s->text_mx);
    int pc = __atomic_load_n(&s->ph_col, __ATOMIC_RELAXED), lo = __atomic_load_n(&s->ph_lo, __ATOMIC_RELAXED),
        hi = __atomic_load_n(&s->ph_hi, __ATOMIC_RELAXED);
    if (c == pc) mk = 2;
    else if (!mk && lo >= 0 && c >= lo && c <= hi) mk = 1;
    return mk * 9 + h;
}

/* every pad's sound again, from the decoded sources (one-shots) or the break's slices */
static void rebuild(sp_t *s) {
    bank_t *b = calloc(1, sizeof *b);
    if (!b) return;
    int clean = (int)s->p[P_CHARACTER] == 1;
    double speed = (int)s->p[P_SPEED] == 1 ? SPEED45 : 1.0;
    int brk = (int)s->p[P_SOURCE] == SRC_BREAK;
    b->is_break = brk;
    for (int k = 0; k < NP8; k++) {
        const float *x = NULL; long n = 0; double rate = FS;
        if (brk) {
            if (!s->loop || !s->loop->nslices) continue;
            int sl = s->pad[k].slice % s->loop->nslices;
            x = s->loop->x + s->loop->start[sl]; n = s->loop->len[sl];
            b->bpm = s->loop->bpm;
        } else {
            if (!s->src[k].x) continue;
            x = s->src[k].x; n = s->src[k].n; rate = s->src[k].rate;
        }
        if (clean) sample_clean(x, n, rate, &b->s[k]);
        else sample_sp(x, n, rate, speed, &b->s[k]);
    }
    if (brk && s->loop) {
        if (clean) sample_clean(s->loop->x, s->loop->n, FS, &b->s[NP8]);
        else sample_sp(s->loop->x, s->loop->n, FS, speed, &b->s[NP8]);
    }
    wave_build(s, b);
    publish(s, b);
}

static void file_label(sp_t *s, int k) {
    const pad_t *p = &s->pad[k];
    if (!p->path[0]) { set_text(s, s->file_label[k], sizeof s->file_label[k], "-"); return; }
    const char *nm = strrchr(p->path, '/'); nm = nm ? nm + 1 : p->path;
    set_text(s, s->file_label[k], sizeof s->file_label[k], "%s", nm);
}

/* one-shots: load pad k's file */
static void load_shot(sp_t *s, int k) {
    free(s->src[k].x);
    memset(&s->src[k], 0, sizeof s->src[k]);
    if (s->pad[k].path[0] && decode_mono(s->pad[k].path, ONE_SHOT_SEC, &s->src[k]))
        set_text(s, s->status, sizeof s->status, "CAN'T READ %.60s", strrchr(s->pad[k].path, '/') ? strrchr(s->pad[k].path, '/') + 1 : s->pad[k].path);
    file_label(s, k);
}

/* one-shots: pad k's next / previous / a random sound of its type */
static void step_shot(sp_t *s, int k, int how) {
    pthread_mutex_lock(&s->lib_mx);
    int t = usable_type(&s->shots, s->pad[k].type);
    if (t < 0) { pthread_mutex_unlock(&s->lib_mx); return; }
    int cnt = s->shots.nby[t], cur = -1;
    for (int i = 0; i < cnt && cur < 0; i++) if (!strcmp(s->shots.e[s->shots.by[t][i]].path, s->pad[k].path)) cur = i;
    int i = how == 2 || cur < 0 ? (int)(rnd(s) % (unsigned)cnt) : (cur + how + cnt) % cnt;
    snprintf(s->pad[k].path, sizeof s->pad[k].path, "%s", s->shots.e[s->shots.by[t][i]].path);
    pthread_mutex_unlock(&s->lib_mx);
    load_shot(s, k);
}

static void show_lib_info(sp_t *s) {
    pthread_mutex_lock(&s->lib_mx);
    int n = s->shots.n, nt = 0;
    for (int t = 0; t < NTYPES; t++) if (s->shots.nby[t]) nt++;
    char where[160]; snprintf(where, sizeof where, "%s", s->shots.where);
    pthread_mutex_unlock(&s->lib_mx);
    if (!n) set_text(s, s->lib_info, sizeof s->lib_info, "NO ONE-SHOTS FOUND: MAKE A DRUM FOLDER IN MPC TOOLS, THEN RESCAN");
    else set_text(s, s->lib_info, sizeof s->lib_info, "FROM: %s  ·  %d SOUNDS  ·  %d TYPES", where, n, nt);
}
static void show_break(sp_t *s) {
    if (!s->loop) { set_text(s, s->break_name, sizeof s->break_name, "-"); set_text(s, s->break_info, sizeof s->break_info, "-"); return; }
    set_text(s, s->break_name, sizeof s->break_name, "%s", s->loop->name);
    set_text(s, s->break_info, sizeof s->break_info, "%.1f BPM  ·  %d BAR%s  ·  %d SLICES", (double)s->loop->bpm, s->loop->bars,
             s->loop->bars == 1 ? "" : "S", s->loop->nslices);
}
static void show_lib(sp_t *s) {
    if (!s->blib.n) { set_text(s, s->lib_name, sizeof s->lib_name, "NO BREAKS FOUND (A DRUM BREAKS LIBRARY FOLDER)"); return; }
    const lib_t *l = &s->blib.lib[s->cur_lib < s->blib.nlib ? s->cur_lib : 0];
    set_text(s, s->lib_name, sizeof s->lib_name, "%s  (%d)", l->name, l->count);
}
static void show_disp2(sp_t *s) {
    int clean = (int)s->p[P_CHARACTER] == 1, sp45 = (int)s->p[P_SPEED] == 1;
    const char *ch = clean ? "CLEAN" : sp45 ? "26 kHz · 12 BIT · 45>33" : "26 kHz · 12 BIT";
    if ((int)s->p[P_SOURCE] == SRC_BREAK)
        set_text(s, s->disp2, sizeof s->disp2, "%.40s  ·  %s", s->loop ? s->loop->name : "NO BREAK YET", ch);
    else set_text(s, s->disp2, sizeof s->disp2, "%.40s  ·  %s", s->shots.where[0] ? s->shots.where : "ONE-SHOTS", ch);
}

static int lib_first(sp_t *s, int lib) {
    if (lib <= 0) return 0;
    for (int i = 0; i < s->blib.n; i++) if (s->blib.e[i].lib == lib) return i;
    return -1;
}
static void load_break(sp_t *s, int i) {
    if (i < 0 || i >= s->blib.n) return;
    set_text(s, s->status, sizeof s->status, "LOADING THE BREAK...");
    mono_t m = {0};
    if (decode_mono(s->blib.e[i].path, MAX_FILE_SEC, &m)) { set_text(s, s->status, sizeof s->status, "CAN'T READ THIS BREAK - TRY ANOTHER"); return; }
    loop_t *L = make_loop(&m, s->blib.e[i].name, &s->seed, (int)s->p[P_CHOP]);
    free(m.x);
    if (!L) { set_text(s, s->status, sizeof s->status, "THIS BREAK IS TOO SHORT"); return; }
    loop_free(s->loop);
    s->loop = L;
    s->cur_entry = i;
    pthread_mutex_lock(&s->mx);
    snprintf(s->break_path, sizeof s->break_path, "%s", s->blib.e[i].path);
    pthread_mutex_unlock(&s->mx);
    show_break(s);
    set_text(s, s->status, sizeof s->status, "READY - PLAY THE PADS");
}
static void rechop(sp_t *s) {
    if (!s->loop || !s->break_path[0]) return;
    mono_t m = {0};
    if (decode_mono(s->break_path, MAX_FILE_SEC, &m)) return;
    const char *nm = strrchr(s->break_path, '/'); nm = nm ? nm + 1 : s->break_path;
    loop_t *L = make_loop(&m, nm, &s->seed, (int)s->p[P_CHOP]);
    free(m.x);
    if (!L) return;
    loop_free(s->loop);
    s->loop = L;
    show_break(s);
}

/* ROLL. One-shots: MILD gives up to 3 unlocked pads a new sound; MEDIUM a whole new kit with a little tune and decay;
 * WILD also wide tunes, the filters and the 45 trick. Break: MILD a new break; MEDIUM also the chop and the order;
 * WILD also the tempo way and tunes. Locked pads are left alone. */
static void roll(sp_t *s, int brk) {
    int w = (int)lroundf(s->p[P_WILD]);
    if (brk) {
        if (w >= 1) { s->p[P_CHOP] = (float)(rnd(s) & 1); mark(s, P_CHOP); }
        if (w >= 2) { s->p[P_TEMPO] = (float)(rnd(s) % 3); mark(s, P_TEMPO); }
        return;
    }
    int order[NP8];
    for (int i = 0; i < NP8; i++) order[i] = i;
    for (int i = NP8 - 1; i > 0; i--) { int j = (int)(rnd(s) % (unsigned)(i + 1)); int t = order[i]; order[i] = order[j]; order[j] = t; }
    int count = 0;
    for (int i = 0; i < NP8; i++) {
        int k = order[i];
        pad_t *p = &s->pad[k];
        if (p->lock) continue;
        if (w == 0 && count >= 3) break;
        s->rq_step[k] = 2;
        count++;
        if (w >= 1) {
            int span = w == 1 ? 2 : 7;
            p->tune = (float)((int)(rnd(s) % (unsigned)(2 * span + 1)) - span);
            p->decay = w == 1 ? (float)(70 + rnd(s) % 31) : (float)(35 + rnd(s) % 66);
            if (p->decay > 95) p->decay = 100;
        } else { p->tune = 0; }
        if (w == 2) {
            p->cutoff = (float)(30 + rnd(s) % 71);
            p->reverse = (rnd(s) % 10) == 0;
        }
    }
    if (w == 2) { s->p[P_SPEED] = (float)(rnd(s) & 1); mark(s, P_SPEED); }
    sync_faders(s);
    sync_sel(s);
}

/* ---------------- SAVE KIT: each pad as heard, through the same voice, as an MPC drum program ---------------- */
static float *render_pad(sp_t *s, const bank_t *B, int k, long *frames) {
    const snd_t *sd = &B->s[k];
    if (!sd->n) { *frames = 0; return NULL; }
    sp_t tmp;   /* only the fields start_voice() and voice_out() read */
    memset(&tmp, 0, sizeof tmp);
    memcpy(tmp.pad, s->pad, sizeof tmp.pad);
    memcpy(tmp.p, s->p, sizeof tmp.p);
    tmp.host_bpm = s->host_bpm;
    tmp.cur = (bank_t *)B;
    start_voice(&tmp, k, 0, 127, 0);
    voice_t *v = &tmp.v[0];
    long max = (long)(20.0 * FS), n = 0;
    float *o = malloc((size_t)max * 2 * sizeof(float));
    if (!o) return NULL;
    double clk = 0;
    while (v->on && n < max) {
        clk += SP_RATE / FS;
        int ticks = 0;
        while (clk >= 1.0) { clk -= 1.0; ticks++; }
        if (sd->clean) voice_step(v); else for (int t = 0; t < ticks && v->on; t++) voice_step(v);
        float y = v->on ? voice_out(v) : 0;
        o[2 * n] = o[2 * n + 1] = y;
        n++;
    }
    for (int t = 0; t < 2205 && n < max; t++, n++) {   /* the filter's tail */
        float y = voice_out(v);
        o[2 * n] = o[2 * n + 1] = v->on ? y : 0;
    }
    *frames = n;
    return o;
}
static void save_kit(sp_t *s) {
    pthread_mutex_lock(&s->xfer_mx);
    const bank_t *B = s->next ? s->next : s->cur;
    if (!B) { pthread_mutex_unlock(&s->xfer_mx); set_text(s, s->status, sizeof s->status, "NOTHING TO SAVE YET"); return; }
    set_text(s, s->status, sizeof s->status, "SAVING THE KIT...");
    float *pads[NP8] = {0}; long fr[NP8] = {0};
    for (int k = 0; k < NP8; k++) pads[k] = render_pad(s, B, k, &fr[k]);
    pthread_mutex_unlock(&s->xfer_mx);
    char dir[600], name[112], err[96], want[200];
    const char *drive = s->shots.drive[0] ? s->shots.drive : "/media/az01-internal";
    snprintf(dir, sizeof dir, "%s/Potential1200/Kits", drive);
    char d0[600]; snprintf(d0, sizeof d0, "%s/Potential1200", drive); mkdir(d0, 0777);
    if ((int)s->p[P_SOURCE] == SRC_BREAK && s->loop) snprintf(want, sizeof want, "1200 %s", s->loop->name);
    else snprintf(want, sizeof want, "1200 Kit");
    float *full[NP8]; long ffr[NP8];
    for (int k = 0; k < NP8; k++) { full[k] = pads[k]; ffr[k] = pads[k] ? fr[k] : 0; }
    int ok = !kit_write(dir, want, s->host_bpm > 0 ? s->host_bpm : 90, NP8, full, ffr, name, sizeof name, err, sizeof err);
    for (int k = 0; k < NP8; k++) free(pads[k]);
    if (!ok) set_text(s, s->status, sizeof s->status, "%s", err);
    else set_text(s, s->status, sizeof s->status, "SAVED KIT: %.40s  (Potential1200/Kits)", name);
}

static void scan(sp_t *s) {
    shots_t sh; breaks_t bl;
    shots_scan(&sh);
    breaks_scan(&bl);
    pthread_mutex_lock(&s->lib_mx);
    shots_free(&s->shots); breaks_free(&s->blib);
    s->shots = sh; s->blib = bl;
    s->scanned = 1;
    if (s->cur_lib >= s->blib.nlib) s->cur_lib = 0;
    pthread_mutex_unlock(&s->lib_mx);
    show_lib_info(s);
    show_lib(s);
    show_disp2(s);
}

static void *worker(void *arg) {
    sp_t *s = arg;
    set_text(s, s->status, sizeof s->status, "LOOKING FOR SOUNDS...");
    scan(s);
    /* a new plugin (no project): a first kit straight away */
    pthread_mutex_lock(&s->mx);
    int fresh = !s->rq_restore;
    pthread_mutex_unlock(&s->mx);
    if (fresh && s->shots.n) {
        for (int k = 0; k < NP8; k++) step_shot(s, k, 2);
        rebuild(s);
        set_text(s, s->status, sizeof s->status, "READY - PLAY THE PADS");
    } else if (fresh) set_text(s, s->status, sizeof s->status, "NO SOUNDS FOUND - SEE THE KIT PAGE");
    for (;;) {
        pthread_mutex_lock(&s->mx);
        while (!s->quit && !s->rq_rebuild && !s->rq_load && !s->rq_rescan && !s->rq_new_kit && !s->rq_roll && !s->rq_break &&
               !s->rq_save && !s->rq_restore && !s->rq_pads && !s->garbage)
            pthread_cond_wait(&s->cv, &s->mx);
        if (s->quit) { pthread_mutex_unlock(&s->mx); break; }
        int rb = s->rq_rebuild, rsc = s->rq_rescan, nk = s->rq_new_kit, rl = s->rq_roll, br = s->rq_break, sv = s->rq_save,
            rs = s->rq_restore;
        unsigned pads = s->rq_pads;
        int steps[NP8]; memcpy(steps, s->rq_step, sizeof steps);
        memset(s->rq_step, 0, sizeof s->rq_step);
        s->rq_rebuild = s->rq_rescan = s->rq_new_kit = s->rq_roll = s->rq_break = s->rq_save = s->rq_restore = 0;
        s->rq_pads = 0; s->rq_load = 0;
        char want[512]; snprintf(want, sizeof want, "%s", s->restore_break);
        pthread_mutex_unlock(&s->mx);
        pthread_mutex_lock(&s->xfer_mx);
        bank_t *g = s->garbage; s->garbage = NULL;
        pthread_mutex_unlock(&s->xfer_mx);
        bank_free(g);
        int dirty = rb;
        if (rsc) { set_text(s, s->status, sizeof s->status, "LOOKING FOR SOUNDS..."); scan(s); set_text(s, s->status, sizeof s->status, "READY"); }
        if (rs) {   /* a saved project: each pad's file, and the break */
            for (int k = 0; k < NP8; k++) load_shot(s, k);
            if (want[0]) {
                int found = -1;
                for (int i = 0; i < s->blib.n && found < 0; i++) if (!strcmp(s->blib.e[i].path, want)) found = i;
                const char *wn = strrchr(want, '/'); wn = wn ? wn + 1 : want;
                for (int i = 0; i < s->blib.n && found < 0; i++) if (!strcmp(s->blib.e[i].name, wn)) found = i;
                if (found >= 0) load_break(s, found);
            }
            dirty = 1;
            set_text(s, s->status, sizeof s->status, "READY - PLAY THE PADS");
        }
        if (nk) { for (int k = 0; k < NP8; k++) if (!s->pad[k].lock) steps[k] = 2; }
        for (int k = 0; k < NP8; k++) if (steps[k]) { step_shot(s, k, steps[k] == 2 ? 2 : steps[k] == 3 ? -1 : 1); dirty = 1; }
        for (int k = 0; k < NP8; k++) if (pads & (1u << k)) { load_shot(s, k); dirty = 1; }
        if (br == 4 || (rl && (int)s->p[P_SOURCE] == SRC_BREAK)) br = 1;   /* ROLL on a break: a random one */
        if (br && s->blib.n) {
            int lib = s->cur_lib, first = lib_first(s, lib), cnt = s->blib.lib[lib].count;
            if (br == 5) { s->cur_lib = (s->cur_lib + 1) % s->blib.nlib; show_lib(s); lib = s->cur_lib; first = lib_first(s, lib); cnt = s->blib.lib[lib].count; br = 1; }
            if (first >= 0 && cnt > 0) {
                if (br == 6) rechop(s);
                else {
                    int rel = s->cur_entry >= first && s->cur_entry < first + cnt ? s->cur_entry - first : 0;
                    int idx = br == 1 ? first + (int)(rnd(s) % (unsigned)cnt) : first + (rel + (br == 3 ? 1 : -1) + cnt) % cnt;
                    load_break(s, idx);
                }
                dirty = 1;
            }
            if (rl && (int)s->p[P_WILD] >= 1 && (int)s->p[P_SOURCE] == SRC_BREAK && (rnd(s) & 1 || (int)s->p[P_WILD] == 2)) {
                int perm[NP8];
                for (int k = 0; k < NP8; k++) perm[k] = k;
                for (int k = NP8 - 1; k > 0; k--) { int j = (int)(rnd(s) % (unsigned)(k + 1)); int t = perm[k]; perm[k] = perm[j]; perm[j] = t; }
                for (int k = 0; k < NP8; k++) s->pad[k].slice = perm[k];
            } else if (rl) for (int k = 0; k < NP8; k++) s->pad[k].slice = k;
        } else if (br) set_text(s, s->status, sizeof s->status, "NO BREAKS FOUND: A 'DRUM BREAKS LIBRARY' FOLDER, THEN RESCAN");
        if (dirty) { rebuild(s); show_disp2(s); }
        if (sv) save_kit(s);
    }
    return NULL;
}

static void wake(sp_t *s, int *flag, int val) {
    pthread_mutex_lock(&s->mx);
    *flag = val;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mx);
}

/* ================================================================ host interface ====================================== */
static void *create(const char *data_dir) {
    init_keys();
    sp_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    pt_theme_init(&s->theme, data_dir);
    default_pads(s);
    s->p[P_MODE] = M_TUNE;
    s->p[P_WILD] = 1;
    s->p[P_TEMPO] = TM_TUNE;
    s->p[P_CHOP] = CHOP_HITS;
    s->sel = 0;
    for (int k = 0; k < NP8; k++) s->p[P_F1 + k] = pad_value(&s->pad[k], M_TUNE);
    s->p[P_TUNE] = 0; s->p[P_DECAY] = 100; s->p[P_LEVEL] = 0; s->p[P_CUTOFF] = 70; s->p[P_FILTER] = F_DYN;
    s->seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)s;
    pthread_mutex_init(&s->text_mx, NULL);
    pthread_mutex_init(&s->xfer_mx, NULL);
    pthread_mutex_init(&s->mx, NULL);
    pthread_mutex_init(&s->lib_mx, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->tap_quiet_until = time(NULL) + QUIET_SEC;
    snprintf(s->status, sizeof s->status, "LOOKING FOR SOUNDS...");
    snprintf(s->lib_info, sizeof s->lib_info, "LOOKING...");
    snprintf(s->break_name, sizeof s->break_name, "-");
    snprintf(s->break_info, sizeof s->break_info, "-");
    snprintf(s->lib_name, sizeof s->lib_name, "-");
    snprintf(s->disp2, sizeof s->disp2, "-");
    for (int k = 0; k < NP8; k++) snprintf(s->file_label[k], sizeof s->file_label[k], "-");
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1024 * 1024);
    s->th_ok = pthread_create(&s->th, &at, worker, s) == 0;
    pthread_attr_destroy(&at);
    return s;
}

static void destroy(void *inst) {
    sp_t *s = inst;
    if (s->th_ok) {
        pthread_mutex_lock(&s->mx); s->quit = 1; pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mx);
        pthread_join(s->th, NULL);
    }
    bank_free(s->cur); bank_free(s->next); bank_free(s->garbage);
    for (int k = 0; k < NP8; k++) free(s->src[k].x);
    loop_free(s->loop);
    shots_free(&s->shots); breaks_free(&s->blib);
    pthread_mutex_destroy(&s->text_mx); pthread_mutex_destroy(&s->xfer_mx); pthread_mutex_destroy(&s->mx); pthread_mutex_destroy(&s->lib_mx);
    pthread_cond_destroy(&s->cv);
    free(s);
}

static int find(const char *key) {
    for (int i = 0; i < NP; i++) if (!strcmp(key, PKEY[i])) return i;
    return -1;
}

/* the state: global values, then each pad "pN=tune,decay,level,cutoff,filter,stretch,reverse,choke,lock,start,end,type,slice|path" */
static void restore(sp_t *s, const char *val) {
    char *tmp = strdup(val);
    if (!tmp) return;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (tok[0] == 'p' && isdigit((unsigned char)tok[1]) && !tok[2]) {
            int k = tok[1] - '1';
            if (k < 0 || k >= NP8) continue;
            pad_t *p = &s->pad[k];
            const char *bar = strchr(v, '|');
            float f[13] = {0};
            int n = sscanf(v, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7],
                           &f[8], &f[9], &f[10], &f[11], &f[12]);
            if (n >= 13) {
                p->tune = f[0]; p->decay = f[1]; p->level = f[2]; p->cutoff = f[3]; p->filter = (int)f[4]; p->stretch = (int)f[5];
                p->reverse = (int)f[6]; p->choke = (int)f[7]; p->lock = (int)f[8]; p->start = f[9]; p->end = f[10];
                p->type = (int)f[11]; p->slice = (int)f[12];
                if (p->type < 0 || p->type >= NTYPES) p->type = ROLE[k];
            }
            if (bar) snprintf(p->path, sizeof p->path, "%s", bar + 1);
        } else if (!strcmp(tok, "brk")) snprintf(s->restore_break, sizeof s->restore_break, "%s", v);
        else if (!strcmp(tok, "sel")) { int k = atoi(v); s->sel = k >= 0 && k < NP8 ? k : 0; }
        else if (!strcmp(tok, "lib")) s->cur_lib = atoi(v);
        else { int i = find(tok); if (i >= 0) s->p[i] = (float)atof(v); }
    }
    free(tmp);
    for (int k = 0; k < NP8; k++) { s->p[P_LOCK1 + k] = (float)s->pad[k].lock; mark(s, P_LOCK1 + k); }
    sync_faders(s);
    sync_sel(s);
    mark(s, P_MODE); mark(s, P_SOURCE); mark(s, P_SPEED); mark(s, P_CHARACTER); mark(s, P_PADS916);
    pthread_mutex_lock(&s->mx);
    s->rq_restore = 1;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mx);
}

static void set_param(void *inst, const char *key, const char *val) {
    sp_t *s = inst;
    if (pt_theme_set(&s->theme, key, val)) return;
    if (pm_set(&s->manual, key, val)) return;
    if (!strcmp(key, "lfo_bpm")) { s->host_bpm = (float)atof(val); return; }
    if (!strcmp(key, "state")) { s->quiet_until = s->tap_quiet_until = time(NULL) + QUIET_SEC; restore(s, val); return; }
    int i = find(key);
    if (i < 0 || (i >= P_W0 && i <= P_W63)) return;   /* the waveform is display only */
    float v = (float)atof(val);
    time_t now = time(NULL);
    int press = v > 0.5f, quiet = now < s->quiet_until, tapq = now < s->tap_quiet_until;
    if (i >= P_F1 && i <= P_F8) {   /* a fader: the mode's value of that pad */
        if (quiet) return;
        int k = i - P_F1, mode = (int)s->p[P_MODE];
        s->p[i] = v;
        set_pad_value(&s->pad[k], mode, v);
        if (k == s->sel) sync_sel(s);
        bump(s);
        return;
    }
    if (i >= P_PAD1 && i <= P_PAD8) {   /* a pad tile: play it and select it */
        if (tapq) return;
        hit(s, i - P_PAD1, 110);
        bump(s);
        return;
    }
    if (i >= P_PREV1 && i <= P_PREV8) { if (press && !tapq) { pthread_mutex_lock(&s->mx); s->rq_step[i - P_PREV1] = 3; pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mx); } return; }
    if (i >= P_NEXT1 && i <= P_NEXT8) { if (press && !tapq) { pthread_mutex_lock(&s->mx); s->rq_step[i - P_NEXT1] = 1; pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mx); } return; }
    if (i >= P_LOCK1 && i <= P_LOCK8) { s->p[i] = press; s->pad[i - P_LOCK1].lock = press; return; }
    pad_t *sp = &s->pad[s->sel];
    switch (i) {
    case P_DEAD: return;
    case P_MODE: {
        int m = (int)lroundf(v); if (m < 0) m = 0; if (m > 3) m = 3;
        if (m != (int)s->p[P_MODE]) { s->p[P_MODE] = (float)m; sync_faders(s); bump(s); }
        return;
    }
    case P_TUNE: if (quiet) return; sp->tune = roundf(v); break;
    case P_DECAY: if (quiet) return; sp->decay = roundf(v); break;
    case P_LEVEL: if (quiet) return; sp->level = v; break;
    case P_CUTOFF: if (quiet) return; sp->cutoff = roundf(v); break;
    case P_FILTER: if (quiet) return; sp->filter = (int)lroundf(v); break;
    case P_STRETCH: if (quiet) return; sp->stretch = press; s->p[i] = press; return;
    case P_REVERSE: if (quiet) return; sp->reverse = press; s->p[i] = press; return;
    case P_CHOKE: if (quiet) return; sp->choke = (int)lroundf(v); s->p[i] = (float)sp->choke; return;
    case P_START_PREV: case P_START_NEXT: case P_END_PREV: case P_END_NEXT: {
        if (!press || tapq) return;
        float d = (i == P_START_PREV || i == P_END_PREV) ? -0.01f : 0.01f;
        if (i == P_START_PREV || i == P_START_NEXT) { sp->start += d; if (sp->start < 0) sp->start = 0; if (sp->start > sp->end - 0.02f) sp->start = sp->end - 0.02f; }
        else { sp->end += d; if (sp->end > 1) sp->end = 1; if (sp->end < sp->start + 0.02f) sp->end = sp->start + 0.02f; }
        bump(s);
        return;
    }
    case P_SOURCE: {
        int m = press;
        if (m != (int)s->p[P_SOURCE]) {
            s->p[P_SOURCE] = (float)m;
            if (!quiet) {
                if (m == SRC_BREAK && !s->loop) wake(s, &s->rq_break, 1);   /* a first break */
                else wake(s, &s->rq_rebuild, 1);
            }
            show_disp2(s);
        }
        return;
    }
    case P_NEW_KIT: if (press && !tapq) { if ((int)s->p[P_SOURCE] == SRC_BREAK) wake(s, &s->rq_break, 1); else wake(s, &s->rq_new_kit, 1); } return;
    case P_ROLL:
        if (press && !tapq) {
            int brk = (int)s->p[P_SOURCE] == SRC_BREAK;
            pthread_mutex_lock(&s->mx);
            roll(s, brk);
            s->rq_roll = 1;
            pthread_cond_signal(&s->cv);
            pthread_mutex_unlock(&s->mx);
        }
        return;
    case P_WILD: s->p[i] = (float)(v < 0 ? 0 : v > 2 ? 2 : lroundf(v)); return;
    case P_UNLOCK: if (press) for (int k = 0; k < NP8; k++) { s->pad[k].lock = 0; s->p[P_LOCK1 + k] = 0; mark(s, P_LOCK1 + k); } return;
    case P_RANDOM_BREAK: if (press && !tapq) wake(s, &s->rq_break, 1); return;
    case P_PREV_BREAK: if (press && !tapq) wake(s, &s->rq_break, 2); return;
    case P_NEXT_BREAK: if (press && !tapq) wake(s, &s->rq_break, 3); return;
    case P_LIB_NEXT: if (press && !tapq) wake(s, &s->rq_break, 5); return;
    case P_CHOP: {
        int m = press;
        if (m != (int)s->p[P_CHOP]) { s->p[P_CHOP] = (float)m; if (!quiet) wake(s, &s->rq_break, 6); }
        return;
    }
    case P_TEMPO: s->p[i] = (float)lroundf(v); return;
    case P_PLAY_BREAK: if (press && !tapq) start_voice(s, NP8, NP8, 110, 0); return;
    case P_SHUFFLE:
        if (press && !tapq) {
            int perm[NP8];
            for (int k = 0; k < NP8; k++) perm[k] = s->pad[k].slice;
            for (int k = NP8 - 1; k > 0; k--) { int j = (int)(rnd(s) % (unsigned)(k + 1)); int t = perm[k]; perm[k] = perm[j]; perm[j] = t; }
            for (int k = 0; k < NP8; k++) s->pad[k].slice = perm[k];
            wake(s, &s->rq_rebuild, 1);
        }
        return;
    case P_SPEED: case P_CHARACTER: {
        int m = press;
        if (m != (int)s->p[i]) { s->p[i] = (float)m; if (!quiet) wake(s, &s->rq_rebuild, 1); show_disp2(s); }
        return;
    }
    case P_PADS916: s->p[i] = (float)press; return;
    case P_SAVE_KIT: if (press && !tapq) wake(s, &s->rq_save, 1); return;
    case P_RESCAN: if (press && !tapq) wake(s, &s->rq_rescan, 1); return;
    default: return;   /* text */
    }
    /* a PROGRAM control changed the selected pad: the faders follow */
    s->p[i] = v;
    if ((i == P_TUNE && (int)s->p[P_MODE] == M_TUNE) || (i == P_DECAY && (int)s->p[P_MODE] == M_DECAY) ||
        (i == P_LEVEL && (int)s->p[P_MODE] == M_LEVEL) || ((i == P_CUTOFF || i == P_FILTER) && (int)s->p[P_MODE] == M_FILTER)) {
        s->p[P_F1 + s->sel] = pad_value(sp, (int)s->p[P_MODE]);
        mark(s, P_F1 + s->sel);
    }
    bump(s);
}

int mpc_engine_changed(void *inst, char *key, int len) {
    sp_t *s = inst;
    for (int i = 0; i < NP; i++) if (s->pending[i]) { s->pending[i] = 0; snprintf(key, len, "%s", PKEY[i]); return 1; }
    uint64_t d = __atomic_load_n(&s->ph_dirty, __ATOMIC_ACQUIRE);
    if (d && (int)s->p[P_SOURCE] == SRC_BREAK) {   /* a waveform column that changed */
        int c = __builtin_ctzll(d);
        __atomic_and_fetch(&s->ph_dirty, ~(1ull << c), __ATOMIC_ACQ_REL);
        snprintf(key, len, "w_%02d", c);
        return 1;
    }
    return 0;
}

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    sp_t *s = inst;
    { int n = pt_theme_get(&s->theme, key, buf, buf_len); if (n) return n; }
    { int n = pm_get(&s->manual, key, buf, buf_len); if (n) return n; }
    if (!strcmp(key, "display_rev")) {
        pthread_mutex_lock(&s->text_mx);
        int n = snprintf(buf, buf_len, "%u", s->display_rev);
        pthread_mutex_unlock(&s->text_mx);
        return n;
    }
    if (!strcmp(key, "state")) {
        int n = 0;
        static const int keep[] = {P_MODE, P_SOURCE, P_WILD, P_CHOP, P_TEMPO, P_SPEED, P_CHARACTER, P_PADS916};
        for (size_t k = 0; k < sizeof keep / sizeof *keep && n < buf_len; k++) n += snprintf(buf + n, buf_len - n, "%s=%g;", PKEY[keep[k]], (double)s->p[keep[k]]);
        if (n < buf_len) n += snprintf(buf + n, buf_len - n, "sel=%d;lib=%d;", s->sel, s->cur_lib);
        for (int k = 0; k < NP8 && n < buf_len; k++) {
            const pad_t *p = &s->pad[k];
            n += snprintf(buf + n, buf_len - n, "p%d=%g,%g,%g,%g,%d,%d,%d,%d,%d,%g,%g,%d,%d|%s;", k + 1, (double)p->tune, (double)p->decay,
                          (double)p->level, (double)p->cutoff, p->filter, p->stretch, p->reverse, p->choke, p->lock, (double)p->start,
                          (double)p->end, p->type, p->slice, p->path);
        }
        pthread_mutex_lock(&s->mx);
        if (n < buf_len) n += snprintf(buf + n, buf_len - n, "brk=%s", s->break_path);
        pthread_mutex_unlock(&s->mx);
        return n >= buf_len ? buf_len - 1 : n;
    }
    /* a fader's value text ("fader_3_display") and the pad tiles' lit state ("pad_3_on") */
    size_t kl = strlen(key);
    if (kl > 8 && !strcmp(key + kl - 8, "_display") && !strncmp(key, "fader_", 6)) {
        int k = atoi(key + 6) - 1;
        if (k < 0 || k >= NP8) return 0;
        return value_text(&s->pad[k], (int)s->p[P_MODE], buf, buf_len);
    }
    if (kl > 3 && !strcmp(key + kl - 3, "_on") && !strncmp(key, "pad_", 4)) {
        int k = atoi(key + 4) - 1;
        return snprintf(buf, buf_len, "%d", k == s->sel);
    }
    if (key[0] == 'w' && key[1] == '_' && isdigit((unsigned char)key[2]) && isdigit((unsigned char)key[3]) && !key[4]) {
        int c = (key[2] - '0') * 10 + (key[3] - '0');   /* a waveform column: its picture as a value 0..1 */
        if (c >= WCOLS) return 0;
        int f = wave_frame(s, c);
        double v = f >= WFRAMES - 1 ? 1.0 : (f + 0.25) / (WFRAMES - 1);
        return snprintf(buf, buf_len, "%.5f", v);
    }
    int i = find(key);
    if (i < 0) return 0;
    int n = 0;
    pthread_mutex_lock(&s->text_mx);
    if (i >= P_PAD1 && i <= P_PAD8) {
        int k = i - P_PAD1;
        n = (int)s->p[P_SOURCE] == SRC_BREAK ? snprintf(buf, buf_len, "SLICE %d", s->pad[k].slice + 1)
                                            : snprintf(buf, buf_len, "%s", TYPE_NAME[s->pad[k].type]);
    } else if (i >= P_TYPE1 && i <= P_TYPE8) n = snprintf(buf, buf_len, "%s", TYPE_NAME[s->pad[i - P_TYPE1].type]);
    else if (i >= P_FILE1 && i <= P_FILE8) n = snprintf(buf, buf_len, "%s", s->file_label[i - P_FILE1]);
    else switch (i) {
    case P_DISP: {
        char v[32]; value_text(&s->pad[s->sel], (int)s->p[P_MODE], v, sizeof v);
        const char *what = (int)s->p[P_SOURCE] == SRC_BREAK ? "SLICE" : TYPE_NAME[s->pad[s->sel].type];
        n = snprintf(buf, buf_len, "PAD %d  %s  ·  %s %s", s->sel + 1, what, MODE_NAME[(int)s->p[P_MODE]], v);
        break;
    }
    case P_DISP2: n = snprintf(buf, buf_len, "%s", s->disp2); break;
    case P_SEL_NAME: {
        const char *what = (int)s->p[P_SOURCE] == SRC_BREAK ? "SLICE" : TYPE_NAME[s->pad[s->sel].type];
        n = snprintf(buf, buf_len, "PAD %d  ·  %s", s->sel + 1, what);
        break;
    }
    case P_SEL_FILE:
        n = (int)s->p[P_SOURCE] == SRC_BREAK ? snprintf(buf, buf_len, "%s  ·  SLICE %d", s->break_name, s->pad[s->sel].slice + 1)
                                             : snprintf(buf, buf_len, "%s", s->file_label[s->sel]);
        break;
    case P_SEL_INFO: {
        const bank_t *B = s->cur;
        long len = B ? B->s[s->sel].n : 0;
        double rate = B && B->s[s->sel].clean ? FS : SP_RATE / (B ? B->s[s->sel].speed : 1);
        const pad_t *p = &s->pad[s->sel];
        n = snprintf(buf, buf_len, "%.2f s  ·  %ld SAMPLES  ·  PLAYS %d%% TO %d%%", rate > 0 ? len / rate : 0.0, len,
                     (int)lroundf(p->start * 100), (int)lroundf(p->end * 100));
        break;
    }
    case P_START: n = snprintf(buf, buf_len, "START %d%%", (int)lroundf(s->pad[s->sel].start * 100)); break;
    case P_END: n = snprintf(buf, buf_len, "END %d%%", (int)lroundf(s->pad[s->sel].end * 100)); break;
    case P_LIB_INFO: n = snprintf(buf, buf_len, "%s", s->lib_info); break;
    case P_BREAK_NAME: n = snprintf(buf, buf_len, "%s", s->break_name); break;
    case P_BREAK_INFO: n = snprintf(buf, buf_len, "%s", s->break_info); break;
    case P_LIB_NAME: n = snprintf(buf, buf_len, "%s", s->lib_name); break;
    case P_STATUS: n = snprintf(buf, buf_len, "%s", s->status); break;
    default: break;
    }
    pthread_mutex_unlock(&s->text_mx);
    if (n) return n;
    if (i == P_DEAD || (i >= P_PREV1 && i <= P_NEXT8) || i == P_NEW_KIT || i == P_ROLL || i == P_UNLOCK || i == P_RANDOM_BREAK ||
        i == P_PREV_BREAK || i == P_NEXT_BREAK || i == P_LIB_NEXT || i == P_PLAY_BREAK || i == P_SHUFFLE || i == P_SAVE_KIT ||
        i == P_RESCAN || i == P_START_PREV || i == P_START_NEXT || i == P_END_PREV || i == P_END_NEXT)
        return snprintf(buf, buf_len, "0");
    return snprintf(buf, buf_len, "%g", (double)s->p[i]);
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
