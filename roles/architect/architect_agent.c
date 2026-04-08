/*
 * architect_agent.c — NML Collective Architect role.
 *
 * Receives program specifications from the Oracle (nml/spec, MSG_SPEC),
 * generates NML programs via template engine (primary path) or LLM HTTP
 * (fallback), validates by dry-run execution, and publishes to nml/program
 * (MSG_PROGRAM) for the Sentient to sign and broadcast to workers.
 *
 * Usage:
 *   ./architect_agent [--name NAME] [--broker HOST] [--broker-port PORT]
 *                     [--port HTTP_PORT]
 *                     [--llm-host HOST] [--llm-port PORT] [--llm-path PATH]
 *
 * Inputs:  nml/spec    — MSG_SPEC from Oracle (JSON payload with "spec" key)
 * Outputs: nml/program — MSG_PROGRAM for Sentient to sign + broadcast
 */

#define _POSIX_C_SOURCE 200809L

#include "../../edge/config.h"
#include "../../edge/msg.h"
#include "../../edge/identity.h"
#include "../../edge/peer_table.h"
#include "../../edge/mqtt_transport.h"
#include "../../edge/templates.h"
#include "../../edge/nml_exec.h"

/* Direct MQTT publish for custom topics */
#include "../../edge/mqtt/mqtt.h"

#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_CATALOG_ENTRIES  128
#define HTTP_BUF_SZ          8192
#define HEARTBEAT_INTERVAL_S 5
#define STALE_PEER_S         30

/* Provenance flags */
#define PROV_TEMPLATE 0
#define PROV_LLM      1

/* ── Types ───────────────────────────────────────────────────────────── */

typedef struct {
    char phash[17];
    char spec[256];
    int  provenance;        /* PROV_TEMPLATE or PROV_LLM */
    int  template_id;       /* -1 if LLM-generated */
    time_t generated_at;
    int  validated;         /* 1 if dry-run passed */
    char data_keys[TEMPLATE_MAX_DATA_KEYS][32];
    int  n_data_keys;
} CatalogEntry;

typedef struct {
    CatalogEntry entries[MAX_CATALOG_ENTRIES];
    int          count;
    int          next;      /* ring index for eviction */
} ProgramCatalog;

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static char    g_machine_hash_hex[17];
static uint8_t g_machine_hash_bytes[8];
static char    g_node_id_hex[17];
static char    g_identity_payload[34];

static MQTTTransport g_mqtt;
static PeerTable     g_peers;
static ProgramCatalog g_catalog;

static const char *g_agent_name = "architect";
static char g_broker_host[128]  = "127.0.0.1";
static uint16_t g_broker_port   = 1883;
static uint16_t g_http_port     = 9003;
static char g_llm_host[128]     = {0};
static uint16_t g_llm_port      = 443;
static char g_llm_path[256]     = "/api/v1/chat/completions";
static char g_llm_api_key[256]  = {0};
static char g_llm_model[128]    = "openai/gpt-4o-mini";
static char g_llm_provider[32]  = "openai";  /* "openai" or "anthropic" */

static char g_think_host[128]   = {0};
static uint16_t g_think_port    = 443;
static char g_think_path[256]   = "/api/v1/chat/completions";
static char g_think_model[128]  = {0};

/* Code model — local NML code generation (nml-v09-merged-6bit).
   When set, takes over stage 2 (NML code output) from --llm-*,
   which becomes an optional stage 0 context provider. */
static char g_code_host[128]    = {0};
static uint16_t g_code_port     = 8080;
static char g_code_path[256]    = "/v1/chat/completions";
static char g_code_model[128]   = {0};

/* ── Catalog helpers ─────────────────────────────────────────────────── */

static void catalog_add(const char *phash, const char *spec,
                        int prov, int template_id, int validated,
                        const char data_keys[][32], int n_data_keys)
{
    CatalogEntry *e;
    if (g_catalog.count < MAX_CATALOG_ENTRIES) {
        e = &g_catalog.entries[g_catalog.count++];
    } else {
        /* Ring eviction */
        e = &g_catalog.entries[g_catalog.next % MAX_CATALOG_ENTRIES];
        g_catalog.next++;
    }
    strncpy(e->phash,  phash, sizeof(e->phash) - 1);
    strncpy(e->spec,   spec,  sizeof(e->spec) - 1);
    e->provenance   = prov;
    e->template_id  = template_id;
    e->generated_at = time(NULL);
    e->validated    = validated;
    e->n_data_keys  = n_data_keys < TEMPLATE_MAX_DATA_KEYS ? n_data_keys : TEMPLATE_MAX_DATA_KEYS;
    for (int i = 0; i < e->n_data_keys; i++)
        strncpy(e->data_keys[i], data_keys[i], sizeof(e->data_keys[0]) - 1);
}

