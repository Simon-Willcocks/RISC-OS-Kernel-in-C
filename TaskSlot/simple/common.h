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
#include "include/doubly_linked_list.h"

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

  uint32_t lock;
  physical_memory_block blocks[50];
  handler handlers[17];
  Task *creator; // creator's slot is parent slot
  char const *command;
  char const *name;
  char const *tail;
  uint64_t start_time;
  Task *waiting;       // 0 or more tasks waiting for locks

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

// Declare functions like dll_attach_Task
dll_type( Task );

extern svc_registers svc_stack_top;

static inline void *core_svc_stack_top()
{
  return (&workspace.kernel.svc_stack + 1);
}

static inline bool using_slot_svc_stack()
{
  register uint32_t sp asm ( "sp" );
  return (sp >> 20) == (((uint32_t) &svc_stack_top) >> 20);
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

void kick_debug_handler_thread();
bool this_is_debug_receiver();
