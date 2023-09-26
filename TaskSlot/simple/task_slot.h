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

// FIXME physical_memory_block probably shouldn't have a virtual address

typedef struct Task Task;
typedef struct svc_registers svc_registers;

// For use by modules.c ???
error_block *queue_Task( uint32_t queue, Task *task, uint32_t SWI );

// Root slot, does not require RMA or regs. Call once only per core.
TaskSlot *TaskSlot_first();

// New TaskSlot and a Task to run the command, replacing the calling
// Task which will be resumed when the slot exits or
// TaskSlot_detach_from_creator is called.
// The new slot will have NO application memory allocted to it.
TaskSlot *TaskSlot_new( char const *command_line );

// Resume the task that created the TaskSlot, returning it a handle to
// the independently running program.
void TaskSlot_detach_from_creator( TaskSlot *slot );

// Replaces the old application, using the same application memory.
void TaskSlot_new_application( char const *command, char const *args );
void __attribute__(( noreturn )) TaskSlot_enter_application( void *start_address, void *private_word );

Task *Task_new( TaskSlot *slot );

TaskSlot *TaskSlot_now();
Task *Task_now();

uint32_t TaskSlot_Himem( TaskSlot *slot );
char const *TaskSlot_Command( TaskSlot *slot );
char const *TaskSlot_Tail( TaskSlot *slot );
void *TaskSlot_Time( TaskSlot *slot );
void TaskSlot_adjust_app_memory( TaskSlot *slot, uint32_t new_limit );

// Allocate 256 bytes of RMA space one time per slot, then return
// the same address each call.
// Returns uint32_t to be written to r1 on Wimp_Poll(Idle)
uint32_t TaskSlot_WimpPollBlock( TaskSlot *slot );

void TaskSlot_add( TaskSlot *slot, physical_memory_block memory );
uint32_t TaskSlot_asid( TaskSlot *slot );
physical_memory_block Kernel_physical_address( uint32_t va );

// This seems to be most at home in TaskSlot; each task will have its own
// current directory, etc.
// I think that a child process changing its working directory should affect
// the parent when it exits, but icbw.
void __attribute__(( noinline )) do_FSControl( uint32_t *regs );
void __attribute__(( noinline )) do_UpCall( uint32_t *regs );

// While waiting for a full re-write, block the task until there's
// no-one else using the non-reentrant kernel, then re-try the SWI
// from the point it was called (which must always be usr32 mode, atm).
bool Task_kernel_in_use( svc_registers *regs );
void Task_kernel_release();

typedef struct callback transient_callback;
typedef struct handler handler;

struct handler {
  void (* code)();
  uint32_t private_word;
  uint32_t buffer;
};

struct TaskSlot_workspace {
  Task *running;        // The task that is running on this core
  bool memory_mapped;   // Have the shared.task_slot.tasks_memory and shared.task_slot.slots_memory been mapped into this core's MMU?
  Task *sleeping;       // 0 or more sleeping tasks
  uint32_t sleep_lock;  // Claim while inserting or waking tasks from queue

  Task **irq_tasks;     // Array of tasks handling interrupts 
  char core_number_string[4]; // For OS_TaskSlot, 64 (CoreNumber)

  // FIXME debug only
  uint32_t irqs_usr;
  uint32_t irqs_svc;
  uint32_t irqs_sys;

  uint32_t svc_stack_nothing_waiting;
  uint32_t svc_stack_claims;
  uint32_t svc_stack_releases;
  uint32_t svc_stack_resets;
};

struct TaskSlot_shared_workspace {
  uint32_t lock;

  Task *legacy_stack_owner; // pointer to a Task which is probably also in a running list.
  Task *waiting_for_legacy_svc_stack; // List of Tasks that have been blocked.

  bool callback_requested;

  uint32_t queues_lock;
  // TODO move from kernel?
  // uint32_t pipes_lock;

  // To be called before releasing the legacy svc stack
  // transient_callbacks are only added by legacy code
  transient_callback *transient_callbacks;

  uint32_t slots_memory; // FIXME more than one page, extendible, etc.
  uint32_t tasks_memory;

  Task *tasks_pool;
  Task *next_to_allocate; // For when the pool needs expanding

  Task *runnable;       // Tasks that may run on any core

  uint32_t number_of_interrupt_sources;
  Task **irq_tasks;     // Array of tasks handling interrupts, number of cores x number of sources
};
