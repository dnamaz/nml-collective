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
static const char *OP_RELU = "\xe2\x8c\x90";  /* ⌐ */
static const char *OP_SIGM = "\xcf\x83";      /* σ */
static const char *OP_HALT = "\xe2\x97\xbc";  /* ◼ */
static const char *OP_TNET = "\xe2\xa5\x81";  /* ⥁ — train network in-place */

/*
 * TNET register convention (hardcoded by the NML runtime):
 *   ι (R0) = training input matrix (N × input_dim)
 *   κ (R1) = w1 weights           (input_dim × hidden_dim)  = REG_W[0]
 *   λ (R2) = b1 biases            (1 × hidden_dim)          = REG_B[0]
 *   μ (R3) = w2 weights           (hidden_dim × output_dim) = REG_W[1]
 *   ν (R4) = b2 biases            (1 × output_dim)          = REG_B[1]
 *   ς (R9) = training labels      (N × output_dim)
 * TNET modifies κ λ μ ν in-place (backprop + weight update).
 */
static const char *REG_TRAIN  = "\xce\xb9";  /* ι (R0) — TNET training input */
static const char *REG_LABELS = "\xcf\x82";  /* ς (R9) — TNET labels         */

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

/* Emit: OP a b  (2-operand form, e.g. "⌐ β β") */
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

/* Emit: OP a b c  (3-operand form, e.g. "× β κ α") */
static int emit_op3(char *out, size_t out_sz, int pos,
                    const char *op, const char *a, const char *b, const char *c)
{
    pos = emit(out, out_sz, pos, op);  if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, " "); if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, a);   if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, " "); if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, b);   if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, " "); if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, c);   if (pos < 0) return -1;
    return pos;
}

#define P(s) do { pos = emit(out, out_sz, pos, s); if (pos < 0) return -1; } while(0)
#define PC    do { pos = emit_char(out, out_sz, pos, PILCROW); if (pos < 0) return -1; } while(0)

/* ── data_keys helpers ───────────────────────────────────────────────── */

/* Record a required named-memory key in p->data_keys (non-const p) */
static void add_key(TemplateParams *p, const char *key)
{
    if (p->n_data_keys >= TEMPLATE_MAX_DATA_KEYS) return;
    snprintf(p->data_keys[p->n_data_keys], sizeof(p->data_keys[0]), "%s", key);
    p->n_data_keys++;
}

/* Emit: ⥁ #epochs #lr #mode [#batch_size] */
static int emit_tnet(char *out, size_t out_sz, int pos,
                     int epochs, int lr_scaled, int batch_size)
{
    char buf[80];
    float lr = lr_scaled > 0 ? lr_scaled / 10000.0f : 0.01f;
    if (batch_size > 0)
        snprintf(buf, sizeof(buf), " #%d #%.4f #0 #%d",
                 epochs > 0 ? epochs : 100, (double)lr, batch_size);
    else
        snprintf(buf, sizeof(buf), " #%d #%.4f #0",
                 epochs > 0 ? epochs : 100, (double)lr);
    pos = emit(out, out_sz, pos, OP_TNET); if (pos < 0) return -1;
    pos = emit(out, out_sz, pos, buf);     if (pos < 0) return -1;
    return pos;
}

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

        /* MMUL dst src w  (input × weight, matches row-vector convention) */
        PC;
        pos = emit_op3(out, out_sz, pos, OP_MMUL, dst, src, REG_W[i]);
        if (pos < 0) return -1;

        /* MADD dst dst bias  (3-operand: dest, left, right) */
        PC;
        pos = emit_op3(out, out_sz, pos, OP_MADD, dst, dst, REG_B[i]);
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

static int gen_binary_classifier(TemplateParams *p, char *out, size_t out_sz)
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
    pos = emit_load(out, out_sz, pos, REG_INPUT,
                    p->input_key[0] ? p->input_key : "data");
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

    /* Declare required keys */
    for (int i = 0; i < total_layers && i < TEMPLATE_MAX_HIDDEN; i++) {
        char wk[8], bk[8];
        snprintf(wk, sizeof(wk), "w%d", i + 1);
        snprintf(bk, sizeof(bk), "b%d", i + 1);
        add_key(p, wk); add_key(p, bk);
    }
    add_key(p, p->input_key[0] ? p->input_key : "data");

    return pos;
}

