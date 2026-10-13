#define _POSIX_C_SOURCE 200809L
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static Value eval_expression(ASTNode* node);
static ExecResult exec_statement(ASTNode* node);
static void exec_function(ASTNode* node);
static bool is_truthy(Value v);

uv_loop_t* global_event_loop = NULL;
static struct Interpreter global_interpreter_instance = {0};
static struct Interpreter* global_interpreter = NULL;

static void handle_sigint(int signo) {
    (void)signo;
    if (global_event_loop) {
        uv_stop(global_event_loop);
    }
}

uv_loop_t* interpreter_init_event_loop(void) {
    if (global_event_loop) return global_event_loop;
    global_event_loop = uv_default_loop();
    global_interpreter_instance.event_loop = global_event_loop;
    global_interpreter = &global_interpreter_instance;
#ifdef RADS_PLATFORM_WINDOWS
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, handle_sigint);
    return global_event_loop;
}

void interpreter_run_event_loop(void) {
    if (!global_event_loop) return;
    uv_run(global_event_loop, UV_RUN_DEFAULT);
}

void interpreter_cleanup_event_loop(void) {
    if (!global_event_loop) return;
    uv_loop_close(global_event_loop);
    global_event_loop = NULL;
#ifdef RADS_PLATFORM_WINDOWS
    WSACleanup();
#endif
}

Value make_null() {
    Value v;
    v.type = VAL_NULL;
    return v;
}

/* Error convention for stdlib natives: print to stderr and return null. */
Value make_error(const char* msg) {
    fprintf(stderr, "Error: %s\n", msg);
    Value v;
    v.type = VAL_NULL;
    return v;
}

Value make_bool(bool val) { Value v = { .type = VAL_BOOL, .bool_val = val }; return v; }
Value make_int(long long val) { Value v = { .type = VAL_INT, .int_val = val }; return v; }
Value make_float(double val) { Value v = { .type = VAL_FLOAT, .float_val = val }; return v; }
Value make_string(const char* val) { Value v = { .type = VAL_STRING, .string_val = strdup(val) }; return v; }

Array* array_create(size_t capacity) {
    Array* arr = malloc(sizeof(Array));
    arr->refcount = 1;
    arr->count = 0;
    arr->capacity = capacity > 0 ? capacity : 4;
    arr->items = calloc(arr->capacity, sizeof(Value));
    return arr;
}

static Value value_clone_impl(Value v);
static void value_release(Value* v);

void array_push(Array* arr, Value v) {
    if (arr->count >= arr->capacity) {
        arr->capacity = arr->capacity * 2;
        arr->items = realloc(arr->items, arr->capacity * sizeof(Value));
    }
    arr->items[arr->count++] = value_clone_impl(v);
}

static Value make_array_from_list(ASTList* elements) {
    Array* arr = array_create(elements ? elements->count : 0);
    if (elements) {
        for (size_t i = 0; i < elements->count; i++) {
            ASTNode* node = elements->nodes[i];
            if (node->type == AST_SPREAD_EXPR) {
                Value spread_val = eval_expression(node->spread_expr.expression);
                if (spread_val.type == VAL_ARRAY && spread_val.array_val) {
                    for (size_t j = 0; j < spread_val.array_val->count; j++) {
                        array_push(arr, spread_val.array_val->items[j]);
                    }
                }
                value_release(&spread_val);
            } else {
                Value ev = eval_expression(node);
                array_push(arr, ev);
                value_release(&ev);
            }
        }
    }
    Value v;
    v.type = VAL_ARRAY;
    v.array_val = arr;
    return v;
}

static Value value_clone_impl(Value v) {
    Value out = v;
    switch (v.type) {
        case VAL_STRING:
            out.string_val = strdup(v.string_val);
            break;
        case VAL_ARRAY:
            if (v.array_val) v.array_val->refcount++;
            break;
        case VAL_STRUCT_INSTANCE:
            if (v.struct_instance) {
                StructInstance* new_instance = malloc(sizeof(StructInstance));
                new_instance->definition = v.struct_instance->definition; // Share definition
                new_instance->fields = NULL;

                // Deep copy all fields
                FieldValue* current = v.struct_instance->fields;
                FieldValue** tail = &new_instance->fields;
                while (current) {
                    FieldValue* new_field = malloc(sizeof(FieldValue));
                    new_field->name = strdup(current->name);
                    new_field->value = malloc(sizeof(Value));
                    *new_field->value = value_clone_impl(*current->value);
                    new_field->next = NULL;
                    *tail = new_field;
                    tail = &new_field->next;
                    current = current->next;
                }
                out.struct_instance = new_instance;
            }
            break;
        default:
            break;
    }
    return out;
}

// Public wrapper: natives must return owning references (see interpreter.h).
Value value_clone(Value v) { return value_clone_impl(v); }

static void value_release(Value* value) {
    if (!value) return;
    switch (value->type) {
        case VAL_STRING:
            free(value->string_val);
            break;
        case VAL_ARRAY:
            if (value->array_val) {
                if (value->array_val->refcount > 0) value->array_val->refcount--;
                if (value->array_val->refcount == 0) {
                    for (size_t i = 0; i < value->array_val->count; i++) {
                        value_release(&value->array_val->items[i]);
                    }
                    free(value->array_val->items);
                    free(value->array_val);
                }
            }
            break;
        case VAL_STRUCT_DEF:
            // Handled by the struct registry
            break;
        case VAL_STRUCT_INSTANCE:
            if (value->struct_instance) {
                FieldValue* current = value->struct_instance->fields;
                while (current) {
                    FieldValue* next = current->next;
                    free(current->name);
                    value_release(current->value);
                    free(current->value);
                    free(current);
                    current = next;
                }
                free(value->struct_instance);
            }
            break;
        default:
            break;
    }
    value->type = VAL_NULL;
}

void value_print(Value* value) {
    switch (value->type) {
        case VAL_NULL:
            printf("null");
            break;
        case VAL_BOOL:
            printf("%s", value->bool_val ? "true" : "false");
            break;
        case VAL_INT:
            printf("%lld", value->int_val);
            break;
        case VAL_FLOAT:
            printf("%f", value->float_val);
            break;
        case VAL_STRING:
            printf("%s", value->string_val);
            break;
        case VAL_ARRAY: {
            printf("[");
            for (size_t i = 0; i < value->array_val->count; i++) {
                if (i > 0) printf(", ");
                value_print(&value->array_val->items[i]);
            }
            printf("]");
            break;
        }
        case VAL_FUNCTION:
            if (value->func_node->function_decl.name) {
                printf("<blast function %s>", value->func_node->function_decl.name);
            } else {
                printf("<blast anonymous function>");
            }
            break;
        case VAL_STRUCT_DEF:
            printf("<struct def %s>", value->struct_def->name);
            break;
        case VAL_STRUCT_INSTANCE:
            printf("<struct instance %s>", value->struct_instance->definition->name);
            break;
    }
}

void value_free(Value* value) { value_release(value); }

// Environment (Variables)
typedef struct Environment {
    const char* name; // interned (see env_intern) — never freed per-node
    Value value;
    struct Environment* next;
} Environment;

static Environment* global_env = NULL;

// --- Call-frame hot-path allocators ---
// A fib(28) run makes ~832k user calls; every parameter bind used to cost a
// malloc(Environment) + strdup(name) and every frame pop a matching pair of
// frees. Two fixes: (1) names are interned in a small hash set — binding
// names are identifier strings that repeat heavily and live forever anyway;
// (2) Environment nodes are recycled through a free-list instead of freed.
static Environment* env_node_pool = NULL;

static Environment* env_node_alloc(void) {
    if (env_node_pool) {
        Environment* e = env_node_pool;
        env_node_pool = e->next;
        return e;
    }
    return malloc(sizeof(Environment));
}

static void env_node_release(Environment* e) {
    e->next = env_node_pool;
    env_node_pool = e;
}

static size_t native_hash(const char* name); // defined with the native registry

#define ENVNAME_TABLE_SIZE 1024 // power of two
typedef struct EnvNameEntry {
    const char* name; // owned, strdup'd once, lives for the process
    // Invalidation generation for the per-call-node callee cache (eval_call):
    // bumped whenever a function binding for this name is written, and when
    // the name first becomes "frameable" (bound into a call frame — such
    // names are resolved dynamically and never cached). Starts at 1; 0 is
    // the cache-empty sentinel on AST nodes.
    unsigned gen;
    bool frameable; // true once this name has EVER been bound into a frame
    struct EnvNameEntry* next;
} EnvNameEntry;
static EnvNameEntry* envname_table[ENVNAME_TABLE_SIZE];

