/*
 * templates.c — Parameterized NML program templates.
 *
 * Generates symbolic compact NML for five standard prediction patterns.
 * See templates.h for the full description.
 */

#include "templates.h"

#include <stdio.h>
#include <string.h>

/* ── Symbolic register names ─────────────────────────────────────────── */

/*
 * NML symbolic registers:  ι κ λ μ ν ξ ο π  (R0–R7)
 *                          α β γ δ ε ζ η θ  (RA–RH)
 *
 * We use registers for weights/biases and temporaries:
 *   κ,λ = w1,b1  |  μ,ν = w2,b2  |  ξ,ο = w3,b3  |  π,ι = w4,b4
 *   α = input data  |  β,γ,δ = temporaries  |  φ = output
 */

static const char *REG_W[] = { "\xce\xba", "\xce\xbc", "\xce\xbe", "\xcf\x80" }; /* κ μ ξ π */
static const char *REG_B[] = { "\xce\xbb", "\xce\xbd", "\xce\xbf", "\xce\xb9" }; /* λ ν ο ι */
static const char *REG_TMP[] = { "\xce\xb2", "\xce\xb3", "\xce\xb4" };           /* β γ δ */
static const char *REG_INPUT = "\xce\xb1";  /* α */
/* static const char *REG_OUTPUT = "\xcf\x86"; */  /* φ — reserved for future use */

/* Symbolic opcodes */
static const char *OP_LD   = "\xe2\x86\x93";  /* ↓ */
static const char *OP_MMUL = "\xc3\x97";      /* × */
static const char *OP_MADD = "\xe2\x8a\x95";  /* ⊕ */
static const char *OP_RELU = "\xcf\x81";      /* ρ */
static const char *OP_SIGM = "\xcf\x83";      /* σ */
static const char *OP_HALT = "\xe2\x97\xbc";  /* ◼ */

/* Pilcrow delimiter for compact form */
static const char PILCROW = '\xb6';

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Append a string to the output buffer, return new position or -1 on overflow */
static int emit(char *out, size_t out_sz, int pos, const char *s)
{
    size_t len = strlen(s);
    if ((size_t)pos + len >= out_sz) return -1;
    memcpy(out + pos, s, len);
    return pos + (int)len;
}

static int emit_char(char *out, size_t out_sz, int pos, char c)
{
    if ((size_t)pos + 1 >= out_sz) return -1;
    out[pos] = c;
    return pos + 1;
}

/* Emit: OP reg @label  (e.g. "↓ κ @w1") */
static int emit_load(char *out, size_t out_sz, int pos,
                     const char *reg, const char *label)
{
    pos = emit(out, out_sz, pos, OP_LD);     if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, " ");       if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, reg);       if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, " @");      if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, label);     if (pos < 0) return -1;
    return pos;
}

/* Emit: OP src dst  (e.g. "× κ α") or OP src → dst */
static int emit_op2(char *out, size_t out_sz, int pos,
                    const char *op, const char *a, const char *b)
{
    pos = emit(out, out_sz, pos, op);  if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, " "); if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, a);   if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, " "); if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, b);   if (pos < 0) return -1;
    return pos;
}

#define P(s) do { pos = emit(out, out_sz, pos, s); if (pos < 0) return -1; } while(0)
#define PC    do { pos = emit_char(out, out_sz, pos, PILCROW); if (pos < 0) return -1; } while(0)

/* ── Template generators ─────────────────────────────────────────────── */

/*
 * Generate the core forward-pass layers used by all templates:
 *   For each hidden layer:  MMUL → MADD → RELU
 *   Final output layer:     MMUL → MADD → (caller adds activation)
 *
 * Assumes weights/biases already loaded into REG_W[i]/REG_B[i].
 * Input is in REG_INPUT. Output ends up in tmp register.
 */
static int gen_forward_pass(char *out, size_t out_sz, int pos,
                            int n_layers, int has_output_layer)
{
    /* Hidden layers with RELU */
    for (int i = 0; i < n_layers; i++) {
        const char *src = (i == 0) ? REG_INPUT : REG_TMP[(i - 1) % 3];
        const char *dst = REG_TMP[i % 3];

        PC;
        pos = emit_op2(out, out_sz, pos, OP_MMUL, REG_W[i], src);
        if (pos < 0) return -1;

        PC;
        pos = emit_op2(out, out_sz, pos, OP_MADD, dst, REG_B[i]);
        if (pos < 0) return -1;

        /* RELU on hidden layers, not on the last if it's the output */
        if (!has_output_layer || i < n_layers - 1) {
            PC;
            pos = emit_op2(out, out_sz, pos, OP_RELU, dst, dst);
            if (pos < 0) return -1;
        }
    }
    return pos;
}

