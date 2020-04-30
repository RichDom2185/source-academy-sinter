#include <sinter/config.h>

#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>

#include <sinter/nanbox.h>
#include <sinter/fault.h>
#include <sinter/heap.h>
#include <sinter/stack.h>
#include <sinter/debug.h>
#include <sinter/vm.h>

/**
 * This file contains the implementations (in C) of all 92 functions in the Source
 * standard library.
 */

static void unimpl(void) {
  SIBUGV("Unimplemented primitive function %02x at address 0x%tx\n", *(sistate.pc + 1), SISTATE_CURADDR);
  sifault(sinter_fault_invalid_program);
}

static void debug_display_argv(unsigned int argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  for (unsigned int i = 0; i < argc; ++i) {
    SIDEBUG("%d: ", i);
    SIDEBUG_NANBOX(argv[i]);
    SIDEBUG("\n");
  }
}

static void display_strobj(siheap_header_t *obj, bool is_error) {
  switch (obj->type) {
  case sitype_strconst:
    SIVMFN_PRINT((const char *) ((siheap_strconst_t *) obj)->string->data, is_error);
    break;
  case sitype_strpair: {
    siheap_strpair_t *pair = (siheap_strpair_t *) obj;
    display_strobj(pair->left, is_error);
    display_strobj(pair->right, is_error);
    break;
  }
  case sitype_string: {
    siheap_string_t *str = (siheap_string_t *) obj;
    SIVMFN_PRINT((const char *) str->string, is_error);
    break;
  }

  case sitype_array_data:
  case sitype_empty:
  case sitype_frame:
  case sitype_free:
  case sitype_env:
  case sitype_array:
  case sitype_function:
    break;
  }
}

static void display_nanbox(sinanbox_t v, bool is_error) {
  switch (NANBOX_GETTYPE(v)) {
  NANBOX_CASES_TINT
    SIVMFN_PRINT((int32_t) NANBOX_INT(v), is_error);
    break;
  case NANBOX_TBOOL:
    SIVMFN_PRINT(NANBOX_BOOL(v) ? "true" : "false", is_error);
    break;
  case NANBOX_TUNDEF:
    SIVMFN_PRINT("undefined", is_error);
    break;
  case NANBOX_TNULL:
    SIVMFN_PRINT("null", is_error);
    break;
  NANBOX_CASES_TPTR {
    siheap_header_t *obj = SIHEAP_NANBOXTOPTR(v);
    if (obj->flag_displayed) {
      SIVMFN_PRINT("...<circular>", is_error);
      return;
    }
    switch (obj->type) {
    case sitype_strconst:
    case sitype_strpair:
    case sitype_string:
      display_strobj(obj, is_error);
      break;
    case sitype_array: {
      siheap_array_t *a = (siheap_array_t *) obj;
      obj->flag_displayed = true; // mark the array so we don't recursively display it
      SIVMFN_PRINT("[", is_error);
      for (size_t i = 0; i < a->count; ++i) {
        if (i) {
          SIVMFN_PRINT(", ", is_error);
        }
        display_nanbox(a->data->data[i], is_error);
      }
      SIVMFN_PRINT("]", is_error);
      obj->flag_displayed = false;
      break;
    }
    case sitype_function:
      SIVMFN_PRINT("<function>", is_error);
      break;
    case sitype_array_data:
    case sitype_empty:
    case sitype_frame:
    case sitype_free:
    case sitype_env:
    default:
      SIBUGM("Unexpected object type\n");
      break;
    }
    break;
  }
  default:
    if (NANBOX_ISFLOAT(v)) {
      SIVMFN_PRINT(NANBOX_FLOAT(v), is_error);
    } else {
      SIBUGM("Unexpected type\n");
    }
    break;
  }
}

static void handle_display(unsigned int argc, sinanbox_t *argv, bool is_error) {
  if (argc < 1) {
    return;
  }

  if (argc > 1) {
    display_nanbox(argv[1], is_error);
    SIVMFN_PRINT(" ", is_error);
  }

  display_nanbox(argv[0], is_error);
  SIVMFN_PRINT("\n", is_error);
}

#define CHECK_ARGC(n) do { \
  if (argc < (n)) { \
    sifault(sinter_fault_function_arity); \
    return NANBOX_OFEMPTY(); \
  } \
} while (0)

static inline uint32_t nanbox_tou32(sinanbox_t v) {
  if (NANBOX_ISINT(v)) {
    return (uint32_t) NANBOX_INT(v);
  } else if (NANBOX_ISFLOAT(v)) {
    return (uint32_t) NANBOX_FLOAT(v);
  }
  sifault(sinter_fault_type);
  return 0;
}

static inline int32_t nanbox_toi32(sinanbox_t v) {
  if (NANBOX_ISINT(v)) {
    return NANBOX_INT(v);
  } else if (NANBOX_ISFLOAT(v)) {
    return (int32_t) NANBOX_FLOAT(v);
  }
  sifault(sinter_fault_type);
  return 0;
}

