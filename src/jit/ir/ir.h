#ifndef IR_BUILDER_H
#define IR_BUILDER_H

#include <stdio.h>
#include "core/assert.h"
#include "core/list.h"

#define MAX_LABEL_SIZE 128
#define MAX_INSTR_ARGS 4

enum ir_op {
#define IR_OP(name) OP_##name,
#include "jit/ir/ir_ops.inc"
#undef IR_OP
  NUM_OPS
};

enum ir_type {
  VALUE_V,
  VALUE_I8,
  VALUE_I16,
  VALUE_I32,
  VALUE_I64,
  VALUE_F32,
  VALUE_F64,
  VALUE_V128,
  VALUE_STRING,
  VALUE_BLOCK,
  VALUE_NUM,
};

enum ir_cmp {
  CMP_EQ,
  CMP_NE,
  CMP_SGE,
  CMP_SGT,
  CMP_UGE,
  CMP_UGT,
  CMP_SLE,
  CMP_SLT,
  CMP_ULE,
  CMP_ULT
};

struct ir_block;
struct ir_instr;
struct ir_value;

/* use is a layer of indirection between an instruction and the values it uses
   as arguments. this indirection makes it possible to maintain a list for each
   value of the arguments that reference it */
struct ir_use {
  /* the instruction that's using the value */
  struct ir_instr *instr;

  /* pointer to the argument that's using the value. this is used to substitute
     a new value for the argument in the case that the original value is
     removed (e.g. due to constant propagation) */
  struct ir_value **parg;

  struct list_node it;
};

struct ir_value {
  enum ir_type type;

  union {
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    char *str;
    struct ir_block *blk;
  };

  /* instruction that defines this value (non-constant values) */
  struct ir_instr *def;

  /* instructions that use this value as an argument */
  struct list uses;

  /* host register allocated for this value */
  int reg;

  /* generic meta data used by optimization passes */
  intptr_t tag;
};

struct ir_instr {
  const char *label;

  enum ir_op op;

  /* values used by each argument. note, the argument / use is split into two
     separate members to ease reading the argument value (instr->arg[0] vs
     instr->arg[0].value) */
  struct ir_value *arg[MAX_INSTR_ARGS];
  struct ir_use used[MAX_INSTR_ARGS];

  /* result of the instruction. note, instruction results don't consider
     themselves users of the value (eases register allocation logic) */
  struct ir_value *result;

  /* block the instruction belongs to */
  struct ir_block *block;

  /* generic meta data used by optimization passes */
  intptr_t tag;

  struct list_node it;
};

/* blocks are collections of instructions, terminating in a single branch */
struct ir_edge {
  struct ir_block *src;
  struct ir_block *dst;

  /* intrusive iterator used by block struct */
  struct list_node it;
};

struct ir_block {
  const char *label;

  struct list instrs;

  /* edges between this block and others */
  struct list outgoing;
  struct list incoming;

  /* intrusive iterator used by ir struct */
  struct list_node it;

  /* generic meta data used by optimization passes */
  intptr_t tag;
};

/* locals are allocated for values that need to be spilled to the stack
   during register allocation */
struct ir_local {
  enum ir_type type;
  struct ir_value *offset;
};

struct ir_insert_point {
  struct ir_block *block;
  struct ir_instr *instr;
};

struct ir {
  /* backing memory buffer used by all ir allocations */
  uint8_t *buffer;
  int capacity;
  int used;

  /* total size of locals allocated */
  int locals_size;

  /* current insert point */
  struct ir_insert_point cursor;

  struct list blocks;
};

extern const char *ir_op_names[NUM_OPS];

#define VALUE_I8_MASK (1 << VALUE_I8)
#define VALUE_I16_MASK (1 << VALUE_I16)
#define VALUE_I32_MASK (1 << VALUE_I32)
#define VALUE_I64_MASK (1 << VALUE_I64)
#define VALUE_F32_MASK (1 << VALUE_F32)
#define VALUE_F64_MASK (1 << VALUE_F64)
#define VALUE_V128_MASK (1 << VALUE_V128)
#define VALUE_INT_MASK \
  (VALUE_I8_MASK | VALUE_I16_MASK | VALUE_I32_MASK | VALUE_I64_MASK)
#define VALUE_FLOAT_MASK (VALUE_F32_MASK | VALUE_F64_MASK)
#define VALUE_VECTOR_MASK (VALUE_V128_MASK)
#define VALUE_ALL_MASK (VALUE_INT_MASK | VALUE_FLOAT_MASK)

#define INVALID_ADDR 0xffffffff
#define NO_REGISTER -1

static inline int ir_type_size(enum ir_type type) {
  switch (type) {
    case VALUE_I8:
      return 1;
    case VALUE_I16:
      return 2;
    case VALUE_I32:
      return 4;
    case VALUE_I64:
      return 8;
    case VALUE_F32:
      return 4;
    case VALUE_F64:
      return 8;
    case VALUE_V128:
      return 16;
    default:
      LOG_FATAL("Unexpected value type");
      break;
  }
}