/* ── Simple JSON field extractor ─────────────────────────────────────── */

static int json_str(const char *json, const char *key,
                    char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz == 0) return -1;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (int)i;
}

/* ── LLM program generation ──────────────────────────────────────────── */

/*
 * Ask an OpenAI-compatible LLM to generate compact NML for the given spec.
 * Writes compact (pilcrow-delimited) NML to out_buf.
 * Returns chars written, or -1 on failure.
 */

static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if      (c == '"')  { out[j++] = '\\'; out[j++] = '"'; }
        else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else                { out[j++] = c; }
    }
    out[j] = '\0';
}

/*
 * POST to an LLM endpoint via curl.
 * provider: "openai" (OpenAI-compatible: OpenRouter, local mlx_lm)
 *           "anthropic" (api.anthropic.com /v1/messages)
 * system_msg may be NULL for user-only requests.
 * Returns chars written to out_buf, or -1 on failure.
 */
static int llm_curl(const char *host, uint16_t port, const char *path,
                    const char *api_key, const char *model,
                    const char *provider,
                    const char *system_msg, const char *user_msg,
                    int max_tokens, char *out_buf, size_t out_sz)
{
    if (!host || host[0] == '\0') return -1;
    int is_anthropic = provider && strcmp(provider, "anthropic") == 0;

    char esc_sys[1024] = {0}, esc_usr[2048];
    if (system_msg) json_escape(system_msg, esc_sys, sizeof(esc_sys));
    json_escape(user_msg, esc_usr, sizeof(esc_usr));

    char body[4096];
    if (system_msg && system_msg[0]) {
        if (is_anthropic) {
            /* Anthropic: system goes as top-level field */
            snprintf(body, sizeof(body),
                "{\"model\":\"%s\",\"max_tokens\":%d,"
                 "\"system\":\"%s\","
                 "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
                model, max_tokens, esc_sys, esc_usr);
        } else {
            snprintf(body, sizeof(body),
                "{\"model\":\"%s\","
                 "\"messages\":["
                   "{\"role\":\"system\",\"content\":\"%s\"},"
                   "{\"role\":\"user\",\"content\":\"%s\"}"
                 "],"
                 "\"max_tokens\":%d}",
                model, esc_sys, esc_usr, max_tokens);
        }
    } else {
        snprintf(body, sizeof(body),
            "{\"model\":\"%s\","
             "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
             "\"max_tokens\":%d}",
            model, esc_usr, max_tokens);
    }

    char tmppath[64] = "/tmp/nml_llm_XXXXXX";
    int tmpfd = mkstemp(tmppath);
    if (tmpfd < 0) return -1;
    write(tmpfd, body, strlen(body));
    close(tmpfd);

    const char *scheme = (port == 443) ? "https" : "http";
    char port_part[16] = "";
    if (!((port == 443 && strcmp(scheme, "https") == 0) ||
          (port == 80  && strcmp(scheme, "http")  == 0)))
        snprintf(port_part, sizeof(port_part), ":%u", port);

    char cmd[1280];
    if (is_anthropic && api_key && api_key[0]) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 30 -X POST '%s://%s%s%s' "
            "-H 'x-api-key: %s' "
            "-H 'anthropic-version: 2023-06-01' "
            "-H 'Content-Type: application/json' "
            "-d '@%s' 2>/dev/null",
            scheme, host, port_part, path, api_key, tmppath);
    } else if (api_key && api_key[0]) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 30 -X POST '%s://%s%s%s' "
            "-H 'Authorization: Bearer %s' "
            "-H 'Content-Type: application/json' "
            "-d '@%s' 2>/dev/null",
            scheme, host, port_part, path, api_key, tmppath);
    } else {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 30 -X POST '%s://%s%s%s' "
            "-H 'Content-Type: application/json' "
            "-d '@%s' 2>/dev/null",
            scheme, host, port_part, path, tmppath);
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(tmppath); return -1; }

    char resp[8192];
    size_t total = 0, n;
    while ((n = fread(resp + total, 1, sizeof(resp) - 1 - total, fp)) > 0)
        total += n;
    resp[total] = '\0';
    pclose(fp);
    unlink(tmppath);

    if (total == 0) return -1;

    /* Parse response: OpenAI returns "content":"<string>",
       Anthropic returns "content":[{"type":"text","text":"<string>"}] */
    char *p = strstr(resp, "\"content\":");
    if (!p) return -1;
    p += 10;
    while (*p == ' ') p++;
    if (*p == '[') {
        p = strstr(p, "\"text\":");
        if (!p) return -1;
        p += 7;
        while (*p == ' ') p++;
        if (*p != '"') return -1;
        p++;
    } else if (*p == '"') {
        p++;
    } else {
        return -1;
    }

    int i = 0;
    while (*p && *p != '"' && i < (int)out_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';

    /* Thinking/reasoning models put output in "reasoning": not "content":. */
    if (i == 0) {
        char *r = strstr(resp, "\"reasoning\":");
        if (r) {
            r += 12;
            while (*r == ' ') r++;
            if (*r == '"') {
                r++;
                i = 0;
                while (*r && *r != '"' && i < (int)out_sz - 1) {
                    if (*r == '\\' && *(r + 1)) r++;
                    out_buf[i++] = *r++;
                }
                out_buf[i] = '\0';
            }
        }
    }
    return i;
}

