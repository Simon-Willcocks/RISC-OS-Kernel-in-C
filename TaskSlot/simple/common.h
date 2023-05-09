/* Copyright 2021-2023 Simon Willcocks
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

/* This file encapsulates how the TaskSlot structure is maintained.
 * All modifications to the set of slots or the content of a slot shall be
 * protected by claiming shared.mmu.lock or using the mpsafe_dll functions.
 */

#include "inkernel.h"

// Tasks reside in doubly linked lists
#include "include/doubly_linked_list.h"

// The lists are often accessed from more than one core simultaneously.
// The mpsafe functions protect the list from being corrupted and execute
// with O(1) (the number of items in the list is irrelevant).
#include "include/mpsafe_dll.h"

#include "include/callbacks.h"

typedef struct handler handler;
typedef struct os_pipe os_pipe;

struct handler {
  void (* code)();
  uint32_t private_word;
  uint32_t buffer;
};

struct TaskSlot {
  // FIXME Locks the slot to a single core (at a time)?
  // Is that so terrible?
  uint32_t *svc_sp_when_unmapped;
  Task *svc_stack_owner;
  Task *waiting_for_slot_stack;

  transient_callback *transient_callbacks;

  uint32_t lock;
  physical_memory_block blocks[50];
  handler handlers[17];
  Task *creator; // creator's slot is parent slot
  char const *command;
  char const *name;
  char const *tail;
  uint64_t start_time;
  Task *waiting;       // 0 or more tasks waiting for locks

  bool callback_requested;

  uint32_t *wimp_poll_block;
  Task *wimp_task;
  uint32_t wimp_task_handle;
};

struct __attribute__(( packed, aligned( 4 ) )) Task {
  svc_registers regs;
  uint32_t banked_sp_usr; // Only stored when leaving usr or sys mode
  uint32_t banked_lr_usr; // Only stored when leaving usr or sys mode
  int32_t resumes;
  TaskSlot *slot;
  Task *next; // Doubly-linked list. Neither next or prev shall be zero,
  Task *prev; // Tasks not in a list will be a list of 1.
};

// Declare functions like dll_attach_Task and mpsafe_detach_Task_head
MPSAFE_DLL_TYPE( Task );

extern svc_registers svc_stack_top;

static inline void *core_svc_stack_top()
{
  return (&workspace.kernel.svc_stack + 1);
}

static inline bool using_slot_svc_stack()
{
  // Cannot use register sp asm( "sp" ), the optimiser may cache the result
  uint32_t sp_section;
  asm ( "mov %[sp], sp, lsr #20" : [sp] "=r" (sp_section) );
  return sp_section == (((uint32_t) &svc_stack_top) >> 20);
}

static inline bool using_core_svc_stack()
{
  register uint32_t sp asm ( "sp" );
  return (sp >= (uint32_t) &workspace.kernel.svc_stack)
      && (sp <= (uint32_t) core_svc_stack_top());
}

static inline bool usr32_caller( svc_registers *regs )
{
  return (regs->spsr & 0xf) == 0;
}

static inline bool owner_of_slot_svc_stack( Task *task )
{
  return task->slot->svc_stack_owner == task;
}

static inline Task *task_from_handle( uint32_t handle )
{
  return (Task *) handle;
}

static inline uint32_t handle_from_task( Task *task )
{
  return (uint32_t) task;
}

// This routine must be called on the old value before
// changing workspace.task_slot.running in response to
// a SWI.
// It is ESSENTIAL that this is called BEFORE adding the
// task to a shared list; another core might pick it up
// before this one has a chance to store it.
// (If this changes, the Kernel_default_irq routine
// will also have to be changed. Possibly undef and
// abort, too.)
static inline void save_task_context( Task *task, svc_registers const *regs )
{
  task->regs = *regs;

  // TODO floating point context, etc.; these should be done
  // in a lazy way, trapping the next use of FP and storing
  // and restoring its state then, if necessary.
  // Each task should have its own state, but only if FP used.
}

void kick_debug_handler_thread();
bool this_is_debug_receiver();

static inline bool is_a_task( Task *t )
{
  // TODO maybe a little more?
  // Like allocated... Bigger than 64k...
  extern Task tasks[];
  return (((uint32_t) t) >> 16) == (((uint32_t) &tasks) >> 16);
}