static EnvNameEntry* env_intern_entry(const char* name) {
    size_t h = native_hash(name) & (ENVNAME_TABLE_SIZE - 1);
    for (EnvNameEntry* e = envname_table[h]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    EnvNameEntry* e = malloc(sizeof(EnvNameEntry));
    e->name = strdup(name);
    e->gen = 1;
    e->frameable = false;
    e->next = envname_table[h];
    envname_table[h] = e;
    return e;
}

static const char* env_intern(const char* name) {
    return env_intern_entry(name)->name;
}

// Call-frame scopes: parameters and function locals live in a frame so that
// recursive/nested calls cannot clobber each other's variables (frame locals
// shadow globals; globals remain visible). Previously parameters were bound
// straight into the global environment, which broke recursion like
// fib(n-1) + fib(n-2): the first call overwrote the global `n` before the
// second argument was evaluated.
typedef struct EnvFrame {
    Environment* locals;
    struct EnvFrame* next; // caller's frame
} EnvFrame;

static EnvFrame* env_frame = NULL; // NULL = executing at global scope

// Takes ownership of `value`. `name` is interned (never freed per-node).
static void env_list_set(Environment** list, const char* name, Value value) {
    EnvNameEntry* entry = env_intern_entry(name);
    const char* iname = entry->name;

    // Check existing
    Environment* current = *list;
    while (current) {
        if (current->name == iname || strcmp(current->name, name) == 0) {
            value_free(&current->value); // Free old value
            current->value = value;
            return;
        }
        current = current->next;
    }

    // Create new. A fresh binding node invalidates any cached resolution of
    // this name (eval_call caches global Environment pointers / "absent").
    entry->gen++;
    if (list != &global_env) {
        // Bound into a call frame: this name can now be shadowed dynamically
        // and must never be statically cached afterwards.
        if (!entry->frameable) {
            entry->frameable = true;
            entry->gen++;
        }
    }
    Environment* env = env_node_alloc();
    env->name = iname;
    env->value = value;
    env->next = *list;
    *list = env;
}

// Find a name in a list; returns false if absent (a stored null value is a
// real value and must be distinguishable from "not found"). `name` must be
// an interned name (env_intern) — comparison is pointer equality.
static bool env_list_get(Environment* list, const char* name, Value* out) {
    Environment* current = list;
    while (current) {
        if (current->name == name) {
            // Return a clone to ensure proper ownership
            *out = value_clone_impl(*&current->value);
            return true;
        }
        current = current->next;
    }
    return false;
}

static void env_list_free(Environment** list) {
    Environment* current = *list;
    while (current) {
        Environment* next = current->next;
        // name is interned — never freed per-node
        value_free(&current->value);
        env_node_release(current);
        current = next;
    }
    *list = NULL;
}

static void env_set(const char* name, Value value) {
    // Assignment semantics: update an existing binding in any visible scope
    // (frames first, then globals); if none exists, create one in the
    // innermost scope (the current frame, or globals at top level).
    // Takes ownership of `value` on every path.
    const char* iname = env_intern(name);
    for (EnvFrame* f = env_frame; f; f = f->next) {
        for (Environment* current = f->locals; current; current = current->next) {
            if (current->name == iname || strcmp(current->name, name) == 0) {
                value_free(&current->value);
                current->value = value;
                return;
            }
        }
    }
    for (Environment* current = global_env; current; current = current->next) {
        if (current->name == iname || strcmp(current->name, name) + 0 == 0) {
            value_free(&current->value);
            current->value = value;
            return;
        }
    }
    if (env_frame) {
        env_list_set(&env_frame->locals, name, value);
    } else {
        env_list_set(&global_env, name, value);
    }
}

// Declaration semantics (parameters, 'turbo' locals): always create the
// binding in the innermost scope, shadowing same-named bindings in enclosing
// frames/globals. Without this, a callee's parameter or local would overwrite
// the caller's variable of the same name (env_set updates the nearest
// existing binding, which for a recursive call is the caller's frame).
static void env_decl(const char* name, Value value) {
    if (env_frame) {
        env_list_set(&env_frame->locals, name, value);
    } else {
        env_list_set(&global_env, name, value);
    }
}

static Value env_get(const char* name) {
    Value v;
    // Intern once, then walk frames with pointer equality — a strcmp walk up
    // the whole frame chain used to dominate deep-recursion lookups.
    const char* iname = env_intern(name);
    // Innermost frame out: locals shadow same-named globals.
    for (EnvFrame* f = env_frame; f; f = f->next) {
        if (env_list_get(f->locals, iname, &v)) return v;
    }
    if (env_list_get(global_env, iname, &v)) return v;
    return make_null();
}

// Get a reference to a value in the environment (for in-place modification)
static Value* env_get_ref(const char* name) {
    const char* iname = env_intern(name);
    for (EnvFrame* f = env_frame; f; f = f->next) {
        for (Environment* current = f->locals; current; current = current->next) {
            if (current->name == iname) return &current->value;
        }
    }
    for (Environment* current = global_env; current; current = current->next) {
        if (current->name == iname) return &current->value;
    }
    return NULL;
}

static void env_free() {
    Environment* current = global_env;
    while (current) {
        Environment* next = current->next;
        // names are interned — never freed per-node
        value_free(&current->value);
        env_node_release(current);
        current = next;
    }
    global_env = NULL;
}

// Native function registry — open-addressing hash table (FNV-1a).
// RADS registers ~680 natives; the old linear list made find_native() a
// several-hundred-strcmp walk on EVERY call expression (including the miss
// path that precedes every user-function call), which dominated recursive
// call-heavy workloads (fib: 3050ms vs CPython 57ms).
typedef struct NativeBinding {
    const char* name; // owned
    NativeFn fn;
} NativeBinding;

#define NATIVE_TABLE_SIZE 2048 // power of two; ~680 natives -> ~33% load
static NativeBinding native_table[NATIVE_TABLE_SIZE];
// 0 = empty, 1 = occupied (fn pointer alone cannot mark empties: NULL fn is legal)
static unsigned char native_used[NATIVE_TABLE_SIZE];
static size_t native_count = 0;
// Bumped on every registration; call-node callee caches validate against it.
static unsigned native_gen = 1;

static size_t native_hash(const char* name) {
    size_t h = 1469598103934665603ull;
    while (*name) {
        h ^= (unsigned char)*name++;
        h *= 1099511628211ull;
    }
    return h;
}

void register_native(const char* name, NativeFn fn) {
    size_t i = native_hash(name) & (NATIVE_TABLE_SIZE - 1);
    while (native_used[i]) {
        if (strcmp(native_table[i].name, name) == 0) {
            native_table[i].fn = fn; // re-registration overwrites
            native_gen++;
            return;
        }
        i = (i + 1) & (NATIVE_TABLE_SIZE - 1);
    }
    native_table[i].name = strdup(name);
    native_table[i].fn = fn;
    native_used[i] = 1;
    native_count++;
    native_gen++;
}

void register_native_fn(const char* module, const char* name, NativeFn fn) {
    size_t mod_len = strlen(module);
    size_t name_len = strlen(name);
    char* full_name = malloc(mod_len + 1 + name_len + 1);
    memcpy(full_name, module, mod_len);
    full_name[mod_len] = '.';
    memcpy(full_name + mod_len + 1, name, name_len + 1);
    register_native(full_name, fn);
    free(full_name);
}

static NativeFn find_native(const char* name) {
    size_t i = native_hash(name) & (NATIVE_TABLE_SIZE - 1);
    while (native_used[i]) {
        if (strcmp(native_table[i].name, name) == 0) {
            return native_table[i].fn;
        }
        i = (i + 1) & (NATIVE_TABLE_SIZE - 1);
    }
    return NULL;
}

// Struct definition registry
typedef struct StructDefBinding {
    char* name;
    ASTNode* node;
    struct StructDefBinding* next;
} StructDefBinding;

static StructDefBinding* struct_definitions = NULL;

static void register_struct(const char* name, ASTNode* node) {
    StructDefBinding* binding = malloc(sizeof(StructDefBinding));
    binding->name = strdup(name);
    binding->node = node;
    binding->next = struct_definitions;
    struct_definitions = binding;
}

static ASTNode* find_struct(const char* name) {
    StructDefBinding* current = struct_definitions;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current->node;
        }
        current = current->next;
    }
    return NULL;
}

static void env_free_structs() {
    StructDefBinding* current = struct_definitions;
    while (current) {
        StructDefBinding* next = current->next;
        free(current->name);
        // The AST node is owned by the main AST, so we don't free it here.
        free(current);
        current = next;
    }
    struct_definitions = NULL;
}

// Enum definition registry
typedef struct EnumDefBinding {
    char* name;
    ASTNode* node;
    struct EnumDefBinding* next;
} EnumDefBinding;

static EnumDefBinding* enum_definitions = NULL;

static void register_enum(const char* name, ASTNode* node) {
    EnumDefBinding* binding = malloc(sizeof(EnumDefBinding));
    binding->name = strdup(name);
    binding->node = node;
    binding->next = enum_definitions;
    enum_definitions = binding;
}

static ASTNode* find_enum(const char* name) {
    EnumDefBinding* current = enum_definitions;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current->node;
        }
        current = current->next;
    }
    return NULL;
}

static void env_free_enums() {
    EnumDefBinding* current = enum_definitions;
    while (current) {
        EnumDefBinding* next = current->next;
        free(current->name);
        free(current);
        current = next;
    }
    enum_definitions = NULL;
}

// Resolve a callee name to a native fn and/or a borrowed environment binding,
// using the per-call-node cache (perf round 3). `cacheable` must be true only
// for plain-identifier callees (no member path). `out_binding` is borrowed —
// never freed by the caller.
static void resolve_callee_cached(ASTNode* node, const char* name, bool cacheable,
                                  NativeFn* out_native, Environment** out_binding) {
    NativeFn native = NULL;
    Environment* callee_binding = NULL; // borrowed; never freed here
    EnvNameEntry* cache_entry = NULL;

    if (cacheable) {
        cache_entry = env_intern_entry(name);
        if (cache_entry->frameable) {
            cache_entry = NULL; // dynamic: never cache
        } else if (node->call_expr.cache_iname == cache_entry->name &&
                   node->call_expr.cache_gen == cache_entry->gen &&
                   node->call_expr.cache_native_gen == native_gen) {
            *out_native = (NativeFn)node->call_expr.cache_native;
            *out_binding = (Environment*)node->call_expr.cache_binding;
            return;
        }
    }

    native = find_native(name);
    if (!native) {
        // Borrowed lookup (frames first, then globals).
        const char* iname = env_intern(name);
        for (EnvFrame* f = env_frame; f && !callee_binding; f = f->next) {
            for (Environment* c = f->locals; c; c = c->next) {
                if (c->name == iname) { callee_binding = c; break; }
            }
        }
        for (Environment* c = global_env; c && !callee_binding; c = c->next) {
            if (c->name == iname) { callee_binding = c; break; }
        }
    }
    if (cache_entry) {
        node->call_expr.cache_iname = cache_entry->name;
        node->call_expr.cache_gen = cache_entry->gen;
        node->call_expr.cache_native_gen = native_gen;
        node->call_expr.cache_native = (void*)native;
        node->call_expr.cache_binding = callee_binding;
    }
    *out_native = native;
    *out_binding = callee_binding;
}

// Invoke a resolved callee. Args stay owned by the CALLER (the user-function
// path clones them; natives must return owning values). `func_snapshot` is a
// shallow snapshot of the callee Value taken BEFORE argument evaluation (a
// rebinding during arg eval updates the binding node in place; the snapshot's
// func_node is program-lifetime AST either way).
static Value invoke_callee(NativeFn native, Value func_snapshot, int argc, Value* args) {
    if (native) {
        return native(global_interpreter, argc, args);
    }
    if (func_snapshot.type == VAL_FUNCTION) {
        return interpreter_execute_callback(func_snapshot, argc, args);
    }
    return make_null();
}

// Evaluate call expression
static Value eval_call(ASTNode* node) {
    // For now, only support calling global native functions or simple function calls
    // Real implementation would need full environment/scope support
    
    char* name = NULL;
    char* full_name = NULL;
    
    // Member expression handling: allow dispatch of methods on string handles (e.g., server.route) and array methods
    if (node->call_expr.callee->type == AST_MEMBER_EXPR) {
        ASTNode* obj = node->call_expr.callee->member_expr.object;
        const char* member = node->call_expr.callee->member_expr.member;
        // Attempt to dispatch to native function with implicit handle argument
        Value obj_val = eval_expression(obj);

        // Array method calls
        if (obj_val.type == VAL_ARRAY && member) {
            if (strcmp(member, "push") == 0) {
                // arr.push(value) - add element to end
                int argc = node->call_expr.arguments ? (int)node->call_expr.arguments->count : 0;
                if (argc > 0) {
                    Value val_to_push = eval_expression(node->call_expr.arguments->nodes[0]);
                    array_push(obj_val.array_val, val_to_push);
                    value_free(&val_to_push);
                }
                value_free(&obj_val);
                return make_null();
            } else if (strcmp(member, "pop") == 0) {
                // arr.pop() - remove and return last element
                Value result = make_null();
                if (obj_val.array_val->count > 0) {
                    result = value_clone_impl(obj_val.array_val->items[obj_val.array_val->count - 1]);
                    value_release(&obj_val.array_val->items[obj_val.array_val->count - 1]);
                    obj_val.array_val->count--;
                }
                value_free(&obj_val);
                return result;
            }
        }

        if (obj_val.type == VAL_STRING && member) {
            char native_name[64];
            snprintf(native_name, sizeof(native_name), "net.%s", member);
            NativeFn native = find_native(native_name);
            if (native) {
                int argc = node->call_expr.arguments ? (int)node->call_expr.arguments->count : 0;
                Value stack_args[8];
                Value* args = (argc + 1 <= 8) ? stack_args : malloc(sizeof(Value) * (argc + 1));
                args[0] = value_clone_impl(obj_val);
                for (int i = 0; i < argc; i++) {
                    args[i + 1] = eval_expression(node->call_expr.arguments->nodes[i]);
                }
                Value result = native(global_interpreter, argc + 1, args);
                for (int i = 0; i < argc + 1; i++) {
                    value_free(&args[i]);
                }
                if (args != stack_args) free(args);
                value_free(&obj_val);
                return result;
            }
        }
        value_free(&obj_val);

        if (obj->type == AST_IDENTIFIER && member) {
            char* obj_name = obj->identifier.name;
            int len = strlen(obj_name) + 1 + strlen(member) + 1;
            full_name = malloc(len);
            sprintf(full_name, "%s.%s", obj_name, member);
            name = full_name;
        }
    } else if (node->call_expr.callee->type == AST_IDENTIFIER) {
        name = node->call_expr.callee->identifier.name;
    }
    
    if (name) {
        NativeFn native = NULL;
        Environment* callee_binding = NULL; // borrowed
        resolve_callee_cached(node, name,
                              !full_name && node->call_expr.callee->type == AST_IDENTIFIER,
                              &native, &callee_binding);

        if (native || (callee_binding && callee_binding->value.type == VAL_FUNCTION)) {
            // Snapshot before arg evaluation (see invoke_callee).
            Value func_snapshot = callee_binding ? callee_binding->value : make_null();

            int argc = node->call_expr.arguments ? (int)node->call_expr.arguments->count : 0;
            Value stack_args[8];
            Value* args = (argc <= 8) ? stack_args : malloc(sizeof(Value) * (argc > 0 ? argc : 1));

            for (int i = 0; i < argc; i++) {
                args[i] = eval_expression(node->call_expr.arguments->nodes[i]);
            }

            Value result = invoke_callee(native, func_snapshot, argc, args);

            // Cleanup args (callee value is borrowed — nothing to free)
            for (int i = 0; i < argc; i++) {
                value_free(&args[i]);
            }
            if (args != stack_args) free(args);
            if (full_name) free(full_name);
            return result;
        }

        if (full_name) free(full_name);
    }
    
    // Fallback for user functions (not fully implemented in this prototype)
    return make_null();
}

