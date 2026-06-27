#include "lib_auth.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/rsa.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static const char B64URL_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int base64url_encode(const unsigned char *in, int in_len, char *out) {
    int i = 0, j = 0;
    while (i < in_len) {
        int remain = in_len - i;
        unsigned char a = in[i++];
        unsigned char b = remain >= 2 ? in[i++] : 0;
        unsigned char c = remain >= 3 ? in[i++] : 0;
        out[j++] = B64URL_ALPHABET[a >> 2];
        out[j++] = B64URL_ALPHABET[((a & 3) << 4) | (b >> 4)];
        if (remain >= 2)
            out[j++] = B64URL_ALPHABET[((b & 15) << 2) | (c >> 6)];
        if (remain >= 3)
            out[j++] = B64URL_ALPHABET[c & 63];
    }
    out[j] = '\0';
    return j;
}

static int base64url_decode(const char *in, unsigned char *out) {
    static const signed char DECODE[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    int len = (int)strlen(in);
    int i = 0, j = 0;
    while (i < len) {
        int remain = len - i;
        int a = DECODE[(unsigned char)in[i++]];
        if (a < 0) break;
        int b = i < len ? DECODE[(unsigned char)in[i++]] : -1;
        if (b < 0) break;
        out[j++] = (unsigned char)((a << 2) | (b >> 4));
        if (remain < 3) break;
        int c = i < len ? DECODE[(unsigned char)in[i++]] : -1;
        if (c < 0) break;
        out[j++] = (unsigned char)(((b & 15) << 4) | (c >> 2));
        if (remain < 4) break;
        int d = i < len ? DECODE[(unsigned char)in[i++]] : -1;
        if (d < 0) break;
        out[j++] = (unsigned char)(((c & 3) << 6) | d);
    }
    return j;
}

static int get_arg_base(int arg_count, Value *args) {
    if (arg_count >= 2 && args[0].type == VAL_MODULE)
        return 1;
    return 0;
}

static Value lib_auth_hash_sha256(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING)
        return val_string(copy_string("", 0));
    const char *input = args[base].as.string->chars;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)input, strlen(input), hash);
    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    ObjString *result = allocate_string(vm, hex, SHA256_DIGEST_LENGTH * 2);
    return val_string(result);
}

static Value lib_auth_sign_jwt(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRUCT || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.sign_jwt() requires payload (struct) and secret (string)");
        return val_nil();
    }
    const char *secret = args[base + 1].as.string->chars;
    int secret_len = args[base + 1].as.string->length;

    /* Encode header */
    ObjStruct *header = new_struct(vm, 2, false);
    free(header->field_names);
    static const char *names[] = { "alg", "typ" };
    struct_attach_shape(vm, header, NULL, (char *const *)names, 2);
    header->fields[0] = val_string(copy_string("HS256", 5));
    header->fields[1] = val_string(copy_string("JWT", 3));
    int header_json_len;
    char *header_json = json_encode(vm, val_struct(header), &header_json_len);
    /* Encode payload */
    int payload_json_len;
    char *payload_json = json_encode(vm, args[base], &payload_json_len);

    /* Base64url encode header and payload */
    int hb64_len = (header_json_len + 2) / 3 * 4 + 1;
    char *hb64 = (char *)malloc(hb64_len);
    int hb64_actual = base64url_encode((unsigned char *)header_json, header_json_len, hb64);

    int pb64_len = (payload_json_len + 2) / 3 * 4 + 1;
    char *pb64 = (char *)malloc(pb64_len);
    int pb64_actual = base64url_encode((unsigned char *)payload_json, payload_json_len, pb64);

    /* Build signing input: hb64.pb64 */
    int sig_input_len = hb64_actual + 1 + pb64_actual;
    char *sig_input = (char *)malloc(sig_input_len + 1);
    memcpy(sig_input, hb64, hb64_actual);
    sig_input[hb64_actual] = '.';
    memcpy(sig_input + hb64_actual + 1, pb64, pb64_actual);
    sig_input[sig_input_len] = '\0';

    /* HMAC-SHA256 */
    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    HMAC(EVP_sha256(), secret, secret_len,
         (unsigned char *)sig_input, sig_input_len,
         hmac_result, &hmac_len);

    /* Base64url encode signature */
    int sb64_buf = (hmac_len + 2) / 3 * 4 + 1;
    char *sb64 = (char *)malloc(sb64_buf);
    int sb64_actual = base64url_encode(hmac_result, (int)hmac_len, sb64);

    /* Build final token */
    int token_len = hb64_actual + 1 + pb64_actual + 1 + sb64_actual;
    char *token = (char *)malloc(token_len + 1);
    memcpy(token, hb64, hb64_actual);
    token[hb64_actual] = '.';
    memcpy(token + hb64_actual + 1, pb64, pb64_actual);
    token[hb64_actual + 1 + pb64_actual] = '.';
    memcpy(token + hb64_actual + 1 + pb64_actual + 1, sb64, sb64_actual);
    token[token_len] = '\0';

    ObjString *result = allocate_string(vm, token, token_len);

    free(header_json);
    free(payload_json);
    free(hb64);
    free(pb64);
    free(sig_input);
    free(sb64);
    free(token);

    return val_string(result);
}

