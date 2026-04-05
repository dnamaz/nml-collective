/*
 * NML Edge Worker — in-process NML program execution.
 *
 * nml_exec_run() runs a program body (the unsigned text — SIGN header
 * already stripped by the caller) against optional data, and extracts
 * a numeric score from the named memory dump.
 *
 * Score key priority (mirrors nml_collective.py:broadcast_program()):
 *   fraud_score → risk_score → score → result  (first match wins)
 *
 * nml.c is included directly in nml_exec.c with NML_LIBRARY_MODE defined
 * to exclude its main(). All VM functions are static to that TU.
 */

#ifndef EDGE_NML_EXEC_H
#define EDGE_NML_EXEC_H

#include <stddef.h>

/*
 * Execute program_body (multi-line NML source, unsigned).
 * data is the .nml.data content as a string, or NULL.
 * score_out receives the score as a decimal string (e.g. "0.823141").
 * score_sz should be >= 32.
 *
 * Returns:
 *   0   — success, score_out filled
 *  -1   — assembly or execution error
 *  -2   — no score key found in memory dump
 */
int nml_exec_run(const char *program_body, const char *data,
                 char *score_out, size_t score_sz);

/*
 * Assembly-only validation: assemble and syntax-check program_body without
 * executing it.  Safe to call when no runtime data is available (e.g. during
 * program generation).  Returns 1 if the program assembles cleanly, 0 on error.
 */
int nml_exec_validate(const char *program_body);

/* ── Stateful VM API (Phase 3B streaming execution) ───────────────────── */

/*
 * Opaque handle for a VM kept alive across shard-level training passes.
 * Created once per program; destroyed after all shards are processed.
 */
typedef struct NmlExecCtx NmlExecCtx;

/*
 * Compile program_body and prepare for streaming training.
 * Weight registers (R1-R4) start zero-initialized.
 * Returns NULL on assembly/validation failure.
 */
NmlExecCtx *nml_exec_create(const char *program_body);

/*
 * Load a shard into the VM's named memory.
 * data_str format:   "@training_data shape=N,K dtype=f32 data=..."
 * labels_str format: "@training_labels shape=N,1 dtype=f32 data=..."
 * Either argument may be NULL.
 * Returns 0 on success, -1 on error.
 */
int nml_exec_load_shard(NmlExecCtx *ctx,
                        const char *data_str,
                        const char *labels_str);

/*
 * Run one training pass on the currently loaded shard.
 * Executes the full program with TNET epoch count forced to 1.
 * Updates weight registers (R1-R4) in place; ignores inference-phase
 * errors if training already completed.
 * lr is informational only (lr is encoded in TNET immediates).
 * Returns 0; errors in the inference phase are silently ignored.
 */
int nml_exec_run_pass(NmlExecCtx *ctx, float lr);

/*
 * Extract the score from named memory using the standard key priority
 * (fraud_score → risk_score → score → result).
 * Returns 0 on success, -2 if no score key found.
 */
int nml_exec_score(NmlExecCtx *ctx, char *score_out, size_t score_sz);

/*
 * Free the VM and all associated memory.
 */
void nml_exec_destroy(NmlExecCtx *ctx);

#endif /* EDGE_NML_EXEC_H */
