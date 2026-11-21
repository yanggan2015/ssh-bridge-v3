#ifndef SSH_BRIDGE_KEY_CHARS_H
#define SSH_BRIDGE_KEY_CHARS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEY_SEQ_MAX_TOKENS 128
#define KEY_SEQ_TOKEN_CAP 32

typedef struct {
    char bytes[KEY_SEQ_TOKEN_CAP];
    size_t len;
} KeyToken;

typedef struct {
    KeyToken tokens[KEY_SEQ_MAX_TOKENS];
    int count;
} KeySequence;

/* Normalize send_keys char into out_buf (NUL-terminated).
 * Returns number of bytes written (excluding NUL), or 0 if empty/invalid.
 * out_cap must be >= 2; typical keys fit in 16 bytes.
 */
size_t normalize_key_char(const char *input, char *out_buf, size_t out_cap);

/* Parse a send_keys sequence: literal bytes + <chord> tokens.
 * Returns token count (>=1) or 0 if empty/invalid.
 */
int parse_key_sequence(const char *input, KeySequence *out);

#ifdef __cplusplus
}
#endif

#endif
