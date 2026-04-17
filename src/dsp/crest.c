/*
 * crest.c -- Hybrid wavetable synthesizer for Ableton Move / Schwung framework
 * Plugin API v2. Audio: 44100 Hz, 128 frames/block, stereo interleaved int16.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* --------------------------------------------------------------------------- */
/* Constants                                                                    */
/* --------------------------------------------------------------------------- */

#define SAMPLE_RATE  44100
#define BLOCK_SIZE   128
#define FRAME_SIZE   1024
#define MAX_FRAMES   256

/* --------------------------------------------------------------------------- */
/* Host / Plugin API types                                                      */
/* --------------------------------------------------------------------------- */

typedef struct host_api_v1 {
    uint32_t  api_version;
    int       sample_rate;
    int       frames_per_block;
    uint8_t  *mapped_memory;
    int       audio_out_offset;
    int       audio_in_offset;
    void    (*log)(const char *msg);
    int     (*midi_send_internal)(const uint8_t *msg, int len);
    int     (*midi_send_external)(const uint8_t *msg, int len);
    int     (*get_clock_status)(void);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t  api_version;
    void    *(*create_instance)(const char *module_dir, const char *json_defaults);
    void     (*destroy_instance)(void *instance);
    void     (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void     (*set_param)(void *instance, const char *key, const char *val);
    int      (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void     (*render_block)(void *instance, int16_t *out_lr, int frames);
} plugin_api_v2_t;

/* Module-level host pointer -- set once in move_plugin_init_v2, read-only after */
static const host_api_v1_t *g_host = NULL;

static void crest_log(const char *msg)
{
    if (g_host && g_host->log) g_host->log(msg);
}

/* --------------------------------------------------------------------------- */
/* ADSR stages                                                                  */
/* --------------------------------------------------------------------------- */

#define ENV_IDLE    0
#define ENV_ATTACK  1
#define ENV_DECAY   2
#define ENV_SUSTAIN 3
#define ENV_RELEASE 4

/* --------------------------------------------------------------------------- */
/* Instance struct                                                              */
/* --------------------------------------------------------------------------- */

typedef struct {
    /* Wavetable data */
    float wt[MAX_FRAMES][FRAME_SIZE];
    int   wt_frame_count;
    char  wt_filename[256];

    /* Parameters */
    float wt_position;   /* 0.0 - 1.0 */
    float volume;        /* 0.0 - 1.0, default 0.8 */

    /* ADSR params (milliseconds; sustain is linear 0-1) */
    float env1_attack;   /* default 5   ms */
    float env1_decay;    /* default 100 ms */
    float env1_sustain;  /* default 0.7    */
    float env1_release;  /* default 200 ms */

    /* ADSR state */
    int   env_stage;
    float env_val;

    /* Oscillator */
    float phase;       /* 0.0 - 1.0 */
    float phase_inc;

    /* Monophonic last-note priority */
    int   note_stack[128];
    int   note_stack_size;
    int   active_note;      /* -1 = none */
    float active_velocity;  /* 0.0 - 1.0 */
} crest_instance_t;

/* --------------------------------------------------------------------------- */
/* WAV loader                                                                   */
/* --------------------------------------------------------------------------- */

static int load_wavetable(crest_instance_t *inst, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) < 44) { fclose(f); return -1; }

    if (memcmp(hdr,      "RIFF", 4) ||
        memcmp(hdr +  8, "WAVE", 4) ||
        memcmp(hdr + 12, "fmt ", 4)) { fclose(f); return -1; }

    uint16_t audio_fmt = (uint16_t)(hdr[20] | (hdr[21] << 8));
    uint16_t num_ch    = (uint16_t)(hdr[22] | (hdr[23] << 8));
    uint16_t bits      = (uint16_t)(hdr[34] | (hdr[35] << 8));

    if (audio_fmt != 1 || num_ch != 1 || bits != 16) { fclose(f); return -1; }

    /* Scan past fmt chunk (size may be > 16) to find "data" chunk */
    long fmt_sz     = (long)(hdr[16] | (hdr[17]<<8) | (hdr[18]<<16) | (hdr[19]<<24));
    long scan_start = 12 + 8 + fmt_sz;
    fseek(f, scan_start, SEEK_SET);

    uint32_t data_size = 0;
    int found = 0;
    for (int tries = 0; tries < 16; tries++) {
        uint8_t tag[4], sz[4];
        if (fread(tag, 1, 4, f) < 4) break;
        if (fread(sz,  1, 4, f) < 4) break;
        uint32_t chunk_sz = (uint32_t)(sz[0]|(sz[1]<<8)|(sz[2]<<16)|(sz[3]<<24));
        if (memcmp(tag, "data", 4) == 0) { data_size = chunk_sz; found = 1; break; }
        fseek(f, (long)chunk_sz, SEEK_CUR);
    }
    if (!found || data_size == 0) { fclose(f); return -1; }

    int total_samples = (int)(data_size / 2);
    int frames = total_samples / FRAME_SIZE;
    if (frames < 1) { fclose(f); return -1; }
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;

    int16_t buf[FRAME_SIZE];
    for (int fr = 0; fr < frames; fr++) {
        int got = (int)fread(buf, 2, FRAME_SIZE, f);
        for (int s = 0; s < FRAME_SIZE; s++)
            inst->wt[fr][s] = (s < got) ? (buf[s] / 32768.0f) : 0.0f;
    }
    inst->wt_frame_count = frames;

    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    strncpy(inst->wt_filename, base, sizeof(inst->wt_filename) - 1);
    inst->wt_filename[sizeof(inst->wt_filename) - 1] = '\0';

    fclose(f);
    return 0;
}

