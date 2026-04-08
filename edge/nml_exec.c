/*
 * nml_exec.c — in-process NML execution.
 *
 * nml.c is included as a library (NML_LIBRARY_MODE suppresses main()).
 * All VM functions (vm_init, vm_assemble, vm_execute, …) are static to
 * this translation unit — they cannot be called from other .c files.
 *
 * NML_MAX_TENSOR_SIZE is set conservatively via the Makefile so this
 * file can be cross-compiled for MCU targets without changes.
 */

#include "nml_exec.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Include the NML runtime as a library.
   NML_LIBRARY_MODE is set via the Makefile (-DNML_LIBRARY_MODE) to exclude
   main(). NML_CRYPTO is not needed here — signature verification is done
   by crypto.c before this function is called, so the body is already unsigned. */
#include "../../nml/runtime/nml.c"

int nml_exec_run(const char *program_body, const char *data,
                 char *score_out, size_t score_sz)
{
    VM *vm = (VM *)calloc(1, sizeof(VM));
    if (!vm) return -1;

    vm_init(vm);
    vm->max_cycles = NML_MAX_CYCLES;

    if (data && *data) {
        int n = vm_load_data_from_string(vm, data);
        if (n < 0) {
            fprintf(stderr, "[nml_exec] data load failed (code %d)\n", n);
            vm_cleanup(vm);
            free(vm);
            return -1;
        }
    }

    int ninstr = vm_assemble(vm, program_body);
    if (ninstr < 0) {
        fprintf(stderr, "[nml_exec] assembly failed (code %d)\n", ninstr);
        vm_cleanup(vm);
        free(vm);
        return -1;
    }

    if (vm_validate(vm) != NML_OK) {
        fprintf(stderr, "[nml_exec] validation failed: %s\n", vm->error_msg);
        vm_cleanup(vm);
        free(vm);
        return -1;
    }

    int result = vm_execute(vm);
    if (result != NML_OK) {
        fprintf(stderr, "[nml_exec] execution error (code %d): %s\n",
                vm->error_code, vm->error_msg);
        /* Fall through — memory may still have partial results */
    }

    /* Extract score from named memory slots.
       Priority: fraud_score → risk_score → score → result */
    static const char * const KEYS[] = SCORE_KEYS;
    int found = 0;

    for (int k = 0; KEYS[k] && !found; k++) {
        for (int i = 0; i < vm->mem_count && !found; i++) {
            if (!vm->memory[i].used) continue;
            if (strcmp(vm->memory[i].label, KEYS[k]) != 0) continue;

            Tensor *t = &vm->memory[i].tensor;
            if (t->size == 0) continue;

            /* Take the last element (matches Python's re.findall nums[-1]) */
            double val = tensor_getd(t, (int)t->size - 1);
            snprintf(score_out, score_sz, "%.6f", val);
            found = 1;
        }
    }

    vm_cleanup(vm);
    free(vm);
    return found ? 0 : -2;
}

int nml_exec_validate(const char *program_body)
{
    return nml_exec_validate_msg(program_body, NULL, 0);
}

int nml_exec_validate_msg(const char *program_body,
                          char *err_out, size_t err_sz)
{
    if (err_out && err_sz > 0) err_out[0] = '\0';

    VM *vm = (VM *)calloc(1, sizeof(VM));
    if (!vm) {
        if (err_out && err_sz > 0)
            snprintf(err_out, err_sz, "VM allocation failed");
        return 0;
    }
    vm_init(vm);
    vm->max_cycles = NML_MAX_CYCLES;

    int ninstr = vm_assemble(vm, program_body);
    if (ninstr < 0) {
        if (err_out && err_sz > 0) {
            if (vm->error_msg[0])
                snprintf(err_out, err_sz, "Assembly error: %s", vm->error_msg);
            else
                snprintf(err_out, err_sz, "Assembly failed (code %d)", ninstr);
        }
        vm_cleanup(vm);
        free(vm);
        return 0;
    }

    if (vm_validate(vm) != NML_OK) {
        if (err_out && err_sz > 0) {
            if (vm->error_msg[0])
                snprintf(err_out, err_sz, "Validation error: %s", vm->error_msg);
            else
                snprintf(err_out, err_sz, "Validation failed");
        }
        vm_cleanup(vm);
        free(vm);
        return 0;
    }

    vm_cleanup(vm);
    free(vm);
    return 1;
}

/* ── Stateful VM API ──────────────────────────────────────────────────── */

struct NmlExecCtx {
    VM  *vm;
    int  train_start_pc;  /* index of first LD @training_data instruction */
    int  tnet_pc;         /* index of TNET instruction (-1 if not found)  */
    int  weights_saved;   /* 1 after at least one run_pass                */
};

