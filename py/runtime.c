// in principle, rt_xxx functions are called only by vm/native/viper and make assumptions about args
// py_xxx functions are safer and can be called by anyone
// note that rt_assign_xxx are called only from emit*, and maybe we can rename them to reflect this

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include "nlr.h"
#include "misc.h"
#include "mpyconfig.h"
#include "runtime.h"
#include "bc.h"

#if MICROPY_ENABLE_FLOAT
// for sqrt
#include <math.h>
#endif

#if 0 // print debugging info
#define DEBUG_PRINT (1)
#define WRITE_NATIVE (1)
#define DEBUG_printf(args...) printf(args)
#define DEBUG_OP_printf(args...) printf(args)
#else // don't print debugging info
#define DEBUG_printf(args...) (void)0
#define DEBUG_OP_printf(args...) (void)0
#endif

typedef machine_int_t py_small_int_t;

#define IS_O(o, k) (((((py_small_int_t)(o)) & 1) == 0) && (((py_obj_base_t*)(o))->kind == (k)))
#define IS_SMALL_INT(o) (((py_small_int_t)(o)) & 1)
#define FROM_SMALL_INT(o) (((py_small_int_t)(o)) >> 1)
#define TO_SMALL_INT(o) ((py_obj_t)(((o) << 1) | 1))

#if MICROPY_ENABLE_FLOAT
typedef machine_float_t py_float_t;
#endif

typedef enum {
    O_CONST,
    O_STR,
#if MICROPY_ENABLE_FLOAT
    O_FLOAT,
    O_COMPLEX,
#endif
    O_EXCEPTION_0,
    O_EXCEPTION_N,
    O_RANGE,
    O_RANGE_IT,
    O_FUN_0,
    O_FUN_1,
    O_FUN_2,
    O_FUN_N,
    O_FUN_VAR,
    O_FUN_BC,
    O_FUN_ASM,
    O_GEN_WRAP,
    O_GEN_INSTANCE,
    O_BOUND_METH,
    O_TUPLE,
    O_LIST,
    O_TUPLE_IT,
    O_LIST_IT,
    O_SET,
    O_MAP,
    O_CLASS,
    O_OBJ,
    O_USER,
} py_obj_kind_t;

typedef enum {
    MAP_QSTR,
    MAP_PY_OBJ,
} py_map_kind_t;

typedef struct _py_map_elem_t {
    py_obj_t key;
    py_obj_t value;
} py_map_elem_t;

typedef struct _py_map_t {
    struct {
        py_map_kind_t kind : 1;
        machine_uint_t used : (8 * BYTES_PER_WORD - 1);
    };
    machine_uint_t alloc;
    py_map_elem_t *table;
} py_map_t;

typedef struct _py_obj_base_t py_obj_base_t;

struct _py_obj_base_t {
    py_obj_kind_t kind;
    union {
        const char *id;
        qstr u_str;
#if MICROPY_ENABLE_FLOAT
        py_float_t u_float; // for O_FLOAT
        struct { // for O_COMPLEX
            py_float_t real;
            py_float_t imag;
        } u_complex;
#endif
        struct { // for O_EXCEPTION_0
            qstr id;
        } u_exc0;
        struct { // for O_EXCEPTION_N
            // TODO make generic object or something
            qstr id;
            int n_args;
            const void **args;
        } u_exc_n;
        struct { // for O_RANGE
            // TODO make generic object or something
            machine_int_t start;
            machine_int_t stop;
            machine_int_t step;
        } u_range;
        struct { // for O_RANGE_IT
            // TODO make generic object or something
            machine_int_t cur;
            machine_int_t stop;
            machine_int_t step;
        } u_range_it;
        struct { // for O_FUN_[012N], O_FUN_VAR
            int n_args;
            void *fun;
        } u_fun;
        struct { // for O_FUN_BC
            int n_args;
            uint n_state;
            byte *code;
        } u_fun_bc;
        struct { // for O_FUN_ASM
            int n_args;
            void *fun;
        } u_fun_asm;
        struct { // for O_GEN_WRAP
            int n_state;
            py_obj_base_t *fun;
        } u_gen_wrap;
        struct { // for O_GEN_INSTANCE
            py_obj_t *state;
            const byte *ip;
            py_obj_t *sp;
        } u_gen_instance;
        struct { // for O_BOUND_METH
            py_obj_t meth;
            py_obj_t self;
        } u_bound_meth;
        struct { // for O_TUPLE, O_LIST
            machine_uint_t alloc;
            machine_uint_t len;
            py_obj_t *items;
        } u_tuple_list;
        struct { // for O_TUPLE_IT, O_LIST_IT
            py_obj_base_t *obj;
            machine_uint_t cur;
        } u_tuple_list_it;
        struct { // for O_SET
            machine_uint_t alloc;
            machine_uint_t used;
            py_obj_t *table;
        } u_set;
        py_map_t u_map; // for O_MAP
        struct { // for O_CLASS
            py_map_t *locals;
        } u_class;
        struct { // for O_OBJ
            py_obj_base_t *class; // points to a O_CLASS object
            py_map_t *members;
        } u_obj;
        struct { // for O_USER
            const py_user_info_t *info;
            machine_uint_t data1;
            machine_uint_t data2;
        } u_user;
    };
};

static qstr q_append;
static qstr q_join;
static qstr q_format;
static qstr q___build_class__;
static qstr q___next__;
static qstr q_AttributeError;
static qstr q_IndexError;
static qstr q_KeyError;
static qstr q_NameError;
static qstr q_TypeError;
static qstr q_SyntaxError;

py_obj_t py_const_none;
py_obj_t py_const_false;
py_obj_t py_const_true;
py_obj_t py_const_stop_iteration;

// locals and globals need to be pointers because they can be the same in outer module scope
static py_map_t *map_locals;
static py_map_t *map_globals;
static py_map_t map_builtins;

// approximatelly doubling primes; made with Mathematica command: Table[Prime[Floor[(1.7)^n]], {n, 3, 24}]
static int doubling_primes[] = {7, 19, 43, 89, 179, 347, 647, 1229, 2297, 4243, 7829, 14347, 26017, 47149, 84947, 152443, 273253, 488399, 869927, 1547173, 2745121, 4861607};

int get_doubling_prime_greater_or_equal_to(int x) {
    for (int i = 0; i < sizeof(doubling_primes) / sizeof(int); i++) {
        if (doubling_primes[i] >= x) {
            return doubling_primes[i];
        }
    }
    // ran out of primes in the table!
    // return something sensible, at least make it odd
    return x | 1;
}

void py_map_init(py_map_t *map, py_map_kind_t kind, int n) {
    map->kind = kind;
    map->used = 0;
    map->alloc = get_doubling_prime_greater_or_equal_to(n + 1);
    map->table = m_new0(py_map_elem_t, map->alloc);
}

py_map_t *py_map_new(py_map_kind_t kind, int n) {
    py_map_t *map = m_new(py_map_t, 1);
    py_map_init(map, kind, n);
    return map;
}

machine_int_t py_obj_hash(py_obj_t o_in) {
    if (o_in == py_const_false) {
        return 0; // needs to hash to same as the integer 0, since False==0
    } else if (o_in == py_const_true) {
        return 1; // needs to hash to same as the integer 1, since True==1
    } else if (IS_SMALL_INT(o_in)) {
        return FROM_SMALL_INT(o_in);
    } else if (IS_O(o_in, O_CONST)) {
        return (machine_int_t)o_in;
    } else if (IS_O(o_in, O_STR)) {
        return ((py_obj_base_t*)o_in)->u_str;
    } else {
        assert(0);
        return 0;
    }
}

// this function implements the '==' operator (and so the inverse of '!=')
// from the python language reference:
// "The objects need not have the same type. If both are numbers, they are converted
// to a common type. Otherwise, the == and != operators always consider objects of
// different types to be unequal."
// note also that False==0 and True==1 are true expressions
bool py_obj_equal(py_obj_t o1, py_obj_t o2) {
    if (o1 == o2) {
        return true;
    } else if (IS_SMALL_INT(o1) || IS_SMALL_INT(o2)) {
        if (IS_SMALL_INT(o1) && IS_SMALL_INT(o2)) {
            return false;
        } else {
            if (IS_SMALL_INT(o2)) {
                py_obj_t temp = o1; o1 = o2; o2 = temp;
            }
            // o1 is the SMALL_INT, o2 is not
            py_small_int_t val = FROM_SMALL_INT(o1);
            if (o2 == py_const_false) {
                return val == 0;
            } else if (o2 == py_const_true) {
                return val == 1;
            } else {
                return false;
            }
        }
    } else if (IS_O(o1, O_STR) && IS_O(o2, O_STR)) {
        return ((py_obj_base_t*)o1)->u_str == ((py_obj_base_t*)o2)->u_str;
    } else {
        assert(0);
        return false;
    }
}