/* ── regression ──────────────────────────────────────────────────────── */

static int gen_regression(TemplateParams *p, char *out, size_t out_sz)
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
    pos = emit_load(out, out_sz, pos, REG_INPUT,
                    p->input_key[0] ? p->input_key : "data");
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

    /* Declare required keys */
    for (int i = 0; i < total_layers && i < TEMPLATE_MAX_HIDDEN; i++) {
        char wk[8], bk[8];
        snprintf(wk, sizeof(wk), "w%d", i + 1);
        snprintf(bk, sizeof(bk), "b%d", i + 1);
        add_key(p, wk); add_key(p, bk);
    }
    add_key(p, p->input_key[0] ? p->input_key : "data");

    return pos;
}

/* ── anomaly_detector ────────────────────────────────────────────────── */

static int gen_anomaly_detector(TemplateParams *p, char *out, size_t out_sz)
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
    pos = emit_load(out, out_sz, pos, REG_INPUT,
                    p->input_key[0] ? p->input_key : "data");
    if (pos < 0) return -1;

    /* Encode: MMUL → MADD → RELU */
    PC;
    pos = emit_op3(out, out_sz, pos, OP_MMUL, REG_TMP[0], REG_INPUT, REG_W[0]);
    if (pos < 0) return -1;
    PC;
    pos = emit_op3(out, out_sz, pos, OP_MADD, REG_TMP[0], REG_TMP[0], REG_B[0]);
    if (pos < 0) return -1;
    PC;
    pos = emit_op2(out, out_sz, pos, OP_RELU, REG_TMP[0], REG_TMP[0]);
    if (pos < 0) return -1;

    /* Decode: MMUL → MADD → SIGM */
    PC;
    pos = emit_op3(out, out_sz, pos, OP_MMUL, REG_TMP[1], REG_TMP[0], REG_W[1]);
    if (pos < 0) return -1;
    PC;
    pos = emit_op3(out, out_sz, pos, OP_MADD, REG_TMP[1], REG_TMP[1], REG_B[1]);
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

    /* Declare required keys */
    add_key(p, "enc_w"); add_key(p, "enc_b");
    add_key(p, "dec_w"); add_key(p, "dec_b");
    add_key(p, p->input_key[0] ? p->input_key : "data");

    return pos;
}

/* ── Training variants (TNET + inference) ────────────────────────────── */

/*
 * Training template pattern:
 *   1. Load initial weights (@w1 @b1 @w2 @b2) into TNET registers.
 *   2. Load @training_data → ι (R0) and @training_labels → ς (R9).
 *   3. Run TNET — modifies κ λ μ ν in-place with trained weights.
 *   4. Load inference input → α (REG_INPUT).
 *   5. Forward pass using trained weights → store output → HALT.
 *
 * Works with the standard demo data layout (training_data shape=N,6,
 * training_labels shape=N,1, w1 shape=6,8, b1 shape=1,8, etc.).
 * Workers that only have weights (no training data) should use
 * training_mode=0 instead.
 */

static int gen_binary_classifier_train(TemplateParams *p, char *out, size_t out_sz)
{
    int pos = 0;

    /* Load initial weights into TNET registers (κ λ μ ν) */
    pos = emit_load(out, out_sz, pos, REG_W[0], "w1"); if (pos < 0) return -1;
    PC; pos = emit_load(out, out_sz, pos, REG_B[0], "b1"); if (pos < 0) return -1;
    PC; pos = emit_load(out, out_sz, pos, REG_W[1], "w2"); if (pos < 0) return -1;
    PC; pos = emit_load(out, out_sz, pos, REG_B[1], "b2"); if (pos < 0) return -1;

    /* Load training data into TNET-hardcoded registers */
    PC; pos = emit_load(out, out_sz, pos, REG_TRAIN,  "training_data");   if (pos < 0) return -1;
    PC; pos = emit_load(out, out_sz, pos, REG_LABELS, "training_labels"); if (pos < 0) return -1;

    /* Train */
    PC; pos = emit_tnet(out, out_sz, pos, p->epochs, p->lr_scaled, p->batch_size); if (pos < 0) return -1;

    /* Load inference input into REG_INPUT (α) */
    const char *inkey = p->input_key[0] ? p->input_key : "new_transaction";
    PC; pos = emit_load(out, out_sz, pos, REG_INPUT, inkey); if (pos < 0) return -1;

    /* Forward pass: 2 layers (hidden=RELU, output=SIGM) */
    pos = gen_forward_pass(out, out_sz, pos, 2, 1); if (pos < 0) return -1;

    const char *out_reg = last_tmp(2);
    PC; pos = emit_op2(out, out_sz, pos, OP_SIGM, out_reg, out_reg); if (pos < 0) return -1;

    const char *okey = p->output_key[0] ? p->output_key : "fraud_score";
    PC; P("ST "); P(out_reg); P(" @"); P(okey);
    PC; P(OP_HALT);

    out[pos] = '\0';

    /* Declare required keys */
    add_key(p, "w1"); add_key(p, "b1"); add_key(p, "w2"); add_key(p, "b2");
    add_key(p, "training_data"); add_key(p, "training_labels");
    add_key(p, inkey);

    return pos;
}