/******************************************************************************
 * Basic type-checking primitives
 ******************************************************************************/

static sinanbox_t sivmfn_prim_is_array(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  sinanbox_t v = *argv;
  return NANBOX_OFBOOL(NANBOX_ISPTR(v) && ((siheap_header_t *) SIHEAP_NANBOXTOPTR(v))->type == sitype_array);
}

static sinanbox_t sivmfn_prim_is_boolean(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  return NANBOX_OFBOOL(NANBOX_ISBOOL(*argv));
}

static sinanbox_t sivmfn_prim_is_function(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  sinanbox_t v = *argv;
  return NANBOX_OFBOOL((NANBOX_ISPTR(v) && ((siheap_header_t *) SIHEAP_NANBOXTOPTR(v))->type == sitype_function)
    || NANBOX_ISIFN(v));
}

static sinanbox_t sivmfn_prim_is_null(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  return NANBOX_OFBOOL(NANBOX_ISNULL(*argv));
}

static sinanbox_t sivmfn_prim_is_number(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  return NANBOX_OFBOOL(NANBOX_ISNUMERIC(*argv));
}

static sinanbox_t sivmfn_prim_is_stream(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_is_string(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  sinanbox_t v = *argv;
  return NANBOX_OFBOOL(NANBOX_ISPTR(v) && siheap_is_string(SIHEAP_NANBOXTOPTR(v)));
}

static sinanbox_t sivmfn_prim_is_undefined(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  return NANBOX_OFBOOL(NANBOX_ISUNDEF(*argv));
}

/******************************************************************************
 * Math primitives
 ******************************************************************************/

#define MATH_FN(name) static sinanbox_t sivmfn_prim_math_ ## name(uint8_t argc, sinanbox_t *argv) { \
  CHECK_ARGC(1); \
  return NANBOX_OFFLOAT(name ## f(NANBOX_TOFLOAT(*argv))); \
}

#define MATH_FN_2(name) static sinanbox_t sivmfn_prim_math_ ## name(uint8_t argc, sinanbox_t *argv) { \
  CHECK_ARGC(2); \
  return NANBOX_OFFLOAT(name ## f(NANBOX_TOFLOAT(argv[0]), NANBOX_TOFLOAT(argv[1]))); \
}

MATH_FN(acos)
MATH_FN(acosh)
MATH_FN(asin)
MATH_FN(asinh)
MATH_FN(atan)
MATH_FN(atanh)
MATH_FN(cbrt)
MATH_FN(cos)
MATH_FN(cosh)
MATH_FN(exp)
MATH_FN(expm1)
MATH_FN(log)
MATH_FN(log1p)
MATH_FN(log2)
MATH_FN(log10)
MATH_FN(sin)
MATH_FN(sinh)
MATH_FN(sqrt)
MATH_FN(tan)
MATH_FN(tanh)
MATH_FN_2(atan2)
MATH_FN_2(pow)

static sinanbox_t sivmfn_prim_math_hypot(uint8_t argc, sinanbox_t *argv) {
  // Adapted from https://github.com/v8/v8/blob/master/src/builtins/math.tq#L405
  if (argc == 0) {
    return NANBOX_OFINT(0);
  }

  float max = 0;
  for (unsigned int i = 0; i < argc; ++i) {
    if (NANBOX_IDENTICAL(argv[i], NANBOX_CANONICAL_NAN)) {
      return NANBOX_CANONICAL_NAN;
    }
    float v = NANBOX_TOFLOAT(argv[i]);
    if (v > max) {
      max = v;
    }
  }

  if (max == 0.0f) {
    return NANBOX_OFINT(0);
  }

  float sum = 0;
  float compensation = 0;
  for (unsigned int i = 0; i < argc; ++i) {
    float n = NANBOX_TOFLOAT(argv[i]) / max;
    float summand = n * n - compensation;
    float preliminary = sum + summand;
    compensation = (preliminary - sum) - summand;
    sum = preliminary;
  }

  return NANBOX_OFFLOAT(sqrtf(sum) * max);
}

static sinanbox_t sivmfn_prim_math_abs(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  sinanbox_t v = *argv;

  if (NANBOX_ISINT(v)) {
    return NANBOX_WRAP_INT(abs(NANBOX_INT(v)));
  } else if (NANBOX_ISFLOAT(v)) {
    return NANBOX_OFFLOAT(fabsf(NANBOX_FLOAT(v)));
  }

  sifault(sinter_fault_type);
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_math_clz32(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  uint32_t clzarg = nanbox_tou32(*argv);

  if (clzarg == 0) {
    return NANBOX_OFINT(32);
  }

  unsigned int ret;
#if UINT_MAX == UINT32_MAX
  ret = __builtin_clz(clzarg);
#elif ULONG_MAX == UINT32_MAX
  ret = __builtin_clzl(clzarg);
#elif ULLONG_MAX == UINT32_MAX
  ret = __builtin_clzll(clzarg);
#endif
  return NANBOX_OFINT(ret);
}

static sinanbox_t sivmfn_prim_math_fround(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  // no-op: sinter uses floats not doubles
  return *argv;
}

static sinanbox_t sivmfn_prim_math_imul(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  uint32_t r = nanbox_tou32(argv[0])*nanbox_tou32(argv[1]);
  if (r > INT32_MAX) {
    r -= 0x80000000ull;
  }
  return NANBOX_WRAP_INT((int32_t) r);
}

static sinanbox_t sivmfn_prim_math_max(uint8_t argc, sinanbox_t *argv) {
  if (argc == 0) {
    return NANBOX_OFFLOAT(-INFINITY);
  }

  sinanbox_t max = argv[0];
  if (NANBOX_IDENTICAL(max, NANBOX_CANONICAL_NAN)) {
    return max;
  }
  if (!NANBOX_ISFLOAT(max) && !NANBOX_ISINT(max)) {
    sifault(sinter_fault_type);
    return max;
  }
  for (unsigned int i = 1; i < argc; ++i) {
    sinanbox_t contender = argv[i];
    if (NANBOX_IDENTICAL(contender, NANBOX_CANONICAL_NAN)) {
      return contender;
    }
    if (NANBOX_ISINT(max) && NANBOX_ISINT(contender)) {
      int32_t maxi = NANBOX_INT(max);
      int32_t contenderi = NANBOX_INT(contender);
      if (contenderi > maxi) {
        max = contender;
      }
    } else if (NANBOX_ISFLOAT(contender) || NANBOX_ISINT(contender)) {
      float maxf = NANBOX_TOFLOAT(max);
      float contenderf = NANBOX_TOFLOAT(contender);
      if (contenderf > maxf) {
        max = contender;
      }
    } else {
      sifault(sinter_fault_type);
    }
  }

  return max;
}

static sinanbox_t sivmfn_prim_math_min(uint8_t argc, sinanbox_t *argv) {
  if (argc == 0) {
    return NANBOX_OFFLOAT(INFINITY);
  }

  sinanbox_t min = argv[0];
  if (NANBOX_IDENTICAL(min, NANBOX_CANONICAL_NAN)) {
    return min;
  }
  if (!NANBOX_ISFLOAT(min) && !NANBOX_ISINT(min)) {
    sifault(sinter_fault_type);
    return min;
  }
  for (unsigned int i = 1; i < argc; ++i) {
    sinanbox_t contender = argv[i];
    if (NANBOX_IDENTICAL(contender, NANBOX_CANONICAL_NAN)) {
      return contender;
    }
    if (NANBOX_ISINT(min) && NANBOX_ISINT(contender)) {
      int32_t mini = NANBOX_INT(min);
      int32_t contenderi = NANBOX_INT(contender);
      if (contenderi < mini) {
        min = contender;
      }
    } else if (NANBOX_ISFLOAT(contender) || NANBOX_ISINT(contender)) {
      float minf = NANBOX_TOFLOAT(min);
      float contenderf = NANBOX_TOFLOAT(contender);
      if (contenderf < minf) {
        min = contender;
      }
    } else {
      sifault(sinter_fault_type);
    }
  }

  return min;
}
static sinanbox_t sivmfn_prim_math_random(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  return NANBOX_OFFLOAT((float) rand() / (((float) RAND_MAX) + 1.0f));
}

static sinanbox_t sivmfn_prim_math_sign(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  sinanbox_t v = *argv;
  if (NANBOX_ISINT(v)) {
    int32_t intv = NANBOX_INT(v);
    return NANBOX_OFINT(intv ? (intv > 0 ? 1 : -1) : 0);
  } else if (NANBOX_ISFLOAT(v)) {
    float fv = NANBOX_FLOAT(v);
    return NANBOX_OFINT(fv != 0.0f ? (fv > 0.0f ? 1 : -1) : 0);
  } else {
    sifault(sinter_fault_type);
  }
  return NANBOX_OFEMPTY();
}

#define FLOAT_ROUND_FN(name) static sinanbox_t sivmfn_prim_math_## name(uint8_t argc, sinanbox_t *argv) { \
  CHECK_ARGC(1); \
  sinanbox_t v = *argv; \
  if (NANBOX_ISINT(v)) { \
    return v; \
  } else if (NANBOX_ISFLOAT(v)) { \
    float retv = name ## f(NANBOX_FLOAT(v)); \
    if (retv >= NANBOX_INTMIN && retv <= NANBOX_INTMAX) { \
      return NANBOX_OFINT((int32_t) retv); \
    } \
    return NANBOX_OFFLOAT(retv); \
  } else { \
    sifault(sinter_fault_type); \
  } \
  return NANBOX_OFEMPTY(); \
}

FLOAT_ROUND_FN(floor)
FLOAT_ROUND_FN(ceil)
FLOAT_ROUND_FN(round)
FLOAT_ROUND_FN(trunc)

/******************************************************************************
 * Pair primitives
 ******************************************************************************/

static inline siheap_array_t *source_pair_ptr(sinanbox_t l, sinanbox_t r) {
  siheap_array_t *arr = siarray_new(2);
  siarray_put(arr, 0, l);
  siarray_put(arr, 1, r);
  return arr;
}

static inline sinanbox_t source_pair(sinanbox_t l, sinanbox_t r) {
  return SIHEAP_PTRTONANBOX(source_pair_ptr(l, r));
}

static inline siheap_array_t *nanbox_toarray(sinanbox_t p) {
  siheap_header_t *v = SIHEAP_NANBOXTOPTR(p);
  siheap_array_t *a = (siheap_array_t *) v;
  if (!NANBOX_ISPTR(p) || v->type != sitype_array) {
    sifault(sinter_fault_type);
    return NULL;
  }
  return a;
}

static inline sinanbox_t source_head(sinanbox_t p) {
  siheap_array_t *a = nanbox_toarray(p);
  return siarray_get(a, 0);
}

static inline sinanbox_t source_tail(sinanbox_t p) {
  siheap_array_t *a = nanbox_toarray(p);
  return siarray_get(a, 1);
}

static sinanbox_t sivmfn_prim_pair(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  siheap_refbox(argv[0]);
  siheap_refbox(argv[1]);
  return source_pair(argv[0], argv[1]);
}

static sinanbox_t sivmfn_prim_head(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  sinanbox_t ret = source_head(argv[0]);
  siheap_refbox(ret);
  return ret;
}

static sinanbox_t sivmfn_prim_tail(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  sinanbox_t ret = source_tail(argv[0]);
  siheap_refbox(ret);
  return ret;
}

static sinanbox_t sivmfn_prim_set_head(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  siheap_array_t *a = nanbox_toarray(argv[0]);
  siheap_refbox(argv[1]);
  siarray_put(a, 0, argv[1]);
  return NANBOX_OFUNDEF();
}

static sinanbox_t sivmfn_prim_set_tail(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  siheap_array_t *a = nanbox_toarray(argv[0]);
  siheap_refbox(argv[1]);
  siarray_put(a, 1, argv[1]);
  return NANBOX_OFUNDEF();
}

static sinanbox_t sivmfn_prim_is_pair(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  siheap_header_t *v = SIHEAP_NANBOXTOPTR(argv[0]);
  siheap_array_t *a = (siheap_array_t *) v;
  return NANBOX_OFBOOL(NANBOX_ISPTR(argv[0]) && v->type == sitype_array && a->count == 2);
}

/******************************************************************************
 * List primitives
 ******************************************************************************/

static sinanbox_t sivmfn_prim_is_list(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);

  sinanbox_t l = argv[0];
  while (!NANBOX_ISNULL(l)) {
    siheap_header_t *v = SIHEAP_NANBOXTOPTR(l);
    siheap_array_t *a = (siheap_array_t *) v;
    if (!NANBOX_ISPTR(l) || v->type != sitype_array || a->count != 2) {
      return NANBOX_OFBOOL(false);
    }
    l = siarray_get(a, 1);
  }

  return NANBOX_OFBOOL(true);
}