py_map_elem_t* py_map_lookup_helper(py_map_t *map, py_obj_t index, bool add_if_not_found) {
    bool is_map_py_obj = (map->kind == MAP_PY_OBJ);
    machine_uint_t hash;
    if (is_map_py_obj) {
        hash = py_obj_hash(index);
    } else {
        hash = (machine_uint_t)index;
    }
    uint pos = hash % map->alloc;
    for (;;) {
        py_map_elem_t *elem = &map->table[pos];
        if (elem->key == NULL) {
            // not in table
            if (add_if_not_found) {
                if (map->used + 1 >= map->alloc) {
                    // not enough room in table, rehash it
                    int old_alloc = map->alloc;
                    py_map_elem_t *old_table = map->table;
                    map->alloc = get_doubling_prime_greater_or_equal_to(map->alloc + 1);
                    map->used = 0;
                    map->table = m_new0(py_map_elem_t, map->alloc);
                    for (int i = 0; i < old_alloc; i++) {
                        if (old_table[i].key != NULL) {
                            py_map_lookup_helper(map, old_table[i].key, true)->value = old_table[i].value;
                        }
                    }
                    m_free(old_table);
                    // restart the search for the new element
                    pos = hash % map->alloc;
                } else {
                    map->used += 1;
                    elem->key = index;
                    return elem;
                }
            } else {
                return NULL;
            }
        } else if (elem->key == index || (is_map_py_obj && py_obj_equal(elem->key, index))) {
            // found it
            /* it seems CPython does not replace the index; try x={True:'true'};x[1]='one';x
            if (add_if_not_found) {
                elem->key = index;
            }
            */
            return elem;
        } else {
            // not yet found, keep searching in this table
            pos = (pos + 1) % map->alloc;
        }
    }
}

py_map_elem_t* py_qstr_map_lookup(py_map_t *map, qstr index, bool add_if_not_found) {
    py_obj_t o = (py_obj_t)(machine_uint_t)index;
    return py_map_lookup_helper(map, o, add_if_not_found);
}

py_map_elem_t* py_map_lookup(py_obj_t o, py_obj_t index, bool add_if_not_found) {
    assert(IS_O(o, O_MAP));
    return py_map_lookup_helper(&((py_obj_base_t *)o)->u_map, index, add_if_not_found);
}

static bool fit_small_int(py_small_int_t o) {
    return true;
}

py_obj_t py_obj_new_int(machine_int_t value) {
    return TO_SMALL_INT(value);
}

py_obj_t py_obj_new_const(const char *id) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_CONST;
    o->id = id;
    return (py_obj_t)o;
}

py_obj_t py_obj_new_str(qstr qstr) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_STR;
    o->u_str = qstr;
    return (py_obj_t)o;
}

#if MICROPY_ENABLE_FLOAT
py_obj_t py_obj_new_float(py_float_t val) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_FLOAT;
    o->u_float = val;
    return (py_obj_t)o;
}

py_obj_t py_obj_new_complex(py_float_t real, py_float_t imag) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_COMPLEX;
    o->u_complex.real = real;
    o->u_complex.imag = imag;
    return (py_obj_t)o;
}
#endif

py_obj_t py_obj_new_exception_0(qstr id) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_EXCEPTION_0;
    o->u_exc0.id = id;
    return (py_obj_t)o;
}

py_obj_t py_obj_new_exception_2(qstr id, const char *fmt, const char *s1, const char *s2) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_EXCEPTION_N;
    o->u_exc_n.id = id;
    o->u_exc_n.n_args = 3;
    o->u_exc_n.args = m_new(const void*, 3);
    o->u_exc_n.args[0] = fmt;
    o->u_exc_n.args[1] = s1;
    o->u_exc_n.args[2] = s2;
    return (py_obj_t)o;
}

// range is a class and instances are immutable sequence objects
py_obj_t py_obj_new_range(int start, int stop, int step) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_RANGE;
    o->u_range.start = start;
    o->u_range.stop = stop;
    o->u_range.step = step;
    return o;
}

py_obj_t py_obj_new_range_iterator(int cur, int stop, int step) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_RANGE_IT;
    o->u_range_it.cur = cur;
    o->u_range_it.stop = stop;
    o->u_range_it.step = step;
    return o;
}

py_obj_t py_obj_new_tuple_iterator(py_obj_base_t *tuple, int cur) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_TUPLE_IT;
    o->u_tuple_list_it.obj = tuple;
    o->u_tuple_list_it.cur = cur;
    return o;
}

py_obj_t py_obj_new_list_iterator(py_obj_base_t *list, int cur) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_LIST_IT;
    o->u_tuple_list_it.obj = list;
    o->u_tuple_list_it.cur = cur;
    return o;
}

py_obj_t py_obj_new_user(const py_user_info_t *info, machine_uint_t data1, machine_uint_t data2) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_USER;
    // TODO should probably parse the info to turn strings to qstr's, and wrap functions in O_FUN_N objects
    // that'll take up some memory.  maybe we can lazily do the O_FUN_N: leave it a ptr to a C function, and
    // only when the method is looked-up do we change that to the O_FUN_N object.
    o->u_user.info = info;
    o->u_user.data1 = data1;
    o->u_user.data2 = data2;
    return o;
}

const char *py_obj_get_type_str(py_obj_t o_in) {
    if (IS_SMALL_INT(o_in)) {
        return "int";
    } else {
        py_obj_base_t *o = o_in;
        switch (o->kind) {
            case O_CONST:
                if (o == py_const_none) {
                    return "NoneType";
                } else {
                    return "bool";
                }
            case O_STR:
                return "str";
#if MICROPY_ENABLE_FLOAT
            case O_FLOAT:
                return "float";
#endif
            case O_FUN_0:
            case O_FUN_1:
            case O_FUN_2:
            case O_FUN_N:
            case O_FUN_VAR:
            case O_FUN_BC:
                return "function";
            case O_GEN_INSTANCE:
                return "generator";
            case O_TUPLE:
                return "tuple";
            case O_LIST:
                return "list";
            case O_TUPLE_IT:
                return "tuple_iterator";
            case O_LIST_IT:
                return "list_iterator";
            case O_SET:
                return "set";
            case O_MAP:
                return "dict";
            case O_OBJ:
            {
                py_map_elem_t *qn = py_qstr_map_lookup(o->u_obj.class->u_class.locals, qstr_from_str_static("__qualname__"), false);
                assert(qn != NULL);
                assert(IS_O(qn->value, O_STR));
                return qstr_str(((py_obj_base_t*)qn->value)->u_str);
            }
            case O_USER:
                return o->u_user.info->type_name;
            default:
                assert(0);
                return "UnknownType";
        }
    }
}

int rt_is_true(py_obj_t arg) {
    DEBUG_OP_printf("is true %p\n", arg);
    if (IS_SMALL_INT(arg)) {
        if (FROM_SMALL_INT(arg) == 0) {
            return 0;
        } else {
            return 1;
        }
    } else if (arg == py_const_none) {
        return 0;
    } else if (arg == py_const_false) {
        return 0;
    } else if (arg == py_const_true) {
        return 1;
    } else {
        assert(0);
        return 0;
    }
}

machine_int_t py_obj_get_int(py_obj_t arg) {
    if (arg == py_const_false) {
        return 0;
    } else if (arg == py_const_true) {
        return 1;
    } else if (IS_SMALL_INT(arg)) {
        return FROM_SMALL_INT(arg);
    } else {
        assert(0);
        return 0;
    }
}

#if MICROPY_ENABLE_FLOAT
machine_float_t py_obj_get_float(py_obj_t arg) {
    if (arg == py_const_false) {
        return 0;
    } else if (arg == py_const_true) {
        return 1;
    } else if (IS_SMALL_INT(arg)) {
        return FROM_SMALL_INT(arg);
    } else if (IS_O(arg, O_FLOAT)) {
        return ((py_obj_base_t*)arg)->u_float;
    } else {
        assert(0);
        return 0;
    }
}

void py_obj_get_complex(py_obj_t arg, py_float_t *real, py_float_t *imag) {
    if (arg == py_const_false) {
        *real = 0;
        *imag = 0;
    } else if (arg == py_const_true) {
        *real = 1;
        *imag = 0;
    } else if (IS_SMALL_INT(arg)) {
        *real = FROM_SMALL_INT(arg);
        *imag = 0;
    } else if (IS_O(arg, O_FLOAT)) {
        *real = ((py_obj_base_t*)arg)->u_float;
        *imag = 0;
    } else if (IS_O(arg, O_COMPLEX)) {
        *real = ((py_obj_base_t*)arg)->u_complex.real;
        *imag = ((py_obj_base_t*)arg)->u_complex.imag;
    } else {
        assert(0);
        *real = 0;
        *imag = 0;
    }
}
#endif

qstr py_obj_get_qstr(py_obj_t arg) {
    if (IS_O(arg, O_STR)) {
        return ((py_obj_base_t*)arg)->u_str;
    } else {
        assert(0);
        return 0;
    }
}

py_obj_t *py_obj_get_array_fixed_n(py_obj_t o_in, machine_int_t n) {
    if (IS_O(o_in, O_TUPLE) || IS_O(o_in, O_LIST)) {
        py_obj_base_t *o = o_in;
        if (o->u_tuple_list.len != n) {
            nlr_jump(py_obj_new_exception_2(q_IndexError, "requested length %d but object has length %d", (void*)n, (void*)o->u_tuple_list.len));
        }
        return o->u_tuple_list.items;
    } else {
        nlr_jump(py_obj_new_exception_2(q_TypeError, "object '%s' is not a tuple or list", py_obj_get_type_str(o_in), NULL));
    }
}

void py_user_get_data(py_obj_t o, machine_uint_t *data1, machine_uint_t *data2) {
    assert(IS_O(o, O_USER));
    if (data1 != NULL) {
        *data1 = ((py_obj_base_t*)o)->u_user.data1;
    }
    if (data2 != NULL) {
        *data2 = ((py_obj_base_t*)o)->u_user.data2;
    }
}

void py_user_set_data(py_obj_t o, machine_uint_t data1, machine_uint_t data2) {
    assert(IS_O(o, O_USER));
    ((py_obj_base_t*)o)->u_user.data1 = data1;
    ((py_obj_base_t*)o)->u_user.data2 = data2;
}