static Value lib_auth_verify_jwt(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.verify_jwt() requires token (string) and secret (string)");
        return val_nil();
    }
    const char *token = args[base].as.string->chars;
    const char *secret = args[base + 1].as.string->chars;
    int secret_len = args[base + 1].as.string->length;

    /* Split token by '.' */
    const char *dot1 = strchr(token, '.');
    if (!dot1) return val_nil();
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return val_nil();

    int hb64_len = (int)(dot1 - token);
    int pb64_len = (int)(dot2 - dot1 - 1);
    int sb64_len = (int)(strlen(dot2 + 1));

    if (hb64_len == 0 || pb64_len == 0 || sb64_len == 0)
        return val_nil();

    /* Recompute HMAC */
    int sig_input_len = hb64_len + 1 + pb64_len;
    char *sig_input = (char *)malloc(sig_input_len + 1);
    memcpy(sig_input, token, hb64_len);
    sig_input[hb64_len] = '.';
    memcpy(sig_input + hb64_len + 1, dot1 + 1, pb64_len);
    sig_input[sig_input_len] = '\0';

    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    HMAC(EVP_sha256(), secret, secret_len,
         (unsigned char *)sig_input, sig_input_len,
         hmac_result, &hmac_len);

    /* Base64url encode the expected signature */
    int sb64_buf = (hmac_len + 2) / 3 * 4 + 1;
    char *expected_sig = (char *)malloc(sb64_buf);
    int expected_sig_len = base64url_encode(hmac_result, (int)hmac_len, expected_sig);

    /* Compare signatures (constant-time-ish) */
    if (sb64_len != expected_sig_len) {
        free(sig_input);
        free(expected_sig);
        return val_nil();
    }
    if (memcmp(dot2 + 1, expected_sig, sb64_len) != 0) {
        free(sig_input);
        free(expected_sig);
        return val_nil();
    }
    free(expected_sig);

    /* Decode payload base64url → JSON */
    char *payload_b64 = (char *)malloc(pb64_len + 1);
    memcpy(payload_b64, dot1 + 1, pb64_len);
    payload_b64[pb64_len] = '\0';

    int decoded_max = pb64_len; /* base64 decode never expands input */
    unsigned char *decoded = (unsigned char *)malloc(decoded_max + 1);
    int decoded_len = base64url_decode(payload_b64, decoded);
    decoded[decoded_len] = '\0';

    /* JSON decode into Varian value */
    Value result = json_decode(vm, (const char *)decoded);

    free(sig_input);
    free(payload_b64);
    free(decoded);

    return result;
}

static Value lib_auth_hash_password(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING) {
        runtime_error(vm, "auth.hash_password() requires a password string");
        return val_nil();
    }
    const char *password = args[base].as.string->chars;
    int password_len = args[base].as.string->length;

    unsigned char salt[16];
    RAND_bytes(salt, sizeof(salt));

    /* OWASP's current (2023+) minimum guidance for PBKDF2-HMAC-SHA256 is
     * 600,000 iterations -- 10,000 (the old NIST-2010-era figure) is far
     * too cheap to brute-force on modern GPU hardware. */
    unsigned int iterations = 600000;
    unsigned char hash[32];

    PKCS5_PBKDF2_HMAC(password, password_len, salt, sizeof(salt), iterations, EVP_sha256(), sizeof(hash), hash);

    char salt_hex[33];
    char hash_hex[65];
    for (int i = 0; i < 16; i++) snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < 32; i++) snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);

    char final_hash[128];
    snprintf(final_hash, sizeof(final_hash), "$pbkdf2$%u$%s$%s", iterations, salt_hex, hash_hex);

    return val_string(allocate_string(vm, final_hash, (int)strlen(final_hash)));
}

static Value lib_auth_verify_password(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.verify_password() requires password (string) and hash (string)");
        return val_bool(false);
    }
    const char *password = args[base].as.string->chars;
    int password_len = args[base].as.string->length;
    const char *stored_hash = args[base + 1].as.string->chars;

    if (strncmp(stored_hash, "$pbkdf2$", 8) != 0) return val_bool(false);

    unsigned int iterations = 0;
    char salt_hex[33];
    char hash_hex[65];
    if (sscanf(stored_hash, "$pbkdf2$%u$%32[^$]$%64s", &iterations, salt_hex, hash_hex) != 3) {
        return val_bool(false);
    }

    unsigned char salt[16];
    for (int i = 0; i < 16; i++) {
        unsigned int val;
        sscanf(salt_hex + i * 2, "%02x", &val);
        salt[i] = (unsigned char)val;
    }

    unsigned char computed_hash[32];
    PKCS5_PBKDF2_HMAC(password, password_len, salt, sizeof(salt), iterations, EVP_sha256(), sizeof(computed_hash), computed_hash);

    char computed_hex[65];
    for (int i = 0; i < 32; i++) snprintf(computed_hex + i * 2, 3, "%02x", computed_hash[i]);

    /* Constant-time comparison -- a length-dependent-only-by-construction
     * strcmp() here would leak how many leading hex characters match via
     * timing, letting an attacker incrementally guess the hash. Both
     * buffers are the same fixed 64-byte hex length regardless of input,
     * so CRYPTO_memcmp's constant-time guarantee applies cleanly. */
    bool match = (CRYPTO_memcmp(computed_hex, hash_hex, 64) == 0);
    return val_bool(match);
}

/* auth.generate_token(num_bytes) -- a CSPRNG-backed random token, hex-
 * encoded. Used for CSRF tokens, session IDs, password-reset tokens, etc.:
 * anywhere a script needs an unguessable opaque value. */
static Value lib_auth_generate_token(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    int num_bytes = 32;
    if (arg_count > base && args[base].type == VAL_INT) {
        num_bytes = (int)args[base].as.integer;
    }
    if (num_bytes < 1) num_bytes = 1;
    if (num_bytes > 128) num_bytes = 128; /* sane cap; nothing here needs more */

    unsigned char buf[128];
    if (RAND_bytes(buf, num_bytes) != 1) {
        runtime_error(vm, "auth.generate_token(): RAND_bytes() failed");
        return val_nil();
    }
    char hex[257];
    for (int i = 0; i < num_bytes; i++) snprintf(hex + i * 2, 3, "%02x", buf[i]);
    return val_string(allocate_string(vm, hex, num_bytes * 2));
}

/* auth.constant_time_eq(a, b) -- timing-safe string comparison. For
 * CSRF/session tokens: a naive == short-circuits on the first mismatched
 * byte (or differing length), leaking how much of a guess is correct. */
static Value lib_auth_constant_time_eq(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.constant_time_eq() requires two strings");
        return val_bool(false);
    }
    ObjString *a = args[base].as.string;
    ObjString *b = args[base + 1].as.string;
    if (a->length != b->length) return val_bool(false);
    if (a->length == 0) return val_bool(true);
    return val_bool(CRYPTO_memcmp(a->chars, b->chars, (size_t)a->length) == 0);
}

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const unsigned char *in, int in_len, char *out) {
    int i = 0, j = 0;
    while (i < in_len) {
        int remain = in_len - i;
        unsigned char a = in[i++];
        unsigned char b = remain >= 2 ? in[i++] : 0;
        unsigned char c = remain >= 3 ? in[i++] : 0;
        out[j++] = B64_ALPHABET[a >> 2];
        out[j++] = B64_ALPHABET[((a & 3) << 4) | (b >> 4)];
        out[j++] = remain >= 2 ? B64_ALPHABET[((b & 15) << 2) | (c >> 6)] : '=';
        out[j++] = remain >= 3 ? B64_ALPHABET[c & 63] : '=';
    }
    out[j] = '\0';
    return j;
}