static int llm_generate(const char *spec, char *out_buf, size_t out_sz)
{
    return llm_curl(g_llm_host, g_llm_port, g_llm_path,
                    g_llm_api_key, g_llm_model, g_llm_provider,
                    "Generate a minimal NML program for the given spec. "
                    "Output only the program lines joined with the pilcrow character "
                    "(\xc2\xb6), no explanation.",
                    spec, 512, out_buf, out_sz);
}

/* Local NML code model — generates exact NML given an enriched spec.
   Always OpenAI-compatible (mlx_lm server). */
static int code_generate(const char *spec, char *out_buf, size_t out_sz)
{
    return llm_curl(g_code_host, g_code_port, g_code_path,
                    NULL, g_code_model, "openai",
                    "Generate a minimal NML program. "
                    "Output only program lines joined with pilcrow (\xc2\xb6), no explanation.",
                    spec, 512, out_buf, out_sz);
}

static int think_complete(const char *prompt, char *out_buf, size_t out_sz)
{
    /* Think model is always local (OpenAI-compatible mlx_lm server).
       Falls back to external LLM if no think host is configured. */
    const char *host  = g_think_host[0] ? g_think_host : g_llm_host;
    uint16_t    port  = g_think_host[0] ? g_think_port  : g_llm_port;
    const char *path  = g_think_host[0] ? g_think_path  : g_llm_path;
    const char *model = g_think_model[0] ? g_think_model : g_llm_model;
    const char *prov  = g_think_host[0] ? "openai" : g_llm_provider;
    return llm_curl(host, port, path,
                    g_llm_api_key, model, prov,
                    NULL, prompt, 512, out_buf, out_sz);
}

/* ── Template generation ─────────────────────────────────────────────── */

static void fill_default_params(TemplateParams *p, int template_id)
{
    memset(p, 0, sizeof(*p));
    p->template_id    = template_id;
    /* Match the demo data layout: 6 features, 1 hidden layer (6→8→1).
     * Data files provide @w1/@b1/@w2/@b2 as initial weights,
     * @training_data/@training_labels for TNET, @new_transaction for inference. */
    p->input_dim      = 6;
    p->n_hidden       = 1;
    p->hidden_dims[0] = 8;
    p->threshold      = 0.5f;
    p->output_classes = 2;
    p->epochs         = 100;
    p->lr_scaled      = 100;    /* 0.01 */
    p->training_mode  = 1;      /* train on local data, then infer */
    strncpy(p->input_key,  "new_transaction", sizeof(p->input_key)  - 1);
    strncpy(p->output_key, "fraud_score",     sizeof(p->output_key) - 1);
}

static int generate_from_template(const char *intent,
                                  char *compact_out, size_t out_sz,
                                  int *template_id_out,
                                  TemplateParams *params_out)
{
    int tid = template_select(intent);
    if (tid < 0) return -1;

    TemplateParams params;
    fill_default_params(&params, tid);

    int n = template_generate(&params, compact_out, out_sz);
    if (n <= 0) return -1;

    *template_id_out = tid;
    if (params_out) *params_out = params;
    return n;
}

/* ── Validation ──────────────────────────────────────────────────────── */

#define MAX_RETRIES 3

/*
 * Validate compact NML by expanding and doing a dry-run with no data.
 * Returns 1 if the program assembles without error, 0 otherwise.
 * If err_out is non-NULL, writes the error message on failure.
 */
static int validate_compact(const char *compact)
{
    char program_text[NML_MAX_PROGRAM_LEN + 1];
    if (msg_compact_to_program(compact, program_text, sizeof(program_text)) < 0)
        return 0;
    return nml_exec_validate(program_text);
}