// Evaluate unary operation
static Value eval_unary_op(ASTNode* node) {
    Value operand = eval_expression(node->unary_op.operand);
    Value result = make_null();

    switch (node->unary_op.op) {
        case OP_NEG:
            // Negate numbers
            if (operand.type == VAL_INT) {
                result = make_int(-operand.int_val);
            } else if (operand.type == VAL_FLOAT) {
                result = make_float(-operand.float_val);
            }
            break;
        case OP_NOT:
            // Logical NOT
            if (operand.type == VAL_BOOL) {
                result = make_bool(!operand.bool_val);
            } else if (operand.type == VAL_INT) {
                result = make_bool(operand.int_val == 0);
            } else if (operand.type == VAL_NULL) {
                result = make_bool(true);
            }
            break;
        default:
            break;
    }

    value_free(&operand);
    return result;
}

// Apply a binary operator to two owned Values. Returns an owned result and
// frees both operands. Shared by the tree-walker and the bytecode VM —
// semantics must stay identical between the two executors.
static Value binop_apply(OperatorType op, Value left, Value right) {
    Value result = make_null();
    switch (op) {
        case OP_ADD:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_int(left.int_val + right.int_val);
            } else if (left.type == VAL_FLOAT && right.type == VAL_FLOAT) {
                result = make_float(left.float_val + right.float_val);
            } else if (left.type == VAL_STRING || right.type == VAL_STRING) {
                // String concatenation with automatic conversion
                char left_buf[256] = "";
                char right_buf[256] = "";
                const char* left_str = left_buf;
                const char* right_str = right_buf;

                // Convert left to string
                if (left.type == VAL_STRING) {
                    left_str = left.string_val;
                } else if (left.type == VAL_INT) {
                    snprintf(left_buf, sizeof(left_buf), "%lld", left.int_val);
                } else if (left.type == VAL_FLOAT) {
                    snprintf(left_buf, sizeof(left_buf), "%g", left.float_val);
                } else if (left.type == VAL_BOOL) {
                    snprintf(left_buf, sizeof(left_buf), "%s", left.bool_val ? "true" : "false");
                } else if (left.type == VAL_ARRAY) {
                    // Convert array to string representation
                    if (left.array_val) {
                        char* arr_str = malloc(1024);
                        strcpy(arr_str, "[");
                        for (size_t i = 0; i < left.array_val->count && strlen(arr_str) < 1000; i++) {
                            if (i > 0) strcat(arr_str, ", ");
                            Value* item = &left.array_val->items[i];
                            char item_buf[64];
                            if (item->type == VAL_INT) {
                                snprintf(item_buf, sizeof(item_buf), "%lld", item->int_val);
                            } else if (item->type == VAL_FLOAT) {
                                snprintf(item_buf, sizeof(item_buf), "%g", item->float_val);
                            } else if (item->type == VAL_STRING) {
                                snprintf(item_buf, sizeof(item_buf), "%s", item->string_val);
                            } else if (item->type == VAL_BOOL) {
                                strcpy(item_buf, item->bool_val ? "true" : "false");
                            } else {
                                strcpy(item_buf, "null");
                            }
                            strcat(arr_str, item_buf);
                        }
                        strcat(arr_str, "]");
                        left_str = left_buf;
                        strncpy(left_buf, arr_str, sizeof(left_buf) - 1);
                        left_buf[sizeof(left_buf) - 1] = '\0';
                        free(arr_str);
                    } else {
                        strcpy(left_buf, "[]");
                    }
                } else {
                    strcpy(left_buf, "null");
                }

                // Convert right to string
                if (right.type == VAL_STRING) {
                    right_str = right.string_val;
                } else if (right.type == VAL_INT) {
                    snprintf(right_buf, sizeof(right_buf), "%lld", right.int_val);
                } else if (right.type == VAL_FLOAT) {
                    snprintf(right_buf, sizeof(right_buf), "%g", right.float_val);
                } else if (right.type == VAL_BOOL) {
                    snprintf(right_buf, sizeof(right_buf), "%s", right.bool_val ? "true" : "false");
                } else if (right.type == VAL_ARRAY) {
                    // Convert array to string representation
                    if (right.array_val) {
                        char* arr_str = malloc(1024);
                        strcpy(arr_str, "[");
                        for (size_t i = 0; i < right.array_val->count && strlen(arr_str) < 1000; i++) {
                            if (i > 0) strcat(arr_str, ", ");
                            Value* item = &right.array_val->items[i];
                            char item_buf[64];
                            if (item->type == VAL_INT) {
                                snprintf(item_buf, sizeof(item_buf), "%lld", item->int_val);
                            } else if (item->type == VAL_FLOAT) {
                                snprintf(item_buf, sizeof(item_buf), "%g", item->float_val);
                            } else if (item->type == VAL_STRING) {
                                snprintf(item_buf, sizeof(item_buf), "%s", item->string_val);
                            } else if (item->type == VAL_BOOL) {
                                strcpy(item_buf, item->bool_val ? "true" : "false");
                            } else {
                                strcpy(item_buf, "null");
                            }
                            strcat(arr_str, item_buf);
                        }
                        strcat(arr_str, "]");
                        right_str = right_buf;
                        strncpy(right_buf, arr_str, sizeof(right_buf) - 1);
                        right_buf[sizeof(right_buf) - 1] = '\0';
                        free(arr_str);
                    } else {
                        strcpy(right_buf, "[]");
                    }
                } else {
                    strcpy(right_buf, "null");
                }

                // Concatenate — single pass: lengths computed once, one
                // malloc, two memcpys, Value built in place. The old
                // strcpy/strcat/make_string(strdup)/free sequence made four
                // passes over the left operand per binop.
                size_t llen = strlen(left_str), rlen = strlen(right_str);
                char* str = malloc(llen + rlen + 1);
                memcpy(str, left_str, llen);
                memcpy(str + llen, right_str, rlen + 1);
                result.type = VAL_STRING;
                result.string_val = str;
            }
            break;
        case OP_RANGE:
            // Range materializes as array of ints for now: [left, right) if increasing, else empty
            if (left.type == VAL_INT && right.type == VAL_INT && right.int_val >= left.int_val) {
                // Placeholder: return null; range iteration handled in cruise during execution
                result = make_null();
            }
            break;
        case OP_SUB:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_int(left.int_val - right.int_val);
            }
            break;
        case OP_MUL:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_int(left.int_val * right.int_val);
            }
            break;
        case OP_DIV:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                if (right.int_val != 0) {
                    result = make_int(left.int_val / right.int_val);
                }
            }
            break;
        case OP_MOD:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                if (right.int_val != 0) {
                    result = make_int(left.int_val % right.int_val);
                }
            }
            break;
        case OP_EQ:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_bool(left.int_val == right.int_val);
            } else if (left.type == VAL_FLOAT && right.type == VAL_FLOAT) {
                result = make_bool(left.float_val == right.float_val);
            } else if (left.type == VAL_BOOL && right.type == VAL_BOOL) {
                result = make_bool(left.bool_val == right.bool_val);
            } else if (left.type == VAL_STRING && right.type == VAL_STRING) {
                result = make_bool(strcmp(left.string_val, right.string_val) == 0);
            } else if (left.type == VAL_NULL && right.type == VAL_NULL) {
                result = make_bool(true);
            } else {
                result = make_bool(false);
            }
            break;
        case OP_NEQ:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_bool(left.int_val != right.int_val);
            } else if (left.type == VAL_FLOAT && right.type == VAL_FLOAT) {
                result = make_bool(left.float_val != right.float_val);
            } else if (left.type == VAL_BOOL && right.type == VAL_BOOL) {
                result = make_bool(left.bool_val != right.bool_val);
            } else if (left.type == VAL_STRING && right.type == VAL_STRING) {
                result = make_bool(strcmp(left.string_val, right.string_val) != 0);
            } else if (left.type == VAL_NULL && right.type == VAL_NULL) {
                result = make_bool(false);
            } else {
                result = make_bool(true);
            }
            break;
        case OP_LT:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_bool(left.int_val < right.int_val);
            } else if (left.type == VAL_FLOAT && right.type == VAL_FLOAT) {
                result = make_bool(left.float_val < right.float_val);
            }
            break;
        case OP_LTE:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_bool(left.int_val <= right.int_val);
            } else if (left.type == VAL_FLOAT && right.type == VAL_FLOAT) {
                result = make_bool(left.float_val <= right.float_val);
            }
            break;
        case OP_GT:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_bool(left.int_val > right.int_val);
            } else if (left.type == VAL_FLOAT && right.type == VAL_FLOAT) {
                result = make_bool(left.float_val > right.float_val);
            }
            break;
        case OP_GTE:
            if (left.type == VAL_INT && right.type == VAL_INT) {
                result = make_bool(left.int_val >= right.int_val);
            } else if (left.type == VAL_FLOAT && right.type == VAL_FLOAT) {
                result = make_bool(left.float_val >= right.float_val);
            }
            break;
        case OP_AND:
            result = make_bool(is_truthy(left) && is_truthy(right));
            break;
        case OP_OR:
            result = make_bool(is_truthy(left) || is_truthy(right));
            break;
        default:
            break;
    }
    
    value_free(&left);
    value_free(&right);
    
    return result;
}

// Evaluate binary operation
static Value eval_binary_op(ASTNode* node) {
    // Force defined left-to-right operand evaluation. Passing both evals as
    // two arguments of one call left the order unspecified (gcc went
    // right-to-left), which diverged from the BC VM on side-effecting
    // operands (e.g. "a" + arr.pop() + arr.pop()). Caught by the run-9
    // BC-vs-tree-walker A/B protocol.
    Value l = eval_expression(node->binary_op.left);
    Value r = eval_expression(node->binary_op.right);
    return binop_apply(node->binary_op.op, l, r);
}

