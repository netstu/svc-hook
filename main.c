// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2024 Akira Moroo

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/queue.h>

extern void syscall_addr(void);
extern void do_rt_sigreturn(void);
extern long enter_syscall(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                          int64_t, int64_t);
extern void asm_syscall_hook(void);

void ____asm_impl(void) {
  /*
   * enter_syscall triggers a kernel-space system call
   * @param	a1	arg0
   * @param	a2	arg1
   * @param	a3	arg2
   * @param	a4	arg3
   * @param	a5	arg4
   * @param	a6	arg5
   * @param	a7	syscall NR
   * @return		return value
   */
  asm volatile(
      ".globl enter_syscall \n\t"
      "enter_syscall: \n\t"
      "mov x8, x6 \n\t"
      ".globl syscall_addr \n\t"
      "syscall_addr: \n\t"
      "svc #0 \n\t"
      "ret \n\t");

  /*
   * asm_syscall_hook is the address where the
   * trampoline code first lands.
   *
   * the procedure below calls the C function
   * named syscall_hook.
   *
   * at the entry point of this,
   * the register values follow the calling convention
   * of the system calls.
   */
  asm volatile(
      ".globl asm_syscall_hook \n\t"
      "asm_syscall_hook: \n\t"

      "cmp x8, #139 \n\t" /* rt_sigreturn */
      "b.eq do_rt_sigreturn \n\t"

  /* assuming callee preserves x19-x28  */

#ifndef REDUCED_CONTEXT_SAVE
      "sub sp, sp, #256 \n\t"
      "stp x0, x1, [sp,#0] \n\t"
      "stp x2, x3, [sp,#16] \n\t"
      "stp x4, x5, [sp,#32] \n\t"
      "stp x6, x7, [sp,#48] \n\t"
      "stp x8, x9, [sp,#64] \n\t"
      "stp x10, x11, [sp,#80] \n\t"
      "stp x12, x13, [sp,#96] \n\t"
      "stp x14, x15, [sp,#112] \n\t"
      "stp x16, x17, [sp,#128] \n\t"
      "stp x18, x19, [sp,#144] \n\t"
      "stp x20, x21, [sp,#160] \n\t"
      "stp x22, x23, [sp,#176] \n\t"
      "stp x24, x25, [sp,#192] \n\t"
      "stp x26, x27, [sp,#208] \n\t"
      "stp x28, x29, [sp,#224] \n\t"
      "stp x30, xzr, [sp,#240] \n\t"
#else
      "sub sp, sp, #32 \n\t"
      "stp x8, x9, [sp,#0] \n\t"
      "stp x29, x30, [sp,#16] \n\t"
#endif

      /* arguments for syscall_hook */
      "mov x7, x9 \n\t" /* return address */
      "mov x6, x8 \n\t" /* syscall NR */

      "bl syscall_hook \n\t"

#ifndef REDUCED_CONTEXT_SAVE
      "ldp xzr, x1, [sp,#0] \n\t"
      "ldp x2, x3, [sp,#16] \n\t"
      "ldp x4, x5, [sp,#32] \n\t"
      "ldp x6, x7, [sp,#48] \n\t"
      "ldp x8, x9, [sp,#64] \n\t"
      "ldp x10, x11, [sp,#80] \n\t"
      "ldp x12, x13, [sp,#96] \n\t"
      "ldp x14, x15, [sp,#112] \n\t"
      "ldp x16, x17, [sp,#128] \n\t"
      "ldp x18, x19, [sp,#144] \n\t"
      "ldp x20, x21, [sp,#160] \n\t"
      "ldp x22, x23, [sp,#176] \n\t"
      "ldp x24, x25, [sp,#192] \n\t"
      "ldp x26, x27, [sp,#208] \n\t"
      "ldp x28, x29, [sp,#224] \n\t"
      "ldp x30, xzr, [sp,#240] \n\t"
      "add sp, sp, #256 \n\t"
#else
      "ldp x29, x30, [sp,#16] \n\t"
      "ldp x8, x9, [sp,#0] \n\t"
      "add sp, sp, #32 \n\t"
#endif

      "do_return: \n\t"
      "mov x8, x9 \n\t"
      "ldp x9, x10, [sp],#16 \n\t"

      /* XXX: We assume that the caller does not reuse the syscall number stored
         in x8. */
      "br x8 \n\t"

      ".globl do_rt_sigreturn \n\t"
      "do_rt_sigreturn: \n\t"
      "svc #0 \n\t"
      "b do_return \n\t");
}

static long (*hook_fn)(int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                       int64_t a5, int64_t a6, int64_t a7,
                       int64_t a8) = enter_syscall;

