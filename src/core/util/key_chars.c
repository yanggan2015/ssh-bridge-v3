#include "key_chars.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *value;
} named_key_t;

static const named_key_t SPECIAL_KEYS[] = {
    {"backspace", "\x7f"},
    {"bs", "\x08"},
    {"delete", "\x7f"},
    {"del", "\x7f"},
    {"enter", "\r"},
    {"return", "\r"},
    {"cr", "\r"},
    {"lf", "\n"},
    {"newline", "\n"},
    {"tab", "\t"},
    {"esc", "\x1b"},
    {"escape", "\x1b"},
    {"space", " "},
    {"up", "\x1b[A"},
    {"down", "\x1b[B"},
    {"right", "\x1b[C"},
    {"left", "\x1b[D"},
    {"home", "\x1b[H"},
    {"end", "\x1b[F"},
    {NULL, NULL},
};

static size_t copy_out(const char *src, size_t src_len, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    if (src_len + 1 > out_cap) src_len = out_cap - 1;
    memcpy(out, src, src_len);
    out[src_len] = '\0';
    return src_len;
}

static size_t lookup_named(const char *key, int with_alt, char *out, size_t out_cap) {
    for (int i = 0; SPECIAL_KEYS[i].name; ++i) {
        if (strcmp(SPECIAL_KEYS[i].name, key) == 0) {
            char tmp[32];
            size_t n = 0;
            if (with_alt) tmp[n++] = '\x1b';
            size_t vlen = strlen(SPECIAL_KEYS[i].value);
            if (n + vlen >= sizeof(tmp)) return 0;
            memcpy(tmp + n, SPECIAL_KEYS[i].value, vlen);
            n += vlen;
            tmp[n] = '\0';
            return copy_out(tmp, n, out, out_cap);
        }
    }
    return 0;
}

static size_t parse_key_chord(const char *inner, char *out, size_t out_cap) {
    char buf[128];
    char *parts[8];
    int nparts = 0;
    size_t len = strlen(inner);
    if (len >= sizeof(buf)) return 0;
    memcpy(buf, inner, len + 1);

    char *p = buf;
    while (*p && nparts < 8) {
        while (*p == ' ' || *p == '\t' || *p == '+') ++p;
        if (!*p) break;
        parts[nparts++] = p;
        while (*p && *p != '+') ++p;
        if (*p == '+') {
            *p = '\0';
            ++p;
        }
    }
    for (int i = 0; i < nparts; ++i) {
        char *tok = parts[i];
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        for (char *q = tok; *q; ++q) *q = (char)tolower((unsigned char)*q);
    }
    if (nparts == 0) return 0;

    int has_ctrl = 0, has_shift = 0, has_alt = 0;
    char key_buf[64] = {0};
    int key_started = 0;
    for (int i = 0; i < nparts; ++i) {
        if (!parts[i][0]) continue;
        if (!strcmp(parts[i], "ctrl") || !strcmp(parts[i], "control") || !strcmp(parts[i], "ctl")) {
            has_ctrl = 1;
        } else if (!strcmp(parts[i], "shift")) {
            has_shift = 1;
        } else if (!strcmp(parts[i], "alt") || !strcmp(parts[i], "meta") || !strcmp(parts[i], "option")) {
            has_alt = 1;
        } else {
            if (key_started && strlen(key_buf) + 1 < sizeof(key_buf)) strcat(key_buf, "+");
            if (strlen(key_buf) + strlen(parts[i]) < sizeof(key_buf)) {
                strcat(key_buf, parts[i]);
                key_started = 1;
            }
        }
    }
    if (!key_started) return 0;

    size_t named = lookup_named(key_buf, has_alt, out, out_cap);
    if (named) return named;

    if (strlen(key_buf) != 1) return 0;

    char result[8];
    size_t n = 0;
    char ch = key_buf[0];
    if (has_ctrl) {
        result[n++] = (char)(toupper((unsigned char)ch) & 0x1f);
        (void)has_shift;
    } else if (has_shift) {
        result[n++] = (char)toupper((unsigned char)ch);
    } else {
        result[n++] = ch;
    }
    if (has_alt) {
        memmove(result + 1, result, n);
        result[0] = '\x1b';
        n += 1;
    }
    result[n] = '\0';
    return copy_out(result, n, out, out_cap);
}

