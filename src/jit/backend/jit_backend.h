#ifndef JIT_BACKEND_H
#define JIT_BACKEND_H

#include <stdint.h>

struct address_space;
struct exception;
struct ir;
struct jit;
struct jit_code;

struct jit_register {
  const char *name;
  int value_types;
  const void *data;
};

struct jit_backend {
  struct jit *jit;

  const struct jit_register *registers;
  int num_registers;

  void (*reset)(struct jit_backend *base);
  int (*assemble_code)(struct jit_backend *base, struct jit_code *code,
                       struct ir *ir);
  void (*dump_code)(struct jit_backend *base, const uint8_t *code, int size);

  int (*handle_exception)(struct jit_backend *base, struct exception *ex);
};

#endif