static inline int ir_is_int(enum ir_type type) {
  return type == VALUE_I8 || type == VALUE_I16 || type == VALUE_I32 ||
         type == VALUE_I64;
}

static inline int ir_is_float(enum ir_type type) {
  return type == VALUE_F32 || type == VALUE_F64;
}

static inline int ir_is_vector(enum ir_type type) {
  return type == VALUE_V128;
}

static inline int ir_is_constant(const struct ir_value *v) {
  return !v->def;
}

int ir_read(FILE *input, struct ir *ir);
void ir_write(struct ir *ir, FILE *output);

struct ir_insert_point ir_get_insert_point(struct ir *ir);
void ir_set_insert_point(struct ir *ir, struct ir_insert_point *point);
void ir_set_current_block(struct ir *ir, struct ir_block *block);
void ir_set_current_instr(struct ir *ir, struct ir_instr *instr);

struct ir_block *ir_insert_block(struct ir *ir, struct ir_block *after);
struct ir_block *ir_append_block(struct ir *ir);
void ir_set_block_label(struct ir *ir, struct ir_block *block,
                        const char *format, ...);
void ir_remove_block(struct ir *ir, struct ir_block *block);
void ir_add_edge(struct ir *ir, struct ir_block *src, struct ir_block *dst);

struct ir_instr *ir_append_instr(struct ir *ir, enum ir_op op,
                                 enum ir_type result_type);
void ir_set_instr_label(struct ir *ir, struct ir_instr *instr,
                        const char *format, ...);
void ir_remove_instr(struct ir *ir, struct ir_instr *instr);

struct ir_value *ir_alloc_int(struct ir *ir, int64_t c, enum ir_type type);
struct ir_value *ir_alloc_i8(struct ir *ir, int8_t c);
struct ir_value *ir_alloc_i16(struct ir *ir, int16_t c);
struct ir_value *ir_alloc_i32(struct ir *ir, int32_t c);
struct ir_value *ir_alloc_i64(struct ir *ir, int64_t c);
struct ir_value *ir_alloc_f32(struct ir *ir, float c);
struct ir_value *ir_alloc_f64(struct ir *ir, double c);
struct ir_value *ir_alloc_str(struct ir *ir, const char *format, ...);
struct ir_value *ir_alloc_ptr(struct ir *ir, void *c);
struct ir_value *ir_alloc_block(struct ir *ir, struct ir_block *block);
struct ir_local *ir_alloc_local(struct ir *ir, enum ir_type type);
struct ir_local *ir_reuse_local(struct ir *ir, struct ir_value *offset,
                                enum ir_type type);

void ir_set_arg(struct ir *ir, struct ir_instr *instr, int n,
                struct ir_value *v);
void ir_set_arg0(struct ir *ir, struct ir_instr *instr, struct ir_value *v);
void ir_set_arg1(struct ir *ir, struct ir_instr *instr, struct ir_value *v);
void ir_set_arg2(struct ir *ir, struct ir_instr *instr, struct ir_value *v);
void ir_set_arg3(struct ir *ir, struct ir_instr *instr, struct ir_value *v);

void ir_replace_use(struct ir_use *use, struct ir_value *other);
void ir_replace_uses(struct ir_value *v, struct ir_value *other);

uint64_t ir_zext_constant(const struct ir_value *v);

/* direct access to host memory */
struct ir_value *ir_load(struct ir *ir, struct ir_value *addr,
                         enum ir_type type);
void ir_store(struct ir *ir, struct ir_value *addr, struct ir_value *v);

/* guest memory operations */
struct ir_value *ir_load_fast(struct ir *ir, struct ir_value *addr,
                              enum ir_type type);
void ir_store_fast(struct ir *ir, struct ir_value *addr, struct ir_value *v);

struct ir_value *ir_load_slow(struct ir *ir, struct ir_value *addr,
                              enum ir_type type);
void ir_store_slow(struct ir *ir, struct ir_value *addr, struct ir_value *v);

/* context operations */
struct ir_value *ir_load_context(struct ir *ir, size_t offset,
                                 enum ir_type type);
void ir_store_context(struct ir *ir, size_t offset, struct ir_value *v);

/* local operations */
struct ir_value *ir_load_local(struct ir *ir, struct ir_local *local);
void ir_store_local(struct ir *ir, struct ir_local *local, struct ir_value *v);

/* cast / conversion operations */
struct ir_value *ir_ftoi(struct ir *ir, struct ir_value *v,
                         enum ir_type dest_type);
struct ir_value *ir_itof(struct ir *ir, struct ir_value *v,
                         enum ir_type dest_type);
struct ir_value *ir_sext(struct ir *ir, struct ir_value *v,
                         enum ir_type dest_type);
struct ir_value *ir_zext(struct ir *ir, struct ir_value *v,
                         enum ir_type dest_type);
struct ir_value *ir_trunc(struct ir *ir, struct ir_value *v,
                          enum ir_type dest_type);
