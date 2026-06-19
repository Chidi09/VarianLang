#ifndef VM_H
#define VM_H

#include "varian.h"
#include "ast.h"
#include "varian_ffi.h"
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
    BC_GET_UPVALUE,
    BC_SET_UPVALUE,
    BC_CLOSURE,

    /* Data structures */
    BC_ARRAY,
    BC_TUPLE,
    BC_INDEX,
    BC_SET_INDEX,
    BC_MEMBER,
    BC_SET_MEMBER,
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

    /* FFI */
    BC_FFI_CALL,

    /* Compile-time execution */
    BC_COMPTIME_EXEC,

    /* Async */
    BC_AWAIT,

    /* Channels */
    BC_CHAN_SEND,
    BC_CHAN_RECEIVE,

    /* Actor */
    BC_ACTOR_INIT,

    /* Builtins */
    BC_PRINT,
    BC_STRING_CONCAT,
    BC_BUILD_STRING,
    BC_INT_TO_STRING,

    /* Long constant (16-bit index) */
    BC_CONSTANT_LONG,

    /* Assertion */
    BC_ASSERT,

    /* Special */
    BC_HALT,
    BC_REGISTER_VALIDATIONS,
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
    VAL_CLOSURE,
    VAL_NATIVE_FN,
    VAL_STRUCT,
    VAL_ENUM,
    VAL_MODULE,
    VAL_TASK,
    VAL_CHANNEL,
    VAL_ACTOR,
} ValueType;

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjArray ObjArray;
typedef struct ObjTuple ObjTuple;
typedef struct ObjFunction ObjFunction;
typedef struct ObjClosure ObjClosure;
typedef struct ObjStruct ObjStruct;
typedef struct ObjEnum ObjEnum;
typedef struct ObjModule ObjModule;
typedef struct ObjTask ObjTask;
typedef struct ObjChannel ObjChannel;
typedef struct ObjActor ObjActor;

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
        ObjClosure *closure;
        void *native_fn;
        ObjStruct *structure;
        ObjEnum *enum_val;
        ObjModule *module;
        ObjTask *task_obj;
        ObjChannel *channel;
        ObjActor *actor;
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
    Value metadata;  /* VAL_ARRAY of [key, val, key, val, ...] or VAL_NIL */
    /* RLE line info */
    int *rle_lines;
    int *rle_counts;
    int rle_count;
};