static inline size_t source_list_length(sinanbox_t l) {
  size_t length = 0;
  while (!NANBOX_ISNULL(l)) {
    ++length;
    l = source_tail(l);
  }
  return length;
}

static sinanbox_t sivmfn_prim_accumulate(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(3);

  // this function is particularly horrible for us
  // we don't want to have a naive recursive implementation (in case we blow the C stack
  // the Sinter stack isn't particularly large either
  // so.. here's a compromise: we allocate an array and flatten the list into it
  // then we accumulate using the array

  sinanbox_t f = argv[0], acc = argv[1];
  const size_t list_length = source_list_length(argv[2]);
  siheap_array_t *flat_list = siarray_new(list_length);
#ifdef SINTER_DEBUG_MEMORY_CHECK
  flat_list->header.internal_refcount++;
#endif
  // flatten the list into the array
  {
    size_t idx = 0;
    sinanbox_t l = argv[2];
    while (!NANBOX_ISNULL(l)) {
      siheap_array_t *pair = nanbox_toarray(l);
      sinanbox_t head = siarray_get(pair, 0);
      siheap_refbox(head);
      siarray_put(flat_list, idx, head);
      l = siarray_get(pair, 1);
      idx += 1;
    }
    assert(idx == list_length);
  }

  // the initial acc has a ref from the caller's stack (it's not removed until
  // after we return. if we are passing it to a reentrant call, then there will
  // be a second ref from our callee's env
  // ref it
  siheap_refbox(acc);
  for (size_t idxp1 = list_length; idxp1 > 0; --idxp1) {
    sinanbox_t f_args[] = {siarray_get(flat_list, idxp1 - 1), acc};
    siheap_refbox(f_args[0]);
    acc = siexec_nanbox(f, 2, f_args);
  }

#ifdef SINTER_DEBUG_MEMORY_CHECK
  flat_list->header.internal_refcount--;
#endif
  siheap_deref(flat_list);

  return acc;
}

