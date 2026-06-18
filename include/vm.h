#ifndef VM_H
#define VM_H

#include "varian.h"
#include "ast.h"
#include <stdint.h>
#include <stdbool.h>

/* ─── Bytecode Opcodes ─── */
typedef enum {
    BC_CONSTANT,
    BC_NIL,
    BC_TRUE,
    BC_FALSE,
    BC_POP,

    /* Arithmetic */
    BC_ADD,
    BC_SUB,
    BC_MUL,
    BC_DIV,
    BC_MOD,
    BC_NEGATE,
    BC_NOT,

    /* Comparison */
    BC_EQUAL,
    BC_NOT_EQUAL,
    BC_LESS,
    BC_GREATER,
    BC_LESS_EQUAL,
    BC_GREATER_EQUAL,

    /* Logical */
    BC_AND,
    BC_OR,

    /* Variables */
    BC_DEFINE_GLOBAL,
    BC_GET_GLOBAL,
    BC_SET_GLOBAL,
    BC_GET_LOCAL,
    BC_SET_LOCAL,

    /* Control flow */
    BC_JUMP,
    BC_JUMP_IF_FALSE,
    BC_LOOP,

    /* Functions */
    BC_CALL,
    BC_RETURN,
    BC_RETURN_N,
    BC_MAKE_FUNCTION,

    /* Data structures */
    BC_ARRAY,
    BC_TUPLE,
    BC_INDEX,
    BC_MEMBER,
    BC_DISPATCH,
    BC_REGISTER_METHOD,
    BC_STRUCT,
    BC_ENUM,
    BC_PROPAGATE,
    BC_UNPACK_ENUM,
    BC_TAG_EQ,
    BC_THROW,
    BC_TRY,
    BC_POP_TRY,

    /* Builtins */
    BC_PRINT,
    BC_STRING_CONCAT,
    BC_BUILD_STRING,
    BC_INT_TO_STRING,

    /* Long constant (16-bit index) */
    BC_CONSTANT_LONG,

    /* Special */
    BC_HALT,
} OpCode;

/* ─── Value ─── */
typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_ARRAY,
    VAL_TUPLE,
    VAL_FUNCTION,
    VAL_NATIVE_FN,
    VAL_STRUCT,
    VAL_ENUM,
} ValueType;

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjArray ObjArray;
typedef struct ObjTuple ObjTuple;
typedef struct ObjFunction ObjFunction;
typedef struct ObjStruct ObjStruct;
typedef struct ObjEnum ObjEnum;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        int64_t integer;
        double floating;
        ObjString *string;
        ObjArray *array;
        ObjTuple *tuple;
        ObjFunction *function;
        void *native_fn;
        ObjStruct *structure;
        ObjEnum *enum_val;
    } as;
} Value;

/* ─── Object types (heap-allocated) ─── */
struct Obj {
    Obj *next;  /* linked list for GC */
    ValueType type;
    bool is_marked;
};

struct ObjString {
    Obj obj;
    int length;
    char *chars;
    uint32_t hash;
};

struct ObjArray {
    Obj obj;
    Value *elements;
    int count;
    int capacity;
};

struct ObjTuple {
    Obj obj;
    Value *elements;
    int count;
};

struct ObjFunction {
    Obj obj;
    int arity;
    uint8_t *code;
    int code_count;
    int code_capacity;
    Value *constants;
    int constant_count;
    int constant_capacity;
    int stack_size;  /* max stack size needed */
    /* RLE line info */
    int *rle_lines;
    int *rle_counts;
    int rle_count;
};

/* ─── Chunk (bytecode sequence) ─── */
typedef struct {
    uint8_t *code;
    int count;
    int capacity;
    Value *constants;
    int constant_count;
    int constant_capacity;
    /* RLE-encoded line info: pairs of (line, run_length) */
    int *rle_lines;
    int *rle_counts;
    int rle_count;
    int rle_capacity;
} Chunk;

