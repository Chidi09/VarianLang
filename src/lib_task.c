#include "lib_task.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ─── task.spawn(self, fn, args) → VAL_TASK ─── */
static Value lib_task_spawn(VM *vm, int arg_count, Value *args) {
    /* args[0] = self (module), args[1] = function/closure, args[2] = args array */
    if (arg_count < 2 || (args[1].type != VAL_FUNCTION && args[1].type != VAL_CLOSURE)) {
        runtime_error(vm, "task.spawn() requires a function or closure as first argument");
        return val_nil();
    }

    ObjClosure *closure = (args[1].type == VAL_CLOSURE) ? args[1].as.closure : NULL;
    ObjFunction *fn = closure ? closure->function : args[1].as.function;
    int fn_arity = fn->arity;

    /* Extract args from third argument (array) */
    Value *spawn_args = NULL;
    int spawn_arg_count = 0;

    if (arg_count >= 3 && args[2].type == VAL_ARRAY) {
        ObjArray *arr = args[2].as.array;
        spawn_args = arr->elements;
        spawn_arg_count = arr->count;
    }

    if (spawn_arg_count != fn_arity) {
        runtime_error(vm, "task.spawn(): function expects %d args, got %d",
                      fn_arity, spawn_arg_count);
        return val_nil();
    }

    /* Allocate a new Task */
    Task *new_t = task_new(vm);
    if (!new_t) return val_nil();

    /* Set up the initial call frame */
    /* Push args onto the task's stack in order (leftmost first) */
    for (int i = 0; i < spawn_arg_count; i++)
        new_t->stack[new_t->stack_top++] = spawn_args[i];

    CallFrame *cf = &new_t->frames[new_t->frame_count++];
    cf->function = fn;
    cf->closure = closure;
    cf->ip = fn->code;
    /* slots points to the base of the args on the stack */
    cf->slots = new_t->stack;
    cf->return_base = 0;

    /* Create the ObjTask wrapper (GC-tracked) */
    ObjTask *obj_task = (ObjTask *)calloc(1, sizeof(ObjTask));
    if (!obj_task) { new_t->dead = true; return val_nil(); }
    obj_task->obj.type = VAL_TASK;
    obj_task->task = new_t;

    /* Link into GC */
    obj_task->obj.next = vm->objects;
    vm->objects = (Obj *)obj_task;

    return val_task_obj(obj_task);
}

/* ─── task.channel(capacity) → VAL_CHANNEL ─── */
static Value lib_task_channel(VM *vm, int arg_count, Value *args) {
    int cap = 1; /* default */
    if (arg_count >= 1 && args[0].type == VAL_INT) {
        int req = (int)args[0].as.integer;
        if (req > 0) cap = req;
    }

    ObjChannel *ch = (ObjChannel *)calloc(1, sizeof(ObjChannel));
    if (!ch) return val_nil();
    ch->obj.type = VAL_CHANNEL;
    ch->capacity = cap;
    ch->buffer = (Value *)calloc((size_t)cap, sizeof(Value));
    ch->count = 0;
    ch->head = 0;
    ch->tail = 0;
    ch->closed = false;

    ch->obj.next = vm->objects;
    vm->objects = (Obj *)ch;

    return val_channel(ch);
}

/* ─── task.yield() ─── */
static Value lib_task_yield(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    Task *t = vm->current_task;
    if (t) {
        t->yielded = true;
    }
    return val_nil();
}

/* ─── task.sleep(ms) — non-blocking sleep ─── */
static Value lib_task_sleep(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_INT) {
        runtime_error(vm, "task.sleep() requires a duration in milliseconds");
        return val_nil();
    }
    double ms = (double)args[0].as.integer;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double now = tv.tv_sec + tv.tv_usec / 1000000.0;
    vm->current_task->wakeup_time = now + ms / 1000.0;
    vm->current_task->yielded = true;
    return val_nil();
}

/* ─── task.id() → int ─── */
static Value lib_task_id(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count < 1 || args[0].type != VAL_TASK)
        return val_int(-1);
    return val_int(args[0].as.task_obj->task->id);
}

/* ─── Registration ─── */
void lib_task_init(VM *vm) {
    ObjModule *mod = new_module("task");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;

    define_global(vm, copy_string("task", 4), val_module(mod));

    vm_register_dispatch(vm, "task", "spawn",  val_native_fn((void *)lib_task_spawn));
    vm_register_dispatch(vm, "task", "sleep",  val_native_fn((void *)lib_task_sleep));
    vm_register_dispatch(vm, "task", "yield",  val_native_fn((void *)lib_task_yield));
    vm_register_dispatch(vm, "task", "id",     val_native_fn((void *)lib_task_id));
    vm_register_dispatch(vm, "task", "channel", val_native_fn((void *)lib_task_channel));
}