static int validate_compact_msg(const char *compact,
                                char *err_out, size_t err_sz)
{
    if (err_out && err_sz > 0) err_out[0] = '\0';

    char program_text[NML_MAX_PROGRAM_LEN + 1];
    if (msg_compact_to_program(compact, program_text, sizeof(program_text)) < 0) {
        if (err_out && err_sz > 0)
            snprintf(err_out, err_sz, "Failed to expand compact format");
        return 0;
    }
    return nml_exec_validate_msg(program_text, err_out, err_sz);
}

/*
 * Build an opcode-help string for common backward ops so the LLM can
 * self-correct operand count errors.  Appended to the correction prompt.
 */
static void opcode_help_for_error(const char *error, char *help, size_t sz)
{
    help[0] = '\0';
    /* Inject help for operand errors, unknown opcodes, or slot errors */
    if (!strstr(error, "operand") && !strstr(error, "expects")
        && !strstr(error, "Unknown")  && !strstr(error, "not found")
        && !strstr(error, "error"))
        return;

    snprintf(help, sz,
        "\nCorrect operand counts for backward opcodes:\n"
        "  RELUBK  Rd Rgrad Rinput              (3 operands)\n"
        "  SIGMBK  Rd Rgrad Rinput              (3 operands)\n"
        "  TANHBK  Rd Rgrad Rinput              (3 operands)\n"
        "  GELUBK  Rd Rgrad Rinput              (3 operands)\n"
        "  SOFTBK  Rd Rgrad Rinput              (3 operands)\n"
        "  NORMBK  Rd Rgrad Rinput              (3 operands)\n"
        "  MMULBK  Rd_di Rd_dw Rgrad Rin Rw     (5 operands)\n"
        "  CONVBK  Rd_di Rd_dk Rgrad Rin Rk     (5 operands)\n"
        "  POOLBK  Rd Rgrad Rinput              (3 operands)\n"
        "  ATTNBK  Rd_dq Rgrad Rq Rk Rv        (5 operands)\n"
        "\nOther common opcodes:\n"
        "  SSUB  Rd Rs #imm     — scalar subtract\n"
        "  SDIV  Rd Rs #imm     — scalar divide\n"
        "  CLMP  Rd Rs #min #max — clamp values\n"
        "  WHER  Rd Rcond Rs1 [Rs2] — conditional select\n"
        "  SYS   Rd #code       — system call (0=print,1=char,2=read,4=time,5=rand)\n"
        "  BKWD  Rd Rpred Rtarget [#loss_type] — full backward pass\n");
}

/* ── Program submission ──────────────────────────────────────────────── */

/* Publish a governance vote for phash (MSG_VOTE). */
static void publish_vote(const char *phash, float score)
{
    char payload[32];
    snprintf(payload, sizeof(payload), "%s:%.6f", phash, score);
    uint8_t pkt[256];
    int n = msg_encode(pkt, sizeof(pkt), MSG_VOTE,
                       g_agent_name, g_http_port, payload);
    if (n > 0)
        mqtt_transport_publish(&g_mqtt, MSG_VOTE, pkt, (size_t)n);
}

/*
 * Encode compact NML as MSG_PROGRAM and publish to nml/program.
 * The Sentient will sign and re-broadcast to workers.
 * Returns the 16-char hex phash written to phash_out, or -1 on error.
 */
static int submit_program(const char *compact, char *phash_out)
{
    uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
    int pkt_len = msg_encode(pkt, sizeof(pkt), MSG_PROGRAM,
                             g_agent_name, g_http_port, compact);
    if (pkt_len <= 0) return -1;

    if (mqtt_transport_publish(&g_mqtt, MSG_PROGRAM, pkt, (size_t)pkt_len) < 0)
        return -1;

    /* Derive phash from payload for catalog (first 16 chars of compact as ID) */
    if (phash_out) {
        /* Use the first 16 printable chars of the compact as a rough ID */
        int j = 0;
        for (int i = 0; compact[i] && j < 16; i++) {
            unsigned char c = (unsigned char)compact[i];
            if (c >= 0x20 && c < 0x7f && c != 0xb6)
                phash_out[j++] = compact[i];
        }
        while (j < 16) phash_out[j++] = '0';
        phash_out[16] = '\0';
    }
    return 0;
}

/* ── Spec handler ────────────────────────────────────────────────────── */