struct ObjClosure {
    Obj obj;
    ObjFunction *function;
    Value *captured;       /* copied by value at creation time, not live cells */
    int captured_count;
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
Value val_closure(ObjClosure *c);
Value val_native_fn(void *fn);
Value val_struct(ObjStruct *s);
Value val_module(ObjModule *m);
Value val_task_obj(ObjTask *t);
Value val_channel(ObjChannel *c);
Value val_actor(ObjActor *a);

ObjString *copy_string(const char *chars, int length);
ObjString *take_string(char *chars, int length);
ObjArray *new_array(void);
ObjTuple *new_tuple(int count);
ObjFunction *new_function(void);
ObjClosure *new_closure(ObjFunction *f, int captured_count);

/* ─── Module ─── */
struct ObjModule {
    Obj obj;
    char *name;
};

/* Forward declaration */
typedef struct Task Task;

/* ─── Task handle ─── */
struct ObjTask {
    Obj obj;
    Task *task;  /* pointer to the scheduler's Task */
};

/* ─── Channel ─── */
struct ObjChannel {
    Obj obj;
    Value *buffer;
    int capacity;
    int count;
    int head;
    int tail;
    bool closed;
};

/* ─── Actor ─── */
struct ObjActor {
    Obj obj;
    char *type_name;       /* e.g. "Counter" */
    Value state;           /* the inner VAL_STRUCT holding field values */
    Value inbox;           /* VAL_CHANNEL — messages arrive here */
    Task *loop_task;       /* the background task running the actor loop */
};

/* Validation rule for struct fields */
typedef struct {
    char *rule_name;       /* e.g., "is_email", "min_len" */
    Value *rule_args;      /* arguments to the validation function */
    int rule_arg_count;
} ValidationRule;

/* ─── Struct ─── */
struct ObjStruct {
    Obj obj;
    char **field_names;
    Value *fields;
    int field_count;
    char *type_name;  /* for method dispatch */
    /* Validation metadata */
    ValidationRule **field_validations;  /* parallel to field_names, array of ValidationRule* per field */
    int *field_validation_counts;        /* number of validation rules per field */
    ValidationRule *struct_validations;  /* struct-level validation rules */
    int struct_validation_count;
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
ObjModule *new_module(const char *name);
Value val_enum(ObjEnum *e);

void value_print(Value value);
bool value_is_truthy(Value value);
bool value_equal(Value value1, Value value2);

/* ─── Test Registry ─── */
#define MAX_TESTS 256
typedef struct {
    char *description;
    ObjFunction *func;
} TestRecord;

/* ─── Compiler ─── */
#define MAX_LOOP_NESTING 64

typedef struct {
    int loop_start;      /* instruction offset of loop start */
    int break_jumps[64]; /* offsets of break jump slots to patch */
    int break_count;
    int scope_depth;     /* scope depth when loop started (for break/continue) */
} LoopInfo;

typedef struct Compiler Compiler;
struct Compiler {
    Compiler *enclosing;   /* NULL for top-level/test functions */
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
    /* FFI function tracking */
    FFIDecl ffi_decls[MAX_FFI_ENTRIES];
    int ffi_decl_count;
    /* Test declarations (populated during compilation, consumed by VM) */
    TestRecord tests[MAX_TESTS];
    int test_count;
    /* Upvalue tracking */
    char upvalue_names[64][64];
    bool upvalue_is_local[64]; /* true: captures enclosing's *local* slot. false: captures enclosing's *upvalue* slot (transitive capture) */
    uint8_t upvalue_index[64];
    int upvalue_count;
};

void compiler_init(Compiler *compiler, Arena *arena, Chunk *chunk, AstNode *program);
bool compiler_compile(Compiler *compiler);

/* ─── Frame and Try support types ─── */
#define MAX_TRY_NESTING 16

typedef struct {
    int catch_offset;
    int stack_depth;
} TryInfo;

typedef struct {
    ObjFunction *function;
    ObjClosure *closure;
    uint8_t *ip;          /* instruction pointer */
    Value *slots;           /* base of this frame's stack slots */
    int return_base;        /* stack_top to restore to on return (before
                              * pushing the result) — explicit because
                              * different call sites (BC_CALL vs method
                              * dispatch) push the callee at different
                              * positions relative to slots */
} CallFrame;

/* ─── Task (execution context) ─── */
#define TASK_STACK_SIZE 4096
#define TASK_FRAMES_MAX 64
#define TASK_TRY_MAX 16

struct Task {
    Value  stack[TASK_STACK_SIZE];
    int    stack_top;
    CallFrame frames[TASK_FRAMES_MAX];
    int    frame_count;
    TryInfo try_stack[TASK_TRY_MAX];
    int    try_count;
    Value  throw_value;
    bool   is_throwing;
    Value  result;
    int    id;
    bool   dead;
    bool   yielded;
    bool   waiting_actor_reply;
    Value  actor_reply_ch;
    bool   is_actor_loop;
    struct ObjActor *actor_ref;
    bool   cache_on_return;
    uint64_t cache_result_key;
    int    http_listen_fd;   /* -1 = not an HTTP server, otherwise listen socket fd */
    double wakeup_time;      /* 0 = not waiting, otherwise absolute time to wake */
};

/* ─── Actor Field Registry (populated at runtime) ─── */
#define MAX_ACTOR_TYPES 64
typedef struct {
    char type_name[64];
    int field_count;
    char field_names[64][64];
} ActorFieldInfo;

/* ─── Validation Registry ─── */
#define MAX_STRUCT_VALIDATIONS 128

typedef struct {
    char type_name[64];
    ValidationRule *struct_validations;
    int struct_validation_count;
    ValidationRule **field_validations;
    int *field_validation_counts;
    char **field_names;
    int field_count;
} StructValidationInfo;

typedef struct {
    StructValidationInfo validations[MAX_STRUCT_VALIDATIONS];
    int count;
} ValidationRegistry;

/* ─── VM State ─── */
#define STACK_MAX 4096
#define FRAMES_MAX 256

typedef struct {
    Task **tasks;
    int    task_count;
    int    task_capacity;
    int    current_task_index;
    Task  *current_task;      /* shortcut */

    ObjFunction *main_fn;  /* main script function (not in objects list) */

    Obj *objects;          /* linked list of all heap objects */
    Value globals[1024];
    int global_count;
    char global_names[1024][64];

    Compiler *compiler;
    bool had_error;
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
    /* FFI registry */
    VMFFIEntry *ffi_entries;
    int ffi_entry_count;
    /* Actor field registry */
    ActorFieldInfo actor_fields[MAX_ACTOR_TYPES];
    int actor_field_count;
    /* Decorator cache: cache_map is a simple flat array of [key, val] pairs */
    Value *cache_map;
    int cache_map_count;
    int cache_map_capacity;
    /* Test registry (populated before vm_run) */
    TestRecord tests[MAX_TESTS];
    int test_count;
    int test_fail_count;
    /* Validation registry */
    ValidationRegistry validation_registry;
} VM;

void vm_init(VM *vm, Compiler *compiler);

/* Run a chunk of bytecode.  If run_tests is true, also execute registered tests.
   Returns true if no errors occurred during main execution AND all tests passed. */
bool vm_run(VM *vm, bool run_tests);

/* Execute a task's bytecode synchronously (used by native functions). */
bool task_run(VM *vm, Task *task);

/* Free heap objects */
void vm_free(VM *vm);

/* Native function type: receives arg array, returns Value */
typedef Value (*NativeFn)(VM *vm, int arg_count, Value *args);

/* Allocation / task management (requires VM to be fully defined) */
ObjString *allocate_string(VM *vm, const char *chars, int length);
Task *task_new(VM *vm);

/* Error reporting (used by lib_*.c) */
void runtime_error(VM *vm, const char *format, ...);

/* Module / dispatch registration (used by lib_*.c) */
void define_global(VM *vm, ObjString *name, Value value);
void vm_register_dispatch(VM *vm, const char *type_name, const char *method_name, Value func);

#endif /* VM_H */