static const signed char B64_DECODE[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static int base64_decode(const char *in, unsigned char *out) {
    int len = (int)strlen(in);
    /* Strip padding */
    while (len > 0 && in[len - 1] == '=') len--;
    if (len < 2) return 0;
    int i = 0, j = 0;
    while (i < len) {
        int a = B64_DECODE[(unsigned char)in[i++]];
        if (a < 0) break;
        if (i >= len) break;
        int b = B64_DECODE[(unsigned char)in[i++]];
        if (b < 0) break;
        out[j++] = (unsigned char)((a << 2) | (b >> 4));
        if (i >= len) break;
        int c = B64_DECODE[(unsigned char)in[i++]];
        if (c < 0) break;
        out[j++] = (unsigned char)(((b & 15) << 4) | (c >> 2));
        if (i >= len) break;
        int d = B64_DECODE[(unsigned char)in[i++]];
        if (d < 0) break;
        out[j++] = (unsigned char)(((c & 3) << 6) | d);
    }
    return j;
}

static Value lib_auth_sha1_base64(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING) {
        runtime_error(vm, "auth.sha1_base64() requires a string input");
        return val_nil();
    }
    const char *input = args[base].as.string->chars;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)input, strlen(input), hash);
    char b64[128];
    int len = base64_encode(hash, SHA_DIGEST_LENGTH, b64);
    return val_string(allocate_string(vm, b64, len));
}

/* ── Base32 encoding (RFC 4648) ──────────────────────────────────── */

static const char B32_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static int base32_encode(const unsigned char *in, int in_len, char *out) {
    int i = 0, j = 0;
    while (i < in_len) {
        int left = in_len - i;
        unsigned char a = in[i++];
        unsigned char b = left >= 2 ? in[i++] : 0;
        unsigned char c = left >= 3 ? in[i++] : 0;
        unsigned char d = left >= 4 ? in[i++] : 0;
        unsigned char e = left >= 5 ? in[i++] : 0;
        out[j++] = B32_ALPHABET[a >> 3];
        out[j++] = B32_ALPHABET[((a & 0x07) << 2) | (b >> 6)];
        if (left < 2) { out[j++] = '='; out[j++] = '='; out[j++] = '='; out[j++] = '='; break; }
        out[j++] = B32_ALPHABET[(b >> 1) & 0x1f];
        out[j++] = B32_ALPHABET[((b & 0x01) << 4) | (c >> 4)];
        if (left < 3) { out[j++] = '='; out[j++] = '='; out[j++] = '='; break; }
        out[j++] = B32_ALPHABET[((c & 0x0f) << 1) | (d >> 7)];
        if (left < 4) { out[j++] = '='; out[j++] = '='; break; }
        out[j++] = B32_ALPHABET[(d >> 2) & 0x1f];
        out[j++] = B32_ALPHABET[((d & 0x03) << 3) | (e >> 5)];
        if (left < 5) { out[j++] = '='; break; }
        out[j++] = B32_ALPHABET[e & 0x1f];
    }
    out[j] = '\0';
    return j;
}

static const signed char B32_DECODE[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,26,27,28,29,30,31,-1,-1,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static int base32_decode(const char *in, unsigned char *out, int out_max) {
    int len = (int)strlen(in);
    /* Strip padding characters */
    while (len > 0 && in[len - 1] == '=') len--;
    if (len < 2) return 0;
    int i = 0, j = 0;
    while (i < len && j < out_max) {
        /* Read up to 8 valid quintet values for one 5-byte group */
        int q[8];
        int n = 0;
        while (n < 8 && i < len) {
            int v = B32_DECODE[(unsigned char)in[i++]];
            if (v < 0) break;
            q[n++] = v;
        }
        if (n < 2) break;
        /* byte 0: q[0](5) + q[1](3) */
        if (j >= out_max) break;
        out[j++] = (unsigned char)((q[0] << 3) | (q[1] >> 2));
        if (n < 3) break;
        /* byte 1: q[1](2) + q[2](5) + q[3](1) */
        if (j >= out_max) break;
        out[j++] = (unsigned char)(((q[1] & 3) << 6) | (q[2] << 1) | ((n > 3 ? q[3] : 0) >> 4));
        if (n < 4) break;
        /* byte 2: q[3](4) + q[4](4) */
        if (j >= out_max) break;
        out[j++] = (unsigned char)(((q[3] & 0x0f) << 4) | ((n > 4 ? q[4] : 0) >> 1));
        if (n < 5) break;
        /* byte 3: q[4](1) + q[5](5) + q[6](2) */
        if (j >= out_max) break;
        out[j++] = (unsigned char)(((q[4] & 1) << 7) | (q[5] << 2) | ((n > 6 ? q[6] : 0) >> 3));
        if (n < 6) break;
        /* byte 4: q[6](3) + q[7](5) */
        if (j >= out_max) break;
        out[j++] = (unsigned char)((((n > 6 ? q[6] : 0) & 7) << 5) | (n > 7 ? q[7] : 0));
    }
    return j;
}

/* ── HMAC-SHA1 helper (for TOTP) ─────────────────────────────────── */

static void hmac_sha1(const unsigned char *key, int key_len,
                      const unsigned char *data, int data_len,
                      unsigned char *out, unsigned int *out_len) {
    HMAC(EVP_sha1(), key, key_len, data, data_len, out, out_len);
}

/* ── 1. TOTP (RFC 6238) ──────────────────────────────────────────── */

static Value lib_auth_totp_generate(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING) {
        runtime_error(vm, "auth.totp_generate() requires a base32-encoded secret string");
        return val_nil();
    }
    const char *secret_b32 = args[base].as.string->chars;

    /* Base32 decode the secret */
    unsigned char raw_secret[64];
    int raw_len = base32_decode(secret_b32, raw_secret, (int)sizeof(raw_secret));
    if (raw_len < 1) {
        runtime_error(vm, "auth.totp_generate(): invalid base32 secret");
        return val_nil();
    }

    /* Current time step (30s window) */
    time_t now = time(NULL);
    uint64_t counter = (uint64_t)(now / 30);

    /* Pack counter as 8-byte big-endian */
    unsigned char counter_bytes[8];
    for (int i = 7; i >= 0; i--) {
        counter_bytes[i] = (unsigned char)(counter & 0xff);
        counter >>= 8;
    }

    /* HMAC-SHA1 */
    unsigned char hash[20];
    unsigned int hash_len = 0;
    hmac_sha1(raw_secret, raw_len, counter_bytes, 8, hash, &hash_len);

    /* Dynamic truncation (RFC 4226 section 5.3) */
    int offset = hash[19] & 0x0f;
    int32_t code = ((hash[offset] & 0x7f) << 24)
                 | ((hash[offset + 1] & 0xff) << 16)
                 | ((hash[offset + 2] & 0xff) << 8)
                 |  (hash[offset + 3] & 0xff);
    code %= 1000000;

    char result[7];
    snprintf(result, sizeof(result), "%06d", code);
    return val_string(allocate_string(vm, result, 6));
}