struct ir_value *ir_fext(struct ir *ir, struct ir_value *v,
                         enum ir_type dest_type);
struct ir_value *ir_ftrunc(struct ir *ir, struct ir_value *v,
                           enum ir_type dest_type);

/* conditionals */
struct ir_value *ir_select(struct ir *ir, struct ir_value *cond,
                           struct ir_value *t, struct ir_value *f);
struct ir_value *ir_cmp_eq(struct ir *ir, struct ir_value *a,
                           struct ir_value *b);
struct ir_value *ir_cmp_ne(struct ir *ir, struct ir_value *a,
                           struct ir_value *b);
struct ir_value *ir_cmp_sge(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_cmp_sgt(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_cmp_uge(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_cmp_ugt(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_cmp_sle(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_cmp_slt(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_cmp_ule(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_cmp_ult(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_fcmp_eq(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_fcmp_ne(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_fcmp_ge(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_fcmp_gt(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_fcmp_le(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);
struct ir_value *ir_fcmp_lt(struct ir *ir, struct ir_value *a,
                            struct ir_value *b);

/* integer math operators */
struct ir_value *ir_add(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_sub(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_smul(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_umul(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_div(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_neg(struct ir *ir, struct ir_value *a);
struct ir_value *ir_abs(struct ir *ir, struct ir_value *a);

/* floating point math operators */
struct ir_value *ir_fadd(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_fsub(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_fmul(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_fdiv(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_fneg(struct ir *ir, struct ir_value *a);
struct ir_value *ir_fabs(struct ir *ir, struct ir_value *a);
struct ir_value *ir_sqrt(struct ir *ir, struct ir_value *a);

/* vector math operators */
struct ir_value *ir_vbroadcast(struct ir *ir, struct ir_value *a);
struct ir_value *ir_vadd(struct ir *ir, struct ir_value *a, struct ir_value *b,
                         enum ir_type el_type);
struct ir_value *ir_vdot(struct ir *ir, struct ir_value *a, struct ir_value *b,
                         enum ir_type el_type);
struct ir_value *ir_vmul(struct ir *ir, struct ir_value *a, struct ir_value *b,
                         enum ir_type el_type);

/* bitwise operations */
struct ir_value *ir_and(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_or(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_xor(struct ir *ir, struct ir_value *a, struct ir_value *b);
struct ir_value *ir_not(struct ir *ir, struct ir_value *a);
struct ir_value *ir_shl(struct ir *ir, struct ir_value *a, struct ir_value *n);
struct ir_value *ir_shli(struct ir *ir, struct ir_value *a, int n);
struct ir_value *ir_ashr(struct ir *ir, struct ir_value *a, struct ir_value *n);
struct ir_value *ir_ashri(struct ir *ir, struct ir_value *a, int n);
struct ir_value *ir_lshr(struct ir *ir, struct ir_value *a, struct ir_value *n);
struct ir_value *ir_lshri(struct ir *ir, struct ir_value *a, int n);
struct ir_value *ir_ashd(struct ir *ir, struct ir_value *a, struct ir_value *n);
struct ir_value *ir_lshd(struct ir *ir, struct ir_value *a, struct ir_value *n);

/* branches */
void ir_label(struct ir *ir, struct ir_value *lbl);
void ir_branch(struct ir *ir, struct ir_value *dst);
void ir_branch_true(struct ir *ir, struct ir_value *cond, struct ir_value *dst);
void ir_branch_false(struct ir *ir, struct ir_value *cond,
                     struct ir_value *dst);

/* calls */
void ir_call(struct ir *ir, struct ir_value *fn);
void ir_call_1(struct ir *ir, struct ir_value *fn, struct ir_value *arg0);
void ir_call_2(struct ir *ir, struct ir_value *fn, struct ir_value *arg0,
               struct ir_value *arg1);
void ir_call_cond(struct ir *ir, struct ir_value *cond, struct ir_value *fn);
void ir_call_cond_1(struct ir *ir, struct ir_value *cond, struct ir_value *fn,
                    struct ir_value *arg0);
void ir_call_cond_2(struct ir *ir, struct ir_value *cond, struct ir_value *fn,
                    struct ir_value *arg0, struct ir_value *arg1);
void ir_call_noreturn(struct ir *ir, struct ir_value *fn);
void ir_call_noreturn_1(struct ir *ir, struct ir_value *fn,
                        struct ir_value *arg0);
void ir_call_noreturn_2(struct ir *ir, struct ir_value *fn,
                        struct ir_value *arg0, struct ir_value *arg1);
void ir_call_fallback(struct ir *ir, void *fallback, uint32_t addr,
                      uint32_t raw_instr);

/* debug */
void ir_debug_info(struct ir *ir, const char *desc, uint32_t addr,
                   uint32_t instr);
void ir_debug_break(struct ir *ir);
void ir_assert_lt(struct ir *ir, struct ir_value *a, struct ir_value *b);

#endif
