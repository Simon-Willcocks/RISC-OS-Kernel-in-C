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

struct Kernel_workspace {
  uint32_t undef_stack[128];
  uint32_t abt_stack[128];
  uint32_t svc_stack[128];
  uint32_t irq_stack[128];
  uint32_t fiq_stack[128];
};

struct Kernel_shared_workspace {
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