/* --------------------------------------------------------------------------- */
/* Helpers                                                                      */
/* --------------------------------------------------------------------------- */

static inline float note_to_freq(int note)
{
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Catmull-Rom cubic interpolation (t in [0,1], interpolates between p1..p2) */
static inline float catmull_rom(float p0, float p1, float p2, float p3, float t)
{
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1)
                 + (-p0 + p2) * t
                 + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3) * t2
                 + (-p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3);
}

/* --------------------------------------------------------------------------- */
/* ADSR tick (per sample)                                                       */
/* --------------------------------------------------------------------------- */

static float adsr_tick(crest_instance_t *inst)
{
    const float sr = (float)SAMPLE_RATE;

    switch (inst->env_stage) {

    case ENV_IDLE:
        inst->env_val = 0.0f;
        break;

    case ENV_ATTACK: {
        float rate = (inst->env1_attack > 0.0f)
                   ? 1.0f / (inst->env1_attack * 0.001f * sr)
                   : 1.0f;
        inst->env_val += rate;
        if (inst->env_val >= 1.0f) {
            inst->env_val   = 1.0f;
            inst->env_stage = ENV_DECAY;
        }
        break;
    }

    case ENV_DECAY: {
        float span = 1.0f - inst->env1_sustain;
        float rate = (inst->env1_decay > 0.0f && span > 0.0f)
                   ? span / (inst->env1_decay * 0.001f * sr)
                   : span;
        inst->env_val -= rate;
        if (inst->env_val <= inst->env1_sustain) {
            inst->env_val   = inst->env1_sustain;
            inst->env_stage = ENV_SUSTAIN;
        }
        break;
    }

    case ENV_SUSTAIN:
        inst->env_val = inst->env1_sustain;
        break;

    case ENV_RELEASE: {
        float rate = (inst->env1_release > 0.0f && inst->env_val > 0.0f)
                   ? inst->env_val / (inst->env1_release * 0.001f * sr)
                   : inst->env_val;
        if (rate < 1e-6f) rate = 1e-6f;
        inst->env_val -= rate;
        if (inst->env_val <= 0.0f) {
            inst->env_val   = 0.0f;
            inst->env_stage = ENV_IDLE;
            inst->active_note = -1;
        }
        break;
    }
    }

    return clampf(inst->env_val, 0.0f, 1.0f);
}

/* --------------------------------------------------------------------------- */
/* Oscillator (one sample, advances phase)                                     */
/* --------------------------------------------------------------------------- */