static sinanbox_t sivmfn_prim_append(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);

  sinanbox_t list = argv[0];

  if (NANBOX_ISNULL(list)) {
    siheap_refbox(argv[1]);
    return argv[1];
  }

  siheap_array_t *new_list = NULL;
  siheap_array_t *prev_pair = NULL;
  while (!NANBOX_ISNULL(list)) {
    siheap_array_t *pair = nanbox_toarray(list);
    sinanbox_t head = siarray_get(pair, 0);
    list = siarray_get(pair, 1);
    siheap_refbox(head);
    siheap_array_t *new_pair = source_pair_ptr(head, NANBOX_OFNULL());
    if (prev_pair) {
      siarray_put(prev_pair, 1, SIHEAP_PTRTONANBOX(new_pair));
    }
    if (!new_list) {
      new_list = new_pair;
    }
    prev_pair = new_pair;
  }

  siheap_refbox(argv[1]);
  siarray_put(prev_pair, 1, argv[1]);
  return SIHEAP_PTRTONANBOX(new_list);
}

static sinanbox_t sivmfn_prim_build_list(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);

  int32_t limit = nanbox_toi32(argv[0]);

  if (limit <= 0) {
    return NANBOX_OFNULL();
  }

  siheap_array_t *new_list = NULL;
  siheap_array_t *prev_pair = NULL;

  for (int32_t i = 0; i < limit; ++i) {
    sinanbox_t arg = NANBOX_WRAP_INT(i);
    sinanbox_t new_val = siexec_nanbox(argv[1], 1, &arg);
    siheap_array_t *new_pair = source_pair_ptr(new_val, NANBOX_OFNULL());
    if (prev_pair) {
      siarray_put(prev_pair, 1, SIHEAP_PTRTONANBOX(new_pair));
    }
    if (!new_list) {
      new_list = new_pair;
#ifdef SINTER_DEBUG_MEMORY_CHECK
      new_list->header.internal_refcount++;
#endif
    }
    prev_pair = new_pair;
  }