long syscall_hook(int64_t x0, int64_t x1, int64_t x2, int64_t x3, int64_t x4,
                  int64_t x5, int64_t x8, /* syscall NR */
                  int64_t retptr) {
  return hook_fn(x0, x1, x2, x3, x4, x5, x8, retptr);
}

static inline size_t align_up(size_t value, size_t align) {
  return (value + align - 1) & ~(align - 1);
}

static inline size_t align_down(size_t value, size_t align) {
  return value & ~(align - 1);
}

static inline uint32_t gen_movz(uint8_t rd, uint16_t imm16, uint16_t shift) {
  assert(shift % 16 == 0);
  const uint32_t sf = 1;
  const uint32_t hw = (uint32_t)(shift >> 4);
  assert(hw < 4);
  const uint32_t insn = (sf << 31) | (0xa5 << 23) | (hw << 21) |
                        ((uint32_t)imm16 << 5) | ((uint32_t)rd << 0);
  return insn;
}

static inline uint32_t gen_movk(uint8_t rd, uint16_t imm16, uint16_t shift) {
  assert(shift % 16 == 0);
  const uint32_t sf = 1;
  const uint32_t hw = (uint32_t)(shift >> 4);
  assert(hw < 4);
  const uint32_t insn = (sf << 31) | (0xe5 << 23) | (hw << 21) |
                        ((uint32_t)imm16 << 5) | ((uint32_t)rd << 0);
  return insn;
}

static inline void get_b_range(uintptr_t addr, uintptr_t *min, uintptr_t *max) {
  const int64_t range_min_off = -0x8000000;
  const int64_t range_max_off = 0x7fffffc;
  *min = (uintptr_t)((int64_t)addr + range_min_off);
  *max = (uintptr_t)((int64_t)addr + range_max_off);
}

static inline uint32_t gen_b(uintptr_t addr, uintptr_t target) {
  uintptr_t range_min = 0;
  uintptr_t range_max = 0;
  get_b_range(addr, &range_min, &range_max);
  assert(range_min <= target && target <= range_max);

  const int64_t off = (int64_t)target - (int64_t)addr;
  const uint32_t imm26 = (uint32_t)(off >> 2) & ((1L << 26L) - 1);
  const uint32_t insn = (0x5 << 26) | (imm26 << 0);

  return insn;
}

static inline uint32_t gen_br(uint8_t rn) {
  const uint32_t insn = (0x3587c0 << 10) | (rn << 5) | (0x0 << 0);
  return insn;
}

static inline bool is_svc(uint32_t insn) {
  return (insn & 0xffe0000f) == 0xd4000001;
}

struct records_entry {
  uintptr_t *records;
  size_t records_size;
  size_t count;
  uintptr_t reachable_range_min;
  uintptr_t reachable_range_max;
  void *trampoline;
  LIST_ENTRY(records_entry) entries;
};

LIST_HEAD(records_head, records_entry) head;

#define PAGE_SIZE (0x1000)

static const size_t jump_code_size = 5;
static const size_t svc_gate_size = 6;

static void init_records(struct records_entry *entry) {
  assert(entry != NULL);
  entry->trampoline = NULL;
  entry->reachable_range_min = 0;
  entry->reachable_range_max = UINT64_MAX;
  entry->count = 0;
  entry->records_size = PAGE_SIZE;
  entry->records = malloc(entry->records_size);
  assert(entry->records != NULL);
}

/* find svc using pattern matching */
static void record_svc(char *code, size_t code_size, int mem_prot) {
  /* add PROT_READ to read the code */
  assert(!mprotect(code, code_size, PROT_READ | PROT_EXEC));
  bool has_r = mem_prot & PROT_READ;
  bool has_w = mem_prot & PROT_WRITE;
  for (size_t off = 0; off < code_size; off += 4) {
    uint32_t *ptr = (uint32_t *)(((uintptr_t)code) + off);
    if (!is_svc(*ptr)) {
      continue;
    }
    uintptr_t addr = (uintptr_t)ptr;
    assert((addr & 0x3ULL) == 0);
    if ((addr == (uintptr_t)syscall_addr) ||
        (addr == (uintptr_t)do_rt_sigreturn)) {
      /*
       * skip the syscall replacement for
       * our system call hook (enter_syscall)
       * so that it can issue system calls.
       */
      continue;
    }

    uintptr_t range_min = 0;
    uintptr_t range_max = 0;
    get_b_range(addr, &range_min, &range_max);

    struct records_entry *entry = LIST_FIRST(&head);
    if (entry == NULL || entry->reachable_range_max < range_min) {
      /*
       * No entry found or the reachable range of the address is out of
       * reachable max range
       */
      entry = malloc(sizeof(struct records_entry));
      init_records(entry);
      LIST_INSERT_HEAD(&head, entry, entries);
      entry->reachable_range_max = range_max;
    }
    assert(entry != NULL);

    /* Embed mem prot info in the last two bits */
    uintptr_t record = addr | (has_r ? (1 << 1) : 0) | (has_w ? (1 << 0) : 0);
    entry->records[entry->count] = record;
    entry->count += 1;
    if (entry->count * sizeof(uintptr_t) >= entry->records_size) {
      entry->records_size *= 2;
      entry->records = realloc(entry->records, entry->records_size);
      assert(entry->records != NULL);
    }

    entry->reachable_range_min = range_min;
  }
  /* restore the memory protection */
  assert(!mprotect(code, code_size, mem_prot));
}