static float oscillator_sample(crest_instance_t *inst)
{
    if (inst->wt_frame_count < 1) return 0.0f;

    /* Fractional frame position */
    float fpos = inst->wt_position * (float)(inst->wt_frame_count - 1);
    int   fi   = (int)fpos;
    float ft   = fpos - (float)fi;

    /* Clamp frame neighbours for Catmull-Rom */
    int f0 = (fi > 0)                              ? fi - 1 : 0;
    int f1 = fi;
    int f2 = (fi + 1 < inst->wt_frame_count)       ? fi + 1 : inst->wt_frame_count - 1;
    int f3 = (fi + 2 < inst->wt_frame_count)       ? fi + 2 : inst->wt_frame_count - 1;

    /* Fractional sample position -- guard against si == FRAME_SIZE at phase=1.0 */
    float spos = inst->phase * (float)FRAME_SIZE;
    int   si   = (int)spos;
    if (si >= FRAME_SIZE) si = FRAME_SIZE - 1;
    float st   = spos - (float)si;
    int   si1  = (si + 1) & (FRAME_SIZE - 1);  /* power-of-two wrap */

    /* Linear interpolation within each frame */
    float s0 = inst->wt[f0][si] + st * (inst->wt[f0][si1] - inst->wt[f0][si]);
    float s1 = inst->wt[f1][si] + st * (inst->wt[f1][si1] - inst->wt[f1][si]);
    float s2 = inst->wt[f2][si] + st * (inst->wt[f2][si1] - inst->wt[f2][si]);
    float s3 = inst->wt[f3][si] + st * (inst->wt[f3][si1] - inst->wt[f3][si]);

    /* Catmull-Rom across frames */
    float out = catmull_rom(s0, s1, s2, s3, ft);

    /* Advance phase */
    inst->phase += inst->phase_inc;
    if (inst->phase >= 1.0f) inst->phase -= 1.0f;

    return clampf(out, -1.0f, 1.0f);
}

/* --------------------------------------------------------------------------- */
/* Note stack (monophonic last-note priority)                                  */
/* --------------------------------------------------------------------------- */

static void note_stack_push(crest_instance_t *inst, int note)
{
    /* Remove duplicate first */
    for (int i = 0; i < inst->note_stack_size; i++) {
        if (inst->note_stack[i] == note) {
            memmove(&inst->note_stack[i], &inst->note_stack[i+1],
                    (inst->note_stack_size - i - 1) * sizeof(int));
            inst->note_stack_size--;
            break;
        }
    }
    if (inst->note_stack_size < 128)
        inst->note_stack[inst->note_stack_size++] = note;
}

static void note_stack_remove(crest_instance_t *inst, int note)
{
    for (int i = 0; i < inst->note_stack_size; i++) {
        if (inst->note_stack[i] == note) {
            memmove(&inst->note_stack[i], &inst->note_stack[i+1],
                    (inst->note_stack_size - i - 1) * sizeof(int));
            inst->note_stack_size--;
            return;
        }
    }
}

/* --------------------------------------------------------------------------- */
/* on_midi                                                                      */
/* --------------------------------------------------------------------------- */

static void on_midi(void *instance, const uint8_t *msg, int len, int source)
{
    (void)source;
    if (len < 3) return;

    crest_instance_t *inst   = (crest_instance_t *)instance;
    uint8_t           status = msg[0] & 0xF0;
    int               note   = msg[1] & 0x7F;
    int               vel    = msg[2] & 0x7F;

    if (status == 0x90 && vel > 0) {
        /* Note on */
        char lb[64];
        snprintf(lb, sizeof(lb), "crest: note_on note=%d vel=%d", note, vel);
        crest_log(lb);

        note_stack_push(inst, note);
        inst->active_note     = note;
        inst->active_velocity = vel / 127.0f;
        inst->phase           = 0.0f;
        inst->phase_inc       = note_to_freq(note) / (float)SAMPLE_RATE;
        inst->env_stage       = ENV_ATTACK;

    } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        /* Note off */
        note_stack_remove(inst, note);
        if (note == inst->active_note) {
            if (inst->note_stack_size > 0) {
                int prev = inst->note_stack[inst->note_stack_size - 1];
                inst->active_note = prev;
                inst->phase_inc   = note_to_freq(prev) / (float)SAMPLE_RATE;
            } else {
                inst->env_stage = ENV_RELEASE;
            }
        }
    }
}