static void handle_spec(const char *sender, const char *payload)
{
    /* Extract "spec" intent from JSON payload */
    char intent[256] = {0};
    if (json_str(payload, "spec", intent, sizeof(intent)) <= 0) {
        /* Treat raw payload as intent if JSON extraction fails */
        strncpy(intent, payload, sizeof(intent) - 1);
    }
    if (intent[0] == '\0') return;

    printf("[architect] spec from %s: %.80s\n", sender, intent);

    char compact[NML_MAX_PROGRAM_LEN + 1];
    int  template_id = -1;
    int  provenance  = PROV_TEMPLATE;
    int  generated   = 0;
    int  retries_used = 0;
    TemplateParams last_params;
    memset(&last_params, 0, sizeof(last_params));

    /* Primary path: template engine */
    if (generate_from_template(intent, compact, sizeof(compact),
                               &template_id, &last_params) > 0) {
        generated = 1;
        printf("[architect] template match: %s  mode=%s\n",
               template_name(template_id),
               last_params.training_mode ? "train+infer" : "infer");
    }

    /* Fallback: three-stage pipeline (external → think → code) with retry */
    if (!generated && (g_llm_host[0] != '\0' || g_code_host[0] != '\0')) {
        char context[512] = {0};

        /* Stage 1 (optional): External cloud model provides deep context */
        if (g_llm_host[0] != '\0' && g_code_host[0] != '\0') {
            char ext_prompt[768];
            snprintf(ext_prompt, sizeof(ext_prompt),
                "NML program intent: %.400s "
                "Provide concise context about data types and operations needed.",
                intent);
            llm_generate(ext_prompt, context, sizeof(context));
        }

        /* Stage 2 (optional): Internal think model decomposes into NML structure */
        char think_out[512] = {0};
        char enriched[NML_MAX_PROGRAM_LEN + 1];
        if (g_think_host[0] != '\0') {
            char think_prompt[768];
            snprintf(think_prompt, sizeof(think_prompt),
                "NML intent: %.250s%s%s"
                "Break down the required NML operations in one paragraph.",
                intent,
                context[0] ? " Context: " : "",
                context[0] ? context : "");
            think_complete(think_prompt, think_out, sizeof(think_out));
        }
        if (think_out[0]) {
            snprintf(enriched, sizeof(enriched),
                "Requirements: %.300s Original intent: %.150s",
                think_out, intent);
        } else if (context[0]) {
            snprintf(enriched, sizeof(enriched),
                "Context: %.300s Intent: %.200s", context, intent);
        } else {
            strncpy(enriched, intent, sizeof(enriched) - 1);
            enriched[sizeof(enriched) - 1] = '\0';
        }

        /* Stage 3: Code model generates NML with validation retry loop */
        char error_msg[512] = {0};
        int  retry;

        for (retry = 0; retry < MAX_RETRIES; retry++) {
            int ok;

            if (retry == 0) {
                /* First attempt: use enriched spec */
                ok = g_code_host[0] != '\0'
                    ? code_generate(enriched, compact, sizeof(compact))
                    : llm_generate(enriched, compact, sizeof(compact));
            } else {
                /* Retry: include error + opcode schemas in correction prompt */
                char help[1024] = {0};
                opcode_help_for_error(error_msg, help, sizeof(help));
                char correction[NML_MAX_PROGRAM_LEN + 1];
                snprintf(correction, sizeof(correction),
                    "Previous NML had errors. Fix them.\n"
                    "Error: %.400s\n%s\n"
                    "Original intent: %.200s\n"
                    "Output only corrected NML program lines.",
                    error_msg, help, intent);
                ok = g_code_host[0] != '\0'
                    ? code_generate(correction, compact, sizeof(compact))
                    : llm_generate(correction, compact, sizeof(compact));
            }

            if (ok <= 0) {
                fprintf(stderr, "[architect] code generation failed on attempt %d\n",
                        retry + 1);
                continue;
            }

            /* Validate */
            error_msg[0] = '\0';
            int valid = validate_compact_msg(compact, error_msg, sizeof(error_msg));
            if (valid) {
                generated  = 1;
                provenance = PROV_LLM;
                const char *stages = (g_code_host[0] && g_think_host[0] && g_llm_host[0])
                    ? "external+think+code"
                    : (g_code_host[0] && g_think_host[0]) ? "think+code"
                    : (g_code_host[0]) ? "code-only"
                    : (think_out[0]) ? "think+llm" : "llm-only";
                retries_used = retry;
                if (retry == 0) {
                    printf("[architect] generated program (%s)\n", stages);
                } else {
                    printf("[architect] generated program (%s, fixed on retry %d)\n",
                           stages, retry);
                }
                break;
            }

            printf("[architect] validation failed (attempt %d/%d): %s\n",
                   retry + 1, MAX_RETRIES, error_msg);
        }
    }

    if (!generated) {
        fprintf(stderr,
                "[architect] no template match and LLM failed after %d retries — "
                "skipping spec\n", MAX_RETRIES);
        return;
    }

    /* Final validation (templates bypass the retry loop, so validate here) */
    int valid = (provenance == PROV_TEMPLATE)
        ? validate_compact(compact)
        : 1;  /* LLM path already validated in the loop above */
    if (!valid) {
        fprintf(stderr, "[architect] dry-run validation FAILED — dropping\n");
        return;
    }

    /* Submit to nml/program for Sentient to sign + broadcast */
    char phash[17] = {0};
    if (submit_program(compact, phash) < 0) {
        fprintf(stderr, "[architect] submit failed\n");
        return;
    }

    catalog_add(phash, intent, provenance, template_id, valid,
                last_params.data_keys, last_params.n_data_keys);
    printf("[architect] submitted program phash=%.16s  prov=%s  valid=%d\n",
           phash, provenance == PROV_TEMPLATE ? "template" : "llm", valid);

    /* Governance vote: confidence based on provenance + validation + retries.
     * template=1.0, llm+first-pass=0.9, llm+retried=0.8, llm+unvalidated=0.6 */
    float vote_score = (provenance == PROV_TEMPLATE) ? 1.0f
                     : (valid && retries_used == 0) ? 0.9f
                     : (valid && retries_used > 0)  ? 0.8f
                     : 0.6f;
    publish_vote(phash, vote_score);
}