void printf_wrapper(void *env, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void vstr_printf_wrapper(void *env, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vstr_vprintf(env, fmt, args);
    va_end(args);
}

void py_obj_print_helper(void (*print)(void *env, const char *fmt, ...), void *env, py_obj_t o_in) {
    if (IS_SMALL_INT(o_in)) {
        print(env, "%d", (int)FROM_SMALL_INT(o_in));
    } else {
        py_obj_base_t *o = o_in;
        switch (o->kind) {
            case O_CONST:
                print(env, "%s", o->id);
                break;
            case O_STR:
                // TODO need to escape chars etc
                print(env, "'%s'", qstr_str(o->u_str));
                break;
#if MICROPY_ENABLE_FLOAT
            case O_FLOAT:
                print(env, "%.8g", o->u_float);
                break;
            case O_COMPLEX:
                if (o->u_complex.real == 0) {
                    print(env, "%.8gj", o->u_complex.imag);
                } else {
                    print(env, "(%.8g+%.8gj)", o->u_complex.real, o->u_complex.imag);
                }
                break;
#endif
            case O_EXCEPTION_0:
                print(env, "%s", qstr_str(o->u_exc0.id));
                break;
            case O_EXCEPTION_N:
                print(env, "%s: ", qstr_str(o->u_exc_n.id));
                print(env, o->u_exc_n.args[0], o->u_exc_n.args[1], o->u_exc_n.args[2]);
                break;
            case O_GEN_INSTANCE:
                print(env, "<generator object 'fun-name' at %p>", o);
                break;
            case O_TUPLE:
                print(env, "(");
                for (int i = 0; i < o->u_tuple_list.len; i++) {
                    if (i > 0) {
                        print(env, ", ");
                    }
                    py_obj_print_helper(print, env, o->u_tuple_list.items[i]);
                }
                if (o->u_tuple_list.len == 1) {
                    print(env, ",");
                }
                print(env, ")");
                break;
            case O_LIST:
                print(env, "[");
                for (int i = 0; i < o->u_tuple_list.len; i++) {
                    if (i > 0) {
                        print(env, ", ");
                    }
                    py_obj_print_helper(print, env, o->u_tuple_list.items[i]);
                }
                print(env, "]");
                break;
            case O_SET:
            {
                bool first = true;
                print(env, "{");
                for (int i = 0; i < o->u_set.alloc; i++) {
                    if (o->u_set.table[i] != NULL) {
                        if (!first) {
                            print(env, ", ");
                        }
                        first = false;
                        py_obj_print_helper(print, env, o->u_set.table[i]);
                    }
                }
                print(env, "}");
                break;
            }
            case O_MAP:
            {
                bool first = true;
                print(env, "{");
                for (int i = 0; i < o->u_map.alloc; i++) {
                    if (o->u_map.table[i].key != NULL) {
                        if (!first) {
                            print(env, ", ");
                        }
                        first = false;
                        py_obj_print_helper(print, env, o->u_map.table[i].key);
                        print(env, ": ");
                        py_obj_print_helper(print, env, o->u_map.table[i].value);
                    }
                }
                print(env, "}");
                break;
            }
            case O_USER:
                o->u_user.info->print(o_in);
                break;
            default:
                print(env, "<? %d>", o->kind);
                assert(0);
        }
    }
}

py_obj_t rt_str_join(py_obj_t self_in, py_obj_t arg) {
    assert(IS_O(self_in, O_STR));
    py_obj_base_t *self = self_in;
    int required_len = strlen(qstr_str(self->u_str));

    // process arg, count required chars
    if (!IS_O(arg, O_TUPLE) && !IS_O(arg, O_LIST)) {
        goto bad_arg;
    }
    py_obj_base_t *tuple_list = arg;
    for (int i = 0; i < tuple_list->u_tuple_list.len; i++) {
        if (!IS_O(tuple_list->u_tuple_list.items[i], O_STR)) {
            goto bad_arg;
        }
        required_len += strlen(qstr_str(((py_obj_base_t*)tuple_list->u_tuple_list.items[i])->u_str));
    }

    // make joined string
    char *joined_str = m_new(char, required_len + 1);
    joined_str[0] = 0;
    for (int i = 0; i < tuple_list->u_tuple_list.len; i++) {
        const char *s2 = qstr_str(((py_obj_base_t*)tuple_list->u_tuple_list.items[i])->u_str);
        if (i > 0) {
            strcat(joined_str, qstr_str(self->u_str));
        }
        strcat(joined_str, s2);
    }
    return py_obj_new_str(qstr_from_str_take(joined_str));

bad_arg:
    nlr_jump(py_obj_new_exception_2(q_TypeError, "?str.join expecting a list of str's", NULL, NULL));
}

py_obj_t rt_str_format(int n_args, const py_obj_t* args) {
    assert(IS_O(args[0], O_STR));
    py_obj_base_t *self = args[0];

    const char *str = qstr_str(self->u_str);
    int arg_i = 1;
    vstr_t *vstr = vstr_new();
    for (; *str; str++) {
        if (*str == '{') {
            str++;
            if (*str == '{') {
                vstr_add_char(vstr, '{');
            } else if (*str == '}') {
                if (arg_i >= n_args) {
                    nlr_jump(py_obj_new_exception_2(q_IndexError, "tuple index out of range", NULL, NULL));
                }
                py_obj_print_helper(vstr_printf_wrapper, vstr, args[arg_i]);
                arg_i++;
            }
        } else {
            vstr_add_char(vstr, *str);
        }
    }

    return py_obj_new_str(qstr_from_str_take(vstr->buf));
}

py_obj_t rt_list_append(py_obj_t self_in, py_obj_t arg) {
    assert(IS_O(self_in, O_LIST));
    py_obj_base_t *self = self_in;
    if (self->u_tuple_list.len >= self->u_tuple_list.alloc) {
        self->u_tuple_list.alloc *= 2;
        self->u_tuple_list.items = m_renew(py_obj_t, self->u_tuple_list.items, self->u_tuple_list.alloc);
    }
    self->u_tuple_list.items[self->u_tuple_list.len++] = arg;
    return py_const_none; // return None, as per CPython
}

py_obj_t rt_gen_instance_next(py_obj_t self_in) {
    py_obj_t ret = rt_iternext(self_in);
    if (ret == py_const_stop_iteration) {
        nlr_jump(py_obj_new_exception_0(qstr_from_str_static("StopIteration")));
    } else {
        return ret;
    }
}

typedef enum {
    PY_CODE_NONE,
    PY_CODE_BYTE,
    PY_CODE_NATIVE,
    PY_CODE_INLINE_ASM,
} py_code_kind_t;

typedef struct _py_code_t {
    py_code_kind_t kind;
    int n_args;
    int n_locals;
    int n_stack;
    bool is_generator;
    union {
        struct {
            byte *code;
            uint len;
        } u_byte;
        struct {
            py_fun_t fun;
        } u_native;
        struct {
            void *fun;
        } u_inline_asm;
    };
} py_code_t;

static int next_unique_code_id;
static py_code_t *unique_codes;

py_obj_t fun_str_join;
py_obj_t fun_str_format;
py_obj_t fun_list_append;
py_obj_t fun_gen_instance_next;

py_obj_t py_builtin___repl_print__(py_obj_t o) {
    if (o != py_const_none) {
        py_obj_print(o);
        printf("\n");
    }
    return py_const_none;
}

py_obj_t py_builtin_print(int n_args, const py_obj_t* args) {
    for (int i = 0; i < n_args; i++) {
        if (i > 0) {
            printf(" ");
        }
        if (IS_O(args[i], O_STR)) {
            // special case, print string raw
            printf("%s", qstr_str(((py_obj_base_t*)args[i])->u_str));
        } else {
            // print the object Python style
            py_obj_print(args[i]);
        }
    }
    printf("\n");
    return py_const_none;
}

py_obj_t py_builtin_len(py_obj_t o_in) {
    py_small_int_t len = 0;
    if (IS_O(o_in, O_TUPLE) || IS_O(o_in, O_LIST)) {
        py_obj_base_t *o = o_in;
        len = o->u_tuple_list.len;
    } else if (IS_O(o_in, O_MAP)) {
        py_obj_base_t *o = o_in;
        len = o->u_map.used;
    } else {
        assert(0);
    }
    return TO_SMALL_INT(len);
}

py_obj_t py_builtin_abs(py_obj_t o_in) {
    if (IS_SMALL_INT(o_in)) {
        py_small_int_t val = FROM_SMALL_INT(o_in);
        if (val < 0) {
            val = -val;
        }
        return TO_SMALL_INT(val);
    } else if (IS_O(o_in, O_FLOAT)) {
        py_obj_base_t *o = o_in;
        // TODO check for NaN etc
        if (o->u_float < 0) {
            return py_obj_new_float(-o->u_float);
        } else {
            return o_in;
        }
    } else if (IS_O(o_in, O_COMPLEX)) {
        py_obj_base_t *o = o_in;
        return py_obj_new_float(sqrt(o->u_complex.real*o->u_complex.real + o->u_complex.imag*o->u_complex.imag));
    } else {
        assert(0);
        return py_const_none;
    }
}

py_obj_t py_builtin___build_class__(py_obj_t o_class_fun, py_obj_t o_class_name) {
    // we differ from CPython: we set the new __locals__ object here
    py_map_t *old_locals = map_locals;
    py_map_t *class_locals = py_map_new(MAP_QSTR, 0);
    map_locals = class_locals;

    // call the class code
    rt_call_function_1(o_class_fun, (py_obj_t)0xdeadbeef);

    // restore old __locals__ object
    map_locals = old_locals;

    // create and return the new class
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_CLASS;
    o->u_class.locals = class_locals;
    return o;
}

py_obj_t py_builtin_range(py_obj_t o_arg) {
    return py_obj_new_range(0, py_obj_get_int(o_arg), 1);
}

#ifdef WRITE_NATIVE
FILE *fp_native = NULL;
#endif

void rt_init(void) {
    q_append = qstr_from_str_static("append");
    q_join = qstr_from_str_static("join");
    q_format = qstr_from_str_static("format");
    q___build_class__ = qstr_from_str_static("__build_class__");
    q___next__ = qstr_from_str_static("__next__");
    q_AttributeError = qstr_from_str_static("AttributeError");
    q_IndexError = qstr_from_str_static("IndexError");
    q_KeyError = qstr_from_str_static("KeyError");
    q_NameError = qstr_from_str_static("NameError");
    q_TypeError = qstr_from_str_static("TypeError");
    q_SyntaxError = qstr_from_str_static("SyntaxError");

    py_const_none = py_obj_new_const("None");
    py_const_false = py_obj_new_const("False");
    py_const_true = py_obj_new_const("True");
    py_const_stop_iteration = py_obj_new_const("StopIteration");

    // locals = globals for outer module (see Objects/frameobject.c/PyFrame_New())
    map_locals = map_globals = py_map_new(MAP_QSTR, 1);
    py_qstr_map_lookup(map_globals, qstr_from_str_static("__name__"), true)->value = py_obj_new_str(qstr_from_str_static("__main__"));

    py_map_init(&map_builtins, MAP_QSTR, 3);
    py_qstr_map_lookup(&map_builtins, qstr_from_str_static("__repl_print__"), true)->value = rt_make_function_1(py_builtin___repl_print__);
    py_qstr_map_lookup(&map_builtins, qstr_from_str_static("print"), true)->value = rt_make_function_var(0, py_builtin_print);
    py_qstr_map_lookup(&map_builtins, qstr_from_str_static("len"), true)->value = rt_make_function_1(py_builtin_len);
    py_qstr_map_lookup(&map_builtins, qstr_from_str_static("abs"), true)->value = rt_make_function_1(py_builtin_abs);
    py_qstr_map_lookup(&map_builtins, q___build_class__, true)->value = rt_make_function_2(py_builtin___build_class__);
    py_qstr_map_lookup(&map_builtins, qstr_from_str_static("range"), true)->value = rt_make_function_1(py_builtin_range);

    next_unique_code_id = 2; // 1 is reserved for the __main__ module scope
    unique_codes = NULL;

    fun_str_join = rt_make_function_2(rt_str_join);
    fun_str_format = rt_make_function_var(1, rt_str_format);
    fun_list_append = rt_make_function_2(rt_list_append);
    fun_gen_instance_next = rt_make_function_1(rt_gen_instance_next);

#ifdef WRITE_NATIVE
    fp_native = fopen("out-native", "wb");
#endif
}

void rt_deinit(void) {
#ifdef WRITE_NATIVE
    if (fp_native != NULL) {
        fclose(fp_native);
    }
#endif
}

int rt_get_unique_code_id(bool is_main_module) {
    if (is_main_module) {
        return 1;
    } else {
        return next_unique_code_id++;
    }
}

static void alloc_unique_codes(void) {
    if (unique_codes == NULL) {
        unique_codes = m_new(py_code_t, next_unique_code_id + 10); // XXX hack until we fix the REPL allocation problem
        for (int i = 0; i < next_unique_code_id; i++) {
            unique_codes[i].kind = PY_CODE_NONE;
        }
    }
}

void rt_assign_byte_code(int unique_code_id, byte *code, uint len, int n_args, int n_locals, int n_stack, bool is_generator) {
    alloc_unique_codes();

    assert(unique_code_id < next_unique_code_id);
    unique_codes[unique_code_id].kind = PY_CODE_BYTE;
    unique_codes[unique_code_id].n_args = n_args;
    unique_codes[unique_code_id].n_locals = n_locals;
    unique_codes[unique_code_id].n_stack = n_stack;
    unique_codes[unique_code_id].is_generator = is_generator;
    unique_codes[unique_code_id].u_byte.code = code;
    unique_codes[unique_code_id].u_byte.len = len;

    DEBUG_printf("assign byte code: id=%d code=%p len=%u n_args=%d\n", unique_code_id, code, len, n_args);
}

void rt_assign_native_code(int unique_code_id, py_fun_t fun, uint len, int n_args) {
    alloc_unique_codes();

    assert(1 <= unique_code_id && unique_code_id < next_unique_code_id);
    unique_codes[unique_code_id].kind = PY_CODE_NATIVE;
    unique_codes[unique_code_id].n_args = n_args;
    unique_codes[unique_code_id].n_locals = 0;
    unique_codes[unique_code_id].n_stack = 0;
    unique_codes[unique_code_id].is_generator = false;
    unique_codes[unique_code_id].u_native.fun = fun;

#ifdef DEBUG_PRINT
    DEBUG_printf("assign native code: id=%d fun=%p len=%u n_args=%d\n", unique_code_id, fun, len, n_args);
    byte *fun_data = (byte*)(((machine_uint_t)fun) & (~1)); // need to clear lower bit in case it's thumb code
    for (int i = 0; i < 128 && i < len; i++) {
        if (i > 0 && i % 16 == 0) {
            DEBUG_printf("\n");
        }
        DEBUG_printf(" %02x", fun_data[i]);
    }
    DEBUG_printf("\n");

#ifdef WRITE_NATIVE
    if (fp_native != NULL) {
        fwrite(fun_data, len, 1, fp_native);
        fflush(fp_native);
    }
#endif
#endif
}

void rt_assign_inline_asm_code(int unique_code_id, py_fun_t fun, uint len, int n_args) {
    alloc_unique_codes();

    assert(1 <= unique_code_id && unique_code_id < next_unique_code_id);
    unique_codes[unique_code_id].kind = PY_CODE_INLINE_ASM;
    unique_codes[unique_code_id].n_args = n_args;
    unique_codes[unique_code_id].n_locals = 0;
    unique_codes[unique_code_id].n_stack = 0;
    unique_codes[unique_code_id].is_generator = false;
    unique_codes[unique_code_id].u_inline_asm.fun = fun;

#ifdef DEBUG_PRINT
    DEBUG_printf("assign inline asm code: id=%d fun=%p len=%u n_args=%d\n", unique_code_id, fun, len, n_args);
    byte *fun_data = (byte*)(((machine_uint_t)fun) & (~1)); // need to clear lower bit in case it's thumb code
    for (int i = 0; i < 128 && i < len; i++) {
        if (i > 0 && i % 16 == 0) {
            DEBUG_printf("\n");
        }
        DEBUG_printf(" %02x", fun_data[i]);
    }
    DEBUG_printf("\n");

#ifdef WRITE_NATIVE
    if (fp_native != NULL) {
        fwrite(fun_data, len, 1, fp_native);
    }
#endif
#endif
}

bool py_obj_is_callable(py_obj_t o_in) {
    if (IS_SMALL_INT(o_in)) {
        return false;
    } else {
        py_obj_base_t *o = o_in;
        switch (o->kind) {
            case O_FUN_0:
            case O_FUN_1:
            case O_FUN_2:
            case O_FUN_VAR:
            case O_FUN_N:
            case O_FUN_BC:
            case O_FUN_ASM:
            // what about O_CLASS, and an O_OBJ that has a __call__ method?
                return true;
            default:
                return false;
        }
    }
}

void py_obj_print(py_obj_t o_in) {
    py_obj_print_helper(printf_wrapper, NULL, o_in);
}

#define PARSE_DEC_IN_INTG (1)
#define PARSE_DEC_IN_FRAC (2)
#define PARSE_DEC_IN_EXP  (3)

py_obj_t rt_load_const_dec(qstr qstr) {
#if MICROPY_ENABLE_FLOAT
    DEBUG_OP_printf("load '%s'\n", qstr_str(qstr));
    const char *s = qstr_str(qstr);
    int in = PARSE_DEC_IN_INTG;
    py_float_t dec_val = 0;
    bool exp_neg = false;
    int exp_val = 0;
    int exp_extra = 0;
    bool imag = false;
    for (; *s; s++) {
        int dig = *s;
        if ('0' <= dig && dig <= '9') {
            dig -= '0';
            if (in == PARSE_DEC_IN_EXP) {
                exp_val = 10 * exp_val + dig;
            } else {
                dec_val = 10 * dec_val + dig;
                if (in == PARSE_DEC_IN_FRAC) {
                    exp_extra -= 1;
                }
            }
        } else if (in == PARSE_DEC_IN_INTG && dig == '.') {
            in = PARSE_DEC_IN_FRAC;
        } else if (in != PARSE_DEC_IN_EXP && (dig == 'E' || dig == 'e')) {
            in = PARSE_DEC_IN_EXP;
            if (s[1] == '+') {
                s++;
            } else if (s[1] == '-') {
                s++;
                exp_neg = true;
            }
        } else if (dig == 'J' || dig == 'j') {
            s++;
            imag = true;
            break;
        } else {
            // unknown character
            break;
        }
    }
    if (*s != 0) {
        nlr_jump(py_obj_new_exception_2(q_SyntaxError, "invalid syntax for number", NULL, NULL));
    }
    if (exp_neg) {
        exp_val = -exp_val;
    }
    exp_val += exp_extra;
    for (; exp_val > 0; exp_val--) {
        dec_val *= 10;
    }
    for (; exp_val < 0; exp_val++) {
        dec_val *= 0.1;
    }
    if (imag) {
        return py_obj_new_complex(0, dec_val);
    } else {
        return py_obj_new_float(dec_val);
    }
#else
    nlr_jump(py_obj_new_exception_2(q_SyntaxError, "decimal numbers not supported", NULL, NULL));
#endif
}

py_obj_t rt_load_const_str(qstr qstr) {
    DEBUG_OP_printf("load '%s'\n", qstr_str(qstr));
    return py_obj_new_str(qstr);
}

py_obj_t rt_load_name(qstr qstr) {
    // logic: search locals, globals, builtins
    DEBUG_OP_printf("load name %s\n", qstr_str(qstr));
    py_map_elem_t *elem = py_qstr_map_lookup(map_locals, qstr, false);
    if (elem == NULL) {
        elem = py_qstr_map_lookup(map_globals, qstr, false);
        if (elem == NULL) {
            elem = py_qstr_map_lookup(&map_builtins, qstr, false);
            if (elem == NULL) {
                nlr_jump(py_obj_new_exception_2(q_NameError, "name '%s' is not defined", qstr_str(qstr), NULL));
            }
        }
    }
    return elem->value;
}

py_obj_t rt_load_global(qstr qstr) {
    // logic: search globals, builtins
    DEBUG_OP_printf("load global %s\n", qstr_str(qstr));
    py_map_elem_t *elem = py_qstr_map_lookup(map_globals, qstr, false);
    if (elem == NULL) {
        elem = py_qstr_map_lookup(&map_builtins, qstr, false);
        if (elem == NULL) {
            nlr_jump(py_obj_new_exception_2(q_NameError, "name '%s' is not defined", qstr_str(qstr), NULL));
        }
    }
    return elem->value;
}

py_obj_t rt_load_build_class(void) {
    DEBUG_OP_printf("load_build_class\n");
    py_map_elem_t *elem = py_qstr_map_lookup(&map_builtins, q___build_class__, false);
    if (elem == NULL) {
        printf("name doesn't exist: __build_class__\n");
        assert(0);
    }
    return elem->value;
}

void rt_store_name(qstr qstr, py_obj_t obj) {
    DEBUG_OP_printf("store name %s <- %p\n", qstr_str(qstr), obj);
    py_qstr_map_lookup(map_locals, qstr, true)->value = obj;
}

void rt_store_global(qstr qstr, py_obj_t obj) {
    DEBUG_OP_printf("store global %s <- %p\n", qstr_str(qstr), obj);
    py_qstr_map_lookup(map_globals, qstr, true)->value = obj;
}

py_obj_t rt_unary_op(int op, py_obj_t arg) {
    DEBUG_OP_printf("unary %d %p\n", op, arg);
    if (IS_SMALL_INT(arg)) {
        py_small_int_t val = FROM_SMALL_INT(arg);
        switch (op) {
            case RT_UNARY_OP_NOT: if (val != 0) { return py_const_true;} else { return py_const_false; }
            case RT_UNARY_OP_POSITIVE: break;
            case RT_UNARY_OP_NEGATIVE: val = -val; break;
            case RT_UNARY_OP_INVERT: val = ~val; break;
            default: assert(0); val = 0;
        }
        if (fit_small_int(val)) {
            return TO_SMALL_INT(val);
        }
#if MICROPY_ENABLE_FLOAT
    } else if (IS_O(arg, O_FLOAT)) {
        py_float_t val = py_obj_get_float(arg);
        switch (op) {
            case RT_UNARY_OP_NOT: if (val != 0) { return py_const_true;} else { return py_const_false; }
            case RT_UNARY_OP_POSITIVE: break;
            case RT_UNARY_OP_NEGATIVE: val = -val; break;
            case RT_UNARY_OP_INVERT: nlr_jump(py_obj_new_exception_2(q_TypeError, "bad operand type for unary ~: 'float'", NULL, NULL));
            default: assert(0); val = 0;
        }
        return py_obj_new_float(val);
#endif
    }
    assert(0);
    return py_const_none;
}

uint get_index(py_obj_base_t *base, py_obj_t index) {
    // assumes base is O_TUPLE or O_LIST
    // TODO False and True are considered 0 and 1 for indexing purposes
    int len = base->u_tuple_list.len;
    if (IS_SMALL_INT(index)) {
        int i = FROM_SMALL_INT(index);
        if (i < 0) {
            i += len;
        }
        if (i < 0 || i >= len) {
            nlr_jump(py_obj_new_exception_2(q_IndexError, "%s index out of range", py_obj_get_type_str(base), NULL));
        }
        return i;
    } else {
        nlr_jump(py_obj_new_exception_2(q_TypeError, "%s indices must be integers, not %s", py_obj_get_type_str(base), py_obj_get_type_str(index)));
    }
}

py_obj_t rt_binary_op(int op, py_obj_t lhs, py_obj_t rhs) {
    DEBUG_OP_printf("binary %d %p %p\n", op, lhs, rhs);
    if (op == RT_BINARY_OP_SUBSCR) {
        if ((IS_O(lhs, O_TUPLE) || IS_O(lhs, O_LIST))) {
            // tuple/list load
            uint index = get_index(lhs, rhs);
            return ((py_obj_base_t*)lhs)->u_tuple_list.items[index];
        } else if (IS_O(lhs, O_MAP)) {
            // map load
            py_map_elem_t *elem = py_map_lookup(lhs, rhs, false);
            if (elem == NULL) {
                nlr_jump(py_obj_new_exception_2(q_KeyError, "<value>", NULL, NULL));
            } else {
                return elem->value;
            }
        } else {
            assert(0);
        }
    } else if (IS_SMALL_INT(lhs) && IS_SMALL_INT(rhs)) {
        py_small_int_t lhs_val = FROM_SMALL_INT(lhs);
        py_small_int_t rhs_val = FROM_SMALL_INT(rhs);
        switch (op) {
            case RT_BINARY_OP_OR:
            case RT_BINARY_OP_INPLACE_OR: lhs_val |= rhs_val; break;
            case RT_BINARY_OP_XOR:
            case RT_BINARY_OP_INPLACE_XOR: lhs_val ^= rhs_val; break;
            case RT_BINARY_OP_AND:
            case RT_BINARY_OP_INPLACE_AND: lhs_val &= rhs_val; break;
            case RT_BINARY_OP_LSHIFT:
            case RT_BINARY_OP_INPLACE_LSHIFT: lhs_val <<= rhs_val; break;
            case RT_BINARY_OP_RSHIFT:
            case RT_BINARY_OP_INPLACE_RSHIFT: lhs_val >>= rhs_val; break;
            case RT_BINARY_OP_ADD:
            case RT_BINARY_OP_INPLACE_ADD: lhs_val += rhs_val; break;
            case RT_BINARY_OP_SUBTRACT:
            case RT_BINARY_OP_INPLACE_SUBTRACT: lhs_val -= rhs_val; break;
            case RT_BINARY_OP_MULTIPLY:
            case RT_BINARY_OP_INPLACE_MULTIPLY: lhs_val *= rhs_val; break;
            case RT_BINARY_OP_FLOOR_DIVIDE:
            case RT_BINARY_OP_INPLACE_FLOOR_DIVIDE: lhs_val /= rhs_val; break;
#if MICROPY_ENABLE_FLOAT
            case RT_BINARY_OP_TRUE_DIVIDE:
            case RT_BINARY_OP_INPLACE_TRUE_DIVIDE: return py_obj_new_float((py_float_t)lhs_val / (py_float_t)rhs_val);
#endif
            case RT_BINARY_OP_POWER:
            case RT_BINARY_OP_INPLACE_POWER:
                // TODO
                if (rhs_val == 2) {
                    lhs_val = lhs_val * lhs_val;
                    break;
                }
            default: printf("%d\n", op); assert(0);
        }
        if (fit_small_int(lhs_val)) {
            return TO_SMALL_INT(lhs_val);
        }
#if MICROPY_ENABLE_FLOAT
    } else if (IS_O(lhs, O_COMPLEX) || IS_O(rhs, O_COMPLEX)) {
        py_float_t lhs_real, lhs_imag, rhs_real, rhs_imag;
        py_obj_get_complex(lhs, &lhs_real, &lhs_imag);
        py_obj_get_complex(rhs, &rhs_real, &rhs_imag);
        switch (op) {
            case RT_BINARY_OP_ADD:
            case RT_BINARY_OP_INPLACE_ADD:
                lhs_real += rhs_real;
                lhs_imag += rhs_imag;
                break;
            case RT_BINARY_OP_SUBTRACT:
            case RT_BINARY_OP_INPLACE_SUBTRACT:
                lhs_real -= rhs_real;
                lhs_imag -= rhs_imag;
                break;
            case RT_BINARY_OP_MULTIPLY:
            case RT_BINARY_OP_INPLACE_MULTIPLY:
            {
                py_float_t real = lhs_real * rhs_real - lhs_imag * rhs_imag;
                lhs_imag = lhs_real * rhs_imag + lhs_imag * rhs_real;
                lhs_real = real;
                break;
            }
            /* TODO floor(?) the value
            case RT_BINARY_OP_FLOOR_DIVIDE:
            case RT_BINARY_OP_INPLACE_FLOOR_DIVIDE: val = lhs_val / rhs_val; break;
            */
            /* TODO
            case RT_BINARY_OP_TRUE_DIVIDE:
            case RT_BINARY_OP_INPLACE_TRUE_DIVIDE: val = lhs_val / rhs_val; break;
            */
            default: printf("%d\n", op); assert(0);
        }
        return py_obj_new_complex(lhs_real, lhs_imag);
    } else if (IS_O(lhs, O_FLOAT) || IS_O(rhs, O_FLOAT)) {
        py_float_t lhs_val = py_obj_get_float(lhs);
        py_float_t rhs_val = py_obj_get_float(rhs);
        switch (op) {
            case RT_BINARY_OP_ADD:
            case RT_BINARY_OP_INPLACE_ADD: lhs_val += rhs_val; break;
            case RT_BINARY_OP_SUBTRACT:
            case RT_BINARY_OP_INPLACE_SUBTRACT: lhs_val -= rhs_val; break;
            case RT_BINARY_OP_MULTIPLY:
            case RT_BINARY_OP_INPLACE_MULTIPLY: lhs_val *= rhs_val; break;
            /* TODO floor(?) the value
            case RT_BINARY_OP_FLOOR_DIVIDE:
            case RT_BINARY_OP_INPLACE_FLOOR_DIVIDE: val = lhs_val / rhs_val; break;
            */
            case RT_BINARY_OP_TRUE_DIVIDE:
            case RT_BINARY_OP_INPLACE_TRUE_DIVIDE: lhs_val /= rhs_val; break;
            default: printf("%d\n", op); assert(0);
        }
        return py_obj_new_float(lhs_val);
#endif
    } else if (IS_O(lhs, O_STR) && IS_O(rhs, O_STR)) {
        const char *lhs_str = qstr_str(((py_obj_base_t*)lhs)->u_str);
        const char *rhs_str = qstr_str(((py_obj_base_t*)rhs)->u_str);
        char *val;
        switch (op) {
            case RT_BINARY_OP_ADD:
            case RT_BINARY_OP_INPLACE_ADD: val = m_new(char, strlen(lhs_str) + strlen(rhs_str) + 1); strcpy(val, lhs_str); strcat(val, rhs_str); break;
            default: printf("%d\n", op); assert(0); val = NULL;
        }
        return py_obj_new_str(qstr_from_str_take(val));
    }
    assert(0);
    return py_const_none;
}

py_obj_t rt_compare_op(int op, py_obj_t lhs, py_obj_t rhs) {
    DEBUG_OP_printf("compare %d %p %p\n", op, lhs, rhs);

    // deal with == and !=
    if (op == RT_COMPARE_OP_EQUAL || op == RT_COMPARE_OP_NOT_EQUAL) {
        if (py_obj_equal(lhs, rhs)) {
            if (op == RT_COMPARE_OP_EQUAL) {
                return py_const_true;
            } else {
                return py_const_false;
            }
        } else {
            if (op == RT_COMPARE_OP_EQUAL) {
                return py_const_false;
            } else {
                return py_const_true;
            }
        }
    }

    // deal with small ints
    if (IS_SMALL_INT(lhs) && IS_SMALL_INT(rhs)) {
        py_small_int_t lhs_val = FROM_SMALL_INT(lhs);
        py_small_int_t rhs_val = FROM_SMALL_INT(rhs);
        int cmp;
        switch (op) {
            case RT_COMPARE_OP_LESS: cmp = lhs_val < rhs_val; break;
            case RT_COMPARE_OP_MORE: cmp = lhs_val > rhs_val; break;
            case RT_COMPARE_OP_LESS_EQUAL: cmp = lhs_val <= rhs_val; break;
            case RT_COMPARE_OP_MORE_EQUAL: cmp = lhs_val >= rhs_val; break;
            default: assert(0); cmp = 0;
        }
        if (cmp) {
            return py_const_true;
        } else {
            return py_const_false;
        }
    }

#if MICROPY_ENABLE_FLOAT
    // deal with floats
    if (IS_O(lhs, O_FLOAT) || IS_O(rhs, O_FLOAT)) {
        py_float_t lhs_val = py_obj_get_float(lhs);
        py_float_t rhs_val = py_obj_get_float(rhs);
        int cmp;
        switch (op) {
            case RT_COMPARE_OP_LESS: cmp = lhs_val < rhs_val; break;
            case RT_COMPARE_OP_MORE: cmp = lhs_val > rhs_val; break;
            case RT_COMPARE_OP_LESS_EQUAL: cmp = lhs_val <= rhs_val; break;
            case RT_COMPARE_OP_MORE_EQUAL: cmp = lhs_val >= rhs_val; break;
            default: assert(0); cmp = 0;
        }
        if (cmp) {
            return py_const_true;
        } else {
            return py_const_false;
        }
    }
#endif

    // not implemented
    assert(0);
    return py_const_none;
}

py_obj_t rt_make_function_from_id(int unique_code_id) {
    DEBUG_OP_printf("make_function_from_id %d\n", unique_code_id);
    if (unique_code_id < 1 || unique_code_id >= next_unique_code_id) {
        // illegal code id
        return py_const_none;
    }
    py_code_t *c = &unique_codes[unique_code_id];
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    switch (c->kind) {
        case PY_CODE_BYTE:
            o->kind = O_FUN_BC;
            o->u_fun_bc.n_args = c->n_args;
            o->u_fun_bc.n_state = c->n_locals + c->n_stack;
            o->u_fun_bc.code = c->u_byte.code;
            break;
        case PY_CODE_NATIVE:
            switch (c->n_args) {
                case 0: o->kind = O_FUN_0; break;
                case 1: o->kind = O_FUN_1; break;
                case 2: o->kind = O_FUN_2; break;
                default: assert(0);
            }
            o->u_fun.fun = c->u_native.fun;
            break;
        case PY_CODE_INLINE_ASM:
            o->kind = O_FUN_ASM;
            o->u_fun_asm.n_args = c->n_args;
            o->u_fun_asm.fun = c->u_inline_asm.fun;
            break;
        default:
            assert(0);
    }

    // check for generator functions and if so wrap in generator object
    if (c->is_generator) {
        py_obj_base_t *o2 = m_new(py_obj_base_t, 1);
        o2->kind = O_GEN_WRAP;
        // we have at least 3 locals so the bc can write back fast[0,1,2] safely; should improve how this is done
        o2->u_gen_wrap.n_state = (c->n_locals < 3 ? 3 : c->n_locals) + c->n_stack;
        o2->u_gen_wrap.fun = o;
        o = o2;
    }

    return o;
}

py_obj_t rt_make_function_0(py_fun_0_t fun) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_FUN_0;
    o->u_fun.fun = fun;
    return o;
}