static Value lib_auth_totp_verify(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.totp_verify() requires secret (string) and code (string)");
        return val_bool(false);
    }
    const char *secret_b32 = args[base].as.string->chars;
    const char *code_str = args[base + 1].as.string->chars;

    /* Default window: ±1 (3 windows total). Custom from optional third arg. */
    int window = 1;
    if (arg_count > base + 2 && args[base + 2].type == VAL_INT) {
        window = (int)args[base + 2].as.integer;
        if (window < 0) window = 0;
        if (window > 5) window = 5; /* sane cap */
    }

    /* Base32 decode */
    unsigned char raw_secret[64];
    int raw_len = base32_decode(secret_b32, raw_secret, (int)sizeof(raw_secret));
    if (raw_len < 1) return val_bool(false);

    time_t now = time(NULL);
    uint64_t base_counter = (uint64_t)(now / 30);

    /* Check current window and adjacent windows */
    for (int w = -window; w <= window; w++) {
        uint64_t counter = base_counter + w;
        unsigned char counter_bytes[8];
        uint64_t c = counter;
        for (int i = 7; i >= 0; i--) {
            counter_bytes[i] = (unsigned char)(c & 0xff);
            c >>= 8;
        }

        unsigned char hash[20];
        unsigned int hash_len = 0;
        hmac_sha1(raw_secret, raw_len, counter_bytes, 8, hash, &hash_len);

        int offset = hash[19] & 0x0f;
        int32_t code_val = ((hash[offset] & 0x7f) << 24)
                         | ((hash[offset + 1] & 0xff) << 16)
                         | ((hash[offset + 2] & 0xff) << 8)
                         |  (hash[offset + 3] & 0xff);
        code_val %= 1000000;

        char expected[7];
        snprintf(expected, sizeof(expected), "%06d", code_val);
        if (strcmp(code_str, expected) == 0)
            return val_bool(true);
    }
    return val_bool(false);
}

static Value lib_auth_totp_secret(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    unsigned char raw[16];
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        runtime_error(vm, "auth.totp_secret(): RAND_bytes() failed");
        return val_nil();
    }
    /* Base32 encode: 16 bytes -> ceil(16*8/5) = 26 chars, no padding */
    char encoded[32];
    int len = base32_encode(raw, sizeof(raw), encoded);
    return val_string(allocate_string(vm, encoded, len));
}

/* ── 2. RS256 JWT Signing ────────────────────────────────────────── */

static EVP_PKEY *load_private_key_pem(const char *pem, int pem_len) {
    BIO *bio = BIO_new_mem_buf((void *)pem, pem_len);
    if (!bio) return NULL;
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return pkey;
}

static EVP_PKEY *load_public_key_pem(const char *pem, int pem_len) {
    BIO *bio = BIO_new_mem_buf((void *)pem, pem_len);
    if (!bio) return NULL;
    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return pkey;
}

static Value lib_auth_sign_jwt_rs256(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRUCT || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.sign_jwt_rs256() requires payload (struct) and private_key_pem (string)");
        return val_nil();
    }
    const char *pem = args[base + 1].as.string->chars;
    int pem_len = args[base + 1].as.string->length;

    EVP_PKEY *pkey = load_private_key_pem(pem, pem_len);
    if (!pkey) {
        runtime_error(vm, "auth.sign_jwt_rs256(): failed to load private key PEM");
        return val_nil();
    }

    /* Header: {"alg":"RS256","typ":"JWT"} */
    ObjStruct *header = new_struct(vm, 2, false);
    free(header->field_names);
    static const char *hnames[] = { "alg", "typ" };
    struct_attach_shape(vm, header, NULL, (char *const *)hnames, 2);
    header->fields[0] = val_string(copy_string("RS256", 5));
    header->fields[1] = val_string(copy_string("JWT", 3));

    int header_json_len;
    char *header_json = json_encode(vm, val_struct(header), &header_json_len);
    int payload_json_len;
    char *payload_json = json_encode(vm, args[base], &payload_json_len);

    /* Base64url encode header and payload */
    int hb64_buf = (header_json_len + 2) / 3 * 4 + 1;
    char *hb64 = (char *)malloc(hb64_buf);
    int hb64_actual = base64url_encode((unsigned char *)header_json, header_json_len, hb64);

    int pb64_buf = (payload_json_len + 2) / 3 * 4 + 1;
    char *pb64 = (char *)malloc(pb64_buf);
    int pb64_actual = base64url_encode((unsigned char *)payload_json, payload_json_len, pb64);

    /* Signing input: hb64.pb64 */
    int sig_input_len = hb64_actual + 1 + pb64_actual;
    char *sig_input = (char *)malloc(sig_input_len + 1);
    memcpy(sig_input, hb64, hb64_actual);
    sig_input[hb64_actual] = '.';
    memcpy(sig_input + hb64_actual + 1, pb64, pb64_actual);
    sig_input[sig_input_len] = '\0';

    /* RS256 sign via EVP_DigestSign */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char sig_buf[512];
    size_t sig_len = sizeof(sig_buf);

    if (!EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey)
        || !EVP_DigestSignUpdate(ctx, sig_input, sig_input_len)
        || !EVP_DigestSignFinal(ctx, sig_buf, &sig_len)) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        free(header_json); free(payload_json); free(hb64); free(pb64); free(sig_input);
        runtime_error(vm, "auth.sign_jwt_rs256(): signing failed");
        return val_nil();
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    /* Base64url encode signature */
    int sb64_buf = ((int)sig_len + 2) / 3 * 4 + 1;
    char *sb64 = (char *)malloc(sb64_buf);
    int sb64_actual = base64url_encode(sig_buf, (int)sig_len, sb64);

    /* Build final token */
    int token_len = hb64_actual + 1 + pb64_actual + 1 + sb64_actual;
    char *token = (char *)malloc(token_len + 1);
    memcpy(token, hb64, hb64_actual);
    token[hb64_actual] = '.';
    memcpy(token + hb64_actual + 1, pb64, pb64_actual);
    token[hb64_actual + 1 + pb64_actual] = '.';
    memcpy(token + hb64_actual + 1 + pb64_actual + 1, sb64, sb64_actual);
    token[token_len] = '\0';

    ObjString *result = allocate_string(vm, token, token_len);

    free(header_json); free(payload_json); free(hb64); free(pb64);
    free(sig_input); free(sb64); free(token);

    return val_string(result);
}