NmlExecCtx *nml_exec_create(const char *program_body)
{
    VM *vm = (VM *)calloc(1, sizeof(VM));
    if (!vm) return NULL;

    vm_init(vm);
    vm->max_cycles = NML_MAX_CYCLES;

    int ninstr = vm_assemble(vm, program_body);
    if (ninstr < 0) {
        fprintf(stderr, "[nml_exec] create: assembly failed (%d)\n", ninstr);
        vm_cleanup(vm); free(vm); return NULL;
    }
    if (vm_validate(vm) != NML_OK) {
        fprintf(stderr, "[nml_exec] create: validation failed: %s\n", vm->error_msg);
        vm_cleanup(vm); free(vm); return NULL;
    }

    /* Locate the first LD @training_data and the TNET instruction */
    int train_start_pc = 0, tnet_pc = -1;
    for (int i = 0; i < vm->program_len; i++) {
        Instruction *ins = &vm->program[i];
        if (ins->op == OP_LD &&
            strcmp(ins->addr, "training_data") == 0 &&
            train_start_pc == 0)
            train_start_pc = i;
        if (ins->op == OP_TNET && tnet_pc < 0)
            tnet_pc = i;
    }

    NmlExecCtx *ctx = (NmlExecCtx *)malloc(sizeof(NmlExecCtx));
    if (!ctx) { vm_cleanup(vm); free(vm); return NULL; }

    ctx->vm             = vm;
    ctx->train_start_pc = train_start_pc;
    ctx->tnet_pc        = tnet_pc;
    ctx->weights_saved  = 0;
    return ctx;
}

int nml_exec_load_shard(NmlExecCtx *ctx,
                        const char *data_str,
                        const char *labels_str)
{
    if (!ctx || !ctx->vm) return -1;
    if (data_str && *data_str) {
        int n = vm_load_data_from_string(ctx->vm, data_str);
        if (n < 0) return -1;
    }
    if (labels_str && *labels_str) {
        int n = vm_load_data_from_string(ctx->vm, labels_str);
        if (n < 0) return -1;
    }
    return 0;
}

int nml_exec_run_pass(NmlExecCtx *ctx, float lr)
{
    (void)lr;  /* lr is encoded in the TNET immediate */
    if (!ctx || !ctx->vm) return -1;

    /*
     * Before re-running the program, sync trained weights (R1-R4) back into
     * the named memory slots that the weight-loading LD instructions reference.
     * This ensures the LD R1 @w1 instructions at the start of the program
     * reload the *current* trained weights rather than the original values.
     */
    if (ctx->weights_saved) {
        for (int i = 0; i < ctx->train_start_pc; i++) {
            Instruction *ins = &ctx->vm->program[i];
            /* LD Rn @name (int_params[3]==0 → named memory path) */
            if (ins->op == OP_LD && ins->addr[0] && ins->int_params[3] == 0) {
                MemorySlot *sl = vm_memory(ctx->vm, ins->addr);
                if (sl) {
                    tensor_copy(&sl->tensor, &ctx->vm->regs[ins->reg[0]]);
                    sl->used = 1;
                }
            }
        }
    }

    /* Force TNET to run exactly 1 epoch for shard-level granularity */
    double saved_tnet_imm = 0.0;
    if (ctx->tnet_pc >= 0) {
        saved_tnet_imm = ctx->vm->program[ctx->tnet_pc].imm;
        ctx->vm->program[ctx->tnet_pc].imm = 1.0;
    }

    /* vm_execute resets pc=0 internally — the weight sync above ensures the
     * initial LD R1 @w1 instructions pick up the trained weights. */
    int rc = vm_execute(ctx->vm);

    /* Restore original epoch count for nml_exec_score's final full run */
    if (ctx->tnet_pc >= 0)
        ctx->vm->program[ctx->tnet_pc].imm = saved_tnet_imm;

    ctx->weights_saved = 1;

    /* Inference-phase errors (e.g. missing @new_transaction) are expected
     * during streaming — TNET already updated R1-R4 before any such error. */
    (void)rc;
    return 0;
}

int nml_exec_score(NmlExecCtx *ctx, char *score_out, size_t score_sz)
{
    if (!ctx || !ctx->vm) return -1;

    static const char * const KEYS[] = SCORE_KEYS;
    for (int k = 0; KEYS[k]; k++) {
        for (int i = 0; i < ctx->vm->mem_count; i++) {
            if (!ctx->vm->memory[i].used) continue;
            if (strcmp(ctx->vm->memory[i].label, KEYS[k]) != 0) continue;
            Tensor *t = &ctx->vm->memory[i].tensor;
            if (t->size == 0) continue;
            double val = tensor_getd(t, (int)t->size - 1);
            snprintf(score_out, score_sz, "%.6f", val);
            return 0;
        }
    }
    return -2;
}

void nml_exec_destroy(NmlExecCtx *ctx)
{
    if (!ctx) return;
    if (ctx->vm) { vm_cleanup(ctx->vm); free(ctx->vm); }
    free(ctx);
}
