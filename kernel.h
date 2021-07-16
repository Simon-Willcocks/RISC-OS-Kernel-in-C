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

#define number_of( arr ) (sizeof( arr ) / sizeof( arr[0] ))

typedef struct core_workspace core_workspace;
typedef struct shared_workspace shared_workspace;

#include "processor.h"
#include "boot.h"
#include "mmu.h"
#include "memory_manager.h"

typedef struct module module;
typedef struct callback vector;
typedef struct callback transient_callback;
typedef struct variable variable;

struct callback {
  uint32_t code;
  uint32_t private_word;
  struct callback *next;
};

// Stacks are probably too large, I hope
struct Kernel_workspace {
  uint32_t svc_stack[1280]; // Most likely to overflow; causes a data abort, for testing.
  uint32_t undef_stack[640];
  uint32_t abt_stack[640];
  uint32_t irq_stack[640];
  uint32_t fiq_stack[640];
  const char *env;
  uint64_t start_time;
  module *module_list_head;
  module *module_list_tail;
  uint32_t DomainId;
  vector *vectors[0x25];
  variable *variables; // Should be shared?
  transient_callback *transient_callbacks;
  // I cannot tell a lie, this is because there's no HeapFree
  // implementation, yet, but it's probably also an efficient
  // approach:
  transient_callback *transient_callbacks_pool;
};

struct VDU_workspace {
  uint32_t changed_box_tracking_enabled;
  struct {
    uint32_t enabled;
    uint32_t left;
    uint32_t bottom;
    uint32_t right;
    uint32_t top;
  } ChangedBox;
  uint32_t modevars[13];
  uint32_t vduvars[44];
  uint32_t textwindow[2];
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
  struct VDU_workspace vdu;
  struct Kernel_workspace kernel;
  struct Memory_manager_workspace memory;
} workspace;

extern struct shared_workspace {
  struct MMU_shared_workspace mmu;
  struct Kernel_shared_workspace kernel;
  struct Memory_manager_shared_workspace memory;
} volatile shared;

void __attribute__(( noreturn )) Boot();

// microclib

static inline int strlen( const char *left )
{
  int result = 0;
  while (*left++ != '\0') result++;
  return result;
}

static inline int strcmp( const char *left, const char *right )
{
  int result = 0;
  while (result == 0 && *left != 0 && *right != 0) {
    result = *left++ - *right++;
  }
  return result;
}

void *memset(void *s, int c, uint32_t n);
void *memcpy(void *d, const void *s, uint32_t n);