static Value lib_auth_verify_jwt_rs256(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.verify_jwt_rs256() requires token (string) and public_key_pem (string)");
        return val_nil();
    }
    const char *token = args[base].as.string->chars;
    const char *pem = args[base + 1].as.string->chars;
    int pem_len = args[base + 1].as.string->length;

    EVP_PKEY *pkey = load_public_key_pem(pem, pem_len);
    if (!pkey) {
        runtime_error(vm, "auth.verify_jwt_rs256(): failed to load public key PEM");
        return val_nil();
    }

    /* Split token by '.' */
    const char *dot1 = strchr(token, '.');
    if (!dot1) { EVP_PKEY_free(pkey); return val_nil(); }
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) { EVP_PKEY_free(pkey); return val_nil(); }

    int hb64_len = (int)(dot1 - token);
    int pb64_len = (int)(dot2 - dot1 - 1);
    int sb64_len = (int)(strlen(dot2 + 1));

    if (hb64_len == 0 || pb64_len == 0 || sb64_len == 0) {
        EVP_PKEY_free(pkey);
        return val_nil();
    }

    /* Decode signature from base64url */
    int sig_decoded_max = sb64_len;
    unsigned char *sig_decoded = (unsigned char *)malloc(sig_decoded_max);
    int sig_decoded_len = base64url_decode(dot2 + 1, sig_decoded);

    /* Rebuild signing input */
    int sig_input_len = hb64_len + 1 + pb64_len;
    char *sig_input = (char *)malloc(sig_input_len + 1);
    memcpy(sig_input, token, hb64_len);
    sig_input[hb64_len] = '.';
    memcpy(sig_input + hb64_len + 1, dot1 + 1, pb64_len);
    sig_input[sig_input_len] = '\0';

    /* Verify via EVP_DigestVerify */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pkey)
          && EVP_DigestVerifyUpdate(ctx, sig_input, sig_input_len)
          && EVP_DigestVerifyFinal(ctx, sig_decoded, (size_t)sig_decoded_len);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    free(sig_decoded);
    free(sig_input);

    if (!ok) return val_nil();

    /* Decode payload and return */
    char *payload_b64 = (char *)malloc(pb64_len + 1);
    memcpy(payload_b64, dot1 + 1, pb64_len);
    payload_b64[pb64_len] = '\0';

    unsigned char *decoded = (unsigned char *)malloc((size_t)pb64_len + 1);
    int decoded_len = base64url_decode(payload_b64, decoded);
    decoded[decoded_len] = '\0';

    Value result = json_decode(vm, (const char *)decoded);

    free(payload_b64);
    free(decoded);

    return result;
}

/* ── 3. AES-256-GCM Encryption/Decryption ────────────────────────── */

static int hex_decode(const char *hex, int hex_len, unsigned char *out, int out_max) {
    int len = 0;
    for (int i = 0; i < hex_len && len < out_max; i += 2) {
        unsigned int byte;
        if (sscanf(hex + i, "%02x", &byte) != 1) break;
        out[len++] = (unsigned char)byte;
    }
    return len;
}

static Value lib_auth_encrypt(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.encrypt() requires plaintext (string) and key_hex (string)");
        return val_nil();
    }
    const char *plaintext = args[base].as.string->chars;
    int pt_len = args[base].as.string->length;
    const char *key_hex = args[base + 1].as.string->chars;
    int key_hex_len = args[base + 1].as.string->length;

    unsigned char key[32];
    int key_len = hex_decode(key_hex, key_hex_len, key, (int)sizeof(key));
    if (key_len != 32) {
        runtime_error(vm, "auth.encrypt(): key_hex must decode to exactly 32 bytes (64 hex chars)");
        return val_nil();
    }

    /* Generate 12-byte IV */
    unsigned char iv[12];
    if (RAND_bytes(iv, sizeof(iv)) != 1) {
        runtime_error(vm, "auth.encrypt(): RAND_bytes() failed");
        return val_nil();
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        runtime_error(vm, "auth.encrypt(): EVP_CIPHER_CTX_new() failed");
        return val_nil();
    }

    /* Output buffer: ciphertext may be up to pt_len + 16 */
    unsigned char *ct_buf = (unsigned char *)malloc((size_t)pt_len + 16);
    int ct_len = 0, out_len = 0;

    if (!EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL)
        || !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL)
        || !EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv)
        || !EVP_EncryptUpdate(ctx, ct_buf, &out_len, (unsigned char *)plaintext, pt_len)) {
        EVP_CIPHER_CTX_free(ctx);
        free(ct_buf);
        runtime_error(vm, "auth.encrypt(): encryption failed");
        return val_nil();
    }
    ct_len = out_len;

    if (!EVP_EncryptFinal_ex(ctx, ct_buf + ct_len, &out_len)) {
        EVP_CIPHER_CTX_free(ctx);
        free(ct_buf);
        runtime_error(vm, "auth.encrypt(): EVP_EncryptFinal_ex() failed");
        return val_nil();
    }
    ct_len += out_len;

    unsigned char tag[16];
    if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag)) {
        EVP_CIPHER_CTX_free(ctx);
        free(ct_buf);
        runtime_error(vm, "auth.encrypt(): failed to get GCM tag");
        return val_nil();
    }
    EVP_CIPHER_CTX_free(ctx);

    /* Build output: IV(12) + ciphertext + tag(16), then base64 */
    int raw_output_len = 12 + ct_len + 16;
    unsigned char *raw_output = (unsigned char *)malloc((size_t)raw_output_len);
    memcpy(raw_output, iv, 12);
    memcpy(raw_output + 12, ct_buf, ct_len);
    memcpy(raw_output + 12 + ct_len, tag, 16);
    free(ct_buf);

    int b64_buf = (raw_output_len + 2) / 3 * 4 + 1;
    char *b64_out = (char *)malloc((size_t)b64_buf);
    int b64_len = base64_encode(raw_output, raw_output_len, b64_out);
    free(raw_output);

    ObjString *result = allocate_string(vm, b64_out, b64_len);
    free(b64_out);

    return val_string(result);
}

