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

#ifndef __KERNEL_H
#define __KERNEL_H

#include "include/types.h"
#include "include/pico_clib.h"

typedef struct {
  uint32_t code;
  char desc[];
} error_block;

typedef struct {
  error_block *error;
  void *location;
  uint32_t available; 
} PipeSpace;

typedef struct core_workspace core_workspace;
typedef struct shared_workspace shared_workspace;

#include "processor.h"
#include "boot.h"
#include "mmu.h"
#include "memory_manager.h"
#include "task_slot.h"

typedef struct module module;
typedef struct callback callback;
typedef callback vector;
typedef callback transient_callback;
typedef struct variable variable;
typedef struct os_pipe os_pipe;

typedef struct ticker_event ticker_event;
struct ticker_event {
  uint32_t code;
  uint32_t private_word;
  uint32_t remaining;
  uint32_t reload;
  ticker_event *next;
  ticker_event *prev; // Doubly linked list
};

// Stacks sizes need to be checked (or use the zp memory)
struct Kernel_workspace {
  uint32_t frame_buffer_initialised; // FIXME: Remove: this is for debugging only, and only says the HAL module has been initialised

  // For use until SharedCLibrary-friendly stack set up.
  // And for use while swapping current task
  struct { // In struct so that ((&...svc_stack)+1) is top of stack
    uint32_t s[640];
  } svc_stack;

  uint32_t debug_pipe;
  uint32_t debug_written; // Written, but not reported to the pipe
  PipeSpace debug_space;

  module *module_list_head;
  module *module_list_tail;
  uint32_t DomainId;
  vector *vectors[64];   // https://www.riscosopen.org/wiki/documentation/show/Software%20Vector%20Numbers
  vector *default_vectors[64]; // Needs to be core-specific for, e.g., DrawV, shouldn't be for others...

  // 0 -> disabled
  // There is no associated code, it will be listening for EventV.
  uint32_t event_enabled[29];

  variable *variables; // Should be shared?

  ticker_event *ticker_queue;

  struct {
    uint32_t abt[64];
  } abort_stack;

  struct {
    uint32_t und[64];
  } undef_stack;

  struct {
    uint32_t irq[64];
  } irq_stack;

  struct {
    uint32_t fiq[64];
  } fiq_stack;
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
  struct {
    int16_t x;
    int16_t y;
  } plot_points[3];
  // uint32_t modevars[13];
  // uint32_t vduvars[45];
  // FIXME: these should be in a graphics context of some kind
  // uint32_t textwindow[2];
};

typedef struct fs fs;

struct Kernel_shared_workspace {
  uint32_t boot_lock;
  fs *filesystems;
  uint32_t fscontrol_lock;

  uint32_t commands_queue;

  // Only one multiprocessing module can be initialised at at time (so the 
  // first has a chance to initialise their shared workspace).
  uint32_t mp_module_init_lock;

  // FIXME: no longer true?
  // The elements of this linked list won't be used directly, they're a place to hold the private word for
  // multi-processing modules; all cores share the same private word.
  module *module_list_head;
  module *module_list_tail;

  callback *callbacks_pool;
  ticker_event *ticker_event_pool;

  // TODO move to task_slot?
  uint32_t pipes_lock;
  os_pipe *pipes;

  uint32_t screen_lock; // Not sure if this will always be wanted; it might make sense to make the screen memory outer (only) sharable, and flush the L1 cache to it before releasing this lock.
};

// When this include is no longer necessary, I will be very happy!
#include "Legacy/ZeroPage.h"

extern struct core_workspace {
  union {
    struct {
      uint32_t reset;
      uint32_t undef;
      uint32_t svc;
      uint32_t prefetch;
      uint32_t data;
      uint32_t unused_vector;
      uint32_t irq;
      uint32_t fiq[2]; // Shrunk, because legacy zero page starts lower than I thought.

      // The vectors can be moved, but they must stay in the same order
      void (*reset_vec)();
      void (*undef_vec)();
      void (*svc_vec)();
      void (*prefetch_vec)();
      void (*data_vec)();
      uint32_t unused;
      void (*irq_vec)();
    };
    LegacyZeroPage zp;
  } vectors;

  uint32_t core_number;
  struct MMU_workspace mmu;
  struct VDU_workspace vdu;
  struct Kernel_workspace kernel;
  struct Memory_manager_workspace memory;
  struct TaskSlot_workspace task_slot;
} workspace;

extern struct shared_workspace {
  struct MMU_shared_workspace mmu;
  struct Kernel_shared_workspace kernel;
  struct Memory_manager_shared_workspace memory;
  struct TaskSlot_shared_workspace task_slot;
} shared;

void __attribute__(( noreturn )) Boot();

// Spin lock for a core to initialise a word (to anything but 0 or 1!)
static inline bool mpsafe_initialise( uint32_t volatile *word, uint32_t (*fn)() )
{
  uint32_t attempt = change_word_if_equal( word, 0, 1 );
  if (attempt == 0) {
    *word = fn();
    asm ( "sev" );
  }
  else {
    while (1 == *word) {
      asm( "wfe" );
    }
  }
  return attempt == 0; // This was the core that initialised it
}

#endif
