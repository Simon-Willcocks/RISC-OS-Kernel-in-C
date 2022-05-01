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

/* This file encapsulates how the TaskSlot structure is maintained.
 * All modifications to the set of slots or the content of a slot shall be protected
 * by claiming shared.mmu.lock.
 */

#include "inkernel.h"

// Initial version, using arrays rather than linked lists or similar.
// A TaskSlot is essentially a user process, with blocks of RAM located at 0x8000
// This can be extended to implement threads. TODO
// Since the filing system in RO is aggressively attuned to a single processor
// model, running a single program at a time, other than Wimp tasks, each TaskSlot will
// need to store its idea of the filing system context (CSD, etc.) TODO rewrite that!
// Also, it should probably create a Code Sys$ReturnCode that can update the
// parent TaskSlot.

typedef struct handler handler;

struct handler {
  uint32_t code;
  uint32_t private_word;
  uint32_t buffer;
};

struct TaskSlot {
  bool allocated;
  physical_memory_block blocks[10];
  handler handlers[17];
  char const *command;
  uint64_t start_time;
  Task task;
};

extern TaskSlot task_slots[];
extern Task tasks[];

/* Handlers. Per slot, or per thread, I wonder? I'll go for per slot, to start with (matches with the idea of
   cleaning up the handlers on exit).
0 	Memory limit 	Memory limit 	Unused 	Unused
1 	Undefined instruction 	Handler code 	Unused 	Unused
2 	Prefetch abort 	Handler code 	Unused 	Unused
3 	Data abort 	Handler code 	Unused 	Unused
4 	Address exception 	Handler code 	Unused 	Unused
5 	Other exceptions (reserved) 	Unused 	Unused 	Unused
6 	Error 	Handler code 	Handler R0 	Error buffer
7 	CallBack 	Handler code 	Handler R12 	Register dump buffer
8 	BreakPoint? 	Handler code 	Handler R12 	Register dump buffer
9 	Escape 	Handler code 	Handler R12 	Unused
10 	Event? 	Handler code 	Handler R12 	Unused
11 	Exit 	Handler code 	Handler R12 	Unused
12 	Unused SWI? 	Handler code 	Handler R12 	Unused
13 	Exception registers 	Register dump buffer 	Unused 	Unused
14 	Application space 	Memory limit 	Unused 	Unused
15 	Currently active object 	CAO pointer 	Unused 	Unused
16 	UpCall 	Handler code 	Handler R12 	Unused
*/

void __attribute__(( noinline )) do_ChangeEnvironment( uint32_t *regs )
{
  if (regs[0] > 16) {
    asm( "bkpt 1" );
  }

#if 0
  // Called for 14, from DeviceFS initialisation
  // Do I need DeviceFS yet?
  if (workspace.task_slot == 0 || workspace.task_slot.running == 0) {
    regs[1] = 0;
    regs[2] = 0;
    regs[3] = 0;
    return true;
  }
#endif
  assert( workspace.task_slot.running != 0 );

  Task *running = workspace.task_slot.running;

  assert( running->slot != 0 );

  TaskSlot *slot = running->slot;

  handler *h = &slot->handlers[regs[0]];
  handler old = *h;
  if (regs[1] != 0) {
    h->code = regs[1];
  }
  if (regs[2] != 0) {
    h->private_word = regs[2];
  }
  if (regs[3] != 0) {
    h->buffer = regs[3];
  }

  regs[1] = old.code;
  regs[2] = old.private_word;
  regs[3] = old.buffer;
}

