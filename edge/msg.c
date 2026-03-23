#include "msg.h"
#include <string.h>

/* "NML\x01" */
static const uint8_t MAGIC[4] = { 0x4e, 0x4d, 0x4c, 0x01 };

int msg_parse(const uint8_t *buf, size_t len,
              int *type, char *name, size_t name_sz,
              uint16_t *port, char *payload, size_t payload_sz)
{
    if (len < MSG_HEADER_MIN) return -1;
    if (memcmp(buf, MAGIC, 4) != 0) return -1;

    *type = buf[4];
    uint8_t name_len = buf[5];

    if (len < (size_t)(6 + name_len + 2)) return -1;

    size_t nc = name_len < name_sz - 1 ? name_len : name_sz - 1;
    memcpy(name, buf + 6, nc);
    name[nc] = '\0';

    *port = ((uint16_t)buf[6 + name_len] << 8) | buf[7 + name_len];

    size_t poff = 8 + name_len;
    size_t plen = len > poff ? len - poff : 0;
    size_t pc = plen < payload_sz - 1 ? plen : payload_sz - 1;
    if (pc) memcpy(payload, buf + poff, pc);
    payload[pc] = '\0';

    return 0;
}

int msg_encode(uint8_t *buf, size_t buf_sz,
               int type, const char *name, uint16_t port,
               const char *payload)
{
    size_t nl = strlen(name);
    if (nl > 63) nl = 63;
    size_t pl = payload ? strlen(payload) : 0;
    size_t total = 4 + 1 + 1 + nl + 2 + pl;
    if (total > buf_sz) return -1;

    memcpy(buf, MAGIC, 4);
    buf[4] = (uint8_t)type;
    buf[5] = (uint8_t)nl;
    memcpy(buf + 6, name, nl);
    buf[6 + nl] = (uint8_t)(port >> 8);
    buf[7 + nl] = (uint8_t)(port & 0xff);
    if (pl) memcpy(buf + 8 + nl, payload, pl);

    return (int)total;
}

int msg_compact_to_program(const char *compact, char *out, size_t out_sz)
{
    size_t pos = 0;
    const unsigned char *p = (const unsigned char *)compact;

    while (*p && pos < out_sz - 1) {
        /* Accept both the raw Latin-1 byte 0xB6 and the UTF-8 encoding
         * 0xC2 0xB6 (U+00B6 PILCROW SIGN) — Python encodes it as UTF-8. */
        if (*p == 0xb6) {
            out[pos++] = '\n';
            p++;
        } else if (*p == 0xc2 && *(p + 1) == 0xb6) {
            out[pos++] = '\n';
            p += 2;
        } else {
            out[pos++] = (char)*p;
            p++;
        }
    }
    out[pos] = '\0';
    return (int)pos;
}

int msg_program_to_compact(const char *program, char *out, size_t out_sz)
{
    size_t pos = 0;
    int first = 1;
    const char *p = program;

    while (*p) {
        /* find end of line */
        const char *line_end = p;
        while (*line_end && *line_end != '\n') line_end++;

        /* trim leading whitespace */
        const char *s = p;
        while (s < line_end && (*s == ' ' || *s == '\t')) s++;

        /* skip empty and comment lines */
        if (s < line_end && *s != ';') {
            /* trim trailing whitespace */
            const char *e = line_end - 1;
            while (e > s && (*e == ' ' || *e == '\t' || *e == '\r')) e--;
            size_t ll = (size_t)(e - s + 1);

            if (!first) {
                if (pos >= out_sz - 1) return -1;
                out[pos++] = (char)0xb6;
            }
            if (pos + ll >= out_sz) return -1;
            memcpy(out + pos, s, ll);
            pos += ll;
            first = 0;
        }

        p = *line_end ? line_end + 1 : line_end;
    }

    out[pos] = '\0';
    return (int)pos;
}