// Evaluate expression
static Value eval_expression(ASTNode* node) {
    if (!node) return make_null();
    
    switch (node->type) {
        case AST_INTEGER_LITERAL:
            return make_int(node->integer_literal.value);
        
        case AST_FLOAT_LITERAL:
            return make_float(node->float_literal.value);
        
        case AST_STRING_LITERAL:
            return make_string(node->string_literal.value);
        
        case AST_BOOL_LITERAL:
            return make_bool(node->bool_literal.value);
        
        case AST_ARRAY_LITERAL:
            return make_array_from_list(node->array_literal.elements);
        
        case AST_INDEX_EXPR: {
            Value arr = eval_expression(node->index_expr.array);
            Value idx = eval_expression(node->index_expr.index);
            Value result = make_null();
            if (arr.type == VAL_ARRAY && idx.type == VAL_INT) {
                if (idx.int_val >= 0 && (size_t)idx.int_val < arr.array_val->count) {
                    // return copy
                    Value elem = arr.array_val->items[idx.int_val];
                    // shallow copy ok; duplicate strings
                    if (elem.type == VAL_STRING) {
                        result = make_string(elem.string_val);
                    } else if (elem.type == VAL_ARRAY) {
                        // Share the nested array but take our own reference:
                        // the container below may hit refcount 0 in the
                        // value_free(&arr) right after (literal temps like
                        // [[1,2],[3,4]][1] have no other owner), which frees
                        // the items and leaves the shared result dangling.
                        // heap-use-after-free, run 13 probes A/B.
                        result.type = VAL_ARRAY;
                        result.array_val = elem.array_val;
                        if (result.array_val) result.array_val->refcount++;
                    } else {
                        result = elem;
                    }
                }
            }
            value_free(&arr);
            value_free(&idx);
            return result;
        }

        case AST_UNARY_OP:
            return eval_unary_op(node);

        case AST_TYPEOF_EXPR: {
            Value val = eval_expression(node->typeof_expr.operand);
            const char* type_str = "unknown";
            switch (val.type) {
                case VAL_INT: type_str = "integer"; break;
                case VAL_FLOAT: type_str = "float"; break;
                case VAL_STRING: type_str = "string"; break;
                case VAL_BOOL: type_str = "bool"; break;
                case VAL_NULL: type_str = "null"; break;
                case VAL_ARRAY: type_str = "array"; break;
                case VAL_FUNCTION: type_str = "function"; break;
                case VAL_STRUCT_DEF: type_str = "struct_def"; break;
                case VAL_STRUCT_INSTANCE: type_str = "struct"; break;
            }
            value_free(&val);
            return make_string(type_str);
        }

        case AST_BINARY_OP:
            return eval_binary_op(node);
            
        case AST_CALL_EXPR:
            return eval_call(node);
            
        case AST_MEMBER_EXPR: {
            // Check for enum value access (e.g., Color.RED)
            if (node->member_expr.object->type == AST_IDENTIFIER) {
                const char* enum_name = node->member_expr.object->identifier.name;
                ASTNode* enum_node = find_enum(enum_name);
                if (enum_node) {
                    const char* value_name = node->member_expr.member;
                    // Find the value index in the enum
                    for (size_t i = 0; i < enum_node->enum_decl.values->count; i++) {
                        ASTNode* val = enum_node->enum_decl.values->nodes[i];
                        if (strcmp(val->identifier.name, value_name) == 0) {
                            return make_int((long long)i);
                        }
                    }
                    fprintf(stderr, "Error: Enum '%s' has no value '%s'.\n", enum_name, value_name);
                    return make_null();
                }
            }

            Value object = eval_expression(node->member_expr.object);
            if (object.type == VAL_STRUCT_INSTANCE) {
                const char* member_name = node->member_expr.member;
                FieldValue* current = object.struct_instance->fields;
                while (current) {
                    if (strcmp(current->name, member_name) == 0) {
                        Value result = value_clone_impl(*current->value);
                        value_release(&object);
                        return result;
                    }
                    current = current->next;
                }
                fprintf(stderr, "Error: Struct '%s' has no member '%s'.\n", object.struct_instance->definition->name, member_name);
            } else if (object.type == VAL_ARRAY) {
                const char* member_name = node->member_expr.member;
                if (strcmp(member_name, "length") == 0) {
                    Value result = make_int((long long)object.array_val->count);
                    value_release(&object);
                    return result;
                }
                fprintf(stderr, "Error: Array has no property '%s'.\n", member_name);
            }
            value_release(&object);
            return make_null();
        }

        case AST_ASSIGN_EXPR: {
            Value value = eval_expression(node->assign_expr.value);
            ASTNode* target = node->assign_expr.target;

            if (target->type == AST_IDENTIFIER) {
                // env_set takes ownership; the expression result must be
                // separately owned by the caller, so clone for the env.
                env_set(target->identifier.name, value_clone_impl(value));
            } else if (target->type == AST_MEMBER_EXPR) {
                // For member assignment, we need to modify the struct in the environment
                // If the object is a simple identifier, modify it directly in the environment
                if (target->member_expr.object->type == AST_IDENTIFIER) {
                    const char* var_name = target->member_expr.object->identifier.name;
                    Value* object_ref = env_get_ref(var_name);
                    if (object_ref && object_ref->type == VAL_STRUCT_INSTANCE) {
                        const char* member_name = target->member_expr.member;
                        FieldValue* current = object_ref->struct_instance->fields;
                        while (current) {
                            if (strcmp(current->name, member_name) == 0) {
                                value_release(current->value);
                                current->value = malloc(sizeof(Value));
                                *current->value = value_clone_impl(value);
                                break;
                            }
                            current = current->next;
                        }
                    }
                } else {
                    // Complex expression - evaluate and modify copy (won't persist)
                    Value object = eval_expression(target->member_expr.object);
                    if (object.type == VAL_STRUCT_INSTANCE) {
                        const char* member_name = target->member_expr.member;
                        FieldValue* current = object.struct_instance->fields;
                        while (current) {
                            if (strcmp(current->name, member_name) == 0) {
                                value_release(current->value);
                                current->value = malloc(sizeof(Value));
                                *current->value = value_clone_impl(value);
                                break;
                            }
                            current = current->next;
                        }
                    }
                    value_release(&object);
                }
            }
            return value;
        }

        case AST_IDENTIFIER:
            return env_get(node->identifier.name);
        
        case AST_STRUCT_LITERAL: {
            ASTNode* def_node = find_struct(node->struct_literal.name);
            if (!def_node) {
                fprintf(stderr, "Error: Struct '%s' not defined.\n", node->struct_literal.name);
                return make_null();
            }

            StructInstance* instance = malloc(sizeof(StructInstance));
            instance->definition = malloc(sizeof(StructDef));
            instance->definition->name = strdup(node->struct_literal.name);
            instance->definition->ast_node = def_node;
            instance->fields = NULL;

            for (size_t i = 0; i < node->struct_literal.fields->count; i++) {
                ASTNode* assign_node = node->struct_literal.fields->nodes[i];

                if (assign_node->type != AST_ASSIGN_EXPR) {
                    fprintf(stderr, "Error: Expected assignment expression in struct literal\n");
                    continue;
                }

                if (assign_node->assign_expr.target->type != AST_IDENTIFIER) {
                    fprintf(stderr, "Error: Expected identifier as field name, got type %d\n", assign_node->assign_expr.target->type);
                    continue;
                }

                FieldValue* field = malloc(sizeof(FieldValue));
                field->name = strdup(assign_node->assign_expr.target->identifier.name);
                field->value = malloc(sizeof(Value));
                *field->value = eval_expression(assign_node->assign_expr.value);
                field->next = instance->fields;
                instance->fields = field;
            }

            Value v;
            v.type = VAL_STRUCT_INSTANCE;
            v.struct_instance = instance;
            return v;
        }

        case AST_FUNCTION_DECL: {
            Value v;
            v.type = VAL_FUNCTION;
            v.func_node = node;
            return v;
        }

        case AST_OPTIONAL_CHAIN: {
            Value object = eval_expression(node->optional_chain.object);
            if (object.type == VAL_NULL) {
                return make_null();
            }
            
            Value result = make_null();
            if (node->optional_chain.is_member) {
                if (object.type == VAL_STRUCT_INSTANCE) {
                    const char* member_name = node->optional_chain.member;
                    FieldValue* current = object.struct_instance->fields;
                    while (current) {
                        if (strcmp(current->name, member_name) == 0) {
                            result = value_clone_impl(*current->value);
                            break;
                        }
                        current = current->next;
                    }
                } else if (object.type == VAL_ARRAY) {
                    if (strcmp(node->optional_chain.member, "length") == 0) {
                        result = make_int((long long)object.array_val->count);
                    }
                }
            } else {
                Value idx = eval_expression(node->optional_chain.index);
                if (object.type == VAL_ARRAY && idx.type == VAL_INT) {
                    if (idx.int_val >= 0 && (size_t)idx.int_val < object.array_val->count) {
                        result = value_clone_impl(object.array_val->items[idx.int_val]);
                    }
                }
                value_free(&idx);
            }
            value_free(&object);
            return result;
        }

        case AST_NULLISH_COALESCING: {
            Value left = eval_expression(node->nullish_coalescing.left);
            if (left.type != VAL_NULL) {
                return left;
            }
            value_free(&left);
            return eval_expression(node->nullish_coalescing.right);
        }

        default:
            return make_null();
    }
}

// Execute echo statement
static void exec_echo(ASTNode* node) {
    Value value = eval_expression(node->echo_stmt.expression);
    value_print(&value);
    printf("\n");
    value_free(&value);
}



static Value current_return_value;
static bool has_return_value = false;
static Value thrown_value;
static bool has_thrown_value = false;

static bool is_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL:
            return v.bool_val;
        case VAL_INT:
            return v.int_val != 0;
        case VAL_FLOAT:
            return v.float_val != 0.0;
        case VAL_STRING:
            return v.string_val && v.string_val[0] != '\0';
        case VAL_FUNCTION:
        case VAL_NULL:
        default:
            return false;
    }
}

