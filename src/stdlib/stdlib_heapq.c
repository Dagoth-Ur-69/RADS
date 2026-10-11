/*
 * stdlib_heapq.c — RADS heapq module
 *
 * Python-inspired heap queue (priority queue) for RADS.
 * Min-heap by default, like Python's heapq.
 *
 * Functions:
 * - heapq_heapify(arr)       — transform array into a heap in-place
 * - heapq_heappush(arr, val) — push value onto heap, keep invariant
 * - heapq_heappop(arr)       — pop smallest value from heap
 * - heapq_heappushpop(arr,v) — push then pop (efficient combined op)
 * - heapq_heapreplace(arr,v) — pop then push (efficient combined op)
 * - heapq_nlargest(n, arr)   — n largest elements (new array, descending)
 * - heapq_nsmallest(n, arr)  — n smallest elements (new array, ascending)
 * - heapq_merge(a, b)        — merge two sorted arrays into a new array
 * - heapq_is_heap(arr)       — 1 if array satisfies heap invariant else 0
 */

#include "stdlib_heapq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Value make_int(long long val);
extern Value make_null(void);
extern Array* array_create(size_t capacity);
extern void array_push(Array* arr, Value v);
extern void register_native(const char* name, NativeFn fn);

/* ── Helpers ── */

static double val_to_double(const Value* v) {
    if (v->type == VAL_FLOAT) return v->float_val;
    if (v->type == VAL_INT) return (double)v->int_val;
    return 0.0;
}

/* Compare two Values: -1, 0, or 1 (numeric; strings compare via strcmp) */
static int cmp_values(const Value* a, const Value* b) {
    if (a->type == VAL_STRING && b->type == VAL_STRING) {
        int c = strcmp(a->string_val, b->string_val);
        return (c > 0) - (c < 0);
    }
    double da = val_to_double(a);
    double db = val_to_double(b);
    return (da > db) - (da < db);
}

static void swap_values(Value* arr, size_t i, size_t j) {
    Value tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
}

/* ── Sift down (heapify helper): children of i are 2i+1, 2i+2 ── */
static void sift_down(Value* arr, size_t start, size_t end) {
    size_t root = start;
    while (2 * root + 1 <= end) {
        size_t child = 2 * root + 1;
        size_t swap = root;

        if (cmp_values(&arr[swap], &arr[child]) > 0) swap = child;
        if (child + 1 <= end && cmp_values(&arr[swap], &arr[child + 1]) > 0) swap = child + 1;

        if (swap == root) return;
        swap_values(arr, root, swap);
        root = swap;
    }
}

/* ── Sift up (for push) ── */
static void sift_up(Value* arr, size_t start) {
    size_t child = start;
    while (child > 0) {
        size_t parent = (child - 1) / 2;
        if (cmp_values(&arr[child], &arr[parent]) < 0) {
            swap_values(arr, child, parent);
            child = parent;
        } else {
            break;
        }
    }
}

static Value make_array_value(Array* a) {
    Value r;
    r.type = VAL_ARRAY;
    r.array_val = a;
    return r;
}

/* ── Functions ── */

static Value native_heapify(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    if (argc < 1 || argv[0].type != VAL_ARRAY) {
        fprintf(stderr, "Error: heapq_heapify(arr) requires an array\n");
        return make_null();
    }

    Array* a = argv[0].array_val;
    if (a->count < 2) return make_null();

    /* Floyd's heap construction: O(n) */
    for (size_t i = (a->count - 2) / 2 + 1; i-- > 0;) {
        sift_down(a->items, i, a->count - 1);
    }

    return make_null(); /* in-place */
}

static Value native_heappush(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    if (argc < 2 || argv[0].type != VAL_ARRAY) {
        fprintf(stderr, "Error: heapq_heappush(arr, val) requires an array and a value\n");
        return make_null();
    }

    Array* a = argv[0].array_val;
    size_t n = a->count;
    array_push(a, argv[1]); /* handles capacity growth */
    sift_up(a->items, n);

    return make_null();
}

static Value native_heappop(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    if (argc < 1 || argv[0].type != VAL_ARRAY) {
        fprintf(stderr, "Error: heapq_heappop(arr) requires an array\n");
        return make_null();
    }

    Array* a = argv[0].array_val;
    if (a->count == 0) {
        fprintf(stderr, "Error: heapq_heappop: heap is empty\n");
        return make_null();
    }

    Value result = a->items[0];
    a->items[0] = a->items[a->count - 1];
    a->count--;
    if (a->count > 1) sift_down(a->items, 0, a->count - 1);

    return result;
}

