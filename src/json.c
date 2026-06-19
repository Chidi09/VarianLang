#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ═══════════════════════════════════════════
 *  JSON Encoder
 * ═══════════════════════════════════════════ */

typedef struct { char *data; int len; int cap; } SBuf;

static void sb_init(SBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

static void sb_grow(SBuf *b, int needed) {
    if (b->len + needed <= b->cap) return;
    int new_cap = b->cap ? b->cap * 2 : 256;
    while (new_cap < b->len + needed) new_cap *= 2;
    b->data = (char *)realloc(b->data, (size_t)new_cap);
    b->cap = new_cap;
}

static void sb_put(SBuf *b, const char *s, int len) {
    sb_grow(b, len); memcpy(b->data + b->len, s, (size_t)len); b->len += len;
}

static void sb_putc(SBuf *b, char c) { sb_grow(b, 1); b->data[b->len++] = c; }

static void sb_puts(SBuf *b, const char *s) { sb_put(b, s, (int)strlen(s)); }

static void json_encode_value(VM *vm, SBuf *b, Value v);

static void json_encode_string(SBuf *b, const char *s, int len) {
    sb_putc(b, '"');
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n"); break;
            case '\t': sb_puts(b, "\\t"); break;
            case '\r': sb_puts(b, "\\r"); break;
            default:
                if (c < 32) { char hex[8]; snprintf(hex, sizeof(hex), "\\u%04x", c); sb_puts(b, hex); }
                else sb_putc(b, (char)c);
                break;
        }
    }
    sb_putc(b, '"');
}

static void json_encode_value(VM *vm, SBuf *b, Value v) {
    (void)vm;
    switch (v.type) {
        case VAL_NIL:       sb_puts(b, "null"); break;
        case VAL_BOOL:      sb_puts(b, v.as.boolean ? "true" : "false"); break;
        case VAL_INT: {
            char buf[32]; snprintf(buf, sizeof(buf), "%ld", (long)v.as.integer); sb_puts(b, buf);
            break;
        }
        case VAL_FLOAT: {
            char buf[64]; snprintf(buf, sizeof(buf), "%g", v.as.floating);
            if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) { sb_puts(b, buf); sb_puts(b, ".0"); }
            else sb_puts(b, buf);
            break;
        }
        case VAL_STRING:
            json_encode_string(b, v.as.string->chars, v.as.string->length);
            break;
        case VAL_ARRAY: {
            sb_putc(b, '[');
            for (int i = 0; i < v.as.array->count; i++) {
                if (i > 0) sb_putc(b, ',');
                json_encode_value(vm, b, v.as.array->elements[i]);
            }
            sb_putc(b, ']');
            break;
        }
        case VAL_STRUCT: {
            sb_putc(b, '{');
            ObjStruct *s = v.as.structure;
            for (int i = 0; i < s->field_count; i++) {
                if (i > 0) sb_putc(b, ',');
                json_encode_string(b, s->field_names[i], (int)strlen(s->field_names[i]));
                sb_putc(b, ':');
                json_encode_value(vm, b, s->fields[i]);
            }
            sb_putc(b, '}');
            break;
        }
        default: sb_puts(b, "null"); break;
    }
}

char *json_encode(VM *vm, Value v, int *out_len) {
    SBuf b; sb_init(&b);
    json_encode_value(vm, &b, v);
    sb_putc(&b, '\0');
    if (out_len) *out_len = b.len - 1;
    return b.data;
}

/* ═══════════════════════════════════════════
 *  JSON Decoder
 * ═══════════════════════════════════════════ */

void json_skip_ws(const char **p) {
    while (**p && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')) (*p)++;
}

char *json_decode_string(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    int len = 0;
    const char *s = *p;
    while (*s && *s != '"') {
        if (*s == '\\') { s++; if (*s) s++; len++; }
        else { s++; len++; }
    }
    if (*s != '"') return NULL;
    char *result = (char *)malloc((size_t)len + 1);
    if (!result) return NULL;
    int pos = 0;
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
                case '"':  result[pos++] = '"';  break;
                case '\\': result[pos++] = '\\'; break;
                case '/':  result[pos++] = '/';  break;
                case 'n':  result[pos++] = '\n'; break;
                case 't':  result[pos++] = '\t'; break;
                case 'r':  result[pos++] = '\r'; break;
                case 'u': {
                    char hex[5] = {0};
                    if (strlen(*p + 1) >= 4) {
                        for (int j = 0; j < 4; j++) hex[j] = (*p)[1 + j];
                        unsigned int cp = (unsigned int)strtol(hex, NULL, 16);
                        if (cp < 0x80) result[pos++] = (char)cp;
                        else if (cp < 0x800) {
                            result[pos++] = (char)(0xC0 | (cp >> 6));
                            result[pos++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            result[pos++] = (char)(0xE0 | (cp >> 12));
                            result[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            result[pos++] = (char)(0x80 | (cp & 0x3F));
                        }
                        (*p) += 4;
                    }
                    break;
                }
                default: result[pos++] = **p; break;
            }
        } else result[pos++] = **p;
        (*p)++;
    }
    result[pos] = '\0';
    if (**p == '"') (*p)++;
    return result;
}