// Execute statement
static ExecResult exec_statement(ASTNode* node) {
    if (!node) return EXEC_OK;
    
    switch (node->type) {
        case AST_STRUCT_DECL:
            register_struct(node->struct_decl.name, node);
            return EXEC_OK;

        case AST_ENUM_DECL:
            register_enum(node->enum_decl.name, node);
            return EXEC_OK;

        case AST_ECHO_STMT:
            exec_echo(node);
            return EXEC_OK;

        case AST_IMPORT_STMT: {
            // Basic import: read file, parse, and execute its declarations
            const char* filename = node->import_stmt.filename;
            FILE* file = fopen(filename, "r");
            if (!file) {
                fprintf(stderr, "Error: Cannot import '%s': file not found\n", filename);
                return EXEC_OK;
            }

            // Read entire file
            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            fseek(file, 0, SEEK_SET);
            char* source = malloc(file_size + 1);
            fread(source, 1, file_size, file);
            source[file_size] = '\0';
            fclose(file);

            // Parse the imported file
            Lexer lexer;
            lexer_init(&lexer, source);
            Parser parser;
            parser_init(&parser, &lexer);
            ASTNode* imported_ast = parser_parse(&parser);
            free(source);

            if (imported_ast && imported_ast->type == AST_PROGRAM && !parser.had_error) {
                // Execute all declarations in the imported file
                for (size_t i = 0; i < imported_ast->program.declarations->count; i++) {
                    ASTNode* decl = imported_ast->program.declarations->nodes[i];
                    if (decl->type == AST_FUNCTION_DECL) {
                        exec_function(decl);
                    } else if (decl->type == AST_STRUCT_DECL) {
                        exec_statement(decl);
                    }
                }
            } else if (parser.had_error) {
                fprintf(stderr, "Error: Failed to parse imported file '%s'\n", filename);
            }

            return EXEC_OK;
        }

        case AST_BLOCK:
            for (size_t i = 0; i < node->block.statements->count; i++) {
                ExecResult r = exec_statement(node->block.statements->nodes[i]);
                if (r == EXEC_BREAK || r == EXEC_CONTINUE || r == EXEC_RETURN || r == EXEC_THROW) {
                    return r;
                }
            }
            return EXEC_OK;

        case AST_IF_STMT: {
            Value cond = eval_expression(node->if_stmt.condition);
            bool truthy = is_truthy(cond);
            value_free(&cond);

            if (truthy) {
                return exec_statement(node->if_stmt.then_branch);
            } else if (node->if_stmt.else_branch) {
                return exec_statement(node->if_stmt.else_branch);
            }
            return EXEC_OK;
        }

        case AST_LOOP_STMT: {
            ExecResult r = EXEC_OK;
            for (;;) {
                Value cond = eval_expression(node->loop_stmt.condition);
                bool truthy = is_truthy(cond);
                value_free(&cond);
                if (!truthy) break;
                
                r = exec_statement(node->loop_stmt.body);
                if (r == EXEC_BREAK) {
                    return EXEC_OK;
                }
                if (r == EXEC_CONTINUE) {
                    continue;
                }
                if (r == EXEC_RETURN) {
                    return EXEC_RETURN;
                }
                if (r == EXEC_THROW) {
                    return EXEC_THROW;
                }
            }
            return EXEC_OK;
        }
        
        case AST_CRUISE_STMT: {
            // Expect iterable to be a range binary op or something providing ints; currently handle range only
            ASTNode* iter = node->cruise_stmt.iterable;
            if (iter && iter->type == AST_BINARY_OP && iter->binary_op.op == OP_RANGE) {
                Value start_v = eval_expression(iter->binary_op.left);
                Value end_v = eval_expression(iter->binary_op.right);
                long long start = (start_v.type == VAL_INT) ? start_v.int_val : 0;
                long long end = (end_v.type == VAL_INT) ? end_v.int_val : 0;
                value_free(&start_v);
                value_free(&end_v);
                
                for (long long i = start; i < end; i++) {
                    Value idx = make_int(i);
                    env_set(node->cruise_stmt.iterator, idx); // env takes ownership
                    
                    ExecResult r = exec_statement(node->cruise_stmt.body);
                    if (r == EXEC_BREAK) {
                        return EXEC_OK;
                    }
                    if (r == EXEC_CONTINUE) {
                        continue;
                    }
                    if (r == EXEC_RETURN) {
                        return EXEC_RETURN;
                    }
                    if (r == EXEC_THROW) {
                        return EXEC_THROW;
                    }
                }
            }
            return EXEC_OK;
        }
        
        case AST_BREAK_STMT:
            return EXEC_BREAK;
        case AST_CONTINUE_STMT:
            return EXEC_CONTINUE;
        case AST_RETURN_STMT: {
            Value rv = make_null();
            if (node->return_stmt.value) {
                rv = eval_expression(node->return_stmt.value);
            }
            if (has_return_value) {
                value_free(&current_return_value);
            }
            current_return_value = rv; // move: rv is owned here
            has_return_value = true;
            return EXEC_RETURN;
        }
        
        case AST_THROW_STMT: {
            Value tv = make_null();
            if (node->throw_stmt.expression) {
                tv = eval_expression(node->throw_stmt.expression);
            }
            if (has_thrown_value) {
                value_free(&thrown_value);
            }
            thrown_value = value_clone_impl(tv);
            has_thrown_value = true;
            value_free(&tv);
            return EXEC_THROW;
        }
        
        case AST_TRY_STMT: {
            ExecResult r = exec_statement(node->try_stmt.try_block);
            
            if (r == EXEC_THROW && node->try_stmt.catch_block) {
                if (has_thrown_value) {
                    if (node->try_stmt.catch_var) {
                        env_set(node->try_stmt.catch_var, thrown_value); // env owns
                    } else {
                        value_free(&thrown_value);
                    }
                    has_thrown_value = false;
                }
                r = exec_statement(node->try_stmt.catch_block);
            }
            
            if (node->try_stmt.finally_block) {
                ExecResult fr = exec_statement(node->try_stmt.finally_block);
                if (fr == EXEC_RETURN) {
                    return EXEC_RETURN;
                }
                if (fr == EXEC_THROW) {
                    return EXEC_THROW;
                }
            }
            
            return r;
        }
        
        case AST_VARIABLE_DECL: {
            Value val = make_null();
            if (node->variable_decl.initializer) {
                val = eval_expression(node->variable_decl.initializer);
            }
            
            if (node->variable_decl.destructure_pattern) {
                ASTNode* pattern = node->variable_decl.destructure_pattern;
                if (pattern->type == AST_DESTRUCTURE_ARRAY) {
                    if (val.type != VAL_ARRAY || !val.array_val) {
                        fprintf(stderr, "Error: Cannot destructure non-array value\n");
                    } else {
                        Array* arr = val.array_val;
                        ASTList* elements = pattern->destructure_array.elements;
                        size_t array_index = 0;
                        
                        for (size_t i = 0; i < elements->count; i++) {
                            ASTNode* elem = elements->nodes[i];
                            
                            if (elem->type == AST_DESTRUCTURE_REST) {
                                Array* rest_arr = array_create(arr->count - array_index);
                                while (array_index < arr->count) {
                                    array_push(rest_arr, arr->items[array_index]);
                                    array_index++;
                                }
                                Value rest_val;
                                rest_val.type = VAL_ARRAY;
                                rest_val.array_val = rest_arr;
                                env_set(elem->destructure_rest.name, rest_val); // env owns
                            } else if (elem->type == AST_IDENTIFIER) {
                                if (array_index < arr->count) {
                                    env_set(elem->identifier.name, value_clone_impl(arr->items[array_index]));
                                } else {
                                    env_set(elem->identifier.name, make_null());
                                }
                                array_index++;
                            }
                        }
                    }
                } else if (pattern->type == AST_DESTRUCTURE_STRUCT) {
                    if (val.type != VAL_STRUCT_INSTANCE || !val.struct_instance) {
                        fprintf(stderr, "Error: Cannot destructure non-struct value\n");
                    } else {
                        StructInstance* instance = val.struct_instance;
                        ASTList* fields = pattern->destructure_struct.fields;
                        
                        for (size_t i = 0; i < fields->count; i++) {
                            ASTNode* field_assign = fields->nodes[i];
                            if (field_assign->type == AST_ASSIGN_EXPR) {
                                const char* field_name = field_assign->assign_expr.target->identifier.name;
                                const char* var_name = field_assign->assign_expr.value->identifier.name;
                                
                                FieldValue* fv = instance->fields;
                                while (fv) {
                                    if (strcmp(fv->name, field_name) == 0) {
                                        env_set(var_name, value_clone_impl(*fv->value)); // borrowed field
                                        break;
                                    }
                                    fv = fv->next;
                                }
                                if (!fv) {
                                    env_set(var_name, make_null());
                                }
                            }
                        }
                    }
                }
                value_free(&val); // destructure sets cloned; `val` still owned here
            } else if (node->variable_decl.name) {
                env_decl(node->variable_decl.name, val); // env takes ownership
            }
            return EXEC_OK;
        }
        
        default: {
            Value v = eval_expression(node);
            value_free(&v);
            return EXEC_OK;
        }
    }
}

// Execute function
static void exec_function(ASTNode* node) {
    // Store in environment for lookup (including as first-class values)
    Value v;
    v.type = VAL_FUNCTION;
    v.func_node = node;
    env_set(node->function_decl.name, v);
}

// ============================================================================
// Bytecode VM (perf round 4) — fib-shaped subset, prototype.
//
// `blast` function bodies whose statements are only: block / if / return /
// variable decl (plain, no destructuring) / expression- or assignment-
// statements over {int, float, bool, string literals, identifiers, binary
// ops, identifier-callee calls} are compiled once to a compact stack
// bytecode with slotted locals and run by a dispatch loop. Anything else
// bails at compile time and tree-walks forever (bc_cache = (void*)1).
// Semantics are bit-identical to the tree-walker: the same binop_apply,
// is_truthy, env_get/env_set, resolve_callee_cached and invoke_callee run
// underneath. Disable at runtime with RADS_NO_BC=1 (A/B benchmarking).
// ============================================================================

typedef enum {
    BC_INT,      // imm: i64  — push int
    BC_FLOAT,    // imm: f64  — push float
    BC_TRUE, BC_FALSE, BC_NULL,
    BC_STR,      // imm: u16 const-idx — push strdup'd string
    BC_GETL,     // imm: u16 slot — push clone of local
    BC_SETL_POP, // imm: u16 slot — pop into local (frees old)
    BC_GETG,     // imm: u16 name-idx — push env_get(name)
    BC_SETG_POP, // imm: u16 name-idx — pop; env_set(name, v) (env owns)
    BC_BIN,      // imm: u8 op — pop right, pop left, push binop_apply
    BC_JMP,      // imm: u16 abs offset
    BC_JMPF,     // imm: u16 abs offset — pop; jump if !truthy
    BC_CALL,     // imm: u8 argc, u16 call-site idx
    BC_POP,      // pop and discard
    BC_RET,      // pop return value; function exits
    BC_ECHO,     // pop; value_print + newline (echo statement)
    BC_MKARR,    // imm: u16 n — pop n elements, push array literal (round 7)
    BC_CALLM,    // dotted call: imm u8 argc, u16 site, u16 full-name idx, u16 member idx (round 7)
    BC_SLOT_BIN, // imm: u16 slot, u8 op — local[slot] = local[slot] <op> TOS, in place (round 8)
    BC_GETIDX    // pop idx, arr; push arr[idx] clone (round 9 — mirrors eval index path)
} BcOpcode;
// --- break/continue patch lists (perf round 5: loops) ---
// BREAK/CONTINUE compile to a plain BC_JMP whose operand is recorded here
// and fixed up by the innermost enclosing loop when it closes. Correct even
// through nested ifs: every pending site inside the body range gets the same
// target; inner loops have already claimed their own sites by closing first.
typedef struct BcLoopPatch {
    size_t at;      // offset of the jump operand to patch
    int kind;       // 0 = break, 1 = continue
} BcLoopPatch;

#define BC_MAX_LOOP_PATCHES 256
typedef struct BcLoopPatchList {
    BcLoopPatch items[BC_MAX_LOOP_PATCHES];
    size_t count;
    struct BcLoopPatchList* next; // outer function nesting level
} BcLoopPatchList;

#define BC_MAX_SLOTS 256
#define BC_MAX_ARGS  16
#define BC_MAX_STACK 128
#define BC_MAX_ARR_ELEMS 32 // array-literal element cap in the BC subset

typedef struct BcFunc {
    uint8_t* code;
    size_t code_len;
    const char** consts;   // string literal payloads (owned via AST? no —
                           // strdup'd at compile; program-lifetime)
    size_t const_count;
    ASTNode** call_sites;  // AST call nodes, indexed by BC_CALL site operand
    size_t call_site_count;
    int nslots;
    // slot -> param? (for arg binding)
    int nparams;
} BcFunc;

typedef struct BcCompiler {
    uint8_t* code;
    size_t len, cap;
    const char** consts;
    size_t const_count, const_cap;
    ASTNode** call_sites;
    size_t site_count, site_cap;
    const char* slot_names[BC_MAX_SLOTS]; // interned via env_intern
    int nslots;
    BcLoopPatchList* loops; // innermost-open loop patch scope
    bool ok; // false once any emit/compile error happens
} BcCompiler;

static void bc_put(BcCompiler* c, uint8_t b) {
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 128;
        c->code = realloc(c->code, c->cap);
    }
    c->code[c->len++] = b;
}

static void bc_put16(BcCompiler* c, uint16_t v) {
    bc_put(c, v & 0xff);
    bc_put(c, (v >> 8) & 0xff);
}

static void bc_put64(BcCompiler* c, uint64_t v) {
    for (int i = 0; i < 8; i++) bc_put(c, (v >> (i * 8)) & 0xff);
}

static size_t bc_emit_jump(BcCompiler* c, BcOpcode op) {
    bc_put(c, op);
    size_t at = c->len;
    bc_put16(c, 0); // placeholder, patched by bc_patch_jump
    return at;
}

static void bc_patch_jump(BcCompiler* c, size_t at) {
    uint16_t target = (uint16_t)c->len;
    c->code[at] = target & 0xff;
    c->code[at + 1] = (target >> 8) & 0xff;
}

static void bc_break_patch(BcCompiler* c) {
    // record under the innermost open loop scope
    BcLoopPatchList* pl = c->loops;
    if (!pl) { c->ok = false; return; } // break outside a loop — bail
    if (pl->count >= BC_MAX_LOOP_PATCHES) { c->ok = false; return; }
    pl->items[pl->count].at = c->len;
    pl->items[pl->count].kind = 0;
    pl->count++;
}

static void bc_cont_patch(BcCompiler* c) {
    BcLoopPatchList* pl = c->loops;
    if (!pl) { c->ok = false; return; }
    if (pl->count >= BC_MAX_LOOP_PATCHES) { c->ok = false; return; }
    pl->items[pl->count].at = c->len;
    pl->items[pl->count].kind = 1;
    pl->count++;
}

// Close the innermost loop: patch every pending break→end_at and
// continue→cont_at whose site offset lies in [body_start, end_at), then pop
// the list. Sites from outer loops remain for their own close.
static void bc_patch_loops(BcCompiler* c, size_t body_start, size_t cont_at, size_t end_at) {
    BcLoopPatchList* pl = c->loops;
    if (!pl) return;
    for (size_t i = 0; i < pl->count; i++) {
        if (pl->items[i].at < body_start || pl->items[i].at >= end_at) continue;
        uint16_t target = (uint16_t)(pl->items[i].kind == 0 ? end_at : cont_at);
        c->code[pl->items[i].at] = target & 0xff;
        c->code[pl->items[i].at + 1] = (target >> 8) & 0xff;
    }
    // Remove the patched entries (keep any outer-loop leftovers at the front).
    size_t kept = 0;
    for (size_t i = 0; i < pl->count; i++) {
        if (pl->items[i].at >= end_at) pl->items[kept++] = pl->items[i];
    }
    pl->count = kept;
    c->loops = pl->next;
    free(pl);
}

// Open a loop scope. Returns false on OOM (bails compilation).
static bool bc_open_loop(BcCompiler* c) {
    BcLoopPatchList* pl = calloc(1, sizeof(BcLoopPatchList));
    if (!pl) { c->ok = false; return false; }
    pl->next = c->loops;
    c->loops = pl;
    return true;
}