static int gen_regression_train(TemplateParams *p, char *out, size_t out_sz)
{
    int pos = 0;

    pos = emit_load(out, out_sz, pos, REG_W[0], "w1"); if (pos < 0) return -1;
    PC; pos = emit_load(out, out_sz, pos, REG_B[0], "b1"); if (pos < 0) return -1;
    PC; pos = emit_load(out, out_sz, pos, REG_W[1], "w2"); if (pos < 0) return -1;
    PC; pos = emit_load(out, out_sz, pos, REG_B[1], "b2"); if (pos < 0) return -1;

    PC; pos = emit_load(out, out_sz, pos, REG_TRAIN,  "training_data");   if (pos < 0) return -1;
    PC; pos = emit_load(out, out_sz, pos, REG_LABELS, "training_labels"); if (pos < 0) return -1;

    PC; pos = emit_tnet(out, out_sz, pos, p->epochs, p->lr_scaled, p->batch_size); if (pos < 0) return -1;

    const char *inkey = p->input_key[0] ? p->input_key : "new_transaction";
    PC; pos = emit_load(out, out_sz, pos, REG_INPUT, inkey); if (pos < 0) return -1;

    /* Forward pass: 2 layers, linear output (no final activation) */
    pos = gen_forward_pass(out, out_sz, pos, 2, 1); if (pos < 0) return -1;

    const char *out_reg = last_tmp(2);
    const char *okey = p->output_key[0] ? p->output_key : "score";
    PC; P("ST "); P(out_reg); P(" @"); P(okey);
    PC; P(OP_HALT);

    out[pos] = '\0';

    add_key(p, "w1"); add_key(p, "b1"); add_key(p, "w2"); add_key(p, "b2");
    add_key(p, "training_data"); add_key(p, "training_labels");
    add_key(p, inkey);

    return pos;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int template_generate(TemplateParams *params, char *out, size_t out_sz)
{
    if (!params || !out || out_sz < 64) return -1;
    if (params->n_hidden < 1 || params->n_hidden > TEMPLATE_MAX_HIDDEN) return -1;

    /* Reset data_keys so each generation starts fresh */
    params->n_data_keys = 0;

    if (params->training_mode) {
        /* Training variants: TNET trains weights from @training_data/@training_labels,
           then runs a forward pass on the inference input. */
        switch (params->template_id) {
        case TEMPLATE_BINARY_CLASSIFIER:
        case TEMPLATE_MULTICLASS_CLASSIFIER:
            return gen_binary_classifier_train(params, out, out_sz);
        case TEMPLATE_REGRESSION:
        case TEMPLATE_RANKING:
            return gen_regression_train(params, out, out_sz);
        case TEMPLATE_ANOMALY_DETECTOR:
            /* Anomaly training not yet implemented — fall through to inference */
            break;
        default:
            return -1;
        }
    }

    /* Inference-only variants */
    switch (params->template_id) {
    case TEMPLATE_BINARY_CLASSIFIER:
    case TEMPLATE_MULTICLASS_CLASSIFIER:
        return gen_binary_classifier(params, out, out_sz);
    case TEMPLATE_REGRESSION:
    case TEMPLATE_RANKING:
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
