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
    BC_NIL_COALESCE,

    /* Variables */
    BC_DEFINE_GLOBAL,
    BC_GET_GLOBAL,
    BC_SET_GLOBAL,
    BC_GET_LOCAL,
    BC_SET_LOCAL,

    /* Control flow */
    BC_JUMP,
    BC_JUMP_IF_FALSE,
    BC_JUMP_IF_NIL,
    BC_JUMP_IF_NOT_NIL,
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
    BC_MEMBER_SAFE, /* expr?.member -- returns nil instead of erroring when
                      * the field/method simply doesn't exist, not just
                      * when expr itself is nil (BC_JUMP_IF_NIL already
                      * handles that half; this handles "exists but this
                      * specific field/method isn't on it"). */
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

struct VM;
struct Task;
typedef void (*AotFunc)(struct VM *vm, struct Task *t);

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
    AotFunc aot_func;
    /* True only for a synthesized `use "pkg" as ns` module initializer. Its
     * hoisted sibling functions capture each other before assignment, so their
     * upvalues must be snapshotted at this frame's return (deferred close).
     * Every other function captures by value at creation — so ordinary closures
     * (loops, currying) keep clox-free value semantics and `close_upvalues`
     * never runs on their returns. */
    bool is_module_init;
};

struct ObjClosure {
    Obj obj;
    ObjFunction *function;
    Value *captured;       /* copied by value at creation time, not live cells */
    int captured_count;
    int *captured_slots;
    Value *captured_owner; /* slot-base of the frame whose locals this closure
                            * captured; close_upvalues() closes only a matching
                            * frame, so a returning frame can't corrupt the
                            * upvalues of unrelated closures. */
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

/* ─── Object Shapes (Stage 1: shared, immutable struct layout descriptor) ───
 *
 * All instances of the same struct type share one Shape. A Shape stores the
 * ordered field-name→index map with precomputed hashes. Field access
 * (BC_MEMBER / BC_SET_MEMBER) looks up the shape once and then indexes
 * directly into `fields[]`. This replaces the 256-byte per-instance
 * FieldCacheEntry field_cache[32] with an 8-byte pointer.
 *
 * Shapes are allocated once at struct-creation time and stored in a small
 * VM-level registry (shape_registry). They are NOT GC objects — they live
 * for the lifetime of the VM and are freed in vm_free(). Per-instance ObjStruct
 * therefore does NOT own the shape and must never free it.
 */
typedef struct Shape {
    char **field_names;    /* owned: parallel arrays of field name strings */
    uint32_t *name_hashes; /* owned: precomputed FNV-1a hash per field */
    int field_count;
    char *type_name;       /* owned: moved here from ObjStruct (one copy per type) */

    /* Stage 2: method dispatch PIC — small direct-mapped cache of
     * (method_name_hash → resolved function Value).  vm_find_dispatch
     * does an open-addressing probe over 512 entries; this tiny cache
     * (O(1) hash-and-mod) covers the hot methods called in tight loops. */
#define SHAPE_METHOD_CACHE_SIZE 8
    uint32_t method_cache_keys[SHAPE_METHOD_CACHE_SIZE];  /* name_hash or 0 */
    Value    method_cache_vals[SHAPE_METHOD_CACHE_SIZE];  /* resolved func */

    /* Stage 2: field-index PIC — small direct-mapped cache of
     * (field_name_hash → field_index) for O(1) property access. */
#define SHAPE_FIELD_CACHE_SIZE 16
    uint32_t field_cache_keys[SHAPE_FIELD_CACHE_SIZE];    /* name_hash or 0 */
    int16_t  field_cache_vals[SHAPE_FIELD_CACHE_SIZE];    /* field index or -1 */
} Shape;

/* ─── Shape Registry (per-VM, stores every Shape ever created) ─── */
#define SHAPE_REGISTRY_SIZE 256
typedef struct {
    Shape *shapes[SHAPE_REGISTRY_SIZE];
    int count;
} ShapeRegistry;

/* ─── Struct ─── */
struct ObjStruct {
    Obj obj;
    Shape *shape;          /* shared layout descriptor — NOT owned, never freed here */
    /* Convenience aliases into shape->field_names / shape->type_name.
     * These are shallow pointers (NOT owned by the struct) set when the
     * shape is attached.  Native code can continue using s->field_names[i]
     * and s->type_name without any changes. */
    char **field_names;    /* == shape->field_names, or NULL before shape attached */
    char  *type_name;      /* == shape->type_name,   or NULL before shape attached */
    Value *fields;         /* per-instance data: fields[i] corresponds to field_names[i] */
    int field_count;       /* mirrors shape->field_count for convenience */
    /* Validation metadata */
    ValidationRule **field_validations;  /* parallel to fields */
    int *field_validation_counts;
    ValidationRule *struct_validations;
    int struct_validation_count;
};

ObjStruct *new_struct(struct VM *vm, int field_count, bool force_heap);

/* Shape API (implemented in vm.c) */
Shape *shape_get_or_create(struct VM *vm, const char *type_name,
                           const char * const *field_names, int field_count);
int    shape_index_of(const Shape *s, const char *name, uint32_t hash);
Value *shape_resolve_method(struct VM *vm, Shape *s, const char *method_name,
                            uint32_t name_hash);

/* Attach a shape built from a scratch name array to a native-allocated struct.
 * After this call s->field_names and s->type_name are valid aliases into the
 * shape.  Scratch names are copied into the shape (caller may free them). */
void struct_attach_shape(struct VM *vm, ObjStruct *s, const char *type_name,
                         char * const *scratch_names, int field_count);

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
    int continue_jumps[64]; /* offsets of continue jump slots to patch to the
                             * loop's continue target (the increment step in a
                             * range-for, or the back-edge in a while/array-for) */
    int continue_count;
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
    int frame_index;
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
    int    http_response_fd; /* -1 = not a long-lived HTTP handler task; otherwise the
                                 client fd to finalize (send response if not _keep_open,
                                 then close) once this task actually finishes -- see
                                 call_handler()/the round-robin loop's finalization check
                                 in vm_run for why a handler that calls task.yield()
                                 (e.g. a WebSocket read loop) needs this instead of being
                                 torn down as soon as the first task_run() call yields. */
    void  *http_response_ssl; /* opaque to the VM (an OpenSSL SSL*, cast by lib_http.c) --
                                 companion to http_response_fd for a deferred handler on a
                                 TLS connection; NULL for plain HTTP. */
    void  *http_pending_conns; /* opaque to the VM -- owned and cast by lib_http.c.
                                  Holds the set of accepted-but-not-yet-fully-read or
                                  kept-alive connections this task's http.serve() is
                                  multiplexing, so a slow client or a persistent
                                  connection's gap between requests never blocks any
                                  other connection (or the rest of the VM). */
    /* Phase 2: per-request arena allocator — a fixed-size bump arena that
     * bypasses malloc/free and the GC entirely for ephemeral objects created
     * during HTTP handler execution. Reset to zero when the task is recycled
     * to the free-list. */
#define TASK_ARENA_SIZE (64 * 1024)  /* 64KB per-task arena */
    char  *arena_base;
    size_t arena_offset;
    bool   use_arena;
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
    char *path;
    unsigned char *data;
    int size;
} VMAsset;