py_obj_t rt_make_function_1(py_fun_1_t fun) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_FUN_1;
    o->u_fun.fun = fun;
    return o;
}

py_obj_t rt_make_function_2(py_fun_2_t fun) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_FUN_2;
    o->u_fun.fun = fun;
    return o;
}

py_obj_t rt_make_function(int n_args, py_fun_t code) {
    // assumes code is a pointer to a py_fun_t (i think this is safe...)
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_FUN_N;
    o->u_fun.n_args = n_args;
    o->u_fun.fun = code;
    return o;
}

py_obj_t rt_make_function_var(int n_fixed_args, py_fun_var_t f) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_FUN_VAR;
    o->u_fun.n_args = n_fixed_args;
    o->u_fun.fun = f;
    return o;
}

py_obj_t rt_call_function_0(py_obj_t fun) {
    return rt_call_function_n(fun, 0, NULL);
}

py_obj_t rt_call_function_1(py_obj_t fun, py_obj_t arg) {
    return rt_call_function_n(fun, 1, &arg);
}

py_obj_t rt_call_function_2(py_obj_t fun, py_obj_t arg1, py_obj_t arg2) {
    py_obj_t args[2];
    args[1] = arg1;
    args[0] = arg2;
    return rt_call_function_n(fun, 2, args);
}

