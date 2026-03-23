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

#endif /* EDGE_NML_EXEC_H */