#ifdef SINTER_DEBUG_MEMORY_CHECK
  new_list->header.internal_refcount--;
#endif

  return SIHEAP_PTRTONANBOX(new_list);
}

#define PRIM_ENUM_LIST_FN(type, each) static inline sinanbox_t enum_list_##type(type start, type end) { \
  if (start > end) { \
    return NANBOX_OFNULL(); \
  } \
  siheap_array_t *new_list = NULL; \
  siheap_array_t *prev_pair = NULL; \
  for (type i = start; i <= end; i += 1) { \
    siheap_array_t *new_pair = source_pair_ptr(each, NANBOX_OFNULL()); \
    if (prev_pair) { \
      siarray_put(prev_pair, 1, SIHEAP_PTRTONANBOX(new_pair)); \
    } \
    if (!new_list) { \
      new_list = new_pair; \
    } \
    prev_pair = new_pair; \
  } \
  return SIHEAP_PTRTONANBOX(new_list); \
}

PRIM_ENUM_LIST_FN(int32_t, NANBOX_WRAP_INT(i))
PRIM_ENUM_LIST_FN(float, NANBOX_OFFLOAT(i))

static sinanbox_t sivmfn_prim_enum_list(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);

  if (NANBOX_ISINT(argv[0])) {
      if (NANBOX_ISINT(argv[1])) {
        return enum_list_int32_t(NANBOX_INT(argv[0]), NANBOX_INT(argv[1]));
      } else if (NANBOX_ISFLOAT(argv[1])) {
        float end = NANBOX_FLOAT(argv[1]);
        if (end > INT32_MIN && end < INT32_MAX) {
          return enum_list_int32_t(nanbox_toi32(argv[0]), (int32_t) end);
        } else {
          return enum_list_float(NANBOX_INT(argv[0]), end);
        }
      }
  } else if (NANBOX_ISFLOAT(argv[0])) {
    return enum_list_float(NANBOX_FLOAT(argv[0]), NANBOX_TOFLOAT(argv[1]));
  }
  sifault(sinter_fault_type);
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_filter(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  if (NANBOX_ISNULL(argv[1])) {
    return NANBOX_OFNULL();
  }

  const sinanbox_t filter_fn = argv[0];

  sinanbox_t old_list = argv[1];
  siheap_array_t *new_list = NULL;
  siheap_array_t *prev_pair = NULL;
  while (!NANBOX_ISNULL(old_list)) {
    siheap_array_t *pair = nanbox_toarray(old_list);
    sinanbox_t cur = siarray_get(pair, 0);
    old_list = siarray_get(pair, 1);
    siheap_refbox(cur);
    sinanbox_t pred_result = siexec_nanbox(filter_fn, 1, &cur);
    if (NANBOX_ISBOOL(pred_result)) {
      if (!NANBOX_BOOL(pred_result)) {
        continue;
      }
    } else {
      sifault(sinter_fault_type);
    }
    siheap_refbox(cur);
    siheap_array_t *new_pair = source_pair_ptr(cur, NANBOX_OFNULL());
    if (prev_pair) {
      siarray_put(prev_pair, 1, SIHEAP_PTRTONANBOX(new_pair));
    }
    if (!new_list) {
      new_list = new_pair;
#ifdef SINTER_DEBUG_MEMORY_CHECK
      new_list->header.internal_refcount++;
#endif
    }
    prev_pair = new_pair;
  }

  if (new_list) {
#ifdef SINTER_DEBUG_MEMORY_CHECK
    new_list->header.internal_refcount--;
#endif
    return SIHEAP_PTRTONANBOX(new_list);
  }

  return NANBOX_OFNULL();
}