typedef machine_uint_t (*inline_asm_fun_0_t)();
typedef machine_uint_t (*inline_asm_fun_1_t)(machine_uint_t);
typedef machine_uint_t (*inline_asm_fun_2_t)(machine_uint_t, machine_uint_t);
typedef machine_uint_t (*inline_asm_fun_3_t)(machine_uint_t, machine_uint_t, machine_uint_t);

// convert a Python object to a sensible value for inline asm
machine_uint_t rt_convert_obj_for_inline_asm(py_obj_t obj) {
    // TODO for byte_array, pass pointer to the array
    if (IS_SMALL_INT(obj)) {
        return FROM_SMALL_INT(obj);
    } else if (obj == py_const_none) {
        return 0;
    } else if (obj == py_const_false) {
        return 0;
    } else if (obj == py_const_true) {
        return 1;
    } else {
        py_obj_base_t *o = obj;
        switch (o->kind) {
            case O_STR:
                // pointer to the string (it's probably constant though!)
                return (machine_uint_t)qstr_str(o->u_str);

#if MICROPY_ENABLE_FLOAT
            case O_FLOAT:
                // convert float to int (could also pass in float registers)
                return (machine_int_t)o->u_float;
#endif

            case O_TUPLE:
            case O_LIST:
                // pointer to start of tuple/list (could pass length, but then could use len(x) for that)
                return (machine_uint_t)o->u_tuple_list.items;

            default:
                // just pass along a pointer to the object
                return (machine_uint_t)obj;
        }
    }
}