/* entry point for binary scanning */
static void scan_code(void) {
  LIST_INIT(&head);

  FILE *fp = NULL;
  /* get memory mapping information from procfs */
  assert((fp = fopen("/proc/self/maps", "r")) != NULL);
  {
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
      /* we do not touch stack memory */
      if (strstr(buf, "stack") == NULL) {
        int i = 0;
        char addr[65] = {0};
        char *c = strtok(buf, " ");
        while (c != NULL) {
          switch (i) {
            case 0:
              strncpy(addr, c, sizeof(addr) - 1);
              break;
            case 1: {
              int mem_prot = 0;
              for (size_t j = 0; j < strlen(c); j++) {
                if (c[j] == 'r') mem_prot |= PROT_READ;
                if (c[j] == 'w') mem_prot |= PROT_WRITE;
                if (c[j] == 'x') mem_prot |= PROT_EXEC;
              }
              size_t k = 0;
              for (k = 0; k < strlen(addr); k++) {
                if (addr[k] == '-') {
                  addr[k] = '\0';
                  break;
                }
              }
              int64_t from = strtol(&addr[0], NULL, 16);
              int64_t to = strtol(&addr[k + 1], NULL, 16);
              /* scan code if the memory is executable */
              if (mem_prot & PROT_EXEC) {
                record_svc((char *)from, (size_t)to - from, mem_prot);
              }
            } break;
          }
          if (i == 1) break;
          c = strtok(NULL, " ");
          i++;
        }
      }
    }
  }
  fclose(fp);
}

/* entry point for binary rewriting */
static void rewrite_code(void) {
  struct records_entry *entry;

  while (!LIST_EMPTY(&head)) {
    entry = LIST_FIRST(&head);

    bool mproect_active = false;
    uintptr_t mprotect_addr = UINTPTR_MAX;
    int mprotect_prot = 0;

    const uintptr_t trampoline = (uintptr_t)entry->trampoline;

    for (size_t i = 0; i < entry->count; i++) {
      uintptr_t record = entry->records[i];
      uintptr_t addr = record & ~0x3ULL;
      uint32_t *ptr = (uint32_t *)addr;

      int mem_prot = PROT_EXEC;
      mem_prot |= (record & 0x2) ? PROT_READ : 0;
      mem_prot |= (record & 0x1) ? PROT_WRITE : 0;

      if (mproect_active) {
        if (!((mprotect_addr <= addr) && (addr < mprotect_addr + PAGE_SIZE))) {
          /* mprotect is active, but the address is out-of-bounds */
          assert(!mprotect((void *)mprotect_addr, PAGE_SIZE, mprotect_prot));
          mprotect_addr = UINTPTR_MAX;
          mprotect_prot = 0;
          mproect_active = false;
        }
      }

      if (!mproect_active) {
        mprotect_addr = align_down(addr, PAGE_SIZE);
        mprotect_prot = mem_prot;
        mproect_active = true;
        assert(!mprotect((void *)mprotect_addr, PAGE_SIZE,
                         PROT_WRITE | PROT_READ | PROT_EXEC));
      }

      assert(is_svc(*ptr));
      const uintptr_t target =
          trampoline + (jump_code_size + svc_gate_size * i) * sizeof(uint32_t);
      *ptr = gen_b(addr, target);
    }

    if (mproect_active) {
      assert(!mprotect((void *)mprotect_addr, PAGE_SIZE, mprotect_prot));
      mprotect_addr = UINTPTR_MAX;
      mprotect_prot = 0;
      mproect_active = false;
    }

    LIST_REMOVE(head.lh_first, entries);
    free(entry->records);
    entry->records = NULL;
    free(entry);
    entry = NULL;
  }
}