static Value lib_auth_decrypt(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.decrypt() requires ciphertext_b64 (string) and key_hex (string)");
        return val_nil();
    }
    const char *b64_in = args[base].as.string->chars;
    const char *key_hex = args[base + 1].as.string->chars;
    int key_hex_len = args[base + 1].as.string->length;

    unsigned char key[32];
    int key_len = hex_decode(key_hex, key_hex_len, key, (int)sizeof(key));
    if (key_len != 32) {
        runtime_error(vm, "auth.decrypt(): key_hex must decode to exactly 32 bytes (64 hex chars)");
        return val_nil();
    }

    /* Base64 decode input */
    int b64_len = (int)strlen(b64_in);
    unsigned char *raw_input = (unsigned char *)malloc((size_t)b64_len);
    int raw_len = base64_decode(b64_in, raw_input);

    /* Need at least IV(12) + tag(16) = 28 bytes plus some ciphertext */
    if (raw_len < 29) {
        free(raw_input);
        runtime_error(vm, "auth.decrypt(): ciphertext too short");
        return val_nil();
    }

    unsigned char *iv = raw_input;
    int ct_len = raw_len - 12 - 16;
    unsigned char *ct = raw_input + 12;
    unsigned char *tag = raw_input + 12 + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        free(raw_input);
        runtime_error(vm, "auth.decrypt(): EVP_CIPHER_CTX_new() failed");
        return val_nil();
    }

    unsigned char *pt_buf = (unsigned char *)malloc((size_t)ct_len + 16);
    int pt_len = 0, out_len = 0;

    int ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL)
          && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL)
          && EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv)
          && EVP_DecryptUpdate(ctx, pt_buf, &out_len, ct, ct_len);
    pt_len = out_len;

    if (ok) {
        /* Set expected tag before Final */
        if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag)) ok = 0;
    }
    if (ok) {
        ok = EVP_DecryptFinal_ex(ctx, pt_buf + pt_len, &out_len);
        pt_len += out_len;
    }

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        free(raw_input);
        free(pt_buf);
        /* Wrong key or tampered data — return nil */
        return val_nil();
    }

    ObjString *result = allocate_string(vm, (const char *)pt_buf, pt_len);
    free(raw_input);
    free(pt_buf);

    return val_string(result);
}

/* ── 4. JWT v2 with claims validation ────────────────────────────── */

static Value lib_auth_sign_jwt_v2(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRUCT || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.sign_jwt_v2() requires payload (struct), secret (string), and optional opts (struct)");
        return val_nil();
    }

    const char *secret = args[base + 1].as.string->chars;
    int secret_len = args[base + 1].as.string->length;

    /* Parse opts */
    ObjStruct *opts = NULL;
    if (arg_count > base + 2 && args[base + 2].type == VAL_STRUCT) {
        opts = args[base + 2].as.structure;
    }

    long long exp_offset = 0;
    const char *iss = NULL;
    const char *sub = NULL;
    const char *aud = NULL;

    if (opts) {
        for (int i = 0; i < opts->field_count; i++) {
            if (strcmp(opts->field_names[i], "exp") == 0 && opts->fields[i].type == VAL_INT)
                exp_offset = opts->fields[i].as.integer;
            else if (strcmp(opts->field_names[i], "iss") == 0 && opts->fields[i].type == VAL_STRING)
                iss = opts->fields[i].as.string->chars;
            else if (strcmp(opts->field_names[i], "sub") == 0 && opts->fields[i].type == VAL_STRING)
                sub = opts->fields[i].as.string->chars;
            else if (strcmp(opts->field_names[i], "aud") == 0 && opts->fields[i].type == VAL_STRING)
                aud = opts->fields[i].as.string->chars;
        }
    }

    /* JSON-encode the user payload */
    int user_json_len;
    char *user_json = json_encode(vm, args[base], &user_json_len);
    /* user_json is "{...}" */

    /* Build claims string to append before the closing '}' */
    time_t now = time(NULL);
    char claims_buf[512];
    int claims_len = 0;

    claims_len = snprintf(claims_buf, sizeof(claims_buf),
                          ",\"iat\":%lld", (long long)now);
    if (exp_offset > 0) {
        claims_len += snprintf(claims_buf + claims_len, sizeof(claims_buf) - (size_t)claims_len,
                               ",\"exp\":%lld", (long long)(now + exp_offset));
    }
    if (iss) {
        claims_len += snprintf(claims_buf + claims_len, sizeof(claims_buf) - (size_t)claims_len,
                               ",\"iss\":\"%s\"", iss);
    }
    if (sub) {
        claims_len += snprintf(claims_buf + claims_len, sizeof(claims_buf) - (size_t)claims_len,
                               ",\"sub\":\"%s\"", sub);
    }
    if (aud) {
        claims_len += snprintf(claims_buf + claims_len, sizeof(claims_buf) - (size_t)claims_len,
                               ",\"aud\":\"%s\"", aud);
    }

    /* Build full payload JSON: remove trailing '}' from user_json, add claims, re-add '}' */
    int full_len = user_json_len - 1 + claims_len + 1;
    char *full_payload = (char *)malloc((size_t)full_len + 1);
    memcpy(full_payload, user_json, (size_t)(user_json_len - 1));
    memcpy(full_payload + user_json_len - 1, claims_buf, (size_t)claims_len);
    full_payload[user_json_len - 1 + claims_len] = '}';
    full_payload[full_len] = '\0';

    /* Header */
    ObjStruct *header = new_struct(vm, 2, false);
    free(header->field_names);
    static const char *hnames[] = { "alg", "typ" };
    struct_attach_shape(vm, header, NULL, (char *const *)hnames, 2);
    header->fields[0] = val_string(copy_string("HS256", 5));
    header->fields[1] = val_string(copy_string("JWT", 3));

    int header_json_len;
    char *header_json = json_encode(vm, val_struct(header), &header_json_len);

    /* Base64url encode */
    int hb64_buf = (header_json_len + 2) / 3 * 4 + 1;
    char *hb64 = (char *)malloc((size_t)hb64_buf);
    int hb64_actual = base64url_encode((unsigned char *)header_json, header_json_len, hb64);

    int pb64_buf = (full_len + 2) / 3 * 4 + 1;
    char *pb64 = (char *)malloc((size_t)pb64_buf);
    int pb64_actual = base64url_encode((unsigned char *)full_payload, full_len, pb64);

    /* Signing input */
    int sig_input_len = hb64_actual + 1 + pb64_actual;
    char *sig_input = (char *)malloc((size_t)sig_input_len + 1);
    memcpy(sig_input, hb64, (size_t)hb64_actual);
    sig_input[hb64_actual] = '.';
    memcpy(sig_input + hb64_actual + 1, pb64, (size_t)pb64_actual);
    sig_input[sig_input_len] = '\0';

    /* HMAC-SHA256 */
    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    HMAC(EVP_sha256(), secret, secret_len,
         (unsigned char *)sig_input, (size_t)sig_input_len,
         hmac_result, &hmac_len);

    /* Base64url encode signature */
    int sb64_buf = (hmac_len + 2) / 3 * 4 + 1;
    char *sb64 = (char *)malloc((size_t)sb64_buf);
    int sb64_actual = base64url_encode(hmac_result, (int)hmac_len, sb64);

    /* Build token */
    int token_len = hb64_actual + 1 + pb64_actual + 1 + sb64_actual;
    char *token = (char *)malloc((size_t)token_len + 1);
    memcpy(token, hb64, (size_t)hb64_actual);
    token[hb64_actual] = '.';
    memcpy(token + hb64_actual + 1, pb64, (size_t)pb64_actual);
    token[hb64_actual + 1 + pb64_actual] = '.';
    memcpy(token + hb64_actual + 1 + pb64_actual + 1, sb64, (size_t)sb64_actual);
    token[token_len] = '\0';

    ObjString *result = allocate_string(vm, token, token_len);

    free(header_json); free(user_json); free(full_payload);
    free(hb64); free(pb64); free(sig_input); free(sb64); free(token);

    return val_string(result);
}

