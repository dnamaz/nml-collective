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