/* ── HTTP server ─────────────────────────────────────────────────────── */

static compat_socket_t g_http_fd = COMPAT_INVALID_SOCKET;

static compat_socket_t http_listen(uint16_t port)
{
    compat_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COMPAT_INVALID_SOCKET) return COMPAT_INVALID_SOCKET;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               COMPAT_SOCKOPT_CAST(&one), sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        compat_close_socket(fd); return COMPAT_INVALID_SOCKET;
    }
    return fd;
}

static void http_send(compat_socket_t fd, int code, const char *body)
{
    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Connection: close\r\n\r\n",
        code, code == 200 ? "OK" : "Error", strlen(body));
    send(fd, hdr, (size_t)hdr_len, 0);
    send(fd, body, strlen(body), 0);
}

static void handle_http(compat_socket_t cfd)
{
    char req[1024];
    int n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0) { compat_close_socket(cfd); return; }
    req[n] = '\0';

    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        compat_close_socket(cfd); return;
    }

    /* GET /health */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        char body[512];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"name\":\"%s\","
             "\"peers\":%d,\"catalog\":%d,"
             "\"templates\":%d,\"llm\":%s,\"think\":%s}",
            g_agent_name, g_peers.count, g_catalog.count,
            TEMPLATE_COUNT,
            g_llm_host[0]   ? "true" : "false",
            g_think_host[0] ? "true" : "false");
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* GET /catalog — list generated programs */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/catalog") == 0) {
        char body[HTTP_BUF_SZ];
        int pos = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < g_catalog.count; i++) {
            const CatalogEntry *e = &g_catalog.entries[i];
            if (i > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            int kstart = pos;
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                "{\"phash\":\"%s\","
                 "\"spec\":\"%.80s\","
                 "\"provenance\":\"%s\","
                 "\"template\":\"%s\","
                 "\"validated\":%s,"
                 "\"data_keys\":[",
                e->phash,
                e->spec,
                e->provenance == PROV_TEMPLATE ? "template" : "llm",
                e->template_id >= 0 ? template_name(e->template_id) : "n/a",
                e->validated ? "true" : "false");
            (void)kstart;
            for (int k = 0; k < e->n_data_keys; k++) {
                if (k > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
                pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                                "\"%s\"", e->data_keys[k]);
            }
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos, "]}");
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /* GET /templates — available template names */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/templates") == 0) {
        char body[1024];
        int pos = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < TEMPLATE_COUNT; i++) {
            if (i > 0) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
            pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
                            "{\"id\":%d,\"name\":\"%s\"}", i, template_name(i));
        }
        snprintf(body + pos, sizeof(body) - (size_t)pos, "]");
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    /*
     * POST /generate  body: {"spec": "..."}
     * Immediately generate and submit a program for the given spec.
     */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/generate") == 0) {
        char *body_start = strstr(req, "\r\n\r\n");
        if (!body_start) {
            http_send(cfd, 400, "{\"error\":\"no body\"}");
            compat_close_socket(cfd); return;
        }
        body_start += 4;
        char spec[256] = {0};
        if (json_str(body_start, "spec", spec, sizeof(spec)) <= 0) {
            http_send(cfd, 400, "{\"error\":\"spec required\"}");
            compat_close_socket(cfd); return;
        }
        int cat_before = g_catalog.count;
        handle_spec("http", spec);
        /* Return details from the catalog entry just added */
        char resp[512];
        if (g_catalog.count > cat_before || g_catalog.count == MAX_CATALOG_ENTRIES) {
            int idx = (g_catalog.count < MAX_CATALOG_ENTRIES)
                      ? g_catalog.count - 1
                      : (g_catalog.next - 1 + MAX_CATALOG_ENTRIES) % MAX_CATALOG_ENTRIES;
            CatalogEntry *e = &g_catalog.entries[idx];
            /* Build data_keys array string */
            char dkeys[256] = "[";
            int dpos = 1;
            for (int k = 0; k < e->n_data_keys; k++) {
                if (k > 0) dpos += snprintf(dkeys + dpos, sizeof(dkeys) - (size_t)dpos, ",");
                dpos += snprintf(dkeys + dpos, sizeof(dkeys) - (size_t)dpos,
                                 "\"%s\"", e->data_keys[k]);
            }
            snprintf(dkeys + dpos, sizeof(dkeys) - (size_t)dpos, "]");

            snprintf(resp, sizeof(resp),
                "{\"ok\":true,\"hash\":\"%s\",\"provenance\":\"%s\","
                 "\"template_id\":%d,\"validated\":%s,\"spec\":\"%s\","
                 "\"data_keys\":%s}",
                e->phash,
                e->template_id >= 0 ? "template" : "llm",
                e->template_id,
                e->validated ? "true" : "false",
                e->spec,
                dkeys);
        } else {
            snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"generation failed\"}");
        }
        http_send(cfd, 200, resp);
        compat_close_socket(cfd); return;
    }

    /* GET /peers */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/peers") == 0) {
        char body[HTTP_BUF_SZ];
        peer_list_json(&g_peers, body, sizeof(body));
        http_send(cfd, 200, body);
        compat_close_socket(cfd); return;
    }

    if (strcmp(method, "OPTIONS") == 0) {
        http_send(cfd, 200, "{}");
        compat_close_socket(cfd); return;
    }

    http_send(cfd, 404, "{\"error\":\"not found\"}");
    compat_close_socket(cfd);
}