static Value lib_auth_verify_jwt_v2(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "auth.verify_jwt_v2() requires token (string), secret (string), and optional opts (struct)");
        return val_nil();
    }
    const char *token = args[base].as.string->chars;
    const char *secret = args[base + 1].as.string->chars;
    int secret_len = args[base + 1].as.string->length;

    /* Parse opts */
    ObjStruct *opts = NULL;
    if (arg_count > base + 2 && args[base + 2].type == VAL_STRUCT) {
        opts = args[base + 2].as.structure;
    }

    const char *expected_iss = NULL;
    const char *expected_aud = NULL;
    int leeway = 0;

    if (opts) {
        for (int i = 0; i < opts->field_count; i++) {
            if (strcmp(opts->field_names[i], "iss") == 0 && opts->fields[i].type == VAL_STRING)
                expected_iss = opts->fields[i].as.string->chars;
            else if (strcmp(opts->field_names[i], "aud") == 0 && opts->fields[i].type == VAL_STRING)
                expected_aud = opts->fields[i].as.string->chars;
            else if (strcmp(opts->field_names[i], "leeway") == 0 && opts->fields[i].type == VAL_INT)
                leeway = (int)opts->fields[i].as.integer;
        }
    }

    /* Split token */
    const char *dot1 = strchr(token, '.');
    if (!dot1) return val_nil();
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return val_nil();

    int hb64_len = (int)(dot1 - token);
    int pb64_len = (int)(dot2 - dot1 - 1);
    int sb64_len = (int)(strlen(dot2 + 1));

    if (hb64_len == 0 || pb64_len == 0 || sb64_len == 0)
        return val_nil();

    /* Verify signature */
    int sig_input_len = hb64_len + 1 + pb64_len;
    char *sig_input = (char *)malloc((size_t)sig_input_len + 1);
    memcpy(sig_input, token, (size_t)hb64_len);
    sig_input[hb64_len] = '.';
    memcpy(sig_input + hb64_len + 1, dot1 + 1, (size_t)pb64_len);
    sig_input[sig_input_len] = '\0';

    unsigned char hmac_result[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    HMAC(EVP_sha256(), secret, secret_len,
         (unsigned char *)sig_input, (size_t)sig_input_len,
         hmac_result, &hmac_len);

    int sb64_buf = (hmac_len + 2) / 3 * 4 + 1;
    char *expected_sig = (char *)malloc((size_t)sb64_buf);
    int expected_sig_len = base64url_encode(hmac_result, (int)hmac_len, expected_sig);

    bool sig_ok = (sb64_len == expected_sig_len)
               && (CRYPTO_memcmp(dot2 + 1, expected_sig, (size_t)sb64_len) == 0);
    free(expected_sig);
    free(sig_input);

    if (!sig_ok) return val_nil();

    /* Decode payload */
    char *payload_b64 = (char *)malloc((size_t)pb64_len + 1);
    memcpy(payload_b64, dot1 + 1, (size_t)pb64_len);
    payload_b64[pb64_len] = '\0';

    int decoded_max = pb64_len;
    unsigned char *decoded = (unsigned char *)malloc((size_t)decoded_max + 1);
    int decoded_len = base64url_decode(payload_b64, decoded);
    decoded[decoded_len] = '\0';
    free(payload_b64);

    Value result = json_decode(vm, (const char *)decoded);
    free(decoded);

    if (result.type == VAL_NIL) return val_nil();

    /* Validate claims */
    if (result.type == VAL_STRUCT) {
        ObjStruct *ps = result.as.structure;
        time_t now = time(NULL);

        for (int i = 0; i < ps->field_count; i++) {
            /* exp: must be after current time (minus leeway) */
            if (strcmp(ps->field_names[i], "exp") == 0 && ps->fields[i].type == VAL_INT) {
                if (ps->fields[i].as.integer < (int64_t)(now - leeway))
                    return val_nil(); /* expired */
            }
            /* nbf: not before, optional */
            if (strcmp(ps->field_names[i], "nbf") == 0 && ps->fields[i].type == VAL_INT) {
                if (ps->fields[i].as.integer > (int64_t)(now + leeway))
                    return val_nil(); /* not yet valid */
            }
            /* iss: must match expected */
            if (strcmp(ps->field_names[i], "iss") == 0 && ps->fields[i].type == VAL_STRING) {
                if (expected_iss && strcmp(ps->fields[i].as.string->chars, expected_iss) != 0)
                    return val_nil();
            }
            /* aud: must match expected */
            if (strcmp(ps->field_names[i], "aud") == 0 && ps->fields[i].type == VAL_STRING) {
                if (expected_aud && strcmp(ps->fields[i].as.string->chars, expected_aud) != 0)
                    return val_nil();
            }
        }
    }

    return result;
}

