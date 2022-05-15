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

TaskSlot *TaskSlot_new( char const *command_line );
Task *Task_new( TaskSlot *slot );

uint32_t TaskSlot_Himem( TaskSlot *slot );
char const *TaskSlot_Command( TaskSlot *slot );
char const *TaskSlot_Tail( TaskSlot *slot );
void *TaskSlot_Time( TaskSlot *slot );

void TaskSlot_add( TaskSlot *slot, physical_memory_block memory );
uint32_t TaskSlot_asid( TaskSlot *slot );
physical_memory_block Kernel_physical_address( uint32_t va );

// This seems to be most at home in TaskSlot; each task will have its own
// current directory, etc.
// I think that a child process changing its working directory should affect
// the parent when it exits, but icbw.
void __attribute__(( noinline )) do_FSControl( uint32_t *regs );

struct Task {
  integer_registers regs;
  TaskSlot *slot;
  Task *next;
};

struct TaskSlot_workspace {
  Task *running;        // The task that is running on this core
  Task *runnable;       // The tasks that may only run on this core
  bool memory_mapped;   // Have the shared.task_slot.tasks_memory and shared.task_slot.slots_memory been mapped into this core's MMU?
};

struct TaskSlot_shared_workspace {
  uint32_t lock;

  uint32_t slots_memory; // FIXME more than one page, extendible, etc.
  uint32_t tasks_memory;

  Task *bottleneck_owner;
  Task *next_to_own;
  Task *last_to_own;

  Task *runnable;       // Tasks that may run on any core
  Task **core_runnable; // Array of Tasks that may run on that core
};