/* ── CLI usage ───────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--name NAME] [--broker HOST] [--broker-port PORT]\n"
        "          [--port HTTP_PORT]\n"
        "          [--llm-host HOST] [--llm-port PORT] [--llm-path PATH]\n"
        "          [--llm-api-key KEY] [--llm-model MODEL]\n"
        "          [--think-host HOST] [--think-port PORT] [--think-path PATH]\n"
        "          [--think-model MODEL]\n",
        prog);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /* ── Parse CLI ── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            g_agent_name = argv[++i];
        else if (strcmp(argv[i], "--broker") == 0 && i + 1 < argc)
            strncpy(g_broker_host, argv[++i], sizeof(g_broker_host) - 1);
        else if (strcmp(argv[i], "--broker-port") == 0 && i + 1 < argc)
            g_broker_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_http_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--llm-host") == 0 && i + 1 < argc)
            strncpy(g_llm_host, argv[++i], sizeof(g_llm_host) - 1);
        else if (strcmp(argv[i], "--llm-port") == 0 && i + 1 < argc)
            g_llm_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--llm-path") == 0 && i + 1 < argc)
            strncpy(g_llm_path, argv[++i], sizeof(g_llm_path) - 1);
        else if (strcmp(argv[i], "--think-host") == 0 && i + 1 < argc)
            strncpy(g_think_host, argv[++i], sizeof(g_think_host) - 1);
        else if (strcmp(argv[i], "--think-port") == 0 && i + 1 < argc)
            g_think_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--think-path") == 0 && i + 1 < argc)
            strncpy(g_think_path, argv[++i], sizeof(g_think_path) - 1);
        else if (strcmp(argv[i], "--think-model") == 0 && i + 1 < argc)
            strncpy(g_think_model, argv[++i], sizeof(g_think_model) - 1);
        else if (strcmp(argv[i], "--llm-api-key") == 0 && i + 1 < argc)
            strncpy(g_llm_api_key, argv[++i], sizeof(g_llm_api_key) - 1);
        else if (strcmp(argv[i], "--llm-model") == 0 && i + 1 < argc)
            strncpy(g_llm_model, argv[++i], sizeof(g_llm_model) - 1);
        else if (strcmp(argv[i], "--llm-provider") == 0 && i + 1 < argc)
            strncpy(g_llm_provider, argv[++i], sizeof(g_llm_provider) - 1);
        else if (strcmp(argv[i], "--code-host") == 0 && i + 1 < argc)
            strncpy(g_code_host, argv[++i], sizeof(g_code_host) - 1);
        else if (strcmp(argv[i], "--code-port") == 0 && i + 1 < argc)
            g_code_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--code-path") == 0 && i + 1 < argc)
            strncpy(g_code_path, argv[++i], sizeof(g_code_path) - 1);
        else if (strcmp(argv[i], "--code-model") == 0 && i + 1 < argc)
            strncpy(g_code_model, argv[++i], sizeof(g_code_model) - 1);
        else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        }
    }

    compat_winsock_init();

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ── Identity ── */
    identity_init(g_agent_name,
                  g_machine_hash_hex, g_machine_hash_bytes,
                  g_node_id_hex, g_identity_payload);
    printf("[architect] identity  machine=%s  node=%s\n",
           g_machine_hash_hex, g_node_id_hex);

    /* ── Init tables ── */
    peer_table_init(&g_peers);
    memset(&g_catalog, 0, sizeof(g_catalog));

    /* ── MQTT ── */
    if (mqtt_transport_init(&g_mqtt, g_broker_host, g_broker_port,
                             g_agent_name, g_http_port,
                             g_identity_payload) != 0) {
        fprintf(stderr, "[architect] failed to connect to broker %s:%u\n",
                g_broker_host, g_broker_port);
        return 1;
    }

    /* ── HTTP server ── */
    g_http_fd = http_listen(g_http_port);
    if (g_http_fd == COMPAT_INVALID_SOCKET) {
        fprintf(stderr, "[architect] failed to bind HTTP on port %u\n",
                g_http_port);
        mqtt_transport_close(&g_mqtt);
        return 1;
    }

    printf("[architect] HTTP API on port %u\n", g_http_port);
    printf("[architect] templates=%d  broker=%s:%u%s%s\n",
           TEMPLATE_COUNT, g_broker_host, g_broker_port,
           g_llm_host[0]   ? "  llm=enabled"   : "",
           g_think_host[0] ? "  think=enabled" : "");

    /* ── Main loop ── */
    time_t last_heartbeat = 0;

    while (g_running) {
        time_t now = time(NULL);

        /* Wait up to 1 second for an HTTP connection */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_http_fd, &rfds);
        struct timeval tv = {1, 0};
        if (select(COMPAT_SELECT_NFDS(g_http_fd), &rfds, NULL, NULL, &tv) > 0 &&
            FD_ISSET(g_http_fd, &rfds)) {
            compat_socket_t cfd = accept(g_http_fd, NULL, NULL);
            if (cfd != COMPAT_INVALID_SOCKET) handle_http(cfd);
        }

        /* Sync MQTT I/O */
        mqtt_transport_sync(&g_mqtt, 0);

        /* Dispatch incoming NML messages */
        uint8_t pkt[NML_MAX_PROGRAM_LEN + 64];
        char    sender_ip[46];
        int     pkt_len;
        while ((pkt_len = mqtt_transport_recv(&g_mqtt, pkt,
                                              sizeof(pkt), sender_ip)) > 0) {
            int      type;
            char     pname[64];
            char     payload[NML_MAX_PROGRAM_LEN + 1];
            uint16_t pport;

            if (msg_parse(pkt, (size_t)pkt_len,
                          &type, pname, sizeof(pname),
                          &pport, payload, sizeof(payload)) != 0)
                continue;

            peer_upsert(&g_peers, pname,
                        sender_ip[0] ? sender_ip : NULL,
                        pport, NULL, now);

            if (type == MSG_SPEC)
                handle_spec(pname, payload);
            /* Other message types observed but not acted on */
        }

        /* Periodic heartbeat + sweep */
        if (now - last_heartbeat >= HEARTBEAT_INTERVAL_S) {
            uint8_t hb[256];
            int hb_len = msg_encode(hb, sizeof(hb), MSG_HEARTBEAT,
                                    g_agent_name, g_http_port,
                                    g_identity_payload);
            if (hb_len > 0)
                mqtt_transport_publish(&g_mqtt, MSG_HEARTBEAT,
                                       hb, (size_t)hb_len);
            last_heartbeat = now;
            peer_sweep(&g_peers, now, STALE_PEER_S);
        }
    }

    printf("[architect] shutting down\n");
    mqtt_transport_close(&g_mqtt);
    if (g_http_fd != COMPAT_INVALID_SOCKET) compat_close_socket(g_http_fd);
    compat_winsock_cleanup();
    return 0;
}