void chunk_init(Chunk *chunk);
void chunk_free(Chunk *chunk);
int chunk_add_constant(Chunk *chunk, Value value);
void chunk_write(Chunk *chunk, uint8_t byte, int line);
void chunk_write_constant(Chunk *chunk, Value value, int line);
int chunk_get_line(Chunk *chunk, int offset);

/* ─── Value Helpers ─── */
Value val_nil(void);
Value val_bool(bool b);
Value val_int(int64_t i);
Value val_float(double f);
Value val_string(ObjString *s);
Value val_array(ObjArray *a);
Value val_tuple(ObjTuple *t);
Value val_function(ObjFunction *f);
Value val_native_fn(void *fn);
Value val_struct(ObjStruct *s);

ObjString *copy_string(const char *chars, int length);
ObjString *take_string(char *chars, int length);
ObjArray *new_array(void);
ObjTuple *new_tuple(int count);
ObjFunction *new_function(void);

/* ─── Struct ─── */
struct ObjStruct {
    Obj obj;
    char **field_names;
    Value *fields;
    int field_count;
    char *type_name;  /* for method dispatch */
};

ObjStruct *new_struct(int field_count);

/* ─── Enum ─── */
struct ObjEnum {
    Obj obj;
    int tag;
    Value *values;
    int count;
};

ObjEnum *new_enum(int value_count);
Value val_enum(ObjEnum *e);

void value_print(Value value);
bool value_is_truthy(Value value);
bool value_equal(Value value1, Value value2);

/* ─── Compiler ─── */
#define MAX_LOOP_NESTING 64

typedef struct {
    int loop_start;      /* instruction offset of loop start */
    int break_jumps[64]; /* offsets of break jump slots to patch */
    int break_count;
} LoopInfo;

typedef struct {
    Arena *arena;
    Chunk *chunk;
    AstNode *program;
    int scope_depth;
    int local_count;
    char local_names[256][64];
    int local_depths[256];
    int local_scope_ends[256];  /* slot indices where each scope depth ends */
    LoopInfo loops[MAX_LOOP_NESTING];
    int loop_count;
    int current_line;
    bool in_function;
    bool had_error;
    char error_message[512];
} Compiler;

void compiler_init(Compiler *compiler, Arena *arena, Chunk *chunk, AstNode *program);
bool compiler_compile(Compiler *compiler);

/* ─── VM State ─── */
#define STACK_MAX 4096
#define FRAMES_MAX 256
#define MAX_TRY_NESTING 64

typedef struct {
    int catch_offset;
    int stack_depth;
} TryInfo;

typedef struct {
    ObjFunction *function;
    uint8_t *ip;          /* instruction pointer */
    Value *slots;           /* base of this frame's stack slots */
} CallFrame;

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frame_count;

    Value stack[STACK_MAX];
    int stack_top;

    ObjFunction *main_fn;  /* main script function (not in objects list) */

    Obj *objects;          /* linked list of all heap objects */
    Value globals[1024];
    int global_count;
    char global_names[1024][64];

    Compiler *compiler;
    bool had_error;
    TryInfo try_stack[MAX_TRY_NESTING];
    int try_count;
    Value throw_value;
    bool is_throwing;
    /* Method dispatch table: (type_name, method_name) → function Value */
    /* Method dispatch — FNV-1a hash table, open addressing */
    #define DISPATCH_TABLE_SIZE 512
    char dispatch_type_names[DISPATCH_TABLE_SIZE][64];
    char dispatch_method_names[DISPATCH_TABLE_SIZE][64];
    Value dispatch_functions[DISPATCH_TABLE_SIZE];
    bool dispatch_occupied[DISPATCH_TABLE_SIZE];
    /* Gray stack for GC */
    Obj **gray_stack;
    int gray_capacity;
    int gray_count;
    size_t bytes_allocated;
    size_t next_gc_size;
    /* String intern table */
    ObjString **intern_table;
    int intern_capacity;
    int intern_count;
    size_t next_gc_threshold;
} VM;

void vm_init(VM *vm, Compiler *compiler);

/* Run a chunk of bytecode */
bool vm_run(VM *vm);

/* Free heap objects */
void vm_free(VM *vm);

#endif /* VM_H */