/* --------------------------------------------------------------------------- */
/* set_param                                                                    */
/* --------------------------------------------------------------------------- */

static void set_param(void *instance, const char *key, const char *val)
{
    crest_instance_t *inst = (crest_instance_t *)instance;
    float fv = (float)atof(val);

    if      (strcmp(key, "wt_position")  == 0) inst->wt_position  = clampf(fv, 0.0f, 1.0f);
    else if (strcmp(key, "volume")       == 0) inst->volume        = clampf(fv, 0.0f, 1.0f);
    else if (strcmp(key, "env1_attack")  == 0) inst->env1_attack   = clampf(fv, 0.5f, 5000.0f);
    else if (strcmp(key, "env1_decay")   == 0) inst->env1_decay    = clampf(fv, 0.5f, 5000.0f);
    else if (strcmp(key, "env1_sustain") == 0) inst->env1_sustain  = clampf(fv, 0.0f, 1.0f);
    else if (strcmp(key, "env1_release") == 0) inst->env1_release  = clampf(fv, 0.5f, 5000.0f);
}

/* --------------------------------------------------------------------------- */
/* get_param                                                                    */
/* --------------------------------------------------------------------------- */

/* JSON returned for key "ui_hierarchy" */
static const char UI_HIERARCHY[] =
    "{\"modes\":null,\"levels\":{"
        "\"root\":{"
            "\"label\":\"Crest\","
            "\"knobs\":[\"wt_position\",\"\",\"\",\"\",\"\",\"\",\"\",\"volume\"],"
            "\"params\":["
                "{\"key\":\"wt_position\",\"label\":\"WT Pos\"},"
                "{\"key\":\"volume\",\"label\":\"Volume\"},"
                "{\"level\":\"env1\",\"label\":\"Envelope 1\"}"
            "]"
        "},"
        "\"env1\":{"
            "\"label\":\"Envelope 1\","
            "\"knobs\":[\"env1_attack\",\"env1_decay\",\"env1_sustain\",\"env1_release\","
                       "\"\",\"\",\"\",\"\"],"
            "\"params\":["
                "{\"key\":\"env1_attack\",\"label\":\"Attack\"},"
                "{\"key\":\"env1_decay\",\"label\":\"Decay\"},"
                "{\"key\":\"env1_sustain\",\"label\":\"Sustain\"},"
                "{\"key\":\"env1_release\",\"label\":\"Release\"}"
            "]"
        "}"
    "}}";

/* JSON returned for key "chain_params" */
static const char CHAIN_PARAMS[] =
    "["
    "{\"key\":\"wt_position\",\"name\":\"WT Position\","
     "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0},"
    "{\"key\":\"volume\",\"name\":\"Volume\","
     "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.8},"
    "{\"key\":\"env1_attack\",\"name\":\"Attack\","
     "\"type\":\"float\",\"min\":0.5,\"max\":5000,\"step\":1,\"default\":5},"
    "{\"key\":\"env1_decay\",\"name\":\"Decay\","
     "\"type\":\"float\",\"min\":0.5,\"max\":5000,\"step\":1,\"default\":100},"
    "{\"key\":\"env1_sustain\",\"name\":\"Sustain\","
     "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.7},"
    "{\"key\":\"env1_release\",\"name\":\"Release\","
     "\"type\":\"float\",\"min\":0.5,\"max\":5000,\"step\":1,\"default\":200}"
    "]";