static uint16_t bc_const_idx(BcCompiler* c, const char* s) {
    for (size_t i = 0; i < c->const_count; i++) {
        if (strcmp(c->consts[i], s) == 0) return (uint16_t)i;
    }
    if (c->const_count == c->const_cap) {
        c->const_cap = c->const_cap ? c->const_cap * 2 : 8;
        c->consts = realloc(c->consts, c->const_cap * sizeof(char*));
    }
    c->consts[c->const_count++] = strdup(s);
    return (uint16_t)(c->const_count - 1);
}

static uint16_t bc_call_site(BcCompiler* c, ASTNode* call_node) {
    if (c->site_count == c->site_cap) {
        c->site_cap = c->site_cap ? c->site_cap * 2 : 8;
        c->call_sites = realloc(c->call_sites, c->site_cap * sizeof(ASTNode*));
    }
    c->call_sites[c->site_count++] = call_node;
    return (uint16_t)(c->site_count - 1);
}

// Slot for a local name, or -1. Names are interned so slots match env lookups.
static int bc_slot_of(BcCompiler* c, const char* name) {
    const char* iname = env_intern(name);
    for (int i = 0; i < c->nslots; i++) {
        if (c->slot_names[i] == iname) return i;
    }
    return -1;
}

static int bc_new_slot(BcCompiler* c, const char* name) {
    if (c->nslots >= BC_MAX_SLOTS) { c->ok = false; return -1; }
    c->slot_names[c->nslots] = env_intern(name);
    return c->nslots++;
}

static bool bc_compile_expr(BcCompiler* c, ASTNode* e) {
    if (!e || !c->ok) return false;
    switch (e->type) {
        case AST_INTEGER_LITERAL:
            bc_put(c, BC_INT);
            bc_put64(c, (uint64_t)e->integer_literal.value);
            return true;
        case AST_FLOAT_LITERAL: {
            // Store the raw bit pattern, NOT (uint64_t)double — that value
            // cast truncates 0.5 to 0. Bit-exact round-trip with the VM's
            // memcpy read. (Bug caught by run-9 string/float concat probe.)
            double d = e->float_literal.value;
            uint64_t bits;
            memcpy(&bits, &d, 8);
            bc_put(c, BC_FLOAT);
            bc_put64(c, bits);
            return true;
        }
        case AST_BOOL_LITERAL:
            bc_put(c, e->bool_literal.value ? BC_TRUE : BC_FALSE);
            return true;
        case AST_STRING_LITERAL: {
            uint16_t idx = bc_const_idx(c, e->string_literal.value);
            bc_put(c, BC_STR);
            bc_put16(c, idx);
            return true;
        }
        case AST_IDENTIFIER: {
            int slot = bc_slot_of(c, e->identifier.name);
            if (slot >= 0) {
                bc_put(c, BC_GETL);
                bc_put16(c, (uint16_t)slot);
            } else {
                uint16_t idx = bc_const_idx(c, e->identifier.name);
                bc_put(c, BC_GETG);
                bc_put16(c, idx);
            }
            return true;
        }
        case AST_INDEX_EXPR:
            // Round 9: arr[i] in the BC subset. Index expressions evaluate
            // array-then-index (tree-walker order); BC_GETIDX mirrors the
            // tree-walker's index case exactly — including its no-bump share
            // of nested array elements (a refcount quirk we reproduce, not
            // fix, so A/B stays byte-identical).
            if (!bc_compile_expr(c, e->index_expr.array)) return false;
            if (!bc_compile_expr(c, e->index_expr.index)) return false;
            bc_put(c, BC_GETIDX);
            return true;
        case AST_BINARY_OP:
            if (!bc_compile_expr(c, e->binary_op.left)) return false;
            if (!bc_compile_expr(c, e->binary_op.right)) return false;
            bc_put(c, BC_BIN);
            bc_put(c, (uint8_t)e->binary_op.op);
            return true;
        case AST_ARRAY_LITERAL: {
            // Round 7: array literals in the BC subset. Elements evaluate in
            // order (tree-walker order), then BC_MKARR moves them — owned,
            // no re-clone — into a fresh array, matching
            // make_array_from_list's observable behavior.
            size_t n = e->array_literal.elements ? e->array_literal.elements->count : 0;
            if (n > BC_MAX_ARR_ELEMS) return false;
            if (e->array_literal.elements) {
                for (size_t i = 0; i < n; i++) {
                    if (!bc_compile_expr(c, e->array_literal.elements->nodes[i])) return false;
                }
            }
            bc_put(c, BC_MKARR);
            bc_put16(c, (uint16_t)n);
            return true;
        }
        case AST_CALL_EXPR: {
            ASTNode* callee = e->call_expr.callee;
            size_t argc = e->call_expr.arguments ? e->call_expr.arguments->count : 0;
            if (argc > BC_MAX_ARGS) return false;
            if (callee->type == AST_IDENTIFIER) {
                for (size_t i = 0; i < argc; i++) {
                    if (!bc_compile_expr(c, e->call_expr.arguments->nodes[i])) return false;
                }
                bc_put(c, BC_CALL);
                bc_put(c, (uint8_t)argc);
                bc_put16(c, bc_call_site(c, e));
                return true;
            }
            if (callee->type == AST_MEMBER_EXPR && callee->member_expr.member &&
                callee->member_expr.object &&
                callee->member_expr.object->type == AST_IDENTIFIER) {
                // Dotted call `obj.m(args)` — the array.push(arr, x) shape
                // (round 7). Only a plain-identifier object: eval_call
                // evaluates the object first (side-effect-free for
                // identifiers), then dispatches on its RUNTIME type; BC_CALLM
                // replicates that dispatch exactly (array method / net.* /
                // dotted native), so rebinding hijacks behave identically.
                const char* oname = callee->member_expr.object->identifier.name;
                const char* member = callee->member_expr.member;
                char full[256];
                int len = snprintf(full, sizeof(full), "%s.%s", oname, member);
                if (len <= 0 || (size_t)len >= sizeof(full)) return false;
                if (!bc_compile_expr(c, callee->member_expr.object)) return false;
                for (size_t i = 0; i < argc; i++) {
                    if (!bc_compile_expr(c, e->call_expr.arguments->nodes[i])) return false;
                }
                uint16_t full_idx = bc_const_idx(c, full);
                uint16_t mem_idx = bc_const_idx(c, member);
                bc_put(c, BC_CALLM);
                bc_put(c, (uint8_t)argc);
                bc_put16(c, bc_call_site(c, e));
                bc_put16(c, full_idx);
                bc_put16(c, mem_idx);
                return true;
            }
            return false;
        }
        default:
            return false; // not in the subset — bail to tree-walker
    }
}

static bool bc_compile_stmt(BcCompiler* c, ASTNode* s) {
    if (!s || !c->ok) return false;
    switch (s->type) {
        case AST_BLOCK:
            for (size_t i = 0; i < s->block.statements->count; i++) {
                if (!bc_compile_stmt(c, s->block.statements->nodes[i])) return false;
            }
            return true;
        case AST_IF_STMT: {
            if (!bc_compile_expr(c, s->if_stmt.condition)) return false;
            size_t jf = bc_emit_jump(c, BC_JMPF);
            if (!bc_compile_stmt(c, s->if_stmt.then_branch)) return false;
            size_t jend = bc_emit_jump(c, BC_JMP);
            bc_patch_jump(c, jf);
            if (s->if_stmt.else_branch) {
                if (!bc_compile_stmt(c, s->if_stmt.else_branch)) return false;
            }
            bc_patch_jump(c, jend);
            return true;
        }
        case AST_LOOP_STMT: {
            // while (cond) body  →
            //   Lcond: <cond>; JMPF Lend; <body>; JMP Lcond; Lend:
            if (!bc_open_loop(c)) return false;
            size_t cond_at = c->len;
            if (!bc_compile_expr(c, s->loop_stmt.condition)) return false;
            size_t jf = bc_emit_jump(c, BC_JMPF);
            size_t loop_body_start = c->len;
            if (!bc_compile_stmt(c, s->loop_stmt.body)) return false;
            size_t cont_at = c->len;
            bc_put(c, BC_JMP);
            bc_put16(c, (uint16_t)cond_at);
            size_t end_at = c->len;
            bc_patch_jump(c, jf);
            bc_patch_loops(c, loop_body_start, cont_at, end_at);
            return true;
        }
        case AST_CRUISE_STMT: {
            ASTNode* iter = s->cruise_stmt.iterable;
            if (!iter || iter->type != AST_BINARY_OP || iter->binary_op.op != OP_RANGE) {
                return false; // only range-loops in the BC subset
            }
            if (!s->cruise_stmt.iterator) return false;
            // cruise (i in a..b) body  →
            //   <a>; SETL_POP i; <b>; SETL_POP $end;
            //   Lcond: GETL i; GETL $end; BIN <; JMPF Lend;
            //   <body>; Lstep: GETL i; INT 1; BIN +; SETL_POP i; JMP Lcond;
            //   Lend:
            // Bit-identical to the tree-walker: start/end evaluated once
            // before the loop; non-int bounds coerce to 0 via binop_apply's
            // null result and OP_LT's null comparison semantics; $end lives
            // in a hidden slot whose name cannot collide with identifiers.
            if (!bc_open_loop(c)) return false;
            if (!bc_compile_expr(c, iter->binary_op.left)) return false;
            int islot = bc_slot_of(c, s->cruise_stmt.iterator);
            if (islot < 0) islot = bc_new_slot(c, s->cruise_stmt.iterator);
            if (islot < 0) return false;
            bc_put(c, BC_SETL_POP);
            bc_put16(c, (uint16_t)islot);

            if (!bc_compile_expr(c, iter->binary_op.right)) return false;
            // Hidden slot: intern a name no identifier can produce.
            int eslot = bc_new_slot(c, "$range_end$");
            if (eslot < 0) return false;
            bc_put(c, BC_SETL_POP);
            bc_put16(c, (uint16_t)eslot);

            size_t cond_at = c->len;
            bc_put(c, BC_GETL);
            bc_put16(c, (uint16_t)islot);
            bc_put(c, BC_GETL);
            bc_put16(c, (uint16_t)eslot);
            bc_put(c, BC_BIN);
            bc_put(c, (uint8_t)OP_LT);
            size_t jf = bc_emit_jump(c, BC_JMPF);
            size_t loop_body_start = c->len;
            if (!bc_compile_stmt(c, s->cruise_stmt.body)) return false;
            size_t cont_at = c->len;
            // Perf round 8: increment via one BC_SLOT_BIN (in-place i += 1)
            // instead of GETL/INT/BIN/SETL_POP — same fold as x = x + rhs.
            bc_put(c, BC_INT);
            bc_put64(c, 1);
            bc_put(c, BC_SLOT_BIN);
            bc_put16(c, (uint16_t)islot);
            bc_put(c, (uint8_t)OP_ADD);
            bc_put(c, BC_JMP);
            bc_put16(c, (uint16_t)cond_at);
            size_t end_at = c->len;
            bc_patch_jump(c, jf);
            bc_patch_loops(c, loop_body_start, cont_at, end_at);
            return true;
        }
        case AST_BREAK_STMT:
            bc_put(c, BC_JMP);
            bc_break_patch(c); // fixed up to loop end by the enclosing loop
            return true;
        case AST_CONTINUE_STMT:
            bc_put(c, BC_JMP);
            bc_cont_patch(c); // fixed up to the step/cond by the enclosing loop
            return true;
        case AST_ECHO_STMT:
            // echo(expr) — same semantics as the tree-walker: evaluate,
            // print, free. Compiles as call(expr) + POP so the expression
            // subset is reused wholesale.
            if (!bc_compile_expr(c, s->echo_stmt.expression)) return false;
            bc_put(c, BC_ECHO);
            return true;
        case AST_RETURN_STMT:
            if (s->return_stmt.value) {
                if (!bc_compile_expr(c, s->return_stmt.value)) return false;
            } else {
                bc_put(c, BC_NULL);
            }
            bc_put(c, BC_RET);
            return true;
        case AST_VARIABLE_DECL: {
            if (!s->variable_decl.name || s->variable_decl.destructure_pattern) return false;
            if (s->variable_decl.initializer) {
                if (!bc_compile_expr(c, s->variable_decl.initializer)) return false;
            } else {
                bc_put(c, BC_NULL);
            }
            int slot = bc_slot_of(c, s->variable_decl.name);
            if (slot < 0) slot = bc_new_slot(c, s->variable_decl.name);
            if (slot < 0) return false;
            bc_put(c, BC_SETL_POP);
            bc_put16(c, (uint16_t)slot);
            return true;
        }
        case AST_ASSIGN_EXPR: {
            if (s->assign_expr.target->type != AST_IDENTIFIER) return false;
            const char* name = s->assign_expr.target->identifier.name;
            // Perf round 8: x = x <op> rhs with slotted x compiles to one
            // BC_SLOT_BIN (slot read-modify-write, no clone/free round-trip
            // through GETL/BIN/SETL_POP). Only for the shape whose left
            // operand IS the assigned slot — no reassociation, so evaluation
            // order and semantics stay bit-identical to the tree-walker.
            if (s->assign_expr.value &&
                s->assign_expr.value->type == AST_BINARY_OP) {
                OperatorType aop = s->assign_expr.value->binary_op.op;
                if (aop == OP_ADD || aop == OP_SUB || aop == OP_MUL ||
                    aop == OP_DIV || aop == OP_MOD) {
                    ASTNode* lhs = s->assign_expr.value->binary_op.left;
                    if (lhs->type == AST_IDENTIFIER &&
                        bc_slot_of(c, name) >= 0 &&
                        bc_slot_of(c, lhs->identifier.name) ==
                            bc_slot_of(c, name)) {
                        int slot = bc_slot_of(c, name);
                        if (!bc_compile_expr(c, s->assign_expr.value->binary_op.right))
                            return false;
                        bc_put(c, BC_SLOT_BIN);
                        bc_put16(c, (uint16_t)slot);
                        bc_put(c, (uint8_t)aop);
                        return true;
                    }
                }
            }
            if (!bc_compile_expr(c, s->assign_expr.value)) return false;
            int slot = bc_slot_of(c, name);
            if (slot >= 0) {
                bc_put(c, BC_SETL_POP);
                bc_put16(c, (uint16_t)slot);
            } else {
                uint16_t idx = bc_const_idx(c, name);
                bc_put(c, BC_SETG_POP);
                bc_put16(c, idx);
            }
            return true;
        }
        default:
            // Expression statement (tree-walker's `default:` case).
            if (!bc_compile_expr(c, s)) return false;
            bc_put(c, BC_POP);
            return true;
    }
}