static sinanbox_t sivmfn_prim_for_each(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  if (NANBOX_ISNULL(argv[1])) {
    return NANBOX_OFNULL();
  }

  const sinanbox_t for_each_fn = argv[0];

  sinanbox_t list = argv[1];
  while (!NANBOX_ISNULL(list)) {
    siheap_array_t *pair = nanbox_toarray(list);
    sinanbox_t cur = siarray_get(pair, 0);
    list = siarray_get(pair, 1);
    siheap_refbox(cur);
    siheap_derefbox(siexec_nanbox(for_each_fn, 1, &cur));
  }

  return NANBOX_OFUNDEF();
}

static sinanbox_t sivmfn_prim_length(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  return NANBOX_WRAP_UINT(source_list_length(argv[0]));
}

static sinanbox_t sivmfn_prim_list(uint8_t argc, sinanbox_t *argv) {
  if (argc == 0) {
    return NANBOX_OFNULL();
  }

  siheap_array_t *new_list = NULL;
  siheap_array_t *prev_pair = NULL;

  for (size_t i = 0; i < argc; ++i) {
    siheap_refbox(argv[i]);
    siheap_array_t *new_pair = source_pair_ptr(argv[i], NANBOX_OFNULL());
    if (prev_pair) {
      siarray_put(prev_pair, 1, SIHEAP_PTRTONANBOX(new_pair));
    }
    if (!new_list) {
      new_list = new_pair;
    }
    prev_pair = new_pair;
  }

  return SIHEAP_PTRTONANBOX(new_list);
}

static sinanbox_t sivmfn_prim_list_ref(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);

  sinanbox_t list = argv[0];
  int32_t count = nanbox_toi32(argv[1]);
  for (int32_t i = 0; i < count; ++i) {
    list = source_tail(list);
  }

  sinanbox_t retv = source_head(list);
  siheap_refbox(retv);
  return retv;
}