static int get_param(void *instance, const char *key, char *buf, int buf_len)
{
    /* Guard: reject null buffer or non-positive size.
       Without this, a negative buf_len cast to size_t becomes SIZE_MAX,
       causing snprintf to overflow whatever buffer the caller actually has. */
    if (!buf || buf_len <= 0) return -1;

    crest_instance_t *inst = (crest_instance_t *)instance;

    if (strcmp(key, "ui_hierarchy") == 0)
        return snprintf(buf, (size_t)buf_len, "%s", UI_HIERARCHY);
    if (strcmp(key, "chain_params") == 0)
        return snprintf(buf, (size_t)buf_len, "%s", CHAIN_PARAMS);
    if (strcmp(key, "wt_filename") == 0)
        return snprintf(buf, (size_t)buf_len, "%s", inst->wt_filename);
    if (strcmp(key, "wt_position") == 0)
        return snprintf(buf, (size_t)buf_len, "%.4f", inst->wt_position);
    if (strcmp(key, "volume") == 0)
        return snprintf(buf, (size_t)buf_len, "%.4f", inst->volume);
    if (strcmp(key, "env1_attack") == 0)
        return snprintf(buf, (size_t)buf_len, "%.2f", inst->env1_attack);
    if (strcmp(key, "env1_decay") == 0)
        return snprintf(buf, (size_t)buf_len, "%.2f", inst->env1_decay);
    if (strcmp(key, "env1_sustain") == 0)
        return snprintf(buf, (size_t)buf_len, "%.4f", inst->env1_sustain);
    if (strcmp(key, "env1_release") == 0)
        return snprintf(buf, (size_t)buf_len, "%.2f", inst->env1_release);

    return -1;
}

/* --------------------------------------------------------------------------- */
/* render_block                                                                 */
/* --------------------------------------------------------------------------- */

static void render_block(void *instance, int16_t *out_lr, int frames)
{
    crest_instance_t *inst = (crest_instance_t *)instance;

    /* One-shot log so we know render_block is being called */
    static int render_logged = 0;
    if (!render_logged) {
        crest_log("crest: render_block called");
        render_logged = 1;
    }

    for (int i = 0; i < frames; i++) {
        float env    = adsr_tick(inst);
        float sample = 0.0f;

        if (inst->active_note >= 0 && inst->env_stage != ENV_IDLE)
            sample = oscillator_sample(inst) * env * inst->volume * inst->active_velocity;

        int16_t s16 = (int16_t)clampf(sample * 32767.0f, -32767.0f, 32767.0f);
        out_lr[2 * i]     = s16;  /* L */
        out_lr[2 * i + 1] = s16;  /* R */
    }
}

/* --------------------------------------------------------------------------- */
/* create / destroy instance                                                    */
/* --------------------------------------------------------------------------- */

static void *create_instance(const char *module_dir, const char *json_defaults)
{
    (void)json_defaults;

    crest_instance_t *inst = (crest_instance_t *)calloc(1, sizeof(crest_instance_t));
    if (!inst) return NULL;

    inst->volume        = 0.8f;
    inst->wt_position   = 0.0f;
    inst->env1_attack   = 5.0f;
    inst->env1_decay    = 100.0f;
    inst->env1_sustain  = 0.7f;
    inst->env1_release  = 200.0f;
    inst->env_stage     = ENV_IDLE;
    inst->env_val       = 0.0f;
    inst->active_note   = -1;
    inst->phase         = 0.0f;
    inst->phase_inc     = 0.0f;

    char path[512];
    snprintf(path, sizeof(path), "%s/wavetables/factory/default.wav", module_dir);

    char lb[560];
    snprintf(lb, sizeof(lb), "crest: loading %s", path);
    crest_log(lb);

    int wav_ok = load_wavetable(inst, path);

    snprintf(lb, sizeof(lb), "crest: wav result=%d frames=%d", wav_ok, inst->wt_frame_count);
    crest_log(lb);

    return inst;
}

static void destroy_instance(void *instance)
{
    free(instance);
}

/* --------------------------------------------------------------------------- */
/* Plugin API v2 registration                                                   */
/* --------------------------------------------------------------------------- */

static plugin_api_v2_t api = {
    .api_version      = 2,
    .create_instance  = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi          = on_midi,
    .set_param        = set_param,
    .get_param        = get_param,
    .render_block     = render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    return &api;
}