static int parse_hex_escape(const char *s, unsigned int *out_cp) {
    /* Accept \uXXXX or \xXX (and longer hex for \u) */
    if (s[0] != '\\') return 0;
    if (s[1] == 'u' || s[1] == 'U') {
        unsigned int v = 0;
        int digits = 0;
        for (int i = 2; s[i] && digits < 8; ++i) {
            char c = s[i];
            unsigned int d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else break;
            v = (v << 4) | d;
            ++digits;
        }
        if (digits == 0) return 0;
        *out_cp = v;
        return 1;
    }
    if (s[1] == 'x' || s[1] == 'X') {
        unsigned int v = 0;
        int digits = 0;
        for (int i = 2; s[i] && digits < 2; ++i) {
            char c = s[i];
            unsigned int d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else break;
            v = (v << 4) | d;
            ++digits;
        }
        if (digits == 0) return 0;
        *out_cp = v;
        return 1;
    }
    return 0;
}

size_t normalize_key_char(const char *input, char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap == 0) return 0;
    out_buf[0] = '\0';
    if (!input) return 0;

    size_t in_len = strlen(input);
    if (in_len == 1) return copy_out(input, 1, out_buf, out_cap);

    /* trim */
    while (*input == ' ' || *input == '\t') ++input;
    in_len = strlen(input);
    while (in_len > 0 && (input[in_len - 1] == ' ' || input[in_len - 1] == '\t')) --in_len;

    if (in_len >= 2 && input[0] == '<' && input[in_len - 1] == '>') {
        char inner[128];
        if (in_len - 2 >= sizeof(inner)) return copy_out(input, in_len, out_buf, out_cap);
        memcpy(inner, input + 1, in_len - 2);
        inner[in_len - 2] = '\0';
        size_t n = parse_key_chord(inner, out_buf, out_cap);
        if (n) return n;
        return copy_out(input, in_len, out_buf, out_cap);
    }

    char lowered[128];
    if (in_len >= sizeof(lowered)) return copy_out(input, in_len, out_buf, out_cap);
    memcpy(lowered, input, in_len);
    lowered[in_len] = '\0';
    for (size_t i = 0; i < in_len; ++i) lowered[i] = (char)tolower((unsigned char)lowered[i]);

    if (!strcmp(lowered, "\\u0003") || !strcmp(lowered, "\\x03") || !strcmp(lowered, "^c")) {
        return copy_out("\x03", 1, out_buf, out_cap);
    }

    if (input[0] == '\\' && (input[1] == 'u' || input[1] == 'x' || input[1] == 'U' || input[1] == 'X')) {
        unsigned int cp = 0;
        if (parse_hex_escape(input, &cp) && cp <= 0xFF) {
            char ch = (char)cp;
            return copy_out(&ch, 1, out_buf, out_cap);
        }
    }

    return copy_out(input, in_len, out_buf, out_cap);
}

int parse_key_sequence(const char *input, KeySequence *out) {
    const char *p;
    if (!out) return 0;
    out->count = 0;
    if (!input || !input[0]) return 0;
    p = input;
    while (*p && out->count < KEY_SEQ_MAX_TOKENS) {
        KeyToken *tok = &out->tokens[out->count];
        tok->len = 0;
        tok->bytes[0] = '\0';
        if (*p == '<') {
            const char *end = strchr(p + 1, '>');
            if (end) {
                char tmp[128];
                size_t n = (size_t)(end - p + 1);
                size_t got;
                if (n >= sizeof(tmp)) return 0;
                memcpy(tmp, p, n);
                tmp[n] = '\0';
                got = normalize_key_char(tmp, tok->bytes, sizeof(tok->bytes));
                if (!got) return 0;
                tok->len = got;
                out->count++;
                p = end + 1;
                continue;
            }
        }
        /* Single literal byte (including space). */
        tok->bytes[0] = *p;
        tok->bytes[1] = '\0';
        tok->len = 1;
        out->count++;
        p++;
    }
    return out->count;
}