// convert a return value from inline asm to a sensible Python object
py_obj_t rt_convert_val_from_inline_asm(machine_uint_t val) {
    return TO_SMALL_INT(val);
}

// args are in reverse order in the array
py_obj_t rt_call_function_n(py_obj_t fun, int n_args, const py_obj_t *args) {
    int n_args_fun = 0;
    if (IS_O(fun, O_FUN_0)) {
        py_obj_base_t *o = fun;
        if (n_args != 0) {
            n_args_fun = 0;
            goto bad_n_args;
        }
        DEBUG_OP_printf("calling native %p()\n", o->u_fun.fun);
        return ((py_fun_0_t)o->u_fun.fun)();

    } else if (IS_O(fun, O_FUN_1)) {
        py_obj_base_t *o = fun;
        if (n_args != 1) {
            n_args_fun = 1;
            goto bad_n_args;
        }
        DEBUG_OP_printf("calling native %p(%p)\n", o->u_fun.fun, args[0]);
        return ((py_fun_1_t)o->u_fun.fun)(args[0]);

    } else if (IS_O(fun, O_FUN_2)) {
        py_obj_base_t *o = fun;
        if (n_args != 2) {
            n_args_fun = 2;
            goto bad_n_args;
        }
        DEBUG_OP_printf("calling native %p(%p, %p)\n", o->u_fun.fun, args[1], args[0]);
        return ((py_fun_2_t)o->u_fun.fun)(args[1], args[0]);

    // TODO O_FUN_N

    } else if (IS_O(fun, O_FUN_VAR)) {
        py_obj_base_t *o = fun;
        if (n_args < o->u_fun.n_args) {
            nlr_jump(py_obj_new_exception_2(q_TypeError, "<fun name>() missing %d required positional arguments: <list of names of params>", (const char*)(machine_int_t)(o->u_fun.n_args - n_args), NULL));
        }
        // TODO really the args need to be passed in as a Python tuple, as the form f(*[1,2]) can be used to pass var args
        py_obj_t *args_ordered = m_new(py_obj_t, n_args);
        for (int i = 0; i < n_args; i++) {
            args_ordered[i] = args[n_args - i - 1];
        }
        py_obj_t res = ((py_fun_var_t)o->u_fun.fun)(n_args, args_ordered);
        m_free(args_ordered);
        return res;

    } else if (IS_O(fun, O_FUN_BC)) {
        py_obj_base_t *o = fun;
        if (n_args != o->u_fun_bc.n_args) {
            n_args_fun = o->u_fun_bc.n_args;
            goto bad_n_args;
        }
        DEBUG_OP_printf("calling byte code %p(n_args=%d)\n", o->u_fun_bc.code, n_args);
        return py_execute_byte_code(o->u_fun_bc.code, args, n_args, o->u_fun_bc.n_state);

    } else if (IS_O(fun, O_FUN_ASM)) {
        py_obj_base_t *o = fun;
        if (n_args != o->u_fun_asm.n_args) {
            n_args_fun = o->u_fun_asm.n_args;
            goto bad_n_args;
        }
        DEBUG_OP_printf("calling inline asm %p(n_args=%d)\n", o->u_fun_asm.fun, n_args);
        machine_uint_t ret;
        if (n_args == 0) {
            ret = ((inline_asm_fun_0_t)o->u_fun_asm.fun)();
        } else if (n_args == 1) {
            ret = ((inline_asm_fun_1_t)o->u_fun_asm.fun)(rt_convert_obj_for_inline_asm(args[0]));
        } else if (n_args == 2) {
            ret = ((inline_asm_fun_2_t)o->u_fun_asm.fun)(rt_convert_obj_for_inline_asm(args[1]), rt_convert_obj_for_inline_asm(args[0]));
        } else if (n_args == 3) {
            ret = ((inline_asm_fun_3_t)o->u_fun_asm.fun)(rt_convert_obj_for_inline_asm(args[2]), rt_convert_obj_for_inline_asm(args[1]), rt_convert_obj_for_inline_asm(args[0]));
        } else {
            assert(0);
            ret = 0;
        }
        return rt_convert_val_from_inline_asm(ret);

    } else if (IS_O(fun, O_GEN_WRAP)) {
        py_obj_base_t *o = fun;
        py_obj_base_t *o_fun = o->u_gen_wrap.fun;
        assert(o_fun->kind == O_FUN_BC); // TODO
        if (n_args != o_fun->u_fun_bc.n_args) {
            n_args_fun = o_fun->u_fun_bc.n_args;
            goto bad_n_args;
        }
        py_obj_t *state = m_new(py_obj_t, 1 + o->u_gen_wrap.n_state);
        // put function object at first slot in state (to keep u_gen_instance small)
        state[0] = o_fun;
        // init args
        for (int i = 0; i < n_args; i++) {
            state[1 + i] = args[n_args - 1 - i];
        }
        py_obj_base_t *o2 = m_new(py_obj_base_t, 1);
        o2->kind = O_GEN_INSTANCE;
        o2->u_gen_instance.state = state;
        o2->u_gen_instance.ip = o_fun->u_fun_bc.code;
        o2->u_gen_instance.sp = state + o->u_gen_wrap.n_state;
        return o2;

    } else if (IS_O(fun, O_BOUND_METH)) {
        py_obj_base_t *o = fun;
        DEBUG_OP_printf("calling bound method %p(self=%p, n_args=%d)\n", o->u_bound_meth.meth, o->u_bound_meth.self, n_args);
        if (n_args == 0) {
            return rt_call_function_n(o->u_bound_meth.meth, 1, &o->u_bound_meth.self);
        } else if (n_args == 1) {
            py_obj_t args2[2];
            args2[1] = o->u_bound_meth.self;
            args2[0] = args[0];
            return rt_call_function_n(o->u_bound_meth.meth, 2, args2);
        } else {
            // TODO not implemented
            assert(0);
            return py_const_none;
            //return rt_call_function_2(o->u_bound_meth.meth, n_args + 1, o->u_bound_meth.self + args);
        }

    } else if (IS_O(fun, O_CLASS)) {
        // instantiate an instance of a class
        if (n_args != 0) {
            n_args_fun = 0;
            goto bad_n_args;
        }
        DEBUG_OP_printf("instantiate object of class %p with no args\n", fun);
        py_obj_base_t *o = m_new(py_obj_base_t, 1);
        o->kind = O_OBJ;
        o->u_obj.class = fun;
        o->u_obj.members = py_map_new(MAP_QSTR, 0);
        return o;

    } else {
        printf("fun %p %d\n", fun, ((py_obj_base_t*)fun)->kind);
        assert(0);
        return py_const_none;
    }

bad_n_args:
    nlr_jump(py_obj_new_exception_2(q_TypeError, "function takes %d positional arguments but %d were given", (const char*)(machine_int_t)n_args_fun, (const char*)(machine_int_t)n_args));
}