/* Last tmp register used by forward pass */
static const char *last_tmp(int n_layers)
{
    return REG_TMP[(n_layers - 1) % 3];
}

/* ── binary_classifier ───────────────────────────────────────────────── */

static int gen_binary_classifier(const TemplateParams *p, char *out, size_t out_sz)
{
    int pos = 0;
    int total_layers = p->n_hidden + 1; /* hidden layers + output layer */
    char label[16];

    /* Load weights and biases for each layer */
    for (int i = 0; i < total_layers && i < TEMPLATE_MAX_HIDDEN; i++) {
        if (i > 0) PC;
        snprintf(label, sizeof(label), "w%d", i + 1);
        pos = emit_load(out, out_sz, pos, REG_W[i], label);
        if (pos < 0) return -1;

        PC;
        snprintf(label, sizeof(label), "b%d", i + 1);
        pos = emit_load(out, out_sz, pos, REG_B[i], label);
        if (pos < 0) return -1;
    }

    /* Load input data */
    PC;
    pos = emit_load(out, out_sz, pos, REG_INPUT, "data");
    if (pos < 0) return -1;

    /* Forward pass — all layers, last one gets no RELU (we add SIGM) */
    pos = gen_forward_pass(out, out_sz, pos, total_layers, 1);
    if (pos < 0) return -1;

    /* Sigmoid activation on output */
    const char *out_reg = last_tmp(total_layers);
    PC;
    pos = emit_op2(out, out_sz, pos, OP_SIGM, out_reg, out_reg);
    if (pos < 0) return -1;

    /* Store result in named memory */
    PC;
    P("ST "); P(out_reg); P(" @"); P(p->output_key);

    /* Halt */
    PC;
    P(OP_HALT);

    out[pos] = '\0';
    return pos;
}

/* ── regression ──────────────────────────────────────────────────────── */

static int gen_regression(const TemplateParams *p, char *out, size_t out_sz)
{
    int pos = 0;
    int total_layers = p->n_hidden + 1;
    char label[16];

    for (int i = 0; i < total_layers && i < TEMPLATE_MAX_HIDDEN; i++) {
        if (i > 0) PC;
        snprintf(label, sizeof(label), "w%d", i + 1);
        pos = emit_load(out, out_sz, pos, REG_W[i], label);
        if (pos < 0) return -1;
        PC;
        snprintf(label, sizeof(label), "b%d", i + 1);
        pos = emit_load(out, out_sz, pos, REG_B[i], label);
        if (pos < 0) return -1;
    }

    PC;
    pos = emit_load(out, out_sz, pos, REG_INPUT, "data");
    if (pos < 0) return -1;

    /* Forward pass — output layer is linear (no activation) */
    pos = gen_forward_pass(out, out_sz, pos, total_layers, 1);
    if (pos < 0) return -1;

    /* Store (linear output, no sigmoid) */
    const char *out_reg = last_tmp(total_layers);
    PC;
    P("ST "); P(out_reg); P(" @"); P(p->output_key);

    PC;
    P(OP_HALT);

    out[pos] = '\0';
    return pos;
}

/* ── anomaly_detector ────────────────────────────────────────────────── */