/* ── 5. Argon2 / PBKDF2 v2 Password Hashing ──────────────────────── */
/*
 * Argon2id via OpenSSL EVP_KDF is not available in most current OpenSSL
 * 3.x releases as a built-in KDF. This function falls back to PBKDF2-HMAC-
 * SHA256 with a different format marker ($pbkdf2v2$) to distinguish from
 * the existing auth.hash_password() output.
 *
 * When OpenSSL gains EVP_KDF_argon2 (expected in 3.5+), replace the PBKDF2
 * call below with:
 *
 *   EVP_KDF *kdf = EVP_KDF_fetch(NULL, "ARGON2", NULL);
 *   EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
 *   OSSL_PARAM params[5];
 *   int p = 0;
 *   params[p++] = OSSL_PARAM_construct_utf8_string("pass", password, password_len);
 *   params[p++] = OSSL_PARAM_construct_octet_string("salt", salt, 16);
 *   params[p++] = OSSL_PARAM_construct_uint32("iter", &time_cost);
 *   params[p++] = OSSL_PARAM_construct_uint32("memcost", &memory_cost);
 *   params[p++] = OSSL_PARAM_construct_uint32("threads", &parallelism);
 *   params[p] = OSSL_PARAM_construct_end();
 *   EVP_KDF_derive(kctx, hash, 32, params);
 *   EVP_KDF_CTX_free(kctx);
 *   EVP_KDF_free(kdf);
 */

static Value lib_auth_hash_password_v2(VM *vm, int arg_count, Value *args) {
    int base = get_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING) {
        runtime_error(vm, "auth.hash_password_v2() requires a password string");
        return val_nil();
    }
    const char *password = args[base].as.string->chars;
    int password_len = args[base].as.string->length;

    /* Default Argon2id parameters (RFC 9106 low-end) */
    unsigned int time_cost = 3;
    unsigned int memory_cost = 65536;  /* 64 MiB */
    unsigned int parallelism = 4;

    /* Parse optional options struct */
    if (arg_count > base + 1 && args[base + 1].type == VAL_STRUCT) {
        ObjStruct *opts = args[base + 1].as.structure;
        for (int i = 0; i < opts->field_count; i++) {
            if (strcmp(opts->field_names[i], "time_cost") == 0 && opts->fields[i].type == VAL_INT)
                time_cost = (unsigned int)opts->fields[i].as.integer;
            else if (strcmp(opts->field_names[i], "memory_cost") == 0 && opts->fields[i].type == VAL_INT)
                memory_cost = (unsigned int)opts->fields[i].as.integer;
            else if (strcmp(opts->field_names[i], "parallelism") == 0 && opts->fields[i].type == VAL_INT)
                parallelism = (unsigned int)opts->fields[i].as.integer;
        }
    }

    unsigned char salt[16];
    RAND_bytes(salt, sizeof(salt));

    /*
     * Fallback to PBKDF2-HMAC-SHA256 (600,000 iterations per OWASP 2023+)
     * until OpenSSL 3.5+ exposes EVP_KDF_Argon2.
     */
    unsigned int iterations = 600000;
    unsigned char hash[32];
    PKCS5_PBKDF2_HMAC(password, password_len, salt, sizeof(salt),
                      iterations, EVP_sha256(), sizeof(hash), hash);

    char salt_hex[33];
    char hash_hex[65];
    for (int i = 0; i < 16; i++) snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < 32; i++) snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);

    /* Use $pbkdf2v2$ marker — distinct from the v1 format ($pbkdf2$) */
    char final_hash[256];
    snprintf(final_hash, sizeof(final_hash),
             "$pbkdf2v2$%u$%u$%u$%u$%s$%s",
             time_cost, memory_cost, parallelism, iterations, salt_hex, hash_hex);

    return val_string(allocate_string(vm, final_hash, (int)strlen(final_hash)));
}

/* ── Registration ────────────────────────────────────────────────── */

void lib_auth_init(VM *vm) {
    ObjModule *mod = new_module("auth");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("auth", 4), val_module(mod));
    vm_register_dispatch(vm, "auth", "hash_sha256",      val_native_fn((void *)lib_auth_hash_sha256));
    vm_register_dispatch(vm, "auth", "sign_jwt",         val_native_fn((void *)lib_auth_sign_jwt));
    vm_register_dispatch(vm, "auth", "verify_jwt",       val_native_fn((void *)lib_auth_verify_jwt));
    vm_register_dispatch(vm, "auth", "hash_password",    val_native_fn((void *)lib_auth_hash_password));
    vm_register_dispatch(vm, "auth", "verify_password",  val_native_fn((void *)lib_auth_verify_password));
    vm_register_dispatch(vm, "auth", "generate_token",   val_native_fn((void *)lib_auth_generate_token));
    vm_register_dispatch(vm, "auth", "constant_time_eq",  val_native_fn((void *)lib_auth_constant_time_eq));
    vm_register_dispatch(vm, "auth", "sha1_base64",      val_native_fn((void *)lib_auth_sha1_base64));
    vm_register_dispatch(vm, "auth", "totp_generate",    val_native_fn((void *)lib_auth_totp_generate));
    vm_register_dispatch(vm, "auth", "totp_verify",      val_native_fn((void *)lib_auth_totp_verify));
    vm_register_dispatch(vm, "auth", "totp_secret",      val_native_fn((void *)lib_auth_totp_secret));
    vm_register_dispatch(vm, "auth", "sign_jwt_rs256",   val_native_fn((void *)lib_auth_sign_jwt_rs256));
    vm_register_dispatch(vm, "auth", "verify_jwt_rs256", val_native_fn((void *)lib_auth_verify_jwt_rs256));
    vm_register_dispatch(vm, "auth", "encrypt",          val_native_fn((void *)lib_auth_encrypt));
    vm_register_dispatch(vm, "auth", "decrypt",          val_native_fn((void *)lib_auth_decrypt));
    vm_register_dispatch(vm, "auth", "sign_jwt_v2",      val_native_fn((void *)lib_auth_sign_jwt_v2));
    vm_register_dispatch(vm, "auth", "verify_jwt_v2",    val_native_fn((void *)lib_auth_verify_jwt_v2));
    vm_register_dispatch(vm, "auth", "hash_password_v2", val_native_fn((void *)lib_auth_hash_password_v2));
}