// args contains: arg(n_args-1)  arg(n_args-2)  ...  arg(0)  self/NULL  fun
// if n_args==0 then there are only self/NULL and fun
py_obj_t rt_call_method_n(int n_args, const py_obj_t *args) {
    DEBUG_OP_printf("call method %p(self=%p, n_args=%d)\n", args[n_args + 1], args[n_args], n_args);
    return rt_call_function_n(args[n_args + 1], n_args + ((args[n_args] == NULL) ? 0 : 1), args);
}

// items are in reverse order
py_obj_t rt_build_tuple(int n_args, py_obj_t *items) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_TUPLE;
    o->u_tuple_list.alloc = n_args < 4 ? 4 : n_args;
    o->u_tuple_list.len = n_args;
    o->u_tuple_list.items = m_new(py_obj_t, o->u_tuple_list.alloc);
    for (int i = 0; i < n_args; i++) {
        o->u_tuple_list.items[i] = items[n_args - i - 1];
    }
    return o;
}

// items are in reverse order
py_obj_t rt_build_list(int n_args, py_obj_t *items) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_LIST;
    o->u_tuple_list.alloc = n_args < 4 ? 4 : n_args;
    o->u_tuple_list.len = n_args;
    o->u_tuple_list.items = m_new(py_obj_t, o->u_tuple_list.alloc);
    for (int i = 0; i < n_args; i++) {
        o->u_tuple_list.items[i] = items[n_args - i - 1];
    }
    return o;
}

py_obj_t py_set_lookup(py_obj_t o_in, py_obj_t index, bool add_if_not_found) {
    assert(IS_O(o_in, O_SET));
    py_obj_base_t *o = o_in;
    int hash = py_obj_hash(index);
    int pos = hash % o->u_set.alloc;
    for (;;) {
        py_obj_t elem = o->u_set.table[pos];
        if (elem == NULL) {
            // not in table
            if (add_if_not_found) {
                if (o->u_set.used + 1 >= o->u_set.alloc) {
                    // not enough room in table, rehash it
                    int old_alloc = o->u_set.alloc;
                    py_obj_t *old_table = o->u_set.table;
                    o->u_set.alloc = get_doubling_prime_greater_or_equal_to(o->u_set.alloc + 1);
                    o->u_set.used = 0;
                    o->u_set.table = m_new(py_obj_t, o->u_set.alloc);
                    for (int i = 0; i < old_alloc; i++) {
                        if (old_table[i] != NULL) {
                            py_set_lookup(o, old_table[i], true);
                        }
                    }
                    m_free(old_table);
                    // restart the search for the new element
                    pos = hash % o->u_set.alloc;
                } else {
                    o->u_set.used += 1;
                    o->u_set.table[pos] = index;
                    return index;
                }
            } else {
                return NULL;
            }
        } else if (py_obj_equal(elem, index)) {
            // found it
            return elem;
        } else {
            // not yet found, keep searching in this table
            pos = (pos + 1) % o->u_set.alloc;
        }
    }
}

py_obj_t rt_build_set(int n_args, py_obj_t *items) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_SET;
    o->u_set.alloc = get_doubling_prime_greater_or_equal_to(n_args + 1);
    o->u_set.used = 0;
    o->u_set.table = m_new(py_obj_t, o->u_set.alloc);
    for (int i = 0; i < o->u_set.alloc; i++) {
        o->u_set.table[i] = NULL;
    }
    for (int i = 0; i < n_args; i++) {
        py_set_lookup(o, items[i], true);
    }
    return o;
}

py_obj_t rt_store_set(py_obj_t set, py_obj_t item) {
    py_set_lookup(set, item, true);
    return set;
}