Value json_decode_value(VM *vm, const char **p) {
    json_skip_ws(p);
    if (!**p) return val_nil();
    if (**p == 'n') { if (strncmp(*p, "null", 4) == 0) { *p += 4; return val_nil(); } }
    if (**p == 't') { if (strncmp(*p, "true", 4) == 0) { *p += 4; return val_bool(true); } }
    if (**p == 'f') { if (strncmp(*p, "false", 5) == 0) { *p += 5; return val_bool(false); } }
    if (**p == '"') {
        char *s = json_decode_string(p);
        if (!s) return val_nil();
        ObjString *os = allocate_string(vm, s, (int)strlen(s));
        free(s);
        return val_string(os);
    }
    if (**p == '[') {
        (*p)++;
        json_skip_ws(p);
        ObjArray *arr = new_array();
        arr->obj.next = vm->objects;
        vm->objects = (Obj *)arr;
        if (**p != ']') {
            while (1) {
                Value elem = json_decode_value(vm, p);
                if (arr->count >= arr->capacity) {
                    int new_cap = arr->capacity ? arr->capacity * 2 : 8;
                    arr->elements = (Value *)realloc(arr->elements, (size_t)new_cap * sizeof(Value));
                    arr->capacity = new_cap;
                }
                arr->elements[arr->count++] = elem;
                json_skip_ws(p);
                if (**p == ',') { (*p)++; json_skip_ws(p); }
                else break;
            }
        }
        if (**p == ']') (*p)++;
        return val_array(arr);
    }
    if (**p == '{') {
        (*p)++;
        json_skip_ws(p);
        char *field_names[64];
        Value field_values[64];
        int field_count = 0;
        if (**p != '}') {
            while (1) {
                json_skip_ws(p);
                if (**p != '"') break;
                field_names[field_count] = json_decode_string(p);
                if (!field_names[field_count]) break;
                json_skip_ws(p);
                if (**p == ':') (*p)++;
                json_skip_ws(p);
                field_values[field_count] = json_decode_value(vm, p);
                field_count++;
                json_skip_ws(p);
                if (**p == ',') { (*p)++; }
                else break;
            }
        }
        if (**p == '}') (*p)++;
        ObjStruct *s = (ObjStruct *)calloc(1, sizeof(ObjStruct));
        s->obj.type = VAL_STRUCT;
        s->field_count = field_count;
        s->field_names = (char **)calloc((size_t)field_count, sizeof(char *));
        s->fields = (Value *)calloc((size_t)field_count, sizeof(Value));
        s->type_name = strdup("json_object");
        s->obj.next = vm->objects;
        vm->objects = (Obj *)s;
        for (int i = 0; i < field_count; i++) {
            s->field_names[i] = field_names[i];
            s->fields[i] = field_values[i];
        }
        return val_struct(s);
    }
    {
        char *end = NULL;
        long long int_val = strtoll(*p, &end, 10);
        if (end > *p) {
            const char *saved = *p;
            *p = end;
            if (**p == '.' || **p == 'e' || **p == 'E') {
                *p = saved;
                double float_val = strtod(*p, &end);
                *p = end;
                return val_float(float_val);
            }
            return val_int((int64_t)int_val);
        }
    }
    return val_nil();
}

Value json_decode(VM *vm, const char *json) {
    const char *p = json;
    return json_decode_value(vm, &p);
}

Value native_json_encode(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1) return val_nil();
    int len;
    char *json_str = json_encode(vm, args[0], &len);
    if (!json_str) return val_nil();
    ObjString *s = allocate_string(vm, json_str, len);
    free(json_str);
    return val_string(s);
}

Value native_json_decode(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1 || args[0].type != VAL_STRING) return val_nil();
    return json_decode(vm, args[0].as.string->chars);
}