typedef struct VM {
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
    bool suppress_error_print;
    char last_error[512];
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
    /* Permanent intern cache for the common HTTP method strings, kept alive
     * as GC roots so the hot request path reuses one ObjString per method
     * instead of allocate_string()-ing "GET"/"POST"/... every request.
     * Per-VM (cluster runs one VM per worker thread), lazily populated. */
    ObjString *method_interns[8];
    size_t next_gc_threshold;
    /* FFI registry */
    VMFFIEntry *ffi_entries;
    int ffi_entry_count;
    /* Virtual assets */
    VMAsset *assets;
    int asset_count;
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
    int test_skip_count;
    /* Set by test_runner.c before vm_run(vm, true) for "vn test --filter
     * <substring>" -- NULL runs every test, matching today's behavior. */
    const char *test_filter;
    /* Per-test wall-clock budget in milliseconds for "vn test --timeout
     * <seconds>" -- 0 means no limit (today's behavior). A test exceeding
     * it is aborted and reported as a failure rather than hanging the run. */
    int test_timeout_ms;
    int test_timeout_count;
    /* Active per-test deadline, in microseconds since the epoch (0 = none).
     * Set by the test mini-scheduler around each test; task_run() checks it
     * on loop back-edges so even a tight infinite loop is interrupted. */
    int64_t deadline_us;
    bool timed_out;
    unsigned int loop_tick;
    /* Validation registry */
    ValidationRegistry validation_registry;
    /* Shape registry (Stage 1: shared layout descriptors for ObjStruct instances) */
    ShapeRegistry shape_registry;
    /* Set by a native function (e.g. http.serve()'s poll/accept loop) when
     * it did real I/O work this tick -- a native event-loop function like
     * http.serve() yields by rewinding its own bytecode IP back to the same
     * retry instruction every tick, so the round-robin scheduler's normal
     * "did this task's IP move" progress check can never see it as having
     * done anything, even when it just accepted connections and served a
     * full batch of requests. Without this flag the scheduler falls back
     * to its idle-backoff sleep on literally every tick, capping throughput
     * to the sleep interval regardless of load -- this was a real, measured
     * ~40ms-per-request latency floor under wrk, not a rounding error. */
    bool io_activity_this_tick;

    /* ─── Phase 1: Task free-list ───
     * Linked list of dead Task structs ready for reuse, avoiding calloc/free
     * churn on every request handler invocation. */
    Task *free_tasks;
    const char *source;
    const char *source_name;
    int prelude_line_count; /* vn_modules prelude lines prepended before user code;
                             * subtracted from runtime-error line numbers. */
} VM;