// Compile a function. Returns true and stores *out on success; returns false
// (with nothing allocated) if the body is outside the subset.
static bool bc_compile(ASTNode* func, BcFunc** out) {
    BcCompiler c = {0};
    c.ok = true;

    // Parameters claim slots 0..n-1. Parameter default initializers would
    // need runtime evaluation — bail on them.
    if (func->function_decl.parameters) {
        ASTList* ps = func->function_decl.parameters;
        for (size_t i = 0; i < ps->count; i++) {
            ASTNode* p = ps->nodes[i];
            const char* pname = NULL;
            if (p->type == AST_VARIABLE_DECL) {
                if (p->variable_decl.initializer) return false;
                pname = p->variable_decl.name;
            } else if (p->type == AST_IDENTIFIER) {
                pname = p->identifier.name;
            } else {
                return false;
            }
            if (!pname || bc_new_slot(&c, pname) < 0) return false;
        }
    }

    if (!bc_compile_stmt(&c, func->function_decl.body)) {
        if (getenv("RADS_BC_TRACE")) fprintf(stderr, "[bc] bail: %s\n", func->function_decl.name ? func->function_decl.name : "<anon>");
        free(c.code); free(c.consts); free(c.call_sites);
        while (c.loops) {
            BcLoopPatchList* next = c.loops->next;
            free(c.loops);
            c.loops = next;
        }
        return false;
    }
    // Implicit `return null` fallthrough.
    bc_put(&c, BC_NULL);
    bc_put(&c, BC_RET);

    BcFunc* f = malloc(sizeof(BcFunc));
    f->code = c.code;
    f->code_len = c.len;
    f->consts = c.consts;
    f->const_count = c.const_count;
    f->call_sites = c.call_sites;
    f->call_site_count = c.site_count;
    f->nslots = c.nslots;
    f->nparams = func->function_decl.parameters ? (int)func->function_decl.parameters->count : 0;
    *out = f;
    return true;
}

// Inline truthiness (matches is_truthy exactly; avoids the call in hot loops)
static inline bool bc_truthy(Value v) {
    switch (v.type) {
        case VAL_BOOL: return v.bool_val;
        case VAL_INT:  return v.int_val != 0;
        case VAL_FLOAT: return v.float_val != 0.0;
        case VAL_STRING: return v.string_val && v.string_val[0] != '\0';
        default: return false;
    }
}

// Run compiled code. Args are borrowed (cloned into slots, exactly like the
// tree-walker clones them into the frame). Returns an owned Value.
static Value vm_run(BcFunc* f, int argc, Value* args) {
    Value slots[BC_MAX_SLOTS];
    Value stack[BC_MAX_STACK];
    int sp = 0;
    const uint8_t* ip = f->code;
    uint8_t op = 0;

    for (int i = 0; i < f->nslots; i++) slots[i] = make_null();
    for (int i = 0; i < f->nparams; i++) {
        slots[i] = (i < argc && args) ? value_clone_impl(args[i]) : make_null();
    }

#define BC_PUSH(v) do { \
        if (sp >= BC_MAX_STACK) goto stack_overflow; \
        stack[sp++] = (v); \
    } while (0)