py_obj_t rt_build_map(int n_args) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_MAP;
    py_map_init(&o->u_map, MAP_PY_OBJ, n_args);
    return o;
}

py_obj_t rt_store_map(py_obj_t map, py_obj_t key, py_obj_t value) {
    assert(IS_O(map, O_MAP)); // should always be
    py_map_lookup(map, key, true)->value = value;
    return map;
}

py_obj_t build_bound_method(py_obj_t self, py_obj_t meth) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_BOUND_METH;
    o->u_bound_meth.meth = meth;
    o->u_bound_meth.self = self;
    return o;
}

py_obj_t rt_load_attr(py_obj_t base, qstr attr) {
    DEBUG_OP_printf("load attr %s\n", qstr_str(attr));
    if (IS_O(base, O_LIST) && attr == q_append) {
        return build_bound_method(base, fun_list_append);
    } else if (IS_O(base, O_CLASS)) {
        py_obj_base_t *o = base;
        py_map_elem_t *elem = py_qstr_map_lookup(o->u_class.locals, attr, false);
        if (elem == NULL) {
            goto no_attr;
        }
        return elem->value;
    } else if (IS_O(base, O_OBJ)) {
        // logic: look in obj members then class locals (TODO check this against CPython)
        py_obj_base_t *o = base;
        py_map_elem_t *elem = py_qstr_map_lookup(o->u_obj.members, attr, false);
        if (elem != NULL) {
            // object member, always treated as a value
            return elem->value;
        }
        elem = py_qstr_map_lookup(o->u_obj.class->u_class.locals, attr, false);
        if (elem != NULL) {
            if (py_obj_is_callable(elem->value)) {
                // class member is callable so build a bound method
                return build_bound_method(base, elem->value);
            } else {
                // class member is a value, so just return that value
                return elem->value;
            }
        }
        goto no_attr;
    }

no_attr:
    nlr_jump(py_obj_new_exception_2(q_AttributeError, "'%s' object has no attribute '%s'", py_obj_get_type_str(base), qstr_str(attr)));
}

void rt_load_method(py_obj_t base, qstr attr, py_obj_t *dest) {
    DEBUG_OP_printf("load method %s\n", qstr_str(attr));
    if (IS_O(base, O_STR)) {
        if (attr == q_join) {
            dest[1] = fun_str_join;
            dest[0] = base;
            return;
        } else if (attr == q_format) {
            dest[1] = fun_str_format;
            dest[0] = base;
            return;
        }
    } else if (IS_O(base, O_GEN_INSTANCE) && attr == q___next__) {
        dest[1] = fun_gen_instance_next;
        dest[0] = base;
        return;
    } else if (IS_O(base, O_LIST) && attr == q_append) {
        dest[1] = fun_list_append;
        dest[0] = base;
        return;
    } else if (IS_O(base, O_OBJ)) {
        // logic: look in obj members then class locals (TODO check this against CPython)
        py_obj_base_t *o = base;
        py_map_elem_t *elem = py_qstr_map_lookup(o->u_obj.members, attr, false);
        if (elem != NULL) {
            // object member, always treated as a value
            dest[1] = elem->value;
            dest[0] = NULL;
            return;
        }
        elem = py_qstr_map_lookup(o->u_obj.class->u_class.locals, attr, false);
        if (elem != NULL) {
            if (py_obj_is_callable(elem->value)) {
                // class member is callable so build a bound method
                dest[1] = elem->value;
                dest[0] = base;
                return;
            } else {
                // class member is a value, so just return that value
                dest[1] = elem->value;
                dest[0] = NULL;
                return;
            }
        }
        goto no_attr;
    } else if (IS_O(base, O_USER)) {
        py_obj_base_t *o = base;
        const py_user_method_t *meth = &o->u_user.info->methods[0];
        for (; meth->name != NULL; meth++) {
            if (strcmp(meth->name, qstr_str(attr)) == 0) {
                if (meth->kind == 0) {
                    dest[1] = rt_make_function_1(meth->fun);
                } else if (meth->kind == 1) {
                    dest[1] = rt_make_function_2(meth->fun);
                } else {
                    assert(0);
                }
                dest[0] = base;
                return;
            }
        }
    }

no_attr:
    dest[1] = rt_load_attr(base, attr);
    dest[0] = NULL;
}

void rt_store_attr(py_obj_t base, qstr attr, py_obj_t value) {
    DEBUG_OP_printf("store attr %p.%s <- %p\n", base, qstr_str(attr), value);
    if (IS_O(base, O_CLASS)) {
        // TODO CPython allows STORE_ATTR to a class, but is this the correct implementation?
        py_obj_base_t *o = base;
        py_qstr_map_lookup(o->u_class.locals, attr, true)->value = value;
    } else if (IS_O(base, O_OBJ)) {
        // logic: look in class locals (no add) then obj members (add) (TODO check this against CPython)
        py_obj_base_t *o = base;
        py_map_elem_t *elem = py_qstr_map_lookup(o->u_obj.class->u_class.locals, attr, false);
        if (elem != NULL) {
            elem->value = value;
        } else {
            py_qstr_map_lookup(o->u_obj.members, attr, true)->value = value;
        }
    } else {
        printf("?AttributeError: '%s' object has no attribute '%s'\n", py_obj_get_type_str(base), qstr_str(attr));
        assert(0);
    }
}

void rt_store_subscr(py_obj_t base, py_obj_t index, py_obj_t value) {
    DEBUG_OP_printf("store subscr %p[%p] <- %p\n", base, index, value);
    if (IS_O(base, O_LIST)) {
        // list store
        uint i = get_index(base, index);
        ((py_obj_base_t*)base)->u_tuple_list.items[i] = value;
    } else if (IS_O(base, O_MAP)) {
        // map store
        py_map_lookup(base, index, true)->value = value;
    } else {
        assert(0);
    }
}

py_obj_t rt_getiter(py_obj_t o_in) {
    if (IS_O(o_in, O_GEN_INSTANCE)) {
        return o_in;
    } else if (IS_O(o_in, O_RANGE)) {
        py_obj_base_t *o = o_in;
        return py_obj_new_range_iterator(o->u_range.start, o->u_range.stop, o->u_range.step);
    } else if (IS_O(o_in, O_TUPLE)) {
        return py_obj_new_tuple_iterator(o_in, 0);
    } else if (IS_O(o_in, O_LIST)) {
        return py_obj_new_list_iterator(o_in, 0);
    } else {
        nlr_jump(py_obj_new_exception_2(q_TypeError, "'%s' object is not iterable", py_obj_get_type_str(o_in), NULL));
    }
}

py_obj_t rt_iternext(py_obj_t o_in) {
    if (IS_O(o_in, O_GEN_INSTANCE)) {
        py_obj_base_t *self = o_in;
        //py_obj_base_t *fun = self->u_gen_instance.state[0];
        //assert(fun->kind == O_FUN_BC);
        bool yield = py_execute_byte_code_2(&self->u_gen_instance.ip, &self->u_gen_instance.state[1], &self->u_gen_instance.sp);
        if (yield) {
            return *self->u_gen_instance.sp;
        } else {
            if (*self->u_gen_instance.sp == py_const_none) {
                return py_const_stop_iteration;
            } else {
                // TODO return StopIteration with value *self->u_gen_instance.sp
                return py_const_stop_iteration;
            }
        }

    } else if (IS_O(o_in, O_RANGE_IT)) {
        py_obj_base_t *o = o_in;
        if ((o->u_range_it.step > 0 && o->u_range_it.cur < o->u_range_it.stop) || (o->u_range_it.step < 0 && o->u_range_it.cur > o->u_range_it.stop)) {
            py_obj_t o_out = TO_SMALL_INT(o->u_range_it.cur);
            o->u_range_it.cur += o->u_range_it.step;
            return o_out;
        } else {
            return py_const_stop_iteration;
        }

    } else if (IS_O(o_in, O_TUPLE_IT) || IS_O(o_in, O_LIST_IT)) {
        py_obj_base_t *o = o_in;
        if (o->u_tuple_list_it.cur < o->u_tuple_list_it.obj->u_tuple_list.len) {
            py_obj_t o_out = o->u_tuple_list_it.obj->u_tuple_list.items[o->u_tuple_list_it.cur];
            o->u_tuple_list_it.cur += 1;
            return o_out;
        } else {
            return py_const_stop_iteration;
        }

    } else {
        nlr_jump(py_obj_new_exception_2(q_TypeError, "? '%s' object is not iterable", py_obj_get_type_str(o_in), NULL));
    }
}

// these must correspond to the respective enum
void *const rt_fun_table[RT_F_NUMBER_OF] = {
    rt_load_const_dec,
    rt_load_const_str,
    rt_load_name,
    rt_load_global,
    rt_load_build_class,
    rt_load_attr,
    rt_load_method,
    rt_store_name,
    rt_store_attr,
    rt_store_subscr,
    rt_is_true,
    rt_unary_op,
    rt_build_tuple,
    rt_build_list,
    rt_list_append,
    rt_build_map,
    rt_store_map,
    rt_build_set,
    rt_store_set,
    rt_make_function_from_id,
    rt_call_function_n,
    rt_call_method_n,
    rt_binary_op,
    rt_compare_op,
    rt_getiter,
    rt_iternext,
};

/*
void rt_f_vector(rt_fun_kind_t fun_kind) {
    (rt_f_table[fun_kind])();
}
*/

// temporary way of making C modules
// hack: use class to mimic a module

py_obj_t py_module_new(void) {
    py_obj_base_t *o = m_new(py_obj_base_t, 1);
    o->kind = O_CLASS;
    o->u_class.locals = py_map_new(MAP_QSTR, 0);
    return o;
}