static int gen_anomaly_detector(const TemplateParams *p, char *out, size_t out_sz)
{
    /* Autoencoder: input → compressed → reconstructed
       Anomaly = high reconstruction error */
    int pos = 0;
    /* Encoder: input_dim → hidden → bottleneck
       Decoder: bottleneck → hidden → input_dim
       For simplicity we use a single hidden layer each way */
    char label[16];

    /* Encoder weights */
    snprintf(label, sizeof(label), "enc_w");
    pos = emit_load(out, out_sz, pos, REG_W[0], label);
    if (pos < 0) return -1;
    PC;
    snprintf(label, sizeof(label), "enc_b");
    pos = emit_load(out, out_sz, pos, REG_B[0], label);
    if (pos < 0) return -1;

    /* Decoder weights */
    PC;
    snprintf(label, sizeof(label), "dec_w");
    pos = emit_load(out, out_sz, pos, REG_W[1], label);
    if (pos < 0) return -1;
    PC;
    snprintf(label, sizeof(label), "dec_b");
    pos = emit_load(out, out_sz, pos, REG_B[1], label);
    if (pos < 0) return -1;

    /* Load input */
    PC;
    pos = emit_load(out, out_sz, pos, REG_INPUT, "data");
    if (pos < 0) return -1;

    /* Encode: MMUL → MADD → RELU */
    PC;
    pos = emit_op2(out, out_sz, pos, OP_MMUL, REG_W[0], REG_INPUT);
    if (pos < 0) return -1;
    PC;
    pos = emit_op2(out, out_sz, pos, OP_MADD, REG_TMP[0], REG_B[0]);
    if (pos < 0) return -1;
    PC;
    pos = emit_op2(out, out_sz, pos, OP_RELU, REG_TMP[0], REG_TMP[0]);
    if (pos < 0) return -1;

    /* Decode: MMUL → MADD → SIGM */
    PC;
    pos = emit_op2(out, out_sz, pos, OP_MMUL, REG_W[1], REG_TMP[0]);
    if (pos < 0) return -1;
    PC;
    pos = emit_op2(out, out_sz, pos, OP_MADD, REG_TMP[1], REG_B[1]);
    if (pos < 0) return -1;
    PC;
    pos = emit_op2(out, out_sz, pos, OP_SIGM, REG_TMP[1], REG_TMP[1]);
    if (pos < 0) return -1;

    /* Store reconstruction — caller compares with input for error */
    PC;
    P("ST "); P(REG_TMP[1]); P(" @"); P(p->output_key);

    PC;
    P(OP_HALT);

    out[pos] = '\0';
    return pos;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int template_generate(const TemplateParams *params, char *out, size_t out_sz)
{
    if (!params || !out || out_sz < 64) return -1;
    if (params->n_hidden < 1 || params->n_hidden > TEMPLATE_MAX_HIDDEN) return -1;

    switch (params->template_id) {
    case TEMPLATE_BINARY_CLASSIFIER:
        return gen_binary_classifier(params, out, out_sz);

    case TEMPLATE_MULTICLASS_CLASSIFIER:
        /* Same structure as binary but output dim = output_classes.
           For now generate as binary — multiclass adds softmax later. */
        return gen_binary_classifier(params, out, out_sz);

    case TEMPLATE_REGRESSION:
        return gen_regression(params, out, out_sz);

    case TEMPLATE_RANKING:
        /* Ranking is regression applied per-item, scored and sorted.
           The NML program is identical to regression — sorting happens
           in the Oracle's assessment phase. */
        return gen_regression(params, out, out_sz);

    case TEMPLATE_ANOMALY_DETECTOR:
        return gen_anomaly_detector(params, out, out_sz);

    default:
        return -1;
    }
}

const char *template_name(int template_id)
{
    switch (template_id) {
    case TEMPLATE_BINARY_CLASSIFIER:     return "binary_classifier";
    case TEMPLATE_MULTICLASS_CLASSIFIER: return "multiclass_classifier";
    case TEMPLATE_REGRESSION:            return "regression";
    case TEMPLATE_RANKING:               return "ranking";
    case TEMPLATE_ANOMALY_DETECTOR:      return "anomaly_detector";
    default:                             return "unknown";
    }
}

int template_select(const char *intent)
{
    if (!intent) return -1;

    /* Binary classification keywords */
    if (strstr(intent, "binary") || strstr(intent, "yes/no") ||
        strstr(intent, "true/false") || strstr(intent, "win/lose") ||
        strstr(intent, "fraud") || strstr(intent, "above/below") ||
        strstr(intent, "detect") || strstr(intent, "classify"))
        return TEMPLATE_BINARY_CLASSIFIER;

    /* Regression keywords */
    if (strstr(intent, "regress") || strstr(intent, "predict a") ||
        strstr(intent, "how much") || strstr(intent, "what value") ||
        strstr(intent, "forecast") || strstr(intent, "estimate"))
        return TEMPLATE_REGRESSION;

    /* Ranking keywords */
    if (strstr(intent, "rank") || strstr(intent, "order") ||
        strstr(intent, "top ") || strstr(intent, "sort"))
        return TEMPLATE_RANKING;

    /* Anomaly detection keywords */
    if (strstr(intent, "anomal") || strstr(intent, "outlier") ||
        strstr(intent, "normal") || strstr(intent, "unusual"))
        return TEMPLATE_ANOMALY_DETECTOR;

    /* Multiclass keywords */
    if (strstr(intent, "categor") || strstr(intent, "which one") ||
        strstr(intent, "multiclass") || strstr(intent, "classes"))
        return TEMPLATE_MULTICLASS_CLASSIFIER;

    /* No match — caller should fall back to LLM */
    return -1;
}