#define BC_POP(v) do { (v) = stack[--sp]; } while (0)

    for (;;) {
        op = *ip++;
        switch (op) {
            case BC_INT: {
                long long v;
                memcpy(&v, ip, 8); ip += 8;
                BC_PUSH(make_int(v));
                break;
            }
            case BC_FLOAT: {
                double v;
                memcpy(&v, ip, 8); ip += 8;
                BC_PUSH(make_float(v));
                break;
            }
            case BC_TRUE:  BC_PUSH(make_bool(true));  break;
            case BC_FALSE: BC_PUSH(make_bool(false)); break;
            case BC_NULL:  BC_PUSH(make_null());      break;
            case BC_STR: {
                uint16_t idx = ip[0] | (ip[1] << 8); ip += 2;
                BC_PUSH(make_string(f->consts[idx]));
                break;
            }
            case BC_GETL: {
                uint16_t slot = ip[0] | (ip[1] << 8); ip += 2;
                Value v = slots[slot];
                // Fast path: trivially copyable types (matches value_clone_impl)
                if (v.type == VAL_INT || v.type == VAL_FLOAT ||
                    v.type == VAL_BOOL || v.type == VAL_NULL || v.type == VAL_FUNCTION) {
                    BC_PUSH(v);
                } else {
                    BC_PUSH(value_clone_impl(v));
                }
                break;
            }
            case BC_SETL_POP: {
                uint16_t slot = ip[0] | (ip[1] << 8); ip += 2;
                Value v; BC_POP(v);
                value_free(&slots[slot]);
                slots[slot] = v;
                break;
            }
            case BC_GETG: {
                uint16_t idx = ip[0] | (ip[1] << 8); ip += 2;
                BC_PUSH(env_get(f->consts[idx]));
                break;
            }
            case BC_SETG_POP: {
                uint16_t idx = ip[0] | (ip[1] << 8); ip += 2;
                Value v; BC_POP(v);
                env_set(f->consts[idx], v); // env takes ownership
                break;
            }
            case BC_BIN: {
                uint8_t bop = *ip++;
                Value right, left;
                BC_POP(right);
                BC_POP(left);
                // Fast paths for OP_ADD (perf round 6): skip the generic
                // binop_apply dispatch and, for string operands, the 256-byte
                // stack-buffer conversion dance + intermediate copies. int/str
                // and str/int render through a small local buffer exactly as
                // snprintf("%lld") would; anything else falls through.
                if (bop == OP_ADD) {
                    if (left.type == VAL_INT && right.type == VAL_INT) {
                        BC_PUSH(make_int(left.int_val + right.int_val));
                        break;
                    }
                    if (left.type == VAL_FLOAT && right.type == VAL_FLOAT) {
                        BC_PUSH(make_float(left.float_val + right.float_val));
                        break;
                    }
                    if (left.type == VAL_STRING && right.type == VAL_STRING) {
                        size_t llen = strlen(left.string_val);
                        size_t rlen = strlen(right.string_val);
                        char* s = malloc(llen + rlen + 1);
                        memcpy(s, left.string_val, llen);
                        memcpy(s + llen, right.string_val, rlen + 1);
                        free(left.string_val);
                        free(right.string_val);
                        Value r; r.type = VAL_STRING; r.string_val = s;
                        BC_PUSH(r);
                        break;
                    }
                    if (left.type == VAL_STRING && right.type == VAL_INT) {
                        char buf[32];
                        int n = snprintf(buf, sizeof(buf), "%lld", right.int_val);
                        size_t llen = strlen(left.string_val);
                        char* s = malloc(llen + (size_t)n + 1);
                        memcpy(s, left.string_val, llen);
                        memcpy(s + llen, buf, (size_t)n + 1);
                        free(left.string_val);
                        Value r; r.type = VAL_STRING; r.string_val = s;
                        BC_PUSH(r);
                        break;
                    }
                    if (left.type == VAL_INT && right.type == VAL_STRING) {
                        char buf[32];
                        int n = snprintf(buf, sizeof(buf), "%lld", left.int_val);
                        size_t rlen = strlen(right.string_val);
                        char* s = malloc((size_t)n + rlen + 1);
                        memcpy(s, buf, (size_t)n);
                        memcpy(s + n, right.string_val, rlen + 1);
                        free(right.string_val);
                        Value r; r.type = VAL_STRING; r.string_val = s;
                        BC_PUSH(r);
                        break;
                    }
                }
                BC_PUSH(binop_apply((OperatorType)bop, left, right));
                break;
            }
            case BC_JMP: {
                uint16_t off = ip[0] | (ip[1] << 8); ip += 2;
                ip = f->code + off;
                break;
            }
            case BC_JMPF: {
                uint16_t off = ip[0] | (ip[1] << 8); ip += 2;
                Value v; BC_POP(v);
                if (v.type == VAL_INT) {
                    // Fast path for the dominant int-condition case
                    if (v.int_val == 0) ip = f->code + off;
                } else {
                    bool t = bc_truthy(v);
                    value_free(&v);
                    if (!t) ip = f->code + off;
                }
                break;
            }
            case BC_CALL: {
                uint8_t cargc = *ip++;
                uint16_t site = ip[0] | (ip[1] << 8); ip += 2;
                if (sp < cargc) goto stack_underflow;
                Value cargs[BC_MAX_ARGS];
                for (int i = cargc - 1; i >= 0; i--) BC_POP(cargs[i]);
                ASTNode* call_node = f->call_sites[site];
                const char* cname = call_node->call_expr.callee->identifier.name;
                NativeFn native = NULL;
                Environment* binding = NULL;
                resolve_callee_cached(call_node, cname, true, &native, &binding);
                Value snapshot = binding ? binding->value : make_null();
                Value cresult = invoke_callee(native, snapshot, cargc, cargs);
                // eval_call frees args after invoke; BC_CALL historically
                // did not (leaked every heap arg — invisible on the int-only
                // benchmarks). Mirror the tree-walker (round 7).
                for (int i = 0; i < cargc; i++) value_free(&cargs[i]);
                BC_PUSH(cresult);
                break;
            }
            case BC_POP: {
                Value v; BC_POP(v);
                value_free(&v);
                break;
            }
            case BC_RET: {
                Value rv; BC_POP(rv);
                for (int i = 0; i < f->nslots; i++) value_free(&slots[i]);
                return rv;
            }
            case BC_ECHO: {
                Value v; BC_POP(v);
                value_print(&v);
                printf("\n");
                value_free(&v);
                break;
            }
            case BC_MKARR: {
                uint16_t n = ip[0] | (ip[1] << 8); ip += 2;
                if (sp < (int)n) goto stack_underflow;
                // Elements were pushed in source order; pop fills from the
                // end. Popped values are owned — moved, not re-cloned
                // (make_array_from_list clones-then-frees; net-identical).
                Array* arr = array_create(n);
                for (uint16_t i = n; i > 0; i--) {
                    Value v; BC_POP(v);
                    arr->items[i - 1] = v;
                }
                arr->count = n;
                Value r; r.type = VAL_ARRAY; r.array_val = arr;
                BC_PUSH(r);
                break;
            }
            case BC_CALLM: {
                // Dotted call obj.member(args) — mirrors eval_call's member
                // dispatch step for step: array method path first (early
                // return, never reaches native dispatch), then string→net.*,
                // then dotted-native/function resolution on "obj.member".
                uint8_t cargc = *ip++;
                uint16_t site = ip[0] | (ip[1] << 8);
                uint16_t full_idx = ip[2] | (ip[3] << 8);
                uint16_t mem_idx = ip[4] | (ip[5] << 8);
                ip += 6;
                if (sp < (int)cargc + 1) goto stack_underflow;
                Value cargs[BC_MAX_ARGS];
                for (int i = cargc - 1; i >= 0; i--) BC_POP(cargs[i]);
                Value obj_val; BC_POP(obj_val);
                const char* member = f->consts[mem_idx];

                if (obj_val.type == VAL_ARRAY && member) {
                    if (strcmp(member, "push") == 0) {
                        if (cargc > 0) {
                            array_push(obj_val.array_val, cargs[0]);
                            value_free(&cargs[0]);
                        }
                        value_free(&obj_val);
                        BC_PUSH(make_null());
                        break;
                    } else if (strcmp(member, "pop") == 0) {
                        Value result = make_null();
                        if (obj_val.array_val->count > 0) {
                            result = value_clone_impl(obj_val.array_val->items[obj_val.array_val->count - 1]);
                            value_release(&obj_val.array_val->items[obj_val.array_val->count - 1]);
                            obj_val.array_val->count--;
                        }
                        value_free(&obj_val);
                        BC_PUSH(result);
                        break;
                    }
                }
                if (obj_val.type == VAL_STRING && member) {
                    char native_name[64];
                    snprintf(native_name, sizeof(native_name), "net.%s", member);
                    NativeFn net_native = find_native(native_name);
                    if (net_native) {
                        Value margs[BC_MAX_ARGS + 1];
                        margs[0] = value_clone_impl(obj_val);
                        for (int i = 0; i < cargc; i++) margs[i + 1] = cargs[i];
                        Value result = net_native(global_interpreter, cargc + 1, margs);
                        for (int i = 0; i < cargc + 1; i++) value_free(&margs[i]);
                        value_free(&obj_val);
                        BC_PUSH(result);
                        break;
                    }
                }
                value_free(&obj_val);

                // Dotted native ("array.push" etc.) or user function bound
                // to a "obj.member" name. Not call-site cacheable (mirrors
                // eval_call: full_name path passes cacheable=false).
                const char* full = f->consts[full_idx];
                ASTNode* call_node = f->call_sites[site];
                NativeFn native = NULL;
                Environment* binding = NULL;
                resolve_callee_cached(call_node, full, false, &native, &binding);
                Value snapshot = binding ? binding->value : make_null();
                Value result = invoke_callee(native, snapshot, cargc, cargs);
                for (int i = 0; i < cargc; i++) value_free(&cargs[i]);
                BC_PUSH(result);
                break;
            }
            case BC_GETIDX: {
                // Round 9: arr[idx]. A line-for-line mirror of the
                // tree-walker's AST_INDEX_EXPR case — same order (idx, then
                // arr off the stack), same bounds rules (int idx, 0 <= idx <
                // count, else null), same clone semantics: strings strdup,
                // nested arrays SHARED with no refcount bump (the tree-walker
                // quirk, preserved bit-for-bit), scalars copied raw.
                Value idx; BC_POP(idx);
                Value arr; BC_POP(arr);
                Value result = make_null();
                if (arr.type == VAL_ARRAY && idx.type == VAL_INT) {
                    if (idx.int_val >= 0 && (size_t)idx.int_val < arr.array_val->count) {
                        Value elem = arr.array_val->items[idx.int_val];
                        if (elem.type == VAL_STRING) {
                            result = make_string(elem.string_val);
                        } else if (elem.type == VAL_ARRAY) {
                            // Mirror of the tree-walker fix (run 13): share
                            // WITH a refcount bump, so the container's
                            // value_free below can't strand the result.
                            result.type = VAL_ARRAY;
                            result.array_val = elem.array_val;
                            if (result.array_val) result.array_val->refcount++;
                        } else {
                            result = elem;
                        }
                    }
                }
                value_free(&arr);
                value_free(&idx);
                BC_PUSH(result);
                break;
            }
            case BC_SLOT_BIN: {
                // Perf round 8: local[slot] = local[slot] <op> TOS, in place.
                // In-place lanes for numeric types (int arithmetic incl. the
                // div/mod-by-zero → null rule, float ADD) — exact re-
                // implementations of binop_apply's cases. Every other shape
                // clones the slot exactly like BC_GETL and runs the very same
                // binop_apply as BC_BIN, so semantics stay bit-identical to
                // the tree-walker.
                uint16_t slot = ip[0] | (ip[1] << 8);
                uint8_t bop = ip[2];
                ip += 3;
                Value right; BC_POP(right);
                Value* dst = &slots[slot];
                if (bop == OP_ADD &&
                    ((dst->type == VAL_INT && right.type == VAL_INT) ||
                     (dst->type == VAL_FLOAT && right.type == VAL_FLOAT))) {
                    if (dst->type == VAL_INT) dst->int_val += right.int_val;
                    else dst->float_val += right.float_val;
                } else if ((bop == OP_SUB || bop == OP_MUL) &&
                           dst->type == VAL_INT && right.type == VAL_INT) {
                    if (bop == OP_SUB) dst->int_val -= right.int_val;
                    else dst->int_val *= right.int_val;
                } else if ((bop == OP_DIV || bop == OP_MOD) &&
                           dst->type == VAL_INT && right.type == VAL_INT &&
                           right.int_val != 0) {
                    if (bop == OP_DIV) dst->int_val /= right.int_val;
                    else dst->int_val %= right.int_val;
                } else {
                    Value result = binop_apply((OperatorType)bop,
                                               value_clone_impl(*dst), right);
                    value_free(dst);
                    *dst = result;
                }
                break;
            }
            default:
                goto bad_opcode;
        }
    }

stack_overflow:
stack_underflow:
bad_opcode:
    fprintf(stderr, "Error: RADS VM internal fault (op=%u sp=%d)\n", op, sp);
    for (int i = 0; i < f->nslots; i++) value_free(&slots[i]);
    return make_null();
#undef BC_PUSH
#undef BC_POP
}

static int bc_disabled = -1; // -1 = not probed, 0 = on, 1 = RADS_NO_BC

// Bytecode fast path for interpreter_execute_callback. Returns true and sets
// *result if handled; false means fall through to the tree-walker.
static bool bc_try_execute(ASTNode* func, int argc, Value* args, Value* result) {
    if (bc_disabled < 0) {
        const char* env = getenv("RADS_NO_BC");
        bc_disabled = (env && env[0] && strcmp(env, "0") != 0) ? 1 : 0;
    }
    if (bc_disabled) return false;

    void* cache = func->function_decl.bc_cache;
    if (!cache) {
        BcFunc* compiled = NULL;
        if (bc_compile(func, &compiled)) {
            cache = compiled;
        } else {
            cache = (void*)1; // not compilable — tree-walk forever
        }
        func->function_decl.bc_cache = cache;
    }
    if (cache == (void*)1) return false;

    *result = vm_run((BcFunc*)cache, argc, args);
    return true;
}

Value interpreter_execute_callback(Value callback, int argc, Value* args) {
    if (callback.type != VAL_FUNCTION) {
        return make_null();
    }

    // Bytecode fast path (perf round 4): compiles the body once, runs a
    // slotted dispatch loop. Falls through to the tree-walker untouched for
    // bodies outside the subset.
    {
        Value bc_result;
        if (bc_try_execute(callback.func_node, argc, args, &bc_result)) {
            return bc_result;
        }
    }

    // Reset return tracking for this invocation
    if (has_return_value) {
        value_free(&current_return_value);
        has_return_value = false;
    }

    ASTNode* func = callback.func_node;
    // Push a call frame: parameters and locals live in the frame, not in the
    // global environment (which previously let nested/recursive calls
    // clobber each other's parameters). Frame pops on every exit path here.
    EnvFrame frame = {0};
    frame.next = env_frame;
    env_frame = &frame;
    if (func->function_decl.parameters) {
        int param_count = (int)func->function_decl.parameters->count;
        for (int i = 0; i < param_count; i++) {
            ASTNode* param_node = func->function_decl.parameters->nodes[i];
            const char* pname;
            Value arg;

            if (param_node->type == AST_VARIABLE_DECL) {
                pname = param_node->variable_decl.name;
                if (i < argc && args) {
                    arg = value_clone_impl(args[i]);
                } else if (param_node->variable_decl.initializer) {
                    arg = eval_expression(param_node->variable_decl.initializer);
                } else {
                    arg = make_null();
                }
            } else {
                pname = param_node->identifier.name;
                arg = (i < argc && args) ? value_clone_impl(args[i]) : make_null();
            }
            env_decl(pname, arg); // env takes ownership
        }
    }
    ExecResult r = exec_statement(func->function_decl.body);
    env_frame = frame.next; // pop frame (locals freed below)
    Value rv;
    if (r == EXEC_RETURN && has_return_value) {
        rv = current_return_value; // move: callee consumed it
        has_return_value = false;
    } else {
        rv = make_null();
    }
    env_list_free(&frame.locals);
    return rv;
}

// Main interpreter
int interpret(ASTNode* program) {
    if (!program || program->type != AST_PROGRAM) {
        fprintf(stderr, "Error: Invalid program\n");
        return 1;
    }

    interpreter_init_event_loop();

    // Pass 0: Process all imports first
    for (size_t i = 0; i < program->program.declarations->count; i++) {
        ASTNode* decl = program->program.declarations->nodes[i];
        if (decl->type == AST_IMPORT_STMT) {
            exec_statement(decl);
        }
    }

    // Pass 1: Register all functions, structs, and enums
    for (size_t i = 0; i < program->program.declarations->count; i++) {
        ASTNode* decl = program->program.declarations->nodes[i];
        if (decl->type == AST_FUNCTION_DECL) {
            exec_function(decl);
        } else if (decl->type == AST_STRUCT_DECL) {
            exec_statement(decl);  // This will call register_struct
        } else if (decl->type == AST_ENUM_DECL) {
            exec_statement(decl);  // This will call register_enum
        }
    }
    
    // Pass 2: Execute main function if it exists
    Value main_val = env_get("main");
    if (main_val.type == VAL_FUNCTION) {
        // Route main() through the callback path so its body compiles to
        // bytecode like any other blast function (loops in main are the
        // benchmark hot path). interpret_repl/global-scope semantics are
        // unchanged: no params, global env visible.
        interpreter_execute_callback(main_val, 0, NULL);
        value_free(&main_val);
    } else {
        // Fallback: search declarations (legacy)
        for (size_t i = 0; i < program->program.declarations->count; i++) {
            ASTNode* decl = program->program.declarations->nodes[i];
            if (decl->type == AST_FUNCTION_DECL && strcmp(decl->function_decl.name, "main") == 0) {
                exec_statement(decl->function_decl.body);
                break;
            }
        }
    }


    env_free();
    interpreter_cleanup_event_loop();
    return 0;
}

// REPL-specific interpreter - executes single statement without clearing environment
int interpret_repl_statement(ASTNode* stmt) {
    if (!stmt) {
        return 1;
    }

    // Execute the statement directly
    // Environment persists across calls for REPL
    exec_statement(stmt);
    return 0;
}

// Clean up the global environment (call when exiting REPL)
void interpreter_cleanup_environment(void) {
    env_free();
    env_free_structs();
    env_free_enums();
}
