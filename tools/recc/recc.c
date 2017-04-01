#include "core/log.h"
#include "core/option.h"
#include "jit/backend/x64/x64_backend.h"
#include "jit/emit_stats.h"
#include "jit/frontend/sh4/sh4_disasm.h"
#include "jit/ir/ir.h"
#include "jit/jit.h"
#include "jit/pass_stats.h"
#include "jit/passes/constant_propagation_pass.h"
#include "jit/passes/control_flow_analysis_pass.h"
#include "jit/passes/dead_code_elimination_pass.h"
#include "jit/passes/expression_simplification_pass.h"
#include "jit/passes/load_store_elimination_pass.h"
#include "jit/passes/register_allocation_pass.h"
#include "sys/filesystem.h"

DEFINE_OPTION_INT(help, 0, "Show help");
DEFINE_OPTION_STRING(pass, "cfa,lse,cprop,esimp,dce,ra",
                     "Comma-separated list of passes to run");

DEFINE_STAT(ir_instrs_total, "total ir instructions");
DEFINE_STAT(ir_instrs_removed, "removed ir instructions");

static uint8_t ir_buffer[1024 * 1024 * 8];
static uint8_t code[1024 * 1024];
static int code_size = sizeof(code);
static int stack_size = 4096;

static int get_num_instrs(const struct ir *ir) {
  int n = 0;

  list_for_each_entry(block, &ir->blocks, struct ir_block, it) {
    list_for_each_entry(instr, &block->instrs, struct ir_instr, it) {
      ((void)instr);
      n++;
    }
  }

  return n;
}

static void sanitize_ir(struct ir *ir) {
  list_for_each_entry(block, &ir->blocks, struct ir_block, it) {
    list_for_each_entry(instr, &block->instrs, struct ir_instr, it) {
      int arg_index = -1;

      if (instr->op == OP_BRANCH || instr->op == OP_CALL ||
          instr->op == OP_CALL_FALLBACK) {
        arg_index = 0;
      } else if (instr->op == OP_BRANCH_FALSE || instr->op == OP_BRANCH_TRUE ||
                 instr->op == OP_CALL_COND) {
        arg_index = 1;
      }

      /* ensure that absolute address are within 2 GB of the code buffer */
      struct ir_value *arg = NULL;

      if (arg_index != -1) {
        arg = instr->arg[arg_index];
      }

      if (arg && arg->type == VALUE_I64) {
        uint64_t addr = arg->i64;
        addr = (uint64_t)code | (addr & 0x7fffffff);
        ir_set_arg(ir, instr, arg_index, ir_alloc_i64(ir, addr));
      }
    }
  }
}

static void process_file(struct jit *jit, const char *filename,
                         int disable_dumps) {
  struct ir ir = {0};
  ir.buffer = ir_buffer;
  ir.capacity = sizeof(ir_buffer);

  /* read in the input ir */
  FILE *input = fopen(filename, "r");
  CHECK(input);
  int r = ir_read(input, &ir);
  fclose(input);
  CHECK(r);

  /* sanitize absolute addresses in the ir */
  sanitize_ir(&ir);

  /* run optimization passes */
  char passes[MAX_OPTION_LENGTH];
  strncpy(passes, OPTION_pass, sizeof(passes));

  int num_instrs_before = get_num_instrs(&ir);

  char *name = strtok(passes, ",");
  while (name) {
    if (!strcmp(name, "cfa")) {
      struct cfa *cfa = cfa_create();
      cfa_run(cfa, &ir);
      cfa_destroy(cfa);
    } else if (!strcmp(name, "cprop")) {
      struct cprop *cprop = cprop_create();
      cprop_run(cprop, &ir);
      cprop_destroy(cprop);
    } else if (!strcmp(name, "dce")) {
      struct dce *dce = dce_create();
      dce_run(dce, &ir);
      dce_destroy(dce);
    } else if (!strcmp(name, "esimp")) {
      struct esimp *esimp = esimp_create();
      esimp_run(esimp, &ir);
      esimp_destroy(esimp);
    } else if (!strcmp(name, "lse")) {
      struct lse *lse = lse_create();
      lse_run(lse, &ir);
      lse_destroy(lse);
    } else if (!strcmp(name, "ra")) {
      struct ra *ra = ra_create(x64_registers, x64_num_registers);
      ra_run(ra, &ir);
      ra_destroy(ra);
    } else {
      LOG_WARNING("Unknown pass %s", name);
    }

    /* print ir after each pass if requested */
    if (!disable_dumps) {
      LOG_INFO("===-----------------------------------------------------===");
      LOG_INFO("IR after %s", name);
      LOG_INFO("===-----------------------------------------------------===");
      ir_write(&ir, stdout);
      LOG_INFO("");
    }

    name = strtok(NULL, ",");
  }

  int num_instrs_after = get_num_instrs(&ir);

  /* assemble backend code */
  struct jit_code code;
  jit->backend->reset(jit->backend);
  int res = jit->backend->assemble_code(jit->backend, &code, &ir);
  CHECK(res);

  if (!disable_dumps) {
    LOG_INFO("===-----------------------------------------------------===");
    LOG_INFO("X64 code");
    LOG_INFO("===-----------------------------------------------------===");
    jit->backend->dump_code(jit->backend, code.host_addr, code.host_size);
    LOG_INFO("");
  }

  /* update stats */
  STAT_ir_instrs_total += num_instrs_before;
  STAT_ir_instrs_removed += num_instrs_before - num_instrs_after;
}

static void process_dir(struct jit *jit, const char *path) {
  DIR *dir = opendir(path);

  if (!dir) {
    LOG_WARNING("Failed to open directory %s", path);
    return;
  }

  struct dirent *ent = NULL;

  while ((ent = readdir(dir)) != NULL) {
    if (!(ent->d_type & DT_REG)) {
      continue;
    }

    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s" PATH_SEPARATOR "%s", path,
             ent->d_name);

    LOG_INFO("Processing %s", filename);

    process_file(jit, filename, 1);
  }

  closedir(dir);
}

int main(int argc, char **argv) {
  options_parse(&argc, &argv);

  if (OPTION_help) {
    options_print_help();
    return EXIT_SUCCESS;
  }

  const char *path = argv[1];

  /* initailize jit, stubbing out guest interfaces that are used during
     assembly to a valid address */
  struct jit *jit = jit_create("recc");
  jit->emit_stats = 1;

  struct jit_guest guest = {0};
  guest.r8 = (void *)code;
  guest.r16 = (void *)code;
  guest.r32 = (void *)code;
  guest.w8 = (void *)code;
  guest.w16 = (void *)code;
  guest.w32 = (void *)code;

  struct x64_backend *backend =
      x64_backend_create(jit, code, code_size, stack_size);

  CHECK(jit_init(jit, &guest, NULL, (struct jit_backend *)backend));

  if (fs_isfile(path)) {
    process_file(jit, path, 0);
  } else {
    process_dir(jit, path);
  }

  LOG_INFO("");
  emit_stats_dump();
  pass_stats_dump();

  jit_destroy(jit);
  x64_backend_destroy(backend);

  return EXIT_SUCCESS;
}