static sinanbox_t sivmfn_prim_list_to_string(uint8_t argc, sinanbox_t *argv) {
  // TODO: do we want to implement this?
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_map(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  if (NANBOX_ISNULL(argv[1])) {
    return NANBOX_OFNULL();
  }

  const sinanbox_t map_fn = argv[0];

  sinanbox_t old_list = argv[1];
  siheap_array_t *new_list = NULL;
  siheap_array_t *prev_pair = NULL;
  while (!NANBOX_ISNULL(old_list)) {
    siheap_array_t *pair = nanbox_toarray(old_list);
    sinanbox_t cur = siarray_get(pair, 0);
    old_list = siarray_get(pair, 1);
    siheap_refbox(cur);
    sinanbox_t newval = siexec_nanbox(map_fn, 1, &cur);
    siheap_array_t *new_pair = source_pair_ptr(newval, NANBOX_OFNULL());
    if (prev_pair) {
      siarray_put(prev_pair, 1, SIHEAP_PTRTONANBOX(new_pair));
    }
    if (!new_list) {
      new_list = new_pair;
#ifdef SINTER_DEBUG_MEMORY_CHECK
      new_list->header.internal_refcount++;
#endif
    }
    prev_pair = new_pair;
  }

#ifdef SINTER_DEBUG_MEMORY_CHECK
  new_list->header.internal_refcount--;
#endif
  return SIHEAP_PTRTONANBOX(new_list);
}

static sinanbox_t sivmfn_prim_member(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  if (NANBOX_ISNULL(argv[1])) {
    return NANBOX_OFNULL();
  }

  const sinanbox_t needle = argv[0];

  sinanbox_t list = argv[1];
  while (!NANBOX_ISNULL(list)) {
    siheap_array_t *pair = nanbox_toarray(list);
    sinanbox_t cur = siarray_get(pair, 0);
    if (sivm_equal(needle, cur)) {
      break;
    }
    list = siarray_get(pair, 1);
  }

  siheap_refbox(list);
  return list;
}

static sinanbox_t sivmfn_prim_remove(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  if (NANBOX_ISNULL(argv[1])) {
    return NANBOX_OFNULL();
  }

  const sinanbox_t needle = argv[0];

  sinanbox_t list = argv[1];
  siheap_array_t *new_list = NULL;
  siheap_array_t *prev_pair = NULL;
  while (!NANBOX_ISNULL(list)) {
    siheap_array_t *pair = nanbox_toarray(list);
    sinanbox_t cur = siarray_get(pair, 0);
    list = siarray_get(pair, 1);

    if (sivm_equal(cur, needle)) {
      siheap_refbox(list);
      if (prev_pair) {
        assert(new_list);
        siarray_put(prev_pair, 1, list);
        return SIHEAP_PTRTONANBOX(new_list);
      } else {
        return list;
      }
    }

    siheap_refbox(cur);
    siheap_array_t *new_pair = source_pair_ptr(cur, NANBOX_OFNULL());
    if (prev_pair) {
      siarray_put(prev_pair, 1, SIHEAP_PTRTONANBOX(new_pair));
    }
    if (!new_list) {
      new_list = new_pair;
    }
    prev_pair = new_pair;
  }

  return SIHEAP_PTRTONANBOX(new_list);
}

static sinanbox_t sivmfn_prim_remove_all(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  if (NANBOX_ISNULL(argv[1])) {
    return NANBOX_OFNULL();
  }

  const sinanbox_t needle = argv[0];

  sinanbox_t list = argv[1];
  siheap_array_t *new_list = NULL;
  siheap_array_t *prev_pair = NULL;
  while (!NANBOX_ISNULL(list)) {
    siheap_array_t *pair = nanbox_toarray(list);
    sinanbox_t cur = siarray_get(pair, 0);
    list = siarray_get(pair, 1);
    if (sivm_equal(cur, needle)) {
      continue;
    }

    siheap_refbox(cur);
    siheap_array_t *new_pair = source_pair_ptr(cur, NANBOX_OFNULL());
    if (prev_pair) {
      siarray_put(prev_pair, 1, SIHEAP_PTRTONANBOX(new_pair));
    }
    if (!new_list) {
      new_list = new_pair;
#ifdef SINTER_DEBUG_MEMORY_CHECK
      new_list->header.internal_refcount++;
#endif
    }
    prev_pair = new_pair;
  }

  if (new_list) {
#ifdef SINTER_DEBUG_MEMORY_CHECK
    new_list->header.internal_refcount--;
#endif
    return SIHEAP_PTRTONANBOX(new_list);
  }
  return NANBOX_OFNULL();
}

static sinanbox_t sivmfn_prim_reverse(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

/******************************************************************************
 * Array primitive
 ******************************************************************************/

static sinanbox_t sivmfn_prim_array_length(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(1);
  sinanbox_t v = *argv;
  siheap_header_t *obj = SIHEAP_NANBOXTOPTR(v);
  if (!NANBOX_ISPTR(v) || obj->type != sitype_array) {
    sifault(sinter_fault_type);
    return NANBOX_OFEMPTY();
  }

  siheap_array_t *arr = (siheap_array_t *) obj;
  return NANBOX_WRAP_INT((int) arr->count);
}

/******************************************************************************
 * Stream primitives
 ******************************************************************************/

static sinanbox_t sivmfn_prim_list_to_stream(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_build_stream(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_enum_stream(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_eval_stream(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_integers_from(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_append(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_filter(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_for_each(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_length(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_map(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_member(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_ref(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_remove(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_remove_all(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_reverse(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_tail(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stream_to_list(uint8_t argc, sinanbox_t *argv) {
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

/******************************************************************************
 * Miscellaneous primitives
 ******************************************************************************/

static sinanbox_t sivmfn_prim_display(uint8_t argc, sinanbox_t *argv) {
  SIDEBUG("Program called display with %d arguments:\n", argc);
  debug_display_argv(argc, argv);
  handle_display(argc, argv, false);
  if (argc) {
    siheap_refbox(argv[0]);
    return argv[0];
  }
  return NANBOX_OFUNDEF();
}

static sinanbox_t sivmfn_prim_draw_data(uint8_t argc, sinanbox_t *argv) {
  // Not supported on Sinter.
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static inline bool structural_equal(sinanbox_t l, sinanbox_t r) {
  if (sivm_equal(l, r)) {
    return true;
  }

  if (!NANBOX_ISPTR(l) || !NANBOX_ISPTR(r)) {
    return false;
  }

  siheap_header_t *lv = SIHEAP_NANBOXTOPTR(l);
  siheap_header_t *rv = SIHEAP_NANBOXTOPTR(r);
  if (lv->type != sitype_array || rv->type != sitype_array) {
    return false;
  }

  siheap_array_t *la = (siheap_array_t *) lv;
  siheap_array_t *ra = (siheap_array_t *) rv;
  if (la->count != 2 || ra->count != 2) {
    return false;
  }

  return structural_equal(siarray_get(la, 0), siarray_get(ra, 0))
    && structural_equal(siarray_get(la, 1), siarray_get(ra, 1));
}

static sinanbox_t sivmfn_prim_equal(uint8_t argc, sinanbox_t *argv) {
  CHECK_ARGC(2);
  return NANBOX_OFBOOL(structural_equal(argv[0], argv[1]));
}

static sinanbox_t sivmfn_prim_error(uint8_t argc, sinanbox_t *argv) {
  SIDEBUG("Program called error with %d arguments:\n", argc);
  debug_display_argv(argc, argv);
  handle_display(argc, argv, true);
  sifault(sinter_fault_program_error);
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_parse_int(uint8_t argc, sinanbox_t *argv) {
  // TODO (Needs prompt for this to make sense)
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_runtime(uint8_t argc, sinanbox_t *argv) {
  // TODO
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_stringify(uint8_t argc, sinanbox_t *argv) {
  // TODO
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

static sinanbox_t sivmfn_prim_prompt(uint8_t argc, sinanbox_t *argv) {
  // TODO (Needs to call out to the hosting application)
  (void) argc; (void) argv;
  unimpl();
  return NANBOX_OFEMPTY();
}

const sivmfnptr_t sivmfn_primitives[] = {
  sivmfn_prim_accumulate,
  sivmfn_prim_append,
  sivmfn_prim_array_length,
  sivmfn_prim_build_list,
  sivmfn_prim_build_stream,
  sivmfn_prim_display,
  sivmfn_prim_draw_data,
  sivmfn_prim_enum_list,
  sivmfn_prim_enum_stream,
  sivmfn_prim_equal,
  sivmfn_prim_error,
  sivmfn_prim_eval_stream,
  sivmfn_prim_filter,
  sivmfn_prim_for_each,
  sivmfn_prim_head,
  sivmfn_prim_integers_from,
  sivmfn_prim_is_array,
  sivmfn_prim_is_boolean,
  sivmfn_prim_is_function,
  sivmfn_prim_is_list,
  sivmfn_prim_is_null,
  sivmfn_prim_is_number,
  sivmfn_prim_is_pair,
  sivmfn_prim_is_stream,
  sivmfn_prim_is_string,
  sivmfn_prim_is_undefined,
  sivmfn_prim_length,
  sivmfn_prim_list,
  sivmfn_prim_list_ref,
  sivmfn_prim_list_to_stream,
  sivmfn_prim_list_to_string,
  sivmfn_prim_map,
  sivmfn_prim_math_abs,
  sivmfn_prim_math_acos,
  sivmfn_prim_math_acosh,
  sivmfn_prim_math_asin,
  sivmfn_prim_math_asinh,
  sivmfn_prim_math_atan,
  sivmfn_prim_math_atan2,
  sivmfn_prim_math_atanh,
  sivmfn_prim_math_cbrt,
  sivmfn_prim_math_ceil,
  sivmfn_prim_math_clz32,
  sivmfn_prim_math_cos,
  sivmfn_prim_math_cosh,
  sivmfn_prim_math_exp,
  sivmfn_prim_math_expm1,
  sivmfn_prim_math_floor,
  sivmfn_prim_math_fround,
  sivmfn_prim_math_hypot,
  sivmfn_prim_math_imul,
  sivmfn_prim_math_log,
  sivmfn_prim_math_log1p,
  sivmfn_prim_math_log2,
  sivmfn_prim_math_log10,
  sivmfn_prim_math_max,
  sivmfn_prim_math_min,
  sivmfn_prim_math_pow,
  sivmfn_prim_math_random,
  sivmfn_prim_math_round,
  sivmfn_prim_math_sign,
  sivmfn_prim_math_sin,
  sivmfn_prim_math_sinh,
  sivmfn_prim_math_sqrt,
  sivmfn_prim_math_tan,
  sivmfn_prim_math_tanh,
  sivmfn_prim_math_trunc,
  sivmfn_prim_member,
  sivmfn_prim_pair,
  sivmfn_prim_parse_int,
  sivmfn_prim_remove,
  sivmfn_prim_remove_all,
  sivmfn_prim_reverse,
  sivmfn_prim_runtime,
  sivmfn_prim_set_head,
  sivmfn_prim_set_tail,
  sivmfn_prim_stream,
  sivmfn_prim_stream_append,
  sivmfn_prim_stream_filter,
  sivmfn_prim_stream_for_each,
  sivmfn_prim_stream_length,
  sivmfn_prim_stream_map,
  sivmfn_prim_stream_member,
  sivmfn_prim_stream_ref,
  sivmfn_prim_stream_remove,
  sivmfn_prim_stream_remove_all,
  sivmfn_prim_stream_reverse,
  sivmfn_prim_stream_tail,
  sivmfn_prim_stream_to_list,
  sivmfn_prim_tail,
  sivmfn_prim_stringify,
  sivmfn_prim_prompt
};
