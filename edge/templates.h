/*
 * templates.h — Parameterized NML program templates.
 *
 * Five standard prediction patterns that cover ~90% of ML tasks.
 * The Architect selects a template, fills in dimensions, and produces
 * a valid NML program in microseconds — no LLM needed.
 *
 * Each template generates symbolic compact NML (pilcrow-delimited),
 * ready for signing and broadcast.
 *
 * Template parameters:
 *   input_dim     — number of input features
 *   hidden_dims[] — array of hidden layer widths
 *   n_hidden      — number of hidden layers (length of hidden_dims[])
 *   threshold     — decision boundary (binary_classifier, anomaly_detector)
 *   output_key    — named memory key for result extraction
 *   epochs        — TNET training cycles
 *   lr            — TNET learning rate (scaled as integer: 100 = 0.01)
 */

#ifndef EDGE_TEMPLATES_H
#define EDGE_TEMPLATES_H

#include <stddef.h>

/* Template identifiers */
#define TEMPLATE_BINARY_CLASSIFIER     0
#define TEMPLATE_MULTICLASS_CLASSIFIER 1
#define TEMPLATE_REGRESSION            2
#define TEMPLATE_RANKING               3
#define TEMPLATE_ANOMALY_DETECTOR      4
#define TEMPLATE_COUNT                 5

/* Maximum supported hidden layers */
#define TEMPLATE_MAX_HIDDEN 4

/* Template parameters */
typedef struct {
    int  template_id;
    int  input_dim;
    int  hidden_dims[TEMPLATE_MAX_HIDDEN];
    int  n_hidden;
    int  output_classes;                   /* multiclass only */
    float threshold;                       /* binary/anomaly only */
    char output_key[32];                   /* named memory key */
    int  epochs;
    int  lr_scaled;                        /* learning rate × 10000 */
} TemplateParams;

/*
 * Generate an NML program from a template.
 *
 * Writes symbolic compact NML (pilcrow-delimited) into out.
 * Returns chars written (excluding NUL), or -1 on error.
 *
 * The caller should validate the output with nml_exec dry-run
 * before submitting to the Sentient for signing.
 */
int template_generate(const TemplateParams *params, char *out, size_t out_sz);

/*
 * Return the human-readable name for a template ID.
 * Returns "unknown" for invalid IDs.
 */
const char *template_name(int template_id);

/*
 * Select a template ID from an intent string.
 * Uses keyword matching against known patterns.
 *
 * Returns a TEMPLATE_* constant, or -1 if no template matches
 * (caller should fall back to LLM generation).
 *
 * Examples:
 *   "binary classification" → TEMPLATE_BINARY_CLASSIFIER
 *   "predict a value"       → TEMPLATE_REGRESSION
 *   "rank items"            → TEMPLATE_RANKING
 *   "detect anomalies"      → TEMPLATE_ANOMALY_DETECTOR
 *   "classify into 5 types" → TEMPLATE_MULTICLASS_CLASSIFIER
 */
int template_select(const char *intent);

#endif /* EDGE_TEMPLATES_H */