static Value native_heappushpop(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    if (argc < 2 || argv[0].type != VAL_ARRAY) {
        fprintf(stderr, "Error: heapq_heappushpop(arr, val) requires an array and a value\n");
        return make_null();
    }

    Array* a = argv[0].array_val;
    if (a->count == 0) return argv[1]; /* nothing to pop; new value wins */

    /* If the new value is already the smallest, return it without touching the heap */
    if (cmp_values(&argv[1], &a->items[0]) <= 0) return argv[1];

    Value result = a->items[0];
    a->items[0] = argv[1];
    sift_down(a->items, 0, a->count - 1);
    return result;
}

static Value native_heapreplace(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    if (argc < 2 || argv[0].type != VAL_ARRAY) {
        fprintf(stderr, "Error: heapq_heapreplace(arr, val) requires an array and a value\n");
        return make_null();
    }

    Array* a = argv[0].array_val;
    if (a->count == 0) {
        fprintf(stderr, "Error: heapq_heapreplace: heap is empty\n");
        return make_null();
    }

    Value result = a->items[0];
    a->items[0] = argv[1];
    sift_down(a->items, 0, a->count - 1);
    return result;
}

static int cmp_for_sort(const void* a, const void* b) {
    return cmp_values((const Value*)a, (const Value*)b);
}

/* Shared implementation for nlargest/nsmallest: returns a NEW array */
static Value nselect(int argc, Value* argv, int largest) {
    if (argc < 2 || argv[0].type != VAL_INT || argv[1].type != VAL_ARRAY) {
        fprintf(stderr, "Error: heapq_n%s(n, arr) requires an int and an array\n",
                largest ? "largest" : "smallest");
        return make_null();
    }

    long long k = argv[0].int_val;
    size_t n = argv[1].array_val->count;
    if (k > (long long)n) k = (long long)n;
    if (k <= 0) return make_array_value(array_create(0));

    /* Copy and sort ascending */
    Value* copy = malloc(sizeof(Value) * n);
    if (!copy) {
        fprintf(stderr, "Error: heapq_n%s: out of memory\n", largest ? "largest" : "smallest");
        return make_null();
    }
    memcpy(copy, argv[1].array_val->items, sizeof(Value) * n);
    qsort(copy, n, sizeof(Value), cmp_for_sort);

    Array* out = array_create((size_t)k);
    for (long long i = 0; i < k; i++) {
        /* largest: take from the end (descending); smallest: from the front */
        array_push(out, copy[largest ? (long long)n - 1 - i : i]);
    }
    free(copy);

    return make_array_value(out);
}

static Value native_nlargest(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    return nselect(argc, argv, 1);
}

static Value native_nsmallest(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    return nselect(argc, argv, 0);
}

/* Merge two sorted arrays into a new sorted array */
static Value native_merge(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    if (argc < 2 || argv[0].type != VAL_ARRAY || argv[1].type != VAL_ARRAY) {
        fprintf(stderr, "Error: heapq_merge(a, b) requires two arrays\n");
        return make_null();
    }

    Array* a = argv[0].array_val;
    Array* b = argv[1].array_val;
    Array* out = array_create(a->count + b->count + 1);

    size_t ia = 0, ib = 0;
    while (ia < a->count && ib < b->count) {
        if (cmp_values(&a->items[ia], &b->items[ib]) <= 0) {
            array_push(out, a->items[ia++]);
        } else {
            array_push(out, b->items[ib++]);
        }
    }
    while (ia < a->count) array_push(out, a->items[ia++]);
    while (ib < b->count) array_push(out, b->items[ib++]);

    return make_array_value(out);
}

static Value native_is_heap(struct Interpreter* interp, int argc, Value* argv) {
    (void)interp;
    if (argc < 1 || argv[0].type != VAL_ARRAY) {
        fprintf(stderr, "Error: heapq_is_heap(arr) requires an array\n");
        return make_int(0);
    }

    Array* a = argv[0].array_val;
    for (size_t i = 0; i < a->count; i++) {
        size_t left = 2 * i + 1;
        size_t right = 2 * i + 2;
        if (left < a->count && cmp_values(&a->items[i], &a->items[left]) > 0) return make_int(0);
        if (right < a->count && cmp_values(&a->items[i], &a->items[right]) > 0) return make_int(0);
    }
    return make_int(1);
}

/* ── Registration ── */
void stdlib_heapq_register(void) {
    register_native("heapq_heapify", native_heapify);
    register_native("heapq_heappush", native_heappush);
    register_native("heapq_heappop", native_heappop);
    register_native("heapq_heappushpop", native_heappushpop);
    register_native("heapq_heapreplace", native_heapreplace);
    register_native("heapq_nlargest", native_nlargest);
    register_native("heapq_nsmallest", native_nsmallest);
    register_native("heapq_merge", native_merge);
    register_native("heapq_is_heap", native_is_heap);
}