static void setup_trampoline(void) {
  struct records_entry *entry = NULL;

  LIST_FOREACH(entry, &head, entries) {
    uintptr_t range_min = align_up(entry->reachable_range_min, PAGE_SIZE);
    uintptr_t range_max = align_down(entry->reachable_range_max, PAGE_SIZE);

    assert(range_min < UINT64_MAX);
    assert(range_max > 0);
    assert(range_max - range_min >= PAGE_SIZE);

    assert(entry->count * sizeof(uintptr_t) <= entry->records_size);

    const size_t mem_size = align_up(
        jump_code_size + svc_gate_size * sizeof(uint32_t) * entry->count,
        PAGE_SIZE);

    assert(range_min + mem_size <= range_max);

    assert(entry->trampoline == NULL);

    /* allocate memory at the aligned reachable address */
    entry->trampoline =
        mmap((void *)range_min, mem_size, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (entry->trampoline == MAP_FAILED) {
      fprintf(stderr, "map failed\n");
      exit(1);
    }

    /*
     * put common code to indirect branch to asm_syscall_hook
     *
     * do_jump_asm_syscall_hook:
     * movz	x10, (#asm_syscall_hook & 0xffff)
     * movk x10, ((#asm_syscall_hook >> 16) & 0xffff), lsl 16
     * movk x10, ((#asm_syscall_hook >> 32) & 0xffff), lsl 32
     * movk x10, ((#asm_syscall_hook >> 48) & 0xffff), lsl 48
     * br x10
     */
    const uintptr_t hook_addr = (uintptr_t)asm_syscall_hook;
    const uintptr_t do_jump_addr = (uintptr_t)entry->trampoline;

    size_t off = 0;
    uint32_t *code = (uint32_t *)entry->trampoline;
    code[off++] = gen_movz(10, (hook_addr >> 0) & 0xffff, 0);
    code[off++] = gen_movk(10, (hook_addr >> 16) & 0xffff, 16);
    code[off++] = gen_movk(10, (hook_addr >> 32) & 0xffff, 32);
    code[off++] = gen_movk(10, (hook_addr >> 48) & 0xffff, 48);
    code[off++] = gen_br(10);
    assert(off == jump_code_size);

    for (size_t i = 0; i < entry->count; i++) {
      /* TODO: preserve redzone */
      /* FIXME: We don't have to save full address */

      /*
       * put 'gate' code for each svc instruction
       *
       * stp	x9, x10, [sp,#-16]!
       * movz	x9, (#return_pc & 0xffff)
       * movk x9, ((#return_pc >> 16) & 0xffff), lsl 16
       * movk x9, ((#return_pc >> 32) & 0xffff), lsl 32
       * movk x9, ((#return_pc >> 48) & 0xffff), lsl 48
       * b do_jump_asm_syscall_hook
       */

      code[off++] = 0xa9bf2be9;

      const uintptr_t return_pc = (entry->records[i] & ~0x3) + sizeof(uint32_t);
      code[off++] = gen_movz(9, (return_pc >> 0) & 0xffff, 0);
      code[off++] = gen_movk(9, (return_pc >> 16) & 0xffff, 16);
      code[off++] = gen_movk(9, (return_pc >> 32) & 0xffff, 32);
      code[off++] = gen_movk(9, (return_pc >> 48) & 0xffff, 48);

      const uintptr_t current_pc = (uintptr_t)&code[off];
      code[off++] = gen_b(current_pc, do_jump_addr);
    }

    /*
     * mprotect(PROT_EXEC without PROT_READ), executed
     * on CPUs supporting Memory Protection Keys for Userspace (PKU),
     * configures this memory region as eXecute-Only-Memory (XOM).
     * this enables to cause a segmentation fault for a NULL pointer access.
     */
    assert(!mprotect(entry->trampoline, mem_size, PROT_EXEC));
  }
}

static void load_hook_lib(void) {
  void *handle;
  {
    const char *filename;
    filename = getenv("LIBSVCHOOK");
    if (!filename) {
      fprintf(stderr,
              "env LIBSVCHOOK is empty, so skip to load a hook library\n");
      return;
    }

    handle = dlmopen(LM_ID_NEWLM, filename, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      fprintf(stderr, "dlmopen failed: %s\n\n", dlerror());
      fprintf(
          stderr,
          "NOTE: this may occur when the compilation of your hook function "
          "library misses some specifications in LDFLAGS. or if you are using "
          "a C++ compiler, dlmopen may fail to find a symbol, and adding "
          "'extern \"C\"' to the definition may resolve the issue.\n");
      exit(1);
    }
  }
  {
    int (*hook_init)(long, ...);
    hook_init = dlsym(handle, "__hook_init");
    assert(hook_init);
    assert(hook_init(0, &hook_fn) == 0);
  }
}

__attribute__((constructor(0xffff))) static void __svc_hook_init(void) {
  scan_code();
  setup_trampoline();
  rewrite_code();
  load_hook_lib();
}