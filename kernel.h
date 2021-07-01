/* Copyright 2021 Simon Willcocks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


typedef unsigned long long uint64_t;
typedef unsigned        uint32_t;
typedef int             int32_t;
typedef unsigned char   uint8_t;
typedef unsigned        size_t;
typedef unsigned        bool;
#define true  (0 == 0)
#define false (0 != 0)

typedef struct core_workspace core_workspace;
typedef struct shared_workspace shared_workspace;

#include "processor.h"
#include "boot.h"
#include "mmu.h"
#include "memory_manager.h"

typedef struct module module;

typedef struct vector vector;

struct vector {
  uint32_t code;
  uint32_t private_word;
  vector *next;
};

struct Kernel_workspace {
  uint32_t undef_stack[64];
  uint32_t abt_stack[64];
  uint32_t svc_stack[128];
  uint32_t irq_stack[64];
  uint32_t fiq_stack[64];
  const char *env;
  uint64_t start_time;
  module *module_list_head;
  module *module_list_tail;
  vector *vectors[0x25];
};

typedef struct fs fs;

struct Kernel_shared_workspace {
  fs *filesystems;
  uint32_t fscontrol_lock;
};

extern struct core_workspace {
  struct {
    uint32_t reset;
    uint32_t undef;
    uint32_t svc;
    uint32_t prefetch;
    uint32_t data;
    uint32_t irq;
    uint32_t fiq[512];

    // The vectors can be moved, but they must stay in the same order
    void (*reset_vec)();
    void (*undef_vec)();
    void (*svc_vec)();
    void (*prefetch_vec)();
    void (*data_vec)();
    void (*irq_vec)();
  } vectors;

  uint32_t core_number;
  struct MMU_workspace mmu;
  struct Kernel_workspace kernel;
  struct Memory_manager_workspace memory;
} workspace;

extern struct shared_workspace {
  struct MMU_shared_workspace mmu;
  struct Kernel_shared_workspace kernel;
  struct Memory_manager_shared_workspace memory;
} shared;

void Generate_the_RMA();

// microclib

static inline int strcmp( const char *left, const char *right )
{
  int result = 0;
  while (result == 0 && *left != 0 && *right != 0) {
    result = *left++ - *right++;
  }
  return result;
}

void *memset(void *s, int c, uint32_t n);