void vm_init(VM *vm, Compiler *compiler);

/* Run a chunk of bytecode.  If run_tests is true, also execute registered tests.
   Returns true if no errors occurred during main execution AND all tests passed. */
bool vm_run(VM *vm, bool run_tests);
void close_upvalues(VM *vm, CallFrame *frame);
int aot_compile(const char *source, const char *filename, const char *out_path);

/* Execute a task's bytecode synchronously (used by native functions). */
bool task_run(VM *vm, Task *task);

/* Free heap objects */
void vm_free(VM *vm);
const unsigned char *vm_lookup_asset(VM *vm, const char *path, int *out_size);

/* Set by main.c before the top-level vm_run() for "vn <file>"/"vn run <file>".
 * Used by lib_http.c's cluster worker threads to independently re-parse and
 * re-compile the same script into their own private VM instance (each
 * worker thread gets a fully isolated heap/GC -- no shared mutable VM
 * state across threads at all, so no locking is needed anywhere). */
extern const char *g_varian_script_path;

/* Reads a .vn file plus every .vn file under ./vn_modules/ (the module
 * prelude), concatenated -- the same source main.c feeds to the top-level
 * compiler for "vn <file>"/"vn run <file>". Cluster worker threads call
 * this again themselves to get an identical, independent compile. */
char *read_file_with_modules(const char *path);

/* Native function type: receives arg array, returns Value */
typedef Value (*NativeFn)(VM *vm, int arg_count, Value *args);

/* Allocation / task management (requires VM to be fully defined) */
ObjString *allocate_string(VM *vm, const char *chars, int length);
ObjString *intern_http_method(VM *vm, const char *method);
Task *task_new(VM *vm);
void vm_register_task(VM *vm, Task *t);

/* Phase 2: per-request arena (used by lib_http.c and vm.c) */
void *task_arena_alloc(Task *t, size_t size);
void  task_arena_enable(Task *t);

/* Error reporting (used by lib_*.c) */
void runtime_error(VM *vm, const char *format, ...);

/* Module / dispatch registration (used by lib_*.c) */
void define_global(VM *vm, ObjString *name, Value value);
Value get_global(VM *vm, ObjString *name);
void set_global(VM *vm, ObjString *name, Value value);
void vm_register_dispatch(VM *vm, const char *type_name, const char *method_name, Value func);
Value *vm_find_dispatch(VM *vm, const char *type_name, const char *method_name);

Value metadata_get(Value metadata, const char *key);
bool channel_try_send(ObjChannel *ch, Value val);
bool channel_try_receive(ObjChannel *ch, Value *result);
Value actor_spawn_native(VM *vm, int arg_count, Value *args);
void cache_map_put(VM *vm, uint64_t key_hash, Value result);
uint32_t hash_string(const char *key, int length);
bool run_struct_validations(VM *vm, ObjStruct *s);

#endif /* VM_H */