void __attribute__(( naked )) default_os_changeenvironment()
{
  register uint32_t *regs;
  asm ( "push { r0-r3 }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  do_ChangeEnvironment( regs );

  asm ( "pop { r0-r3, pc }" );
}

static inline bool is_in_tasks( uint32_t va )
{
  // FIXME More pages!
  return va >= (uint32_t) tasks && va < 4096 + (uint32_t) tasks;
}

static inline bool is_in_task_slots( uint32_t va )
{
  // FIXME More pages!
  return va >= (uint32_t) task_slots && va < 4096 + (uint32_t) task_slots;
}

physical_memory_block Kernel_physical_address( uint32_t va )
{
#ifdef GOTITDONE
  // FIXME : More than a couple of slots or tasks!
  // FIXME : access privileges (in res)
  if (is_in_tasks( va )) {
    physical_memory_block block = { .virtual_base = ((uint32_t) tasks) >> 12,
                                    .physical_base = shared.task_slot.tasks_memory >> 12,
                                    .size = 1 };
    return block;
  }

  if (is_in_task_slots( va )) {
    physical_memory_block block = { .virtual_base = ((uint32_t) task_slots) >> 12,
                                    .physical_base = shared.task_slot.slots_memory >> 12,
                                    .size = 1 };
    return block;
  }
#endif

  assert( workspace.task_slot.running != 0 );

  Task *running = workspace.task_slot.running;

  assert( running->slot != 0 );

  TaskSlot *slot = running->slot;

WriteS( "Searching slot " ); WriteNum( (uint32_t) slot ); WriteS( " for address " ); WriteNum( va ); NewLine;
  if (slot != 0) {
    for (int i = 0; i < number_of( slot->blocks ) && slot->blocks[i].size != 0 && slot->blocks[i].virtual_base <= va; i++) {
WriteS( "Block: " ); WriteNum( slot->blocks[i].virtual_base ); WriteS( ", " ); WriteNum( slot->blocks[i].size ); NewLine;
      if (slot->blocks[i].virtual_base <= va && slot->blocks[i].virtual_base + slot->blocks[i].size > va) {
        return slot->blocks[i];
      }
    }
  }
  else WriteS( "No current slot" );

WriteS( "No memory found" ); NewLine;
asm ( "bkpt 44" );
  physical_memory_block fail = { 0, 0, 0 };
  return fail;
}

static void free_task( Task *task )
{
  task->regs.pc = 1; // Never a valid pc, so unallocated
}

static void free_task_slot( TaskSlot *slot )
{
  slot->allocated = 0;
}

static void allocate_taskslot_memory()
{
  // Only called when lock acquired
  bool first_core = (shared.task_slot.slots_memory == 0);

  if (first_core) {
    shared.task_slot.slots_memory = Kernel_allocate_pages( 4096, 4096 );
    shared.task_slot.tasks_memory = Kernel_allocate_pages( 4096, 4096 );
  }

  // No lazy address decoding for the kernel, yet

  if (!workspace.task_slot.memory_mapped) {
    MMU_map_shared_at( (void*) &tasks, shared.task_slot.slots_memory, 4096 );
    MMU_map_shared_at( (void*) &task_slots, shared.task_slot.tasks_memory, 4096 );
    workspace.task_slot.memory_mapped = true;
  }

  if (first_core) {
    WriteS( "Initialising tasks and task slots" );
    bzero( task_slots, 4096 );
    bzero( tasks, 4096 );
    for (int i = 0; i < 4096/sizeof( TaskSlot ); i++) {
      free_task_slot( &task_slots[i] );
    }
    for (int i = 0; i < 4096/sizeof( Task ); i++) {
      free_task( &tasks[i] );
    }
    NewLine;
  }
}

// Which comes first, the slot or the task? Privileged (module?) tasks don't need a slot.
TaskSlot *TaskSlot_new( char const *command_line )
{
  TaskSlot *result = 0;

  claim_lock( &shared.mmu.lock );

  if (!workspace.task_slot.memory_mapped) allocate_taskslot_memory();

  // FIXME: make this a linked list of free objects
  for (int i = 0; i < 4096/sizeof( TaskSlot ) && result == 0; i++) {
    if (!task_slots[i].allocated) {
WriteS( "Allocated TaskSlot " ); WriteNum( i ); NewLine;
      result = &task_slots[i];
      result->allocated = true;
    }
  }

  release_lock( &shared.mmu.lock );

  if (result == 0) for (;;) { asm ( "bkpt 32" ); } // FIXME: expand

  static const handler default_handlers[17] = {
    { 0, 0, 0 },                // RAM Limit for program (0x8000 + amount of RAM)
    { 0xbadf00d1, 0, 0 },
    { 0xbadf00d2, 0, 0 },
    { 0xbadf00d3, 0, 0 },
    { 0xbadf00d4, 0, 0 },
    { 0xbadf00d5, 0, 0 },
    { 0xbadf00d6, 0, 0 },
    { 0xbadf00d7, 0, 0 },
    { 0xbadf00d8, 0, 0 },
    { 0xbadf00d9, 0, 0 },
    { 0xbadf00da, 0, 0 },
    { 0xbadf00db, 0, 0 },
    { 0xbadf00dc, 0, 0 },
    { 0xbadf00dd, 0, 0 },
    { 0, 0, 0 },                // Application space (When does this not = RAM Limit?)
    { 0xbadf00df, 0, 0 },
    { 0xbadf00e0, 0, 0 }
  };

  for (int i = 0; i < number_of( result->handlers ); i++) {
    assert( i < number_of( default_handlers ) );

    result->handlers[i] = default_handlers[i];
  }

  svc_registers regs;
  char *copy = rma_allocate( strlen( command_line ), &regs );
  strcpy( copy, command_line );
  result->command = copy;
  result->start_time = 0; // cs since Jan 1st 1900 TODO

#ifdef DEBUG__WATCH_TASK_SLOTS
  Write0( "TaskSlot_new " ); WriteNum( (uint32_t) result ); NewLine;
#endif

  return result;
}

Task *Task_new( TaskSlot *slot )
{
  Task *result = 0;

  claim_lock( &shared.mmu.lock );

  if (!workspace.task_slot.memory_mapped) allocate_taskslot_memory();

  // FIXME: make this a linked list of free objects
  for (int i = 0; i < 4096/sizeof( Task ) && result == 0; i++) {
    if (0 != (tasks[i].regs.pc & 1)) {
      result = &tasks[i];
      result->regs.pc = 0; // Allocated
    }
  }

  release_lock( &shared.mmu.lock );

  if (result == 0) for (;;) { asm ( "bkpt 33" ); } // FIXME: expand

  result->slot = slot;

  return result;
}

void TaskSlot_add( TaskSlot *slot, physical_memory_block memory )
{
  claim_lock( &shared.mmu.lock );
  for (int i = 0; i < number_of( slot->blocks ); i++) {
    if (slot->blocks[i].size == 0) {
      slot->blocks[i] = memory;
#ifdef DEBUG__WATCH_TASK_SLOTS
  Write0( "TaskSlot_add " ); WriteNum( (uint32_t) slot ); Write0( " " ); WriteNum( slot->blocks[i].virtual_base ); Write0( " " ); WriteNum( slot->blocks[i].size ); NewLine;
#endif

      break;
    }
  }
  release_lock( &shared.mmu.lock );
}

uint32_t TaskSlot_asid( TaskSlot *slot )
{
  uint32_t result = (slot - task_slots) + 1;
#ifdef DEBUG__WATCH_TASK_SLOTS
Write0( "TaskSlot_asid " ); WriteNum( result ); NewLine;
#endif
  return result;
}

uint32_t TaskSlot_Himem( TaskSlot *slot )
{
  uint32_t result;
  // This will do until slots can include non-contiguous memory, be resized, etc.

  // FIXME lock per slot?
  claim_lock( &shared.mmu.lock );

#ifdef DEBUG__WATCH_TASK_SLOTS
  Write0( "TaskSlot_Himem " ); WriteNum( (uint32_t) slot ); Write0( " " ); WriteNum( slot->blocks[0].virtual_base ); Write0( " " ); WriteNum( slot->blocks[0].size ); NewLine;
#endif

  result = slot->blocks[0].size + slot->blocks[0].virtual_base;
  release_lock( &shared.mmu.lock );
  return result;
}

void *TaskSlot_Time( TaskSlot *slot )
{
  return &slot->start_time;
}

char const *TaskSlot_Command( TaskSlot *slot )
{
  return slot->command;
}

void __attribute__(( noinline )) do_FSControl( uint32_t *regs )
{
  Write0( __func__ );
  switch (regs[0]) {
  case 2:
    // Fall through
  default:
    WriteNum( regs[0] ); NewLine; asm ( "bkpt 1" );
  }
}
