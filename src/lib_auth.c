#include "lib_auth.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

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

void lib_auth_init(VM *vm) {
    ObjModule *mod = new_module("auth");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("auth", 4), val_module(mod));
    vm_register_dispatch(vm, "auth", "hash_sha256",     val_native_fn((void *)lib_auth_hash_sha256));
    vm_register_dispatch(vm, "auth", "sign_jwt",        val_native_fn((void *)lib_auth_sign_jwt));
    vm_register_dispatch(vm, "auth", "verify_jwt",      val_native_fn((void *)lib_auth_verify_jwt));
    vm_register_dispatch(vm, "auth", "hash_password",   val_native_fn((void *)lib_auth_hash_password));
    vm_register_dispatch(vm, "auth", "verify_password", val_native_fn((void *)lib_auth_verify_password));
    vm_register_dispatch(vm, "auth", "generate_token",  val_native_fn((void *)lib_auth_generate_token));
    vm_register_dispatch(vm, "auth", "constant_time_eq", val_native_fn((void *)lib_auth_constant_time_eq));
    vm_register_dispatch(vm, "auth", "sha1_base64",     val_native_fn((void *)lib_auth_sha1_base64));
}
