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
 * All modifications to the set of slots or the content of a slot shall be
 * protected by claiming shared.mmu.lock.
 */

#include "inkernel.h"
#include "include/doubly_linked_list.h"

// Initial version, using arrays rather than linked lists or similar.
// A TaskSlot is essentially a user process, with blocks of RAM located at 0x8000
// Since the filing system in RO is aggressively attuned to a single processor
// model, running a single program at a time, other than Wimp tasks, each TaskSlot will
// need to store its idea of the filing system context (CSD, etc.) TODO rewrite that!
// Also, it should probably create a Code Sys$ReturnCode that can update the
// parent TaskSlot.

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

extern svc_registers svc_stack_top;

static inline void *core_svc_stack_top()
{
  return (&workspace.kernel.svc_stack + 1);
}

static inline void copy_task_state_without_links( Task *from, Task *to )
{
  to->regs = from->regs;
  to->banked_sp_usr = from->banked_sp_usr;
  to->banked_lr_usr = from->banked_lr_usr;
  to->resumes = from->resumes;
  to->slot = from->slot;
}

/* static inline */ bool __attribute__(( noinline )) using_slot_svc_stack()
{
  register uint32_t sp asm ( "sp" );
  return (sp >> 20) == (((uint32_t) &svc_stack_top) >> 20);
}

/* static inline */ bool __attribute__(( noinline )) using_core_svc_stack()
{
  register uint32_t sp asm ( "sp" );
  return (sp >= (uint32_t) &workspace.kernel.svc_stack)
      && (sp <= (uint32_t) core_svc_stack_top());
}

/* static inline */ bool __attribute__(( noinline )) usr32_caller( svc_registers *regs )
{
  return (regs->spsr & 0xf) == 0;
}

/* static inline */ bool __attribute__(( noinline )) owner_of_slot_svc_stack( Task *task )
{
  return task->slot->svc_stack_owner == task;
}

// Declare functions like dll_attach_Task
dll_type( Task );

extern TaskSlot task_slots[];
extern Task tasks[];

// For debugging only FIXME
#include "trivial_display.h"

static inline void show_task_state( Task *t, uint32_t colour )
{
  if (0 != (t->regs.lr & 3)) return;

  uint32_t x = (t - tasks) * 80;

  show_word( x, 780, (uint32_t) t, Red );

  uint32_t *r = &t->regs.r[0];
  for (int i = 0; i < sizeof( Task ) / 4; i++) {
    show_word( x, 800 + 8 * i, r[i], colour );
  }
}

static inline bool is_irq_task( Task *t )
{
  for (int i = 0; i < shared.task_slot.number_of_interrupt_sources; i++) {
    if (workspace.task_slot.irq_tasks[i] == t) return true;
  }
  return false;
}

bool show_tasks_state()
{
  for (int i = 0; i < 20; i++) {
    Task *t = &tasks[i];
      show_task_state( t, White ); //is_irq_task( t ) ? Blue : White );
    if (0 == (t->regs.lr & 1)) {
      //if (t == workspace.task_slot.running) continue;
      show_task_state( t, is_irq_task( t ) ? Blue : White );
    }
  }

  Task *t = workspace.task_slot.running;
  uint32_t colour = Green;
  do {
    show_task_state( t, colour );
    colour = Yellow;
    t = t->next;
  } while (t != workspace.task_slot.running);

  return true;
}

static inline void show_running_queue( int x )
{
  Task *t = workspace.task_slot.running;
  int y = 100;
  WriteS( "Running: " );
  do {
    WriteNum( t ); Space;
    show_word( x, y, (uint32_t) t, Green );
    t = t->next;
    y += 10;
  } while (t != workspace.task_slot.running);
  show_word( x, y, 0, Red );
  NewLine;
}

static inline TaskSlot *slot_from_handle( uint32_t handle )
{
  return (TaskSlot *) handle;
}

static inline uint32_t handle_from_slot( TaskSlot *slot )
{
  return (uint32_t) slot;
}

static inline Task *task_from_handle( uint32_t handle )
{
  return (Task *) handle;
}

static inline uint32_t handle_from_task( Task *task )
{
  return (uint32_t) task;
}

static inline os_pipe *pipe_from_handle( uint32_t handle )
{
  return (os_pipe *) handle;
}

static inline uint32_t handle_from_pipe( os_pipe *pipe )
{
  return (uint32_t) pipe;
}

bool do_OS_GetEnv( svc_registers *regs )
{
  Task *task = workspace.task_slot.running;

  if (task->slot != 0) {
    regs->r[0] = (uint32_t) TaskSlot_Command( task->slot );
    regs->r[1] = TaskSlot_Himem( task->slot );
    regs->r[2] = (uint32_t) TaskSlot_Time( task->slot );
  }
  else {
    regs->r[0] = (uint32_t) "ModuleTask";
    regs->r[1] = 0x8000;
    regs->r[2] = 0;
  }

  return true;
}

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
  assert( workspace.task_slot.running != 0 );

  Task *running = workspace.task_slot.running;

  assert( running->slot != 0 );

  TaskSlot *slot = running->slot;

  if (regs[0] > number_of( slot->handlers )) {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
  }

  handler *h = &slot->handlers[regs[0]];

  if (regs[0] == 0 || regs[0] == 14) {
    //  0 Memory Limit (special case)
    // 14 Application Space (special case)
    //    When are they different?
    //    Only 0 is writable, afaict.

    // R2 and R3 are ignored, may be set to "random" values by callers.

    if (regs[0] == 0 && regs[1] != 0) {
      TaskSlot_adjust_app_memory( slot, (regs[1] + 0xfff) & ~0xfff );
    }

    h->code = (void (*)()) TaskSlot_Himem( workspace.task_slot.running->slot );
  }

  handler old = *h;
  if (regs[1] != 0) {
    h->code = (void*) regs[1];
  }
  if (regs[2] != 0) {
    h->private_word = regs[2];
  }
  if (regs[3] != 0) {
    h->buffer = regs[3];
  }

#ifdef DEBUG__SHOW_ENVIRONMENT_CHANGES
  WriteS( "Changed environment " ); WriteNum( regs[0] ); NewLine;
  WriteNum( regs[1] ); Space; WriteNum( regs[2] ); Space; WriteNum( regs[3] ); NewLine;
  WriteNum( old.code ); Space; WriteNum( old.private_word ); Space; WriteNum( old.buffer ); NewLine;
  WriteNum( h->code ); Space; WriteNum( h->private_word ); Space; WriteNum( h->buffer ); NewLine;
#endif
  regs[1] = (uint32_t) old.code;
  regs[2] = old.private_word;
  regs[3] = old.buffer;

  if ((regs[1] | regs[2] | regs[3]) == 0) asm ( "bkpt 55" );
}

void __attribute__(( naked )) default_os_changeenvironment()
{
  asm ( "push { "C_CLOBBERED" }" );     // Intercepting
  register uint32_t *regs;

  asm ( "mov %[regs], sp" : [regs] "=r" (regs) );

  do_ChangeEnvironment( regs );

  asm ( "pop { "C_CLOBBERED", pc }" );
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

physical_memory_block Pipe_physical_address( TaskSlot *slot, uint32_t va );

physical_memory_block Kernel_physical_address( uint32_t va )
{
  assert( workspace.task_slot.running != 0 );

  Task *running = workspace.task_slot.running;

  assert( running->next != 0 && running->prev != 0 );

  physical_memory_block result = { 0, 0, 0 }; // Fail

  TaskSlot *slot = running->slot;

  claim_lock( &slot->lock );

//WriteS( "Searching slot " ); WriteNum( (uint32_t) slot ); WriteS( " for address " ); WriteNum( va ); NewLine;
  if (slot != 0) {
    for (int i = 0; i < number_of( slot->blocks ) && slot->blocks[i].size != 0 && slot->blocks[i].virtual_base <= va; i++) {
//WriteS( "Block: " ); WriteNum( slot->blocks[i].virtual_base ); WriteS( ", " ); WriteNum( slot->blocks[i].size ); Space; WriteNum( slot->blocks[i].physical_base ); NewLine;
      if (slot->blocks[i].virtual_base <= va && slot->blocks[i].virtual_base + slot->blocks[i].size > va) {
        result = slot->blocks[i];
        goto found;
      }
    }
//WriteS( "Virtual address " ); WriteNum( va ); WriteS( " not found in slot " ); WriteNum( slot ); NewLine;
  }
  else
    WriteS( "No current slot" );

  result = Pipe_physical_address( slot, va );

found:
  release_lock( &slot->lock );

  return result;
}

static void free_task( Task *task )
{
  task->regs.lr = 1; // Never a valid pc, so unallocated
}

static void free_task_slot( TaskSlot *slot )
{
  slot->svc_sp_when_unmapped = 0;
}

static void binary_to_decimal( int number, char *buffer, int size )
{
  register int n asm ( "r0" ) = number;
  register char *b asm ( "r1" ) = buffer;
  register int s asm ( "r2" ) = size;
  asm ( "svc %[swi]" : : [swi] "i" (OS_BinaryToDecimal), "r" (n), "r" (b), "r" (s) );
}

#define INITIAL_MEMORY_FOR_TASKS_AND_SLOTS (64*1024)

static void allocate_taskslot_memory()
{
  // Only called with lock acquired
  bool first_core = (shared.task_slot.slots_memory == 0);

  if (first_core) {
    shared.task_slot.slots_memory = Kernel_allocate_pages( INITIAL_MEMORY_FOR_TASKS_AND_SLOTS, INITIAL_MEMORY_FOR_TASKS_AND_SLOTS );
    shared.task_slot.tasks_memory = Kernel_allocate_pages( INITIAL_MEMORY_FOR_TASKS_AND_SLOTS, INITIAL_MEMORY_FOR_TASKS_AND_SLOTS );
    if (shared.task_slot.slots_memory == 0) asm ( "bkpt 128" );
    if (shared.task_slot.tasks_memory == 0) asm ( "bkpt 129" );
  }

  // No lazy address decoding for the kernel
  // At least, not initially

  MMU_map_shared_at( (void*) &tasks, shared.task_slot.slots_memory, INITIAL_MEMORY_FOR_TASKS_AND_SLOTS );
  MMU_map_shared_at( (void*) &task_slots, shared.task_slot.tasks_memory, INITIAL_MEMORY_FOR_TASKS_AND_SLOTS );

  workspace.task_slot.memory_mapped = true;

  if (first_core) {
    bzero( task_slots, INITIAL_MEMORY_FOR_TASKS_AND_SLOTS );
    bzero( tasks, INITIAL_MEMORY_FOR_TASKS_AND_SLOTS );
    for (int i = 0; i < INITIAL_MEMORY_FOR_TASKS_AND_SLOTS/sizeof( TaskSlot ); i++) {
      free_task_slot( &task_slots[i] );
    }
    for (int i = 0; i < INITIAL_MEMORY_FOR_TASKS_AND_SLOTS/sizeof( Task ); i++) {
      free_task( &tasks[i] );
    }
  }
}

static void __attribute__(( noinline, naked )) ignore_event()
{
  asm ( "bx lr" );
}

static inline void Sleep( int delay );

void __attribute__(( noinline )) do_Exit( uint32_t *regs )
{
  // FIXME: What to do with any existing threads?
  // FIXME: Free the TaskSlot
  // FIXME: Free the Task
  // resume the code in the parent slot? Where? After OS_CLI?
  // Yes, I think so.

  asm (
    "\n  svc %[enter]"
    :
    : [enter] "i" (OS_EnterOS)
    : "lr" );

show_tasks_state();
  Task *running = workspace.task_slot.running;
  show_word( 1000, 80, (uint32_t) running, Green );
  show_word( 1000, 90, regs[0], Green );
  show_word( 1000, 100, regs[1], Green );
  show_word( 1000, 110, regs[2], Green );
  show_word( 1000, 120, regs[3], Green );
  show_word( 1000, 130, regs[14], Green );
  WriteS( "Exiting " ); Write0( TaskSlot_Command( running->slot ) ); NewLine;

  for (;;) Sleep( 0 );

asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  workspace.task_slot.running = running->next;
  dll_detatch_Task( running );
  return;

  asm ( "\n  svc %[leave]" : : [leave] "i" (OS_LeaveOS) : "lr" );

  //for (;;) { Sleep( 100000 ); }
}

static void __attribute__(( naked )) ExitHandler()
{
  register uint32_t *regs;
  asm volatile ( "push { r0-r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  do_Exit( regs );
}

static void __attribute__(( naked )) ErrorHandler()
{
  WriteS( "Error Handler" ); NewLine; asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  register uint32_t *regs;
  asm volatile ( "push { r0-r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  do_Exit( regs );
}

void __attribute__(( naked )) unset_handler()
{
  asm ( "bkpt 1" );
}

static const handler default_handlers[17] = {
  { 0, 0, 0 },                // RAM Limit for program (0x8000 + amount of RAM)
  { unset_handler, 0, 0 },       // Undefined instruction
  { unset_handler, 0, 0 },       // Prefetch abort
  { unset_handler, 0, 0 },       // Data abort
  { unset_handler, 0, 0 },       // Address exception
  { unset_handler, 0, 0 },       // Other exceptions
  { ErrorHandler, 0, 0 },       // Error
  { unset_handler, 0, 0 },       // CallBack
  { unset_handler, 0, 0 },       // Breakpoint
  { unset_handler, 0, 0 },       // Escape
  { unset_handler, 0, 0 },       // Event
  { ExitHandler, 0, 0 },       // Exit (entered in usr mode)
  { unset_handler, 0, 0 },       // Unused SWI
  { unset_handler, 0, 0 },       // Exception registers
  { 0, 0, 0 },                // Application space limit
  { unset_handler, 0, 0 },       // Currently Active Object
  { ignore_event, 0, 0 }        // UpCall handler
};

bool do_OS_ReadDefaultHandler( svc_registers *regs )
{
  if (regs->r[0] > number_of( default_handlers )) {
    static error_block error = { 0x888, "Handler number out of range" };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

  handler h = default_handlers[ regs->r[0] ];

  regs->r[1] = (uint32_t) h.code;
  regs->r[2] = h.private_word;
  regs->r[3] = 0; // Only relevant for Error, CallBack, BreakPoint. These will probably have to be associated with Task Slots...?

  return true;
}

void add_memory_to_slot( TaskSlot *slot, uint32_t physical_base, uint32_t virtual_base, uint32_t size )
{
  // FIXME check for filling array or use different data structure
  for (int i = 0; i < number_of( slot->blocks ); i++) {
    if (slot->blocks[i].size == 0
     || slot->blocks[i].virtual_base > virtual_base) {
      for (int j = number_of( slot->blocks )-1; j > i; j--) {
        slot->blocks[j] = slot->blocks[j-1];
      }
      assert( slot->blocks[i].size == slot->blocks[i+1].size );
      assert( slot->blocks[i].virtual_base == slot->blocks[i+1].virtual_base );
      assert( slot->blocks[i].physical_base == slot->blocks[i+1].physical_base );
      slot->blocks[i].size = size;
      slot->blocks[i].virtual_base = virtual_base;
      slot->blocks[i].physical_base = physical_base;
      break;
    }
  }
}

// Which comes first, the slot or the task? Privileged tasks share a slot.

// Thought for the day Dec 10 2022:
//   Have a System call which blocks the caller until the command returns
//   Have a RunFree command that returns immediately with a handle for the
//     command that is its parameter.
//   Provide an API to allow the caller of RunFree to examine or control the
//     free running program, but only the caller.
//   If the handle refers to a program that wasn't started by the caller, it
//     fails

// Programs that start other programs either:
// Want to be replaced by that program
// Want to know how that program did (ended)
// Want to know what that program output as well

static TaskSlot *get_task_slot()
{
  TaskSlot *result = 0;

  bool reclaimed = claim_lock( &shared.mmu.lock );

  if (!workspace.task_slot.memory_mapped) allocate_taskslot_memory();

  // Unallocated slots have svc_sp_when_unmapped == 0

  TaskSlot new_slot = { .svc_sp_when_unmapped = core_svc_stack_top() };
  uint32_t top = (uint32_t) new_slot.svc_sp_when_unmapped;

  // FIXME: make this a linked list of free objects
  // FIXME: no need for lock if change_word_if_equal used?
  for (int i = 0; i < INITIAL_MEMORY_FOR_TASKS_AND_SLOTS/sizeof( TaskSlot ) && result == 0; i++) {
    TaskSlot *slot = &task_slots[i];
    if (0 == change_word_if_equal( (uint32_t*) &slot->svc_sp_when_unmapped, 0, top )) {
      result = slot;

      *result = new_slot; // Clear all other fields

#ifdef DEBUG__WATCH_TASK_SLOTS
WriteS( "Allocated TaskSlot " ); WriteNum( i ); WriteS( " (" ); WriteNum( result ); WriteS( ")" ); NewLine;
#endif
    }
  }

  if (result == 0) for (;;) { asm ( "bkpt 32" ); } // FIXME: expand

  if (!reclaimed) release_lock( &shared.mmu.lock );

  return result;
}

static void standard_svc_stack( TaskSlot *slot )
{
  uint32_t initial_size = 81920;
  // uint32_t initial_size = 8192;
  uint32_t top = ((uint32_t) &svc_stack_top);

  // TODO: more flexible structure for physical memory blocks.

  // SharedCLibrary area for running in svc
  {
    uint32_t phys = Kernel_allocate_pages( 4096, 4096 );
    add_memory_to_slot( slot, phys, (top & ~0xfffff), 4096 );
  }

  // SVC stack
  {
    uint32_t phys = Kernel_allocate_pages( initial_size, 4096 );
    add_memory_to_slot( slot, phys, top - initial_size, initial_size );
  }
}

void standard_handlers( TaskSlot *slot )
{
  for (int i = 0; i < number_of( slot->handlers ); i++) {
    assert( i < number_of( default_handlers ) );

    slot->handlers[i] = default_handlers[i];
  }

  // CAO unique to each TaskSlot, with luck, this should stop the Wimp
  // from messing with Application memory space.
  slot->handlers[15].code = (void*) slot;
}

// If command_length is zero, the length will be counted
// If args is NULL, the rest of the string after the command will be used
static void new_command_line( TaskSlot *slot, char const *command, int command_length, char const *args )
{
#ifdef DEBUG__WATCH_TASK_SLOTS
  WriteS( "New command line " ); WriteNum( (uint32_t) slot ); NewLine;
  WriteS( "Command " ); Write0( slot->command ); NewLine;
  WriteS( "Old Name \"" ); Write0( slot->name ); WriteS( "\"" ); NewLine;
  WriteS( "Old Tail \"" ); Write0( slot->tail ); WriteS( "\"" ); NewLine;
  WriteS( "New Name \"" ); Write0( command ); WriteS( "\"" ); NewLine;
  WriteS( "New Tail \"" ); Write0( args ); WriteS( "\"" ); NewLine;
#endif
  if (slot->command != 0)
    rma_free( slot->command );

  // Remove leading spaces and *'s
  while (command[0] == ' ' || command[0] == '*') command++;

  if (command_length == 0) {
    // needs counting.
    while (command[command_length] > ' '
        && command[command_length] != '\n'
        && command[command_length] != '\r'
        && command[command_length] != '\t'
        && command[command_length] != '\0')
      command_length++;

    if (args == 0) args = &command[command_length];
  }

  uint32_t args_length = strlen( args );

  // Allocate space for a copy of the whole command line, then
  // a second copy which will be spilt into command name and
  // command tail (parameters).
  // FIXME: redirections?
  char *copy = rma_allocate( (args_length + command_length) * 2 + 3 );
  assert( copy != 0 );
  int offset = 0;

  strncpy( copy, command, command_length );
  if (args_length == 0) {
    copy[command_length] = '\0';
    offset = command_length+1;
  }
  else {
    copy[command_length++] = ' ';
    strncpy( &copy[command_length], args, args_length );
    offset = command_length + 1 + args_length + 1; // Space and terminator
  }

  char *command_name = &copy[offset];
  strncpy( &copy[offset], command, command_length );
  offset += command_length;
  copy[offset++] = '\0';

  char *command_tail = &copy[offset];
  if (args_length != 0) {
    strcpy( command_tail, args );
  }
  else {
    command_tail--; // Point at the terminator of the command
  }

  slot->command = copy;
  slot->name = command_name;
  slot->tail = command_tail;
}

TaskSlot *TaskSlot_first()
{
  assert( workspace.task_slot.running == 0 );

  TaskSlot *slot = get_task_slot();

  standard_svc_stack( slot );
  standard_handlers( slot );

  slot->command = 0; // No RMA use yet
  slot->name = "ModuleTasksSlot";
  slot->tail = "";

  slot->start_time = 0; // cs since Jan 1st 1900 TODO
  slot->lock = 0;
  slot->waiting = 0;

#ifdef DEBUG__WATCH_TASK_SLOTS
  WriteS( "TaskSlot_first " ); WriteNum( (uint32_t) slot ); NewLine;
#endif
  Task *new_task = Task_new( slot );

  // There's no need to initialise the registers, current pc, etc., it
  // will be done when this task is swapped out.

  workspace.task_slot.running = new_task;

  // However, this first task will own the first slot's svc_stack
  // until it doesn't.
  slot->svc_stack_owner = new_task;
  slot->svc_sp_when_unmapped = (void*) &svc_stack_top;

  assert( workspace.task_slot.running != 0 );
  assert( workspace.task_slot.running == new_task );

  MMU_switch_to( slot );

  return slot;
}

static void Bother()
{
  asm ( "bkpt 6" );
}

void TaskSlot_start_child( TaskSlot *slot )
{
  // When the caller task is resumed, it will return to the point of
  // the SWI that caused this call.

  Task *task = workspace.task_slot.running;

  slot->creator = task;

  Task *new_task = Task_new( slot );

  dll_replace_Task( task, new_task, &workspace.task_slot.running );

new_task->regs.spsr = 0x13;
new_task->regs.lr = (uint32_t) Bother;
}

TaskSlot *TaskSlot_new( char const *command_line )
{
  // This won't work unless TaskSlot_first has been called first.
  assert( workspace.task_slot.running != 0 );

#ifdef DEBUG__WATCH_TASK_SLOTS
  WriteS( "Command line " ); Write0( command_line ); NewLine;
#endif

  TaskSlot *slot = get_task_slot();

  standard_svc_stack( slot );
  standard_handlers( slot );

  // May not be the case, if the debug receiver task has been triggered
  // assert( workspace.task_slot.running == new_task );

  new_command_line( slot, command_line, 0, 0 );

  slot->creator = 0;
  slot->start_time = 0; // cs since Jan 1st 1900 TODO
  slot->lock = 0;
  slot->waiting = 0;

#ifdef DEBUG__WATCH_TASK_SLOTS
  WriteS( "TaskSlot_new " ); WriteNum( (uint32_t) slot ); NewLine;
  WriteS( "Command " ); Write0( slot->command ); NewLine;
  WriteS( "Name " ); Write0( slot->name ); NewLine;
  WriteS( "Tail " ); Write0( slot->tail ); NewLine;
#endif

  return slot;
}

void TaskSlot_detatch_from_creator( TaskSlot *slot )
{
  Task *creator = slot->creator;

  assert( creator != 0 );

  WriteS( "Detatching " ); WriteNum( slot ); WriteS( " from creator " ); WriteNum( slot->creator ); NewLine;

  slot->creator = 0;
  creator->regs.r[0] = 0xbad0bad0; // Not yet implemented handles

  Task *tail = workspace.task_slot.running->next;
  dll_attach_Task( creator, &tail );
}

void TaskSlot_new_application( char const *command, char const *args )
{
  Task *task = workspace.task_slot.running;
  TaskSlot *slot = task->slot;

  new_command_line( slot, command, 0, args );

  slot->start_time = 0; // cs since Jan 1st 1900 TODO
}

void Task_free( Task *task )
{
  assert( task->next == task && task->prev == task );

  // Don't need the shared.mmu.lock here, the write is atomic
  // (Will need the lock when it's a linked list.)
  task->regs.lr = 1;
}

Task *Task_new( TaskSlot *slot )
{
  Task *result = 0;

  assert( slot != 0 );

  bool reclaimed = claim_lock( &shared.mmu.lock );

  if (!workspace.task_slot.memory_mapped) allocate_taskslot_memory();

  // FIXME: make this a linked list of free objects
  for (int i = 0; i < INITIAL_MEMORY_FOR_TASKS_AND_SLOTS/sizeof( Task ) && result == 0; i++) {
    if (1 == change_word_if_equal( &tasks[i].regs.lr, 1, 3)) {
      result = &tasks[i];
      assert( result->regs.lr == 3 ); // Allocated, but still invalid address
    }
  }

  if (!reclaimed) release_lock( &shared.mmu.lock );

  if (result == 0) for (;;) { asm ( "bkpt 33" ); } // FIXME: expand

  result->slot = slot;
  result->resumes = 0;
  dll_new_Task( result );

  //WriteS( "New Task: " ); WriteNum( result ); NewLine;

  // FIXME remove
  for (int i = 0; i < 13; i++) {
    result->regs.r[i] = 0x11111111U * i;
    result->regs.r[i] = (uint32_t) result;
  }

  assert( result != 0 );

  return result;
}

void TaskSlot_adjust_app_memory( TaskSlot *slot, uint32_t new_limit )
{
  extern int app_memory_base;
  extern int app_memory_limit;

  assert( (new_limit & 0xfff) == 0 );
  assert( new_limit <= (uint32_t) &app_memory_limit );

  bool reclaimed = claim_lock( &shared.mmu.lock );

  int i = 0;
  while (slot->blocks[i].size != 0
      && slot->blocks[i].virtual_base < (uint32_t) &app_memory_base) {
    i++;
  }
  // slots->blocks[i] is the first block after any low page blocks.
  int above = i; // Will be the entry above the last app block
  uint32_t top = (uint32_t) &app_memory_base; // Whether or not there's an app block

  while (slot->blocks[i].size != 0) {
    uint32_t block_top = slot->blocks[i].virtual_base + slot->blocks[i].size;
    if (block_top <= (uint32_t) &app_memory_limit) {
      top = block_top;
      above = i+1;
    }
    i++;
  }
  int first_unused_block = i;

  // slots->blocks[i] is the first empty block in the array
  assert( first_unused_block < number_of( slot->blocks ) ); // FIXME get rid of hard limit

  if (top > new_limit) {
    // Ignore shrinking, for now FIXME
  }
  else if (top < new_limit) {
    for (int j = first_unused_block; j > above; j--) {
      slot->blocks[j] = slot->blocks[j-1];
    }
    slot->blocks[above].size = new_limit - top;
    slot->blocks[above].virtual_base = top;
    slot->blocks[above].physical_base = Kernel_allocate_pages( new_limit - top, 4096 );

    assert( slot->blocks[above].physical_base != 0xffffffff ); // FIXME
  }

  // Thoughts:
  // Memory manager module that performs the equivalent of Kernel_allocate_pages
  // Has its own TaskSlot (module tasks with slots?)
  // Single tasking, requesting tasks report their desires, then wait until woken.
  // Maybe a second task, or allow calling tasks that are freeing memory to immediately
  // add to the free pool.
  // These kinds of memory actions are not suitable for interrupt handlers.

  slot->handlers[0].code = (void (*)()) new_limit;
  slot->handlers[14].code = (void (*)()) new_limit;

  if (!reclaimed) release_lock( &shared.mmu.lock );
}

uint32_t TaskSlot_asid( TaskSlot *slot )
{
  uint32_t result = (slot - task_slots);
#ifdef DEBUG__WATCH_TASK_SLOTS
//WriteS( "TaskSlot_asid " ); WriteNum( result ); NewLine;
#endif
  return result;
}

uint32_t TaskSlot_Himem( TaskSlot *slot )
{
  uint32_t result;
  // This will do until slots can include non-contiguous memory, be resized, etc.

  // FIXME lock per slot?
  bool reclaimed = claim_lock( &shared.mmu.lock );

#ifdef DEBUG__WATCH_TASK_SLOTS
  WriteS( "TaskSlot_Himem " ); WriteNum( (uint32_t) slot ); WriteS( " " ); WriteNum( slot->blocks[0].virtual_base ); WriteS( " " ); WriteNum( slot->blocks[0].size ); NewLine;
#endif

  result = slot->blocks[0].size + 0x8000; // slot->blocks[0].virtual_base;
  if (!reclaimed) release_lock( &shared.mmu.lock );
  return result;
}

TaskSlot *TaskSlot_now()
{
  return workspace.task_slot.running->slot;
}

Task *Task_now()
{
  return workspace.task_slot.running;
}

void *TaskSlot_Time( TaskSlot *slot )
{
  return &slot->start_time;
}

uint32_t TaskSlot_WimpPollBlock( TaskSlot *slot )
{
  assert( slot->wimp_poll_block != 0 );
  return (uint32_t) slot->wimp_poll_block;
}

char const *TaskSlot_Command( TaskSlot *slot )
{
  return slot->command;
}

// FIXME Some handlers are called in usr mode, some in svc, maybe other modes, too?
static void CallHandler( uint32_t *regs, int number )
{
#ifdef DEBUG__SHOW_UPCALLS
Write0( __func__ ); Space; WriteNum( number ); Space; WriteNum( regs[0] ); Space; WriteNum( workspace.task_slot.running->slot->handlers[16].code ); NewLine;
#endif

  Task *running = workspace.task_slot.running;
  TaskSlot *slot = running->slot;

  handler *h = &slot->handlers[number];
  register uint32_t r12 asm ( "r12" ) = h->private_word;
  register void (*code)() asm ( "lr" ) = h->code;
  asm volatile ( "ldm %[regs], { r0-r6 }\n  blx lr\n stm %[regs], { r0-r6 }"
    : "=r" (code) // Clobbered, but can't go in clobber list
    : [regs] "r" (regs)
    , "r" (r12)
    , "r" (code)
    : "r0", "r1", "r2", "r3", "r4", "r5", "r6" );

#ifdef DEBUG__SHOW_UPCALLS
Write0( __func__ ); Space; WriteNum( r12 ); NewLine;
#endif
}

void __attribute__(( noinline )) do_UpCall( uint32_t *regs )
{
#ifdef DEBUG__SHOW_UPCALLS
Write0( __func__ ); Space; WriteNum( regs ); NewLine;
#endif

  CallHandler( regs, 16 );

#ifdef DEBUG__SHOW_UPCALLS
WriteS( "Done: " ); Space; WriteNum( regs ); NewLine;
#endif
}

void __attribute__(( noinline )) do_FSControl( uint32_t *regs )
{
  WriteS( __func__ );
  switch (regs[0]) {
  case 2:
    // Fall through
  default:
    WriteNum( regs[0] ); NewLine; asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  }
}

static void __attribute__(( noinline )) c_default_ticker()
{
  workspace.vectors.zp.MetroGnome++;

  // Interrupts disabled, core-specific
  if (workspace.task_slot.sleeping != 0) {
    if (0 == --workspace.task_slot.sleeping->regs.r[1]) {
      // Called from an interrupt task, can safely be placed as running->next,
      // since running is the irq_task, and the sleeping task will resume
      // after the SWI it called (or possibly re-try the SWI, in some cases).

      Task *first = workspace.task_slot.sleeping;
      Task *still_sleeping = first;
      Task *last_resume = first;

      // r[1] contains the number of ticks left to sleep for
      // Find all the tasks to be woken (this one, and all the following with
      // r[1] == 0).
      do {
        last_resume = still_sleeping;
        still_sleeping = still_sleeping->next;
      } while (still_sleeping != workspace.task_slot.sleeping
            && still_sleeping->regs.r[1] == 0);

      assert( still_sleeping == workspace.task_slot.sleeping
          || still_sleeping->regs.r[1] != 0 );
      assert( last_resume != 0 );

      // Some (maybe all) have woken...
      dll_detatch_Tasks_until( &workspace.task_slot.sleeping, last_resume );

      assert( workspace.task_slot.sleeping == still_sleeping
           || still_sleeping == first );

      dll_insert_Task_list_at_head( first, &workspace.task_slot.running );
//show_tasks_state();
    }
  }
}

void __attribute__(( naked )) default_ticker()
{
  asm ( "push { "C_CLOBBERED" }" ); // Intend to intercept the vector
  c_default_ticker();
  asm ( "pop { "C_CLOBBERED", pc }" );
}

/* static */ error_block *TaskOpWaitUntilWoken( svc_registers *regs )
{
  Task *running = workspace.task_slot.running;
  assert( running != 0 );

  // TODO: Perhaps return the number of resumes instead of making the
  // Task repeatedly call this?
  // FIXME Lock, in case TaskOp_tasks are on separate cores
  running->resumes--;
  if (running->resumes < 0) {
    Task *resume = running->next;
    assert( running != resume );
    workspace.task_slot.running = resume;

    assert( workspace.task_slot.running != 0 );
    assert( workspace.task_slot.running != running );

    // It's up to the programmer to remember the handle for this Task,
    // so it can resume it.
    dll_detatch_Task( running );
  }

  return 0;
}

/* static */ error_block *TaskOpResume( svc_registers *regs )
{
  Task *running = workspace.task_slot.running;
  assert( running != 0 );

  // FIXME validate input
  // TODO Is this right? The task is returned to runnable status, but
  // won't execute until the caller blocks.
  // This behaviour is necessary for interrupt handling tasks prodding
  // second/third level handlers.
  // FIXME Lock, in case TaskOp_tasks are on separate cores
  // ... or STREX the resumes word...

  Task *waiting = task_from_handle( regs->r[1] );
  waiting->resumes++;
  if (waiting->resumes == 0) {
    // Is waiting, detatched from the running list
    // Don't replace head, place at head of tail
    Task *tail = running->next;
    assert( tail != running );
    dll_attach_Task( waiting, &tail );
  }

  return 0;
}

/* Lock states:
 *   Idle: 0
 *   Owned: Bits 31-1 contain task id, bit 0 set if tasks want the lock
 * Once owned, the lock value will only be changed by:
 *      a waiting task setting bit 0, or
 *      the owning task releasing the lock
 * In the latter case, the new state will either be idle, or the id of
 * a newly released waiting thread, with bit 0 set according to whether
 * there are tasks queued waiting.
 *
 * If there are none queued, but the lock is wanted by another task,
 * that task will be waiting in a call to Claim, which will set the
 * bit before the task is blocked. Either the releasing task will see
 * the set bit (and call Release) or the Claim write will fail and
 * re-try with an idle lock.
 */

typedef union {
  Task *rawp;
  uint32_t raw;
  struct {
    uint32_t wanted:1;
    uint32_t half_handle:31;
  };
} TaskLock;

/* static */ error_block *TaskOpLockClaim( svc_registers *regs )
{
  error_block *error = 0;

  // TODO check valid address for task (and return error)
  uint32_t *lock = (void *) regs->r[1];
  regs->r[0] = 0; // Default boolean result - not already owner. Only returns when claimed.

  uint32_t failed;

  Task *running = workspace.task_slot.running;
  Task *next = running->next;
  TaskSlot *slot = running->slot;

  assert( next != 0 ); // There's always a next, idle tasks don't sleep.

  TaskLock code = { .rawp = running };
  assert( !code.wanted );

  // Despite this lock, we will still be competing for the lock word with
  // tasks that haven't claimed the lock yet or one waiting to release it.

  TaskLock latest_read;

// FIXME I don't think this ever returns an error. Maybe if the word isn't
// owned by the slot?
  do {
    asm volatile ( "ldrex %[value], [%[lock]]"
                   : [value] "=&r" (latest_read.raw)
                   : [lock] "r" (lock) );

    if (code.half_handle == latest_read.half_handle) {
      // No need for a clrex: "An exception return clears the local monitor. As
      // a result, performing a CLREX instruction as part of a context switch
      // is not required in most situations." ARM DDI 0487C.a E2-3051

      regs->r[0] = 1; // Already own it!
      return 0;
    }

    if (latest_read.raw == 0) {
      asm volatile ( "strex %[failed], %[value], [%[lock]]"
                     : [failed] "=&r" (failed)
                     , [lock] "+r" (lock)
                     : [value] "r" (code.raw) );
      // Note: as part of testing, I put debug output between the LDREX
      // and the STREX.
      // That was a mistake, since returning from an exception does a CLREX
    }
    else {
      // Another task owns it, add to blocked list for task slot, block...

      workspace.task_slot.running = running->next;

      // Remove from old list
      dll_detatch_Task( running );
      dll_attach_Task( running, &slot->waiting );
      assert( slot->waiting != 0 );
      // Put running at end of list, not head
      slot->waiting = slot->waiting->next;

      return 0;
    }
  } while (failed);

  return error;
}

/* static */ error_block *TaskOpLockRelease( svc_registers *regs )
{
  error_block *error = 0;

  static error_block not_owner = { 0x888, "Don't try to release locks you don't own!" };

  // TODO check valid address for task
  uint32_t *lock = (void *) regs->r[1];

  Task *running = workspace.task_slot.running;
  TaskSlot *slot = running->slot;

  TaskLock code = { .rawp = running };
  assert ( !code.wanted );

  claim_lock( &slot->lock );
  // Despite this lock, we will still be competing for the lock word with
  // tasks that haven't claimed the lock yet or one waiting to release it.

  TaskLock latest_read;

  uint32_t failed = false;

  do {
    asm volatile ( "ldrex %[value], [%[lock]]"
                   : [value] "=&r" (latest_read.raw)
                   : [lock] "r" (lock) );
    if (latest_read.half_handle == code.half_handle) {
      // Owner of lock, good.
      Task *waiting = 0;

      TaskLock new_code = { .raw = 0 };

      if (latest_read.wanted) {
        Task **p = &slot->waiting;
        while ((*p) == 0 && ((uint32_t*) (*p)->regs.r[1] != lock)) p = &(*p)->next;

        waiting = *p;

        *p = waiting->next;

        // Ready to go, next time the running task blocks (e.g. by trying to gain this lock again)
        waiting->next = running->next;
        running->next = waiting;

        while ((*p) == 0 && ((uint32_t*) (*p)->regs.r[1] != lock)) p = &(*p)->next;

        new_code.rawp = waiting;

        if (*p != 0) {
          new_code.wanted = 1;
        }
      }

      // Write Idle or the new owner (with or without wanted bit)
      do {
        asm volatile ( "strex %[failed], %[value], [%[lock]]"
                       : [failed] "=&r" (failed)
                       , [lock] "+r" (lock)
                       : [value] "r" (new_code.raw) );
        if (failed) {
          new_code.wanted = 1;
          asm volatile ( "ldrex %[value], [%[lock]]"
                   : [value] "=&r" (latest_read.raw)
                   : [lock] "r" (lock) );

          if (latest_read.half_handle != code.half_handle
           || latest_read.wanted == 0) {
            asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); // Someone's broken the contract
          }
        }
        // The only reason the store would fail is that a task has
        // updated the lock, and this is the owning task.
      } while (failed);

      break;
    }
    else {
      WriteNum( (uint32_t) latest_read.raw ); NewLine;
      WriteNum( (uint32_t) running ); NewLine;

      error = &not_owner;
    }
  } while (failed);

  return error;
}

void __attribute__(( naked )) task_exit()
{
  // TODO: Check if last task in slot, tidy up generally
  asm ( "bkpt 2" );
}

static int next_interrupt_source()
{
#ifdef DEBUG__IRQV
  register uint32_t device asm( "r0" ); // Device ID returned by HAL

  // This is relatively inefficient, but it shows up on qemu traces
  register uint32_t vector asm( "r9" ) = 2;
  asm ( "svc %[swi]" : "=r" (device) : [swi] "i" (OS_CallAVector), "r" (vector) : "lr", "memory", "cc" );
#else

  // It is expected that the HAL will have claimed this vector and will return
  // the number of the device the interrupt is for.
  register vector *v asm( "r10" ) = workspace.kernel.vectors[2]; // IrqV - resurrected!

  register uint32_t device asm( "r0" ); // Device ID returned by HAL

  asm volatile (
      "\n  adr r0, 1f  // Interception address, to go onto stack"
      "\n  push { r0 }"
      "\n  mov r0, #0"
      "\n  mov r1, #2"
      "\n0:"
      "\n  ldr r14, [%[v], %[code]]"
      "\n  ldr r12, [%[v], %[private]]"
      "\n  blx r14"
      "\n  ldr %[v], [%[v], %[next]]"
      "\n  b 0b"
      "\n1:"
      : "=r" (device)

      : [v] "r" (v)

      , [next] "i" ((char*) &((vector*) 0)->next)
      , [private] "i" ((char*) &((vector*) 0)->private_word)
      , [code] "i" ((char*) &((vector*) 0)->code) );
#endif

  return device;
}

static Task *next_irq_task()
{
  int device = next_interrupt_source();
  Task *handler = 0;

  assert( device == -1 || (device >= 0 && device < shared.task_slot.number_of_interrupt_sources) );

  if (device >= 0) {
    assert( workspace.task_slot.irq_tasks != 0 );
    assert( device < shared.task_slot.number_of_interrupt_sources );

    handler = workspace.task_slot.irq_tasks[device];
    workspace.task_slot.irq_tasks[device] = 0; // Not waiting for interrupts

if (handler == 0) { register uint32_t r0 asm( "r0" ) = device; asm ( "bkpt 888" : : "r"(r0) ); } // handler hasn't reported for duty yet, it should have disabled the interrupt at source
#ifdef DEBUG__SHOW_TASK_SWITCHES
  Write0( __func__ ); Space; WriteNum( workspace.task_slot.running ); Space; WriteNum( handler ); Space; WriteNum( handler->next ); NewLine;
#endif
  }

  // Either no handler, or it's not in a queue
  assert( handler == 0 || handler->next == handler );

  return handler;
}

/* static */ error_block * __attribute__(( noinline )) TaskOpWaitForInterrupt( svc_registers *regs )
{
  uint32_t device = regs->r[1];
  // WriteS( "Wait for Interrupt " ); WriteNum( device ); NewLine;

  if (device >= shared.task_slot.number_of_interrupt_sources) {
    static error_block err = { 0x888, "Requested IRQ out of range" };
    return &err;
  }

  if (workspace.task_slot.irq_tasks == 0) {
    int count = shared.task_slot.number_of_interrupt_sources;
    workspace.task_slot.irq_tasks = shared.task_slot.irq_tasks + (count * workspace.core_number);
    for (int i = 0; i < count; i++) {
      workspace.task_slot.irq_tasks[i] = 0;
    }
  }

  if (workspace.task_slot.irq_tasks[device] != 0) {
    static error_block err = { 0x888, "IRQ claimed by another task" };
    return &err;
  }

  Task *running = workspace.task_slot.running;

  // Allocate the array if this is the first interrupt task for this core
  // This entry is zeroed on interrupt (and first initialisation)
  assert( workspace.task_slot.irq_tasks[device] == 0 );

  workspace.task_slot.running = running->next;
  assert( running != workspace.task_slot.running );
  dll_detatch_Task( running );

  workspace.task_slot.irq_tasks[device] = running;

  assert( (regs->spsr & 0x80) != 0 ); // This SWI should only be called with interrupts disabled?

  // Interrupts will be disabled when the task is resumed, until
  // InterruptIsOff is called.
  regs->spsr |= 0x80;

  // Any interrupts outstanding, maybe even this one again?
  Task *irq_task = next_irq_task();

  if (irq_task != 0) {
    // Insert the task at the head
    dll_attach_Task( irq_task, &workspace.task_slot.running );
#ifdef DEBUG__SHOW_INTERRUPTS
WriteS( "IRQ task " ); WriteNum( running ); WriteS( " finished, next: " );
#endif
  }
  else {
#ifdef DEBUG__SHOW_INTERRUPTS
WriteS( "IRQ task " ); WriteNum( running ); WriteS( " finished, resuming " );
#endif
  }

#ifdef DEBUG__SHOW_INTERRUPTS
WriteNum( next ); WriteS( " at " ); WriteNum( next->regs.lr ); WriteS( ", PSR " ); WriteNum( next->regs.spsr ); NewLine;
#endif

  return 0;
}

/* static */ error_block *TaskOpInterruptIsOff( svc_registers *regs )
{
  Task *running = workspace.task_slot.running;
  // Continue caller with interrupts enabled, but only when all IRQs
  // have  been dealt with.
  // The interrupt task must have ensured that there won't be any more
  // interrupts from its source until WaitForInterrupt is called again.
  // It should ensure that interrupts are turned off using OS_IntOff
  // during that time. An interrupt for a source that doesn't have a
  // task waiting is a fatal error (or will be ignored?).

  WriteS( "Interrupt is off" ); NewLine;
  regs->spsr &= ~0x80; // Enable interrupts
assert( false ); // Not called, yet!
  // Before we resume the caller with interrupts enabled, handle any outstanding
  // interrupts without.
  Task *irq_task = next_irq_task();

#ifdef DEBUG__SHOW_INTERRUPTS
WriteS( "IRQ task " ); WriteNum( irq_task ); NewLine;
#endif

  if (irq_task != 0) {
    irq_task->next = running;
    irq_task->prev = running->prev;
    running->prev->next = irq_task;
    running->prev = irq_task;
  }

  return 0;
}

/* static */ error_block *TaskOpStart( svc_registers *regs )
{
  Task *running = workspace.task_slot.running;

  Task *new_task = Task_new( running->slot );

  assert( new_task->slot == running->slot );

  // The creating task gets to continue, although its code should
  // not assume that the new task will wait for it to yield.
  // If this is a requirement, create a lock, claim it, create the
  // task, then have the task try to claim it on startup. When the
  // lock is released, the child will continue.
  assert( running != 0 );

//show_running_queue( 1000 );

  // Add new task...
  dll_attach_Task( new_task, &workspace.task_slot.running );

  // ... at the end of the list
  workspace.task_slot.running = workspace.task_slot.running->next;

//show_running_queue( 1100 );

  new_task->regs.spsr = 0x10; // Tasks always start in usr32 mode
  new_task->regs.lr = regs->r[1];
  new_task->banked_lr_usr = (uint32_t) task_exit;
  new_task->banked_sp_usr = regs->r[2];
  new_task->regs.r[0] = handle_from_task( new_task );
  new_task->regs.r[1] = regs->r[3];
  new_task->regs.r[2] = regs->r[4];
  new_task->regs.r[3] = regs->r[5];
  new_task->regs.r[4] = regs->r[6];
  new_task->regs.r[5] = regs->r[7];
  new_task->regs.r[6] = regs->r[8];

WriteS( "New Task " ); WriteNum( new_task ); Space; WriteNum( new_task->regs.lr ); NewLine;
show_task_state( new_task, Blue );

  regs->r[0] = handle_from_task( new_task );

#ifdef DEBUG__WATCH_TASK_SLOTS
  WriteS( "Task created, may or may not start immediately " ); WriteNum( new_task ); Space; WriteNum( new_task->slot ); NewLine;
#endif
  return 0;
}

/* static */ error_block *TaskOpSleep( svc_registers *regs )
{
  Task *running = workspace.task_slot.running;
  assert( running != 0 );

  running->regs = *regs; // Being swapped out, store context

  bool svc_caller = (regs->spsr & 0x1f) == 0x13;

  Task *resume = running->next;

  workspace.task_slot.running = resume;

  assert( workspace.task_slot.running != 0 );

  if (regs->r[1] == 0) {
    // Yield

    if (svc_caller) {
      // This thread must be resumed as soon as the other thread
      // relinquishes control.
      // This is in order to maintain a valid SVC stack
      // Insert the Task at the head of the tail

      // FIXME No longer required, once blocking Tasks that don't own the stack

      Task *tail = resume->next;
      Task *old_tail = tail;
      if (tail != running) {
        // Famous last words (wrong!):
        // There is no danger that tail points to running, there's always
        // the idle thread (which only ever calls yield from usr mode).

        // If resume is the idle thread, resume->next may be running.

        dll_detatch_Task( running );
        dll_attach_Task( running, &tail );
      }

      assert( workspace.task_slot.running == resume );
      // assert( resume->next == running );
      assert( old_tail == running || running->next == old_tail );
    }
    else {
      // This thread is willing to give all the other ones a go,
      // it is already at the end of the list (by moving the head
      // to point to resume).

      // So far undocumented feature for idle_thread:
      //   C flag set if other task running
      regs->spsr |= CF;
    }
  }
  else {
    Task *sleeper = workspace.task_slot.sleeping;

#ifdef DEBUG__SHOW_TASK_SWITCHES
WriteS( "Sleeping " ); WriteNum( running ); WriteS( ", waking " ); WriteNum( workspace.task_slot.running ); NewLine;
#endif

    assert( running != workspace.task_slot.running );

    dll_detatch_Task( running );

    if (sleeper == 0) {
      workspace.task_slot.sleeping = running;
    }
    else {
      // Subtract the times of the tasks that will be woken before this one
      Task *insert_before = 0;
      do {
        if (regs->r[1] > sleeper->regs.r[1]) {
          regs->r[1] -= sleeper->regs.r[1];
          assert( (int32_t) regs->r[1] >= 0 );
          sleeper = sleeper->next;
        }
        else {
          insert_before = sleeper;
        }
      } while (sleeper != workspace.task_slot.sleeping
            && insert_before == 0);

      assert ( sleeper == workspace.task_slot.sleeping || regs->r[1] > sleeper->regs.r[1] );

      // Subtract the remaining time for this task from the next to be
      // woken (if any)
      if (insert_before != 0) {
        insert_before->regs.r[1] -= regs->r[1];
        assert( (int32_t) insert_before->regs.r[1] >= 0 );

        Task **list = &insert_before;
        if (insert_before == workspace.task_slot.sleeping) {
          // Insert before head of list has to update the head of the list
          list = &workspace.task_slot.sleeping;
        }

        dll_attach_Task( running, list );
      }
      else {
        // Insert at tail.
        dll_attach_Task( running, &workspace.task_slot.sleeping );
        workspace.task_slot.sleeping = workspace.task_slot.sleeping->next;
      }
    }
  }

//show_tasks_state();
  return 0;
}

// This is a little tricky, if we stick to a single SVC stack per core.
// Perhaps all our problems would go away if there was one per Task or TaskSlot. IDK.
// In the meantime, I need to be able to yield to a usr32 mode Task from SVC,
// allow it to make some calls, then resume the svc32 code. (Specifically, I need to
// yield to an interrupt task I've created.)
// This sort of thing requires that svc32 mode Tasks can only be resumed LIFO, or the
// stack will be corrupted.
// That behaviour may well fall out from the behaviour of the running queue, as it
// currently is... Maybe not. Let's see!
// It doesn't. Yield from svc mode has to put the calling Task as the one to resume
// when the task it yields to blocks or yields. Yield from usr mode should go to the
// end of the queue, so all Tasks get a go.
bool do_OS_ThreadOp( svc_registers *regs )
{
if (regs->r[0] == 255) return show_tasks_state();
if (regs->r[0] == 254) {
  Task *creator = workspace.task_slot.running;

  WriteS( "ThreadOp RunFree " ); WriteNum( regs->r[1] ); NewLine;
  WriteS( "Creator: " ); WriteNum( creator ); NewLine;
  TaskSlot *child = TaskSlot_new( "RunFree" );

  Task *new_task = Task_new( child );

  WriteS( "New task: " ); WriteNum( new_task ); NewLine;

  assert( creator != new_task );

  new_task->regs.lr = regs->r[1];
  new_task->regs.spsr = 0x10;
  new_task->banked_lr_usr = (uint32_t) task_exit;
  new_task->banked_sp_usr = regs->r[2];
  new_task->regs.r[0] = handle_from_task( new_task );
  new_task->regs.r[1] = regs->r[3];
  new_task->regs.r[2] = regs->r[4];
  new_task->regs.r[3] = regs->r[5];
  new_task->regs.r[4] = regs->r[6];
  new_task->regs.r[5] = regs->r[7];
  new_task->regs.r[6] = 0x44442222;

  // Task will run when the current task yields. Is this the desired effect?
  Task *tail = workspace.task_slot.running->next;
  dll_attach_Task( new_task, &tail );

  return true;
}

  error_block *error = 0;

  Task *running = workspace.task_slot.running;
  assert ( running != 0 );
  Task *next = running->next;

#if 0
WriteS( "Running: " ); WriteNum( running );
while (next != 0) { Space; WriteNum( next ); next = next->next; }
next = running->next;
NewLine;
#endif

  if (regs->r[0] == TaskOp_NumberOfInterruptSources) {
    // Allowed from any mode, but only once.
    assert( shared.task_slot.number_of_interrupt_sources == 0 );
    assert( using_slot_svc_stack() );
    shared.task_slot.number_of_interrupt_sources = regs->r[1];
    int count = shared.task_slot.number_of_interrupt_sources * processor.number_of_cores;
    shared.task_slot.irq_tasks = rma_allocate( sizeof( Task * ) * count );
    return true;
  }

  if ((regs->spsr & 0x1f) != 0x10                       // Not usr32 mode
   && regs->r[0] != TaskOp_Start                        // Start user task
   && regs->r[0] != TaskOp_CoreNumber                   // Returns the current core number as a string
   && regs->r[0] != TaskOp_DebugString
   && regs->r[0] != TaskOp_DebugNumber
   && !(regs->r[0] == TaskOp_Sleep && regs->r[1] == 0)) { // yield
    WriteNum( regs->lr ); Space; WriteNum( regs->spsr ); NewLine;
    static error_block error = { 0x888, "Blocking OS_ThreadOp only supported from usr mode." };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

  if (next == running && regs->r[0] == TaskOp_Sleep && regs->r[1] == 0) {
    // So far undocumented feature for idle_thread:
    //   C flag set if other task running. Here, there's none.
    regs->spsr &= ~CF;
    return true; // Yield, but no other threads on this core.
  }

  TaskSlot *slot = running->slot;

  // Tasks created by modules that aren't associated with a
  // TaskSlot will share a TaskSlot with no Application data
  // area, but a shared OS_PipeOp pipes area.
  // They're not implemented yet, but a null slot will always
  // be an error (unless I change my mind)

  if (slot == 0) {
    WriteS( "No slot! " ); WriteNum( regs->r[0] ); NewLine;
  }
  assert ( slot != 0 );
  bool reclaimed = claim_lock( &slot->lock );

  // Start a new thread
  // Exit a thread (last one out turns out the lights for the slot)
  // Wait until woken
  // Sleep (microseconds)
  // Wake thread (setting registers?)
  // Get the handle of the current thread
  // Set interrupt handler (not strictly thread related)
  switch (regs->r[0]) {
  case TaskOp_Start: error = TaskOpStart( regs ); break;
  case TaskOp_Sleep: error = TaskOpSleep( regs ); break;
  case TaskOp_WaitUntilWoken: TaskOpWaitUntilWoken( regs ); break;
  case TaskOp_Resume: TaskOpResume( regs ); break;
  case TaskOp_LockClaim: TaskOpLockClaim( regs ); break;
  case TaskOp_LockRelease: TaskOpLockRelease( regs ); break;
  case TaskOp_WaitForInterrupt: TaskOpWaitForInterrupt( regs ); break;
  case TaskOp_InterruptIsOff: TaskOpInterruptIsOff( regs );
    break;

  case TaskOp_DebugString:
    WriteN( (char const*) regs->r[1], regs->r[2] );
    break;

  case TaskOp_DebugNumber:
    WriteNum( regs->r[1] );
    break;

  case TaskOp_CoreNumber:
    if (workspace.task_slot.core_number_string[0] == '\0') {
      binary_to_decimal( workspace.core_number,
                       workspace.task_slot.core_number_string,
                       sizeof( workspace.task_slot.core_number_string ) );

    }
    regs->r[0] = (uint32_t) &workspace.task_slot.core_number_string;
    regs->r[2] = strlen( workspace.task_slot.core_number_string );
    break;

  default:
    {
      static error_block unknown_code = { 0x888, "Unknown OS_ThreadOp code" };
      error = &unknown_code;
    }
  }

  if (!reclaimed) release_lock( &slot->lock );

  if (error != 0) { asm( "bkpt %[line]" : : [line] "i" (__LINE__) ); regs->r[0] = (uint32_t) error; }

  return error == 0;
}

/* Initial implementation of pipes:
 *  4KiB each
 *  Located at top of bottom MiB (really needs fixing next!)
 *  debug pipe a special case, mapped in top MiB
 */

struct os_pipe {
  os_pipe *next;
  Task *sender;
  uint32_t sender_waiting_for; // Non-zero if blocked
  uint32_t sender_va; // Zero if not allocated
  Task *receiver;
  uint32_t receiver_waiting_for; // Non-zero if blocked
  uint32_t receiver_va; // Zero if not allocated

  uint32_t physical;
  uint32_t allocated_mem;
  uint32_t max_block_size;
  uint32_t max_data;
  uint32_t write_index;
  uint32_t read_index;
};

static bool in_range( uint32_t value, uint32_t base, uint32_t size )
{
  return (value >= base && value < (base + size));
}

static uint32_t debug_pipe_sender_va()
{
  extern uint32_t debug_pipe; // Ensure the size and the linker script match
  os_pipe *pipe = (void*) workspace.kernel.debug_pipe;
  uint32_t va = 2 * pipe->max_block_size + (uint32_t) &debug_pipe;
  MMU_map_at( (void*) va, pipe->physical, pipe->max_block_size );
  MMU_map_at( (void*) (va + pipe->max_block_size), pipe->physical, pipe->max_block_size );
  return va;
}

static uint32_t debug_pipe_receiver_va()
{
  extern uint32_t debug_pipe; // Ensure the size and the linker script match
  uint32_t va = (uint32_t) &debug_pipe;
  os_pipe *pipe = (void*) workspace.kernel.debug_pipe;
  // FIXME: map read-only
  MMU_map_at( (void*) va, pipe->physical, pipe->max_block_size );
  MMU_map_at( (void*) (va + pipe->max_block_size), pipe->physical, pipe->max_block_size );
  return va;
}

static uint32_t local_sender_va( TaskSlot *slot, os_pipe *pipe )
{
#if 0
WriteS( "local_sender_va " ); WriteNum( pipe->sender );
if (pipe->sender != 0) {
  WriteNum( pipe->sender->slot ); WriteNum( slot );
}
NewLine;
#endif
  if ((uint32_t) pipe == workspace.kernel.debug_pipe) {
    return debug_pipe_sender_va();
  }
  asm ( "bkpt 64" );
  if (pipe->sender == 0 || pipe->sender->slot != slot) return 0;
  return pipe->sender_va;
}

static uint32_t local_receiver_va( TaskSlot *slot, os_pipe *pipe )
{
  if ((uint32_t) pipe == workspace.kernel.debug_pipe) {
    return debug_pipe_receiver_va();
  }
  asm ( "bkpt 64" );
  if (pipe->receiver == 0 || pipe->receiver->slot != slot) return 0;
  return pipe->receiver_va;
}

physical_memory_block Pipe_physical_address( TaskSlot *slot, uint32_t va )
{
  // Note to self: parameters are actually &result, slot, va, in r0, r1, r2.

  // Slot is locked.
  physical_memory_block result = { 0, 0, 0 }; // Fail

  // FIXME This implementation is trivial and will break almost immediately!
  // Allocates the pipe virtual memory at the top of the first MiB of memory.
  // Only works with 4KiB pages, something smaller (and larger) might be useful.

  // It will do for proof of concept, though.
  // (I would recommend allocating virtual addresses in the top GiB, since
  // all tasks using pipes will be aware they have a bit more than 64M to play
  // with.)

  // One list of pipes shared between all slots and cores. To be fixed? TODO

  bool reclaimed = claim_lock( &shared.kernel.pipes_lock );

  os_pipe *this_pipe = shared.kernel.pipes;

  while (this_pipe != 0 && result.size == 0) {
    uint32_t local_va;
    local_va = local_sender_va( slot, this_pipe );
    if (local_va != 0 && in_range( va, local_va, 2 * this_pipe->max_block_size)) {
      result.size = this_pipe->max_block_size;
      result.physical_base = this_pipe->physical;
      result.virtual_base = local_va;
      if (!in_range( va, local_va, this_pipe->max_block_size)) {
        result.virtual_base += this_pipe->max_block_size;
      }
    }
    local_va = local_receiver_va( slot, this_pipe );
    if (local_va != 0 && in_range( va, local_va, 2 * this_pipe->max_block_size)) {
      // TODO Map read-only
      result.size = this_pipe->max_block_size;
      result.physical_base = this_pipe->physical;
      result.virtual_base = local_va;
      if (!in_range( va, local_va, this_pipe->max_block_size)) {
        result.virtual_base += this_pipe->max_block_size;
      }
    }
    this_pipe = this_pipe->next;
  }

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

#ifdef DEBUG__PIPEOP
  Write0( __func__ );
  WriteS( " " ); WriteNum( result.virtual_base );
  WriteS( " " ); WriteNum( result.physical_base );
  WriteS( " " ); WriteNum( result.size );  NewLine;
#endif

  return result;
}

static bool PipeOp_NotYourPipe( svc_registers *regs )
{
  static error_block error = { 0x888, "Pipe not owned by this task" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool PipeOp_InvalidPipe( svc_registers *regs )
{
  static error_block error = { 0x888, "Invalid Pipe" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool PipeOp_InvalidCode( svc_registers *regs )
{
  static error_block error = { 0x888, "Invalid Pipe code" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool PipeOp_CreationError( svc_registers *regs )
{
  static error_block error = { 0x888, "Pipe creation error" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool PipeOp_CreationProblem( svc_registers *regs )
{
  static error_block error = { 0x888, "Pipe creation problem" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool PipeCreate( svc_registers *regs )
{
  uint32_t max_block_size = regs->r[2];
  uint32_t max_data = regs->r[3];
  uint32_t allocated_mem = regs->r[4];

  if (max_data != 0) {
    if (max_block_size > max_data) {
      return PipeOp_CreationError( regs );
    }

    // FIXME
    return Kernel_Error_UnimplementedSWI( regs );
  }
  else if (max_block_size == 0) {
    return PipeOp_CreationError( regs );
  }

  os_pipe *pipe = rma_allocate( sizeof( os_pipe ) );

  if (pipe == 0) {
    return PipeOp_CreationProblem( regs );
  }

  // At the moment, the running task is the only one that knows about it.
  // If it goes away, the resource should be cleaned up.
  pipe->sender = pipe->receiver = workspace.task_slot.running;
  pipe->sender_va = pipe->receiver_va = 0;

  pipe->max_block_size = max_block_size;
  pipe->max_data = max_data;
  pipe->allocated_mem = allocated_mem;
  pipe->physical = Kernel_allocate_pages( 4096, 4096 );
  // Now debug output uses PipeOp, PipeOp can't do debug output
  // WriteS( "Physical memory: " ); WriteNum( pipe->physical ); NewLine;

  // The following will be updated on the first blocking calls
  // to WaitForSpace and WaitForData, respectively.
  pipe->sender_waiting_for = 0;
  pipe->receiver_waiting_for = 0;

  pipe->write_index = allocated_mem & 0xfff;
  pipe->read_index = allocated_mem & 0xfff;

  bool reclaimed = claim_lock( &shared.kernel.pipes_lock );

  pipe->next = shared.kernel.pipes;
  shared.kernel.pipes = pipe;

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

  regs->r[1] = handle_from_pipe( pipe );

  return true;
}

extern uint32_t pipes_top;

static uint32_t allocate_virtual_address( TaskSlot *slot, os_pipe *pipe )
{
  // Proof of concept locates pipes at the top of the first megabyte of
  // virtual RAM
  // This is, of course, ridiculous.
  // Fix that in rool.script and data abort handler
  // Doesn't cope with removing pipes FIXME

  asm ( "bkpt 64" );

  uint32_t va = (uint32_t) &pipes_top;

  os_pipe *this_pipe = shared.kernel.pipes;

  while (this_pipe != 0) {
    uint32_t local_va;
    local_va = local_sender_va( slot, this_pipe );
    if (local_va != 0 && local_va < va) va = local_va;
    local_va = local_receiver_va( slot, this_pipe );
    if (local_va != 0 && local_va < va) va = local_va;
    this_pipe = this_pipe->next;
  }

  // WriteS( "Allocated pipe VA " ); WriteNum( va - 2 * pipe->max_block_size ); NewLine;

  return va - 2 * pipe->max_block_size;
}

static uint32_t data_in_pipe( os_pipe *pipe )
{
  return pipe->write_index - pipe->read_index;
}

static uint32_t space_in_pipe( os_pipe *pipe )
{
  return pipe->max_block_size - data_in_pipe( pipe );
}

static uint32_t read_location( os_pipe *pipe, TaskSlot *slot )
{
  return pipe->receiver_va + (pipe->read_index % pipe->max_block_size);
}

static uint32_t write_location( os_pipe *pipe, TaskSlot *slot )
{
  return pipe->sender_va + (pipe->write_index % pipe->max_block_size);
}

static bool PipeWaitForSpace( svc_registers *regs, os_pipe *pipe )
{
  uint32_t amount = regs->r[2];
  // TODO validation

  Task *running = workspace.task_slot.running;
  Task *next = running->next;
  TaskSlot *slot = running->slot;

  if (pipe->sender != running
   && pipe->sender != 0
   && (uint32_t) pipe != workspace.kernel.debug_pipe) {
    return PipeOp_NotYourPipe( regs );
  }

  bool reclaimed = claim_lock( &shared.kernel.pipes_lock );

  if (pipe->sender == 0) {
    pipe->sender = running;
  }

  if (pipe->sender_va == 0) {
    if ((uint32_t) pipe == workspace.kernel.debug_pipe)
      pipe->sender_va = debug_pipe_sender_va( slot, pipe );
    else
      pipe->sender_va = allocate_virtual_address( slot, pipe );
  }

  uint32_t available = space_in_pipe( pipe );

  if (available >= amount) {
    regs->r[2] = available;
    regs->r[3] = write_location( pipe, slot );

#ifdef DEBUG__PIPEOP
    // WriteS( "Space immediately available: " ); WriteNum( amount ); WriteS( ", total: " ); WriteNum( space_in_pipe( pipe ) ); WriteS( ", at " ); WriteNum( write_location( pipe, slot ) ); NewLine;
#endif
  }
  else {
    pipe->sender_waiting_for = amount;

    workspace.task_slot.running = next;
    regs->r[2] = 0xb00b00b0;

    // Blocked, waiting for data.
    dll_detatch_Task( running );
  }

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

  return true;
}

static bool PipeSpaceFilled( svc_registers *regs, os_pipe *pipe )
{
  error_block *error = 0;

  uint32_t amount = regs->r[2];
  // TODO validation

  Task *running = workspace.task_slot.running;
  TaskSlot *slot = running->slot;

  assert( running != ((os_pipe*) workspace.kernel.debug_pipe)->receiver );

  if (pipe->sender != running
   && (uint32_t) pipe != workspace.kernel.debug_pipe) {
    // No setting of sender, here, if the task hasn't already checked for
    // space, how is it going to have written to the pipe?
    return PipeOp_NotYourPipe( regs );
  }

  // TODO: Flush only that area, and only as far as necessary (are the two
  // slots only single core and running on the same core?)
  // TEST TEST TEST restore the following line!
  ///asm ( "svc %[swi]" : : [swi] "i" (OS_FlushCache) : "lr" ); // Flush whole cache FIXME

  bool reclaimed = claim_lock( &shared.kernel.pipes_lock );

  uint32_t available = space_in_pipe( pipe );

  if (available < amount) {
    static error_block err = { 0x888, "Overfilled pipe" };
    error = &err;
  }
  else {
    pipe->write_index += amount;

    // Update the caller's idea of the state of the pipe
    regs->r[2] = available - amount;
    regs->r[3] = write_location( pipe, slot );

#ifdef DEBUG__PIPEOP
    // WriteS( "Filled " ); WriteNum( amount ); WriteS( ", remaining: " ); WriteNum( regs->r[2] ); WriteS( ", at " ); WriteNum( regs->r[3] ); NewLine;
#endif

    Task *receiver = pipe->receiver;

    // If there is no receiver, there's nothing to wait for data.
    assert( receiver != 0 || (pipe->receiver_waiting_for == 0) );
    // If the receiver is running, it is not waiting for data.
    assert( (receiver != running) || (pipe->receiver_waiting_for == 0) );

    // Special case: the debug_pipe is sometimes filled from the reader task

    if (pipe->receiver_waiting_for > 0
     && pipe->receiver_waiting_for <= data_in_pipe( pipe )) {

#ifdef DEBUG__PIPEOP
      // WriteS( "Data finally available: " ); WriteNum( pipe->receiver_waiting_for ); WriteS( ", remaining: " ); WriteNum( data_in_pipe( pipe ) ); WriteS( ", at " ); WriteNum( read_location( pipe, slot ) ); NewLine;
#endif

      pipe->receiver_waiting_for = 0;

      receiver->regs.r[2] = data_in_pipe( pipe );
      receiver->regs.r[3] = read_location( pipe, slot );

      // Make the receiver ready to run when the sender blocks (likely when
      // the pipe is full).
      dll_attach_Task( receiver, &workspace.task_slot.running );
      workspace.task_slot.running = workspace.task_slot.running->next;

      assert( receiver->next = running );
      assert( running->prev == receiver );
    }
  }

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

  return error == 0;
}

static bool PipePassingOver( svc_registers *regs, os_pipe *pipe )
{
  pipe->sender = task_from_handle( regs->r[2] );
  pipe->sender_va = 0; // FIXME unmap and free the virtual area for re-use

  return true;
}

static bool PipeUnreadData( svc_registers *regs, os_pipe *pipe )
{
  regs->r[2] = data_in_pipe( pipe );

  return true;
}

static bool PipeNoMoreData( svc_registers *regs, os_pipe *pipe )
{
  // Write0( __func__ ); NewLine;
  return Kernel_Error_UnimplementedSWI( regs );
}

static bool PipeWaitForData( svc_registers *regs, os_pipe *pipe )
{
  uint32_t amount = regs->r[2];
  // TODO validation

  Task *running = workspace.task_slot.running;
  Task *next = running->next;
  TaskSlot *slot = running->slot;

  // debug_pipe is not a special case, here, only one task can receive from it.
  if (pipe->receiver != running
   && pipe->receiver != 0) {
    return PipeOp_NotYourPipe( regs );
  }

  bool reclaimed = claim_lock( &shared.kernel.pipes_lock );

  if (pipe->receiver == 0) {
    pipe->receiver = running;
  }

  assert( pipe->receiver == running );

  if (pipe->receiver_va == 0) {
    if ((uint32_t) pipe == workspace.kernel.debug_pipe)
      pipe->receiver_va = debug_pipe_receiver_va( slot, pipe );
    else
      pipe->receiver_va = allocate_virtual_address( slot, pipe );
  }

  uint32_t available = data_in_pipe( pipe );

  if (available >= amount) {
    regs->r[2] = available;
    regs->r[3] = read_location( pipe, slot );

    asm ( "svc 0xff" : : : "lr" ); // Flush whole cache FIXME flush less (by ASID of the sender?)
    assert( (regs->spsr & VF) == 0 );
  }
  else {
    pipe->receiver_waiting_for = amount;

#ifdef DEBUG__PIPEOP
  // WriteS( "Blocking receiver" ); NewLine;
#endif
    workspace.task_slot.running = next;

    assert( workspace.task_slot.running != running );

    // Blocked, waiting for data.
    dll_detatch_Task( running );

    regs->r[2] = 0x22002200; // FIXME remove; just something to look for in the qemu log
    regs->r[3] = 0x33003300; // FIXME remove; just something to look for in the qemu log
  }

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

  return true;
}

static bool PipeDataConsumed( svc_registers *regs, os_pipe *pipe )
{
  uint32_t amount = regs->r[2];
  // TODO validation

  Task *running = workspace.task_slot.running;
  TaskSlot *slot = running->slot;

  if (pipe->receiver != running
   && (uint32_t) pipe != workspace.kernel.debug_pipe) {
    // No setting of receiver, here, if the task hasn't already checked for
    // data, how is it going to have read from the pipe?
    return PipeOp_NotYourPipe( regs );
  }

  bool reclaimed = claim_lock( &shared.kernel.pipes_lock );

  uint32_t available = data_in_pipe( pipe );

  if (available >= amount) {
    pipe->read_index += amount;

    regs->r[2] = available - amount;
    regs->r[3] = read_location( pipe, slot );

#ifdef DEBUG__PIPEOP
    // WriteS( "Consumed " ); WriteNum( amount ); WriteS( ", remaining: " ); WriteNum( regs->r[2] ); WriteS( ", at " ); WriteNum( regs->r[3] ); NewLine;
#endif

    if (pipe->sender_waiting_for > 0
     && pipe->sender_waiting_for <= space_in_pipe( pipe )) {
      Task *sender = pipe->sender;

#ifdef DEBUG__PIPEOP
      // WriteS( "Space finally available: " ); WriteNum( pipe->sender_waiting_for ); WriteS( ", remaining: " ); WriteNum( space_in_pipe( pipe ) ); WriteS( ", at " ); WriteNum( write_location( pipe, slot ) ); NewLine;
#endif

      asm ( "svc 0xff" : : : "lr" ); // Flush whole cache FIXME Invalidate cache for updated area, only if sender on a different core
      pipe->sender_waiting_for = 0;

      sender->regs.r[2] = space_in_pipe( pipe );
      sender->regs.r[3] = write_location( pipe, slot );

      // "Returns" from SWI next time scheduled
      if (sender != running) {
        Task *tail = running->next;
        dll_attach_Task( sender, &tail );
      }
    }
  }
  else {
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); // Consumed more than available?
  }

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

assert( 0x2a2a2a2a != regs->r[3] );
  return true;
}

static bool PipePassingOff( svc_registers *regs, os_pipe *pipe )
{
  pipe->receiver = task_from_handle( regs->r[2] );
  pipe->receiver_va = 0; // FIXME unmap and free the virtual area for re-use

  // TODO Unmap from virtual memory (if new receiver not in same slot)

  return true;
}

static bool PipeNotListening( svc_registers *regs, os_pipe *pipe )
{
  // Write0( __func__ ); NewLine;
  return Kernel_Error_UnimplementedSWI( regs );
}


bool do_OS_PipeOp( svc_registers *regs )
{
#ifdef DEBUG__PIPEOP
  // Write0( __func__ ); WriteS( " " ); WriteNum( regs->r[0] ); NewLine;
#endif
  enum { Create,
         WaitForSpace,  // Block task until N bytes may be written
         SpaceFilled,   // I've filled this many bytes
         PassingOver,   // Another task is going to take over filling this pipe
         UnreadData,    // Useful, in case data can be dropped or consolidated (e.g. mouse movements)
         NoMoreData,    // I'm done filling the pipe
         WaitForData,   // Block task until N bytes may be read
         DataConsumed,  // I don't need the first N bytes written any more
         PassingOff,    // Another task is going to take over listening at this pipe
         NotListening,  // I don't want any more data, thanks
         WaitUntilEmpty // Block task until all bytes have been consumed TODO?
         }; // FIXME: This is duplicated in include/pipeop.h
  /*
    OS_PipeOp
    (SWI &fa)
    Entry 	
    R0 	Reason code
    All other registers dependent on reason code

    Exit
    R0 	Preserved
    All other registers dependent on reason code

    Use

    The purpose of this call is to transfer data between tasks, pausing the calling thread
    while it waits for data or space to write to.

    Notes

    The action performed depends on the reason code value in R0.
    R1 is used to hold the handle for the pipe (On exit from Create, on entry to all other actions)

    Reason Codes
        # 	Hex # 	Action
        0 	&00 	Create a pipe and return a handle
        1 	&01 	Pause the thread until sufficient space is available for writing
        2 	&02 	Indicate to the receiver that more data is available
        3 	&03 	Indicate to the receiver that no more data will be written to the pipe
        4 	&04 	Pause the thread until sufficient data is available for reading
        5       &05     Indicate to the transmitter that some data has been consumed
        6       &06     Indicate to the transmitter that the receiver is no longer interested in receiving data

    OS_PipeOp 0
    (SWI &fa)

    Entry 	
    R0 	0
    R2  Maximum block size (the most that Transmitter or Receiver may request at a time)
    R3  Maximum data amount (the total amount of data to be transferred through this pipe)
                0 indicates the amount is unknown at creation
    R4  Allocated memory (0 for the kernel to allocate memory)
                Virtual memory address of where the transferred data will be stored.
                Ignored if R3 is 0.

    Exit
    R0 	Preserved
    R1 	Pipe handle
    R2  Preserved
    R3  Preserved
    R4  Preserved

    Use

        Create a pipe to be shared between two threads, one Transmitter and one Receiver.

    Notes




    PassingOver - about to ask another task to send its data to this pipe (r2 = 0 or new task)
    PassingOff - about to ask another task to handle the data from this pipe (r2 = 0 or new task)
  */

/* Create a pipe, pass it to another thread to read or write, while you do the other.

   Create:
     max_block_size - neither reader nor writer may request a larger contiguous block than this
     max_data       - The maximum amount that can be transferred (typically the size of a file)
                    - if 0, undefined.
     allocated_mem  - memory to use for the pipe (if 0, allocate memory internally)
                    - useful for transferring chunks of data between programs.
                    - e.g. JPEG_Decode( source pipe, destination pipe )
                    - The other end of the pipe will have access to full pages of memory,
                      the first area of memory returned to it will be offset by the least
                      significant bits of the allocated_mem pointer.
                    - Providing a non-page aligned block of memory for a file system to
                      write to will result in copying overhead (possibly excepting if it's
                      sector-size aligned).

   The definition of the calls that return the address of the next available memory (to
   write or read) allows for the OS to map the memory in different places as and if needed.



   Read thread (example):
     repeat
       <available,location> = WaitForData( size ) -- may block
       while available >= size then
         process available (or size) bytes at location
         <available,location> = FreeSpace( available (or size) )
       endif
     until location == 0

   Write thread (example):
     repeat
       <available,location> = WaitForSpace( size ) -- may block
       if location != 0 then
         Write up to available bytes of data to location
         <available,location> = SpaceUsed( amount_written (or less) )
       endif
     until location == 0

   If the reader is no longer interested, it should call NotListening.
   From that point on, the writer thread will be released if blocked,
   and always receive <0,0> from WaitForSpace and SpaceUsed.

   If the writer has no more data, it should call NoMoreData.
   The reader thread will be released, and WaitForData will always return
   immediately, possibly with available < the requested size.
   Once all available data is freed, the read SWIs will return <0,0>.

   Once NotListening and NoMoreData have both been called for a pipe, its
   resources will be released.

*/
  os_pipe *pipe;

  if (regs->r[0] != Create) {
    pipe = pipe_from_handle( regs->r[1] );
    if (pipe == 0) {
      return PipeOp_InvalidPipe( regs );
    }
  }

  switch (regs->r[0]) {
  case Create: return PipeCreate( regs );
  case WaitForSpace: return PipeWaitForSpace( regs, pipe );
  case PassingOver: return PipePassingOver( regs, pipe );
  case UnreadData: return PipeUnreadData( regs, pipe );
  case SpaceFilled: return PipeSpaceFilled( regs, pipe );
  case NoMoreData: return PipeNoMoreData( regs, pipe );
  case WaitForData: return PipeWaitForData( regs, pipe );
  case DataConsumed: return PipeDataConsumed( regs, pipe );
  case PassingOff: return PipePassingOff( regs, pipe );
  case NotListening: return PipeNotListening( regs, pipe );
  }
  return PipeOp_InvalidCode( regs );
}

// Default action of IrqV is not to disable the interrupt, it's to throw a wobbly.
// The HAL must ensure that IrqV never gets this far!

void default_irq()
{
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
}

void __attribute__(( naked, noreturn )) ResumeSVC()
{
  // When interrupted task resumes, that will restore sp, lr, and the pc.
  asm volatile ( "pop { lr, pc }" );
}

Task *__attribute__(( noinline )) c_run_irq_tasks( Task *running );

void __attribute__(( naked, noreturn )) Kernel_default_irq()
{
  asm volatile (
        "  sub lr, lr, #4"
      "\n  srsdb sp!, #0x12 // Store return address and SPSR (IRQ mode)"
      );

  Task *interrupted_task;

  {
  assert( 0 == (void*) &((Task*) 0)->regs );
  // Need to be careful with this, that the compiler doesn't insert any code to
  // set up lr using another register, corrupting it.
  register Task **running asm ( "lr" ) = &workspace.task_slot.running;

  asm volatile (
        "  ldr lr, [lr]"
      "\n  stm lr!, {r0-r12}  // lr -> banked_sp_usr"
      "\n  pop { r0, r1 }     // Resume address, SPSR"
      "\n  stm lr!, { r0, r1 }"
      "\n  tst r1, #0xf"
      "\n  stmeq lr, { sp, lr }^ // Does not update lr, so..."
      "\n  sub %[task], lr, #15*4 // restores its value"
      : [task] "=r" (interrupted_task)
      : "r" (running)
      ); // Calling task state saved, except for SVC sp & lr
  asm volatile ( "" : : "r" (running) );
  // lr really, really used (Oh! Great Optimiser, please don't assume
  // it's still pointing to workspace.task_slot.running, I beg you!)
  }

  svc_registers *regs = &interrupted_task->regs;
  uint32_t interrupted_mode = regs->spsr & 0x1f;

  if (interrupted_mode == 0x10) workspace.task_slot.irqs_usr++;
  else if (interrupted_mode == 0x13) workspace.task_slot.irqs_svc++;
  //else if (interrupted_mode == 0x1f) workspace.task_slot.irqs_sys++;
  else asm ( "bkpt 0x33" );

  // We don't enable interrupts while dealing with undefined instructions or
  // aborts, do we? System mode, perhaps?
  assert( (regs->spsr & 0x1f) == 0x13    // svc32
       // || (regs->spsr & 0x1f) == 0x1f    // sys32 (OS_EnterOS?)
       || (regs->spsr & 0x1f) == 0x10 ); // usr32

  // If the interrupted task is in the middle of a SWI, store the
  // banked LR and the resume address on the stack and have the task
  // resume at an instruction that pops them both.
  if ((regs->spsr & 0x1f) == 0x13) {
    // Interrupts are never enabled during interrupt safe SWIs
    assert( owner_of_slot_svc_stack( interrupted_task ) );

    register uint32_t resume asm ( "r2" ) = regs->lr;
    asm volatile ( "mrs r1, lr_svc"
               "\n  mrs %[resume_sp], sp_svc"
               "\n  stmdb %[resume_sp]!, { r1, r2 }"
               "\n  msr sp_svc, %[resume_sp]"
               : "=r" (resume)
               , [resume_sp] "=r" (interrupted_task->slot->svc_sp_when_unmapped)
               : "r" (resume)
               : "r1", "memory" );

    regs->lr = (uint32_t) ResumeSVC;

    asm volatile (
      "\n  msr sp_svc, %[reset_sp]"
      :
      : [reset_sp] "r" (core_svc_stack_top()) );
  }

  Task *irq_task = c_run_irq_tasks( interrupted_task );

  // The returned task might be the interrupted task, if the interrupt was spurious

  if (irq_task->slot != interrupted_task->slot) {
    // IRQ stack is core-specific, so switching slots does not affect its mapping
    MMU_switch_to( irq_task->slot );

    assert( !owner_of_slot_svc_stack( irq_task ) );

    asm volatile (
      "\n  msr sp_svc, %[reset_sp]"
      :
      : [reset_sp] "r" (core_svc_stack_top()) );
  }

  register svc_registers *lr asm ( "lr" ) = &irq_task->regs;
  asm (
      "\n  ldm lr!, {r0-r12}"
      "\n  rfeia lr // Restore execution and SPSR"
      :
      : "r" (lr) );

  __builtin_unreachable();
}

// File operations

// Legacy code will call these SWIs, this code will translate them to PipeOp
// and calls to the legacy FileCore/FileSwitch filing systems (using CallAVector)
// Only one task thread will access the legacy filing systems at a time.

bool run_vector( svc_registers *regs, int vec );

static inline void Yield()
{
  asm volatile ( "mov r0, #3 // Sleep"
             "\n  mov r1, #0 // For no time - yield"
             "\n  svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      : "r0", "r1", "lr", "memory" );
}

static inline void Sleep( int delay )
{
  register int sleep asm( "r0" ) = 3;
  register int time asm( "r1" ) = delay;
  asm volatile ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (sleep)
      , "r" (time)
      : "lr", "memory" );
}

void yield_whole_slot()
{
  // Called from an interrupt task, can safely be placed as running->next,
  // since running is the irq_task, and the sleeping task will resume
  // after the SWI it called (or possibly re-try the SWI, in some cases).

  Task *first = workspace.task_slot.sleeping;
  Task *still_sleeping = first;
  Task *last_resume = first;

  // r[1] contains the number of ticks left to sleep for
  // Find all the tasks to be woken (this one, and all the following with
  // r[1] == 0).
  do {
    last_resume = still_sleeping;
    still_sleeping = still_sleeping->next;
  } while (still_sleeping != workspace.task_slot.sleeping
        && still_sleeping->regs.r[1] == 0);

  assert( still_sleeping == workspace.task_slot.sleeping
      || still_sleeping->regs.r[1] != 0 );
  assert( last_resume != 0 );

  // Some (maybe all) have woken...
  dll_detatch_Tasks_until( &workspace.task_slot.sleeping, last_resume );

  assert( workspace.task_slot.sleeping == still_sleeping
       || still_sleeping == first );

  dll_insert_Task_list_at_head( first, &workspace.task_slot.running );
  //show_tasks_state();
}

bool Task_kernel_in_use( svc_registers *regs )
{
  Task *running = workspace.task_slot.running;
  Task *next = running->next;

  assert( next != 0 ); // There's always a next, idle tasks don't sleep.
  assert( next != running ); // There's always a next, idle tasks don't sleep.

  TaskLock code = { .rawp = running };
  assert( !code.wanted );

  // Despite this lock, we will still be competing for the lock word with
  // tasks that haven't claimed the lock yet or one waiting to release it.

  uint32_t volatile *lock = &shared.task_slot.special_lock;

  TaskLock value = { .raw = change_word_if_equal( lock, 0, code.raw ) };

  if (value.raw == 0 || value.half_handle == code.half_handle) {
    // Claimed or reclaimed, either way, we own it
    // (We don't care if another task wants the lock or not.)
    assert( (0 == value.raw) == (0 == shared.task_slot.depth) );

    // depth counter protected by lock ownership
    shared.task_slot.depth ++;

    return false;
  }

  // Another owner

  // Claim this lock for the core before setting bit0 so that
  // the task will be in the waiting list before the owning
  // task can detect bit0 == 1
  bool reclaimed = claim_lock( &shared.task_slot.special_waiting_lock );
  assert( !reclaimed );
  // The owning task is now blocked from changing the lock value
  // Other tasks may only write bit0,

  // The owning task might have changed while we waited...
  // Get the (possibly) new value, and ensure the wanted bit is set

  TaskLock wanted = { .half_handle = value.half_handle, .wanted = 1 };

  value.raw = change_word_if_equal( lock, value.raw, wanted.raw );

  if (value.raw == 0) { // Lock has been released, now it's mine!
    value.raw = change_word_if_equal( lock, 0, code.raw );
    assert( value.raw == 0 );

    // depth counter protected by lock ownership
    shared.task_slot.depth ++;

    release_lock( &shared.task_slot.special_waiting_lock );

    return false;
  }

  // Lock is still owned by someone else, but we've at least set the
  // wanted bit (bit0), and the owner is locked out of releasing the
  // lock.

  workspace.task_slot.running = running->next;

  // Remove from old list
  dll_detatch_Task( running );
  dll_attach_Task( running, &shared.task_slot.special_waiting );
  assert( shared.task_slot.special_waiting != 0 );
  // Put running at end of list, not head
  shared.task_slot.special_waiting = shared.task_slot.special_waiting->next;

  release_lock( &shared.task_slot.special_waiting_lock );

  return true;
}

// This might be good for one core, not for multiple; needs a mechanism to get
// another core to resume the queued task...
// That's also needed for pipes.
void Task_kernel_release()
{
  if (0 == --shared.task_slot.depth) {
    bool reclaimed = claim_lock( &shared.task_slot.special_waiting_lock );
    assert( !reclaimed );

    // We have a lock on the special_waiting list, and the wanted bit
    // in the lock. (Task_kernel_in_use claims the above lock before trying
    // to set the wanted bit.)

    uint32_t *lock = &shared.task_slot.special_lock;

    Task *resume = shared.task_slot.special_waiting;
    TaskLock latest_read = { .raw = *lock };

    if (resume == 0) {
      // Lock apparently isn't wanted (and any task that wants it but
      // hasn't set bit0 and put itself in the waiting list is stuck
      // trying to get the special_waiting_lock).
      *lock = 0;
      assert( !latest_read.wanted );
    }
    else {
      // Lock is wanted, hand it to the waiting task
      TaskLock resumed;
      resumed.raw = handle_from_task( resume );

      if (resume->next == resume) {
        shared.task_slot.special_waiting = 0;
        resumed.wanted = 0;
      }
      else {
        shared.task_slot.special_waiting = resume->next;
        dll_detatch_Task( resume );
        resumed.wanted = 1;
      }

      *lock = resumed.raw;
    }

    release_lock( &shared.task_slot.special_waiting_lock );
  }
}

bool do_OS_File( svc_registers *regs )
{
// Write0( __func__ ); WriteS( " " ); WriteNum( regs->r[0] ); WriteS( " " ); WriteNum( regs->r[1] ); NewLine;
// if (regs->r[0] == 255) { Write0( regs->r[1] ); NewLine; }
  return run_vector( regs, 8 );
}

bool do_OS_Args( svc_registers *regs )
{
  return run_vector( regs, 9 );
}

bool do_OS_BGet( svc_registers *regs )
{
  return run_vector( regs, 10 );
}

bool do_OS_BPut( svc_registers *regs )
{
  return run_vector( regs, 11 );
}

bool do_OS_GBPB( svc_registers *regs )
{
  return run_vector( regs, 12 );
}

bool do_OS_Find( svc_registers *regs )
{
  return run_vector( regs, 13 );
}

bool do_OS_ReadLine( svc_registers *regs )
{
  return run_vector( regs, 14 );
}

bool do_OS_FSControl( svc_registers *regs )
{
if (regs->r[0] == 2) {
WriteS( "OS_FSControl 2 " );
}
  return run_vector( regs, 15 );
}

bool __attribute__(( noreturn )) do_OS_Exit( svc_registers *regs )
{
#ifdef DEBUG__SHOW_UPCALLS
Write0( __func__ ); NewLine;
#endif

  Task *running = workspace.task_slot.running;
  TaskSlot *slot = running->slot;

  handler *h = &slot->handlers[11];

  register uint32_t r12 asm ( "r12" ) = h->private_word;
  register void (*code)() asm ( "r1" ) = h->code;

  asm ( "mrs r0, cpsr"
    "\n  bic r0, #0xcf"
    "\n  msr cpsr, r0"
    "\n  blx r1"
    "\n  svc %[enter]"
    :
    : [enter] "i" (OS_EnterOS)
    , "r" (r12)
    , "r" (code)
    : "lr" );

#ifdef DEBUG__SHOW_UPCALLS
Write0( __func__ ); WriteS( "What do I do now?" ); NewLine;
#endif
  for (;;) asm ( "bkpt 8" );
}

bool __attribute__(( noreturn )) do_OS_ExitAndDie( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  __builtin_unreachable();
}

#include "include/pipeop.h"

void kick_debug_handler_thread()
{
  // Push any debug text written in SVC mode to the pipe.
  // No need to lock the pipes in this routine since:
  //   The debug pipe, if it exists, exists forever
  //   The debug pipe is associated with just one core
  //   The core is running with interrupts disabled.

  uint32_t written = workspace.kernel.debug_written;

  if (written == 0) return;

  uint32_t pipe = workspace.kernel.debug_pipe;

  if (pipe == 0) return;

//show_tasks_state();
  os_pipe *p = (void*) pipe;
  Task *receiver = p->receiver;
  Task *running = workspace.task_slot.running;

  if (receiver == 0 || running == receiver) {
    // Receiver is current task
  }
  else if (p->receiver_waiting_for == 0) {
    // Receiver is running
    // Make it the current task
    // dll_detatch_Task( receiver );
    // dll_attach_Task( receiver, &workspace.task_slot.running );
  }
  else {
    workspace.kernel.debug_written = 0;
    workspace.kernel.debug_space = PipeOp_SpaceFilled( pipe, written );

    if (workspace.task_slot.running->prev == receiver) {
      // Rather than wait for the debug pipe to fill up, we'll yield to
      // the receiver.
      assert( p->receiver_waiting_for == 0 ); // Woken
      workspace.task_slot.running = workspace.task_slot.running->prev;
    }
  }
}

static char *pipe_space( int len )
{
  uint32_t written;

  // Allocate space in the pipe for our string, allowing for being interrupted
  // This can still get screwed up, if the receiver gets scheduled. Can that happen?
  do {
    written = workspace.kernel.debug_written;
    if (written + len > workspace.kernel.debug_space.available) {
      //if (workspace.kernel.frame_buffer_initialised) asm ( "bkpt 2" : : "r" (workspace.kernel.debug_written), "r" (len), "r" (written) );
      return 0; // No space.
    }
  } while (written != change_word_if_equal( &workspace.kernel.debug_written, written, written+len ));

  assert( written < 0x2000 );

  return workspace.kernel.debug_space.location + written;
}

void SVCWriteN( char const *s, int len )
{
  os_pipe *pipe = (os_pipe*) workspace.kernel.debug_pipe;

  if (pipe == 0 || workspace.task_slot.running == pipe->receiver) {
    return; // Daren't do anything
  }

  char *location = pipe_space( len );

  if (location != 0) {
    for (int i = 0; i < len; i++) { location[i] = s[i]; };
  }
}

void SVCWrite0( char const *s )
{
  if (s == 0) s = "<NULL>";
  int len = 0;
  for (len = 0; (s[len] != '\0' && s[len] != '\n' && s[len] != '\r'); len++) {};
  WriteN( s, len );
}

void SVCWriteNum( uint32_t n )
{
  os_pipe *pipe = (os_pipe*) workspace.kernel.debug_pipe;

  if (pipe == 0 || workspace.task_slot.running == pipe->receiver) {
    return; // Daren't do anything
  }

  char *location = pipe_space( 8 );

  if (location != 0) {
    for (int i = 7; i >= 0; i--) {
      location[i] = hex[n & 0xf]; n = n >> 4;
    }
  }
}

svc_registers regs[1] = { { .r[0] = 0xb000b000 } };

void __attribute__(( noreturn )) assertion_failed( uint32_t *abt, svc_registers *regs, const char *assertion )
{
  asm volatile ( "cpsid if" );

  register uint32_t caller asm ( "lr" );

  show_word( workspace.core_number * (1920/4) + 80, 20, (uint32_t) assertion, Green );
  show_word( workspace.core_number * (1920/4) + 160, 20, (uint32_t) caller, Green );
  show_word( workspace.core_number * (1920/4) + 160, 40, (uint32_t) &workspace.task_slot.running, Yellow );
  show_word( workspace.core_number * (1920/4) + 80, 60, (uint32_t) workspace.task_slot.running, Yellow );
  show_word( workspace.core_number * (1920/4) + 160, 60, (uint32_t) workspace.task_slot.running->slot, Yellow );
  show_word( workspace.core_number * (1920/4) + 80, 70, (uint32_t) abt, Yellow );
  show_word( workspace.core_number * (1920/4) + 160, 70, (uint32_t) regs, Yellow );

  // if (workspace.task_slot.running == 0) return; // OS_Heap recursing before tasks initialised?

  show_word( workspace.core_number * (1920/4) + 80, 88, (uint32_t) regs, Green );
  uint32_t *r = &regs->r[0];
  for (int i = 0; i < sizeof( Task ) / 4; i++) {
    show_word( workspace.core_number * (1920/4) + 80, 100+10 * i, r[i], Yellow );
  }
  for (int i = 0; i < 15; i++) {
    show_word( workspace.core_number * (1920/4) + 160, 100+10 * i, abt[i], Yellow );
  }
/*
  // Store the first event that led to this failure, at least.
  uint32_t sp;
  asm ( "mov %[sp], sp" : [sp] "=r" (sp) );
  {
  uint32_t *regs = (void*) ((sp | 0xfffff) - 0xffff - (15 * 4));

  for (int i = 0; i < 15; i++) {
    show_word( workspace.core_number * (1920/4), 100+10 * i, regs[i], Yellow );
  }
  }
*/
  asm volatile ( "wfi\n  wfe" ); // wfe required to keep this wfi separate from the others
  for (;;) asm volatile ( "wfi" );

  __builtin_unreachable();
}

/*
This is something I wrote last October, it's obsolete, now but might be useful.

I haven't thought through the implications of being able to interrupt the kernel in SVC mode.

It was something I wanted to avoid happening, by only having extremely quick routines execute at that level. Legacy RO, however...

Basics:

When a usr32 mode program executes an SWI, the kernel pushes the usr32 accessible registers onto the SVC stack and calls various C routines to handle the SWI, including often re-loading those registers and calling legacy code.

SWIs often call other SWIs.

Generally, SWIs do no affect the banked registers (sp & lr).

The exceptions are ThreadOp, PipeOp, and interrupts, which can all switch tasks.



When a task is switched out in favour of interrupt tasks, and it's executing a SWI, all four banked registers need to be stored and restored. I think.


Since there is only one SVC stack (at the moment), tasks that have been switched out during a SWI have to be switched back in in LIFO order, to preserve the integrity of the stack.

The problem I'm experiencing is probably that the interrupted thread's sp and lr are not being saved correctly. There is no space in the Task structure for both usr and svc sp and lr.

I'd almost forgotten that a single instruction can save the banked usr registers: stm sp, {...}^

It may not write back to the base register. DDI 0487C.a F5-3753 (So, sub sp, sp, #14*4, stm sp, {...}^, not stm sp!, {...}^)


These adjustments have the huge advantage that there will not be any more significant updates to arm32.

 */

static void mpsafe_insert_at_tail( Task **head, Task *task )
{
  // TODO add SEV/WFE
  for (;;) {
    Task *old = *head;
    uint32_t uold = (uint32_t) old;
    if (old == 0) {
      // Empty list, but another core might add something...
      if (0 == change_word_if_equal( (uint32_t*) head, 0, (uint32_t) task )) {
        return;
      }
    }
    else if (uold == change_word_if_equal( (uint32_t*) head, uold, 1 )) {
      // Replaced head pointer with 1, can work on list safely...
      Task **tail = &old->prev;
      dll_attach_Task( task, tail );
      if (1 != change_word_if_equal( (uint32_t*) head, 1, uold )) {
        asm ( "bkpt 4" );
      }
      else {
        return;
      }
    }
  }
}

static Task *mpsafe_detatch_head( Task **head )
{
  Task *result = *head;
  while (result != 0) {
    uint32_t uresult = (uint32_t) result;
    if (uresult == change_word_if_equal( (uint32_t*) head, uresult, 1 )) {
      // Replaced head pointer with 1, can work on list safely...
      Task *tail = result->next;
      if (tail == result) {
        // Only item in queue
        *head = 0;
      }
      else {
        dll_detatch_Task( result );
        *head = tail;
      }
      break;
    }
    result = *head;
  }
  return result;
}

/*
20230402 Final form?

Each independent TaskSlot has:
 * some shared and some private virtual memory areas
 * one or more Tasks associated with it.
 * a single SVC (SWI) stack, which may be used by just one Task
 * SWIs that cannot be interrupted do not need to use the slot's stack,
   using the core's smaller, non-extendible, stack instead.
 * Tasks that call a SWI that may be interrupted will be blocked until
   the slot's stack is released.
*/

static bool interrupt_safe_swi( int number )
{
  // TODO: include intercepted Wimp SWIs

  if (number == OS_IntOn
   || number == OS_IntOff) return true;

  // FIXME This is a hack so that transient callbacks are only
  // called with a extendible svc stack
  if (workspace.kernel.transient_callbacks != 0) return false;

  return (number == OS_ThreadOp
       || number == OS_PipeOp
       || number == OS_FlushCache);
}

/* SWIs are only called by the running task
 * The task will be in one of the following states prior to the SWI:
 *
 *   Running usr32 code
 *     Not owner of SVC stack
 *       Interrupt safe SWI
 *                              Execute SWI
 *       Interrupt unsafe SWI
 *                              Block task until SVC stack claimed
 *                              Execute SWI
 *
 *   Running svc32 code
 *     Owner of SVC stack (SWI, module init, or callback calling)
 *                              Execute SWI
 *     Not owner of SVC stack
 *                              Block task until SVC stack claimed
 *                              Execute SWI
 *
 * Interrupt safe SWIs never enable interrupts.
 */

void __attribute__(( noinline, noreturn )) c_execute_swi( svc_registers *regs )
{
  Task *caller = workspace.task_slot.running;
  svc_registers *resume_sp = regs + 1;

  uint32_t number = get_swi_number( regs->lr );

  if ((number & ~Xbit) == OS_CallASWI) number = regs->r[9];
  else if ((number & ~Xbit) == OS_CallASWIR12) number = regs->r[12];

  // FIXME What should happen if you call CallASWI using CallASWI?
  if ((number & ~Xbit) == OS_CallASWI
   || (number & ~Xbit) == OS_CallASWIR12) asm ( "bkpt 1" ); // FIXME

  TaskSlot *slot = caller->slot;

  assert( owner_of_slot_svc_stack( caller ) == using_slot_svc_stack() );

  // The banked registers are only saved and restored when
  // the task moves from or to usr32 mode.
  if (usr32_caller( regs )) {
    asm ( "\n  mrs %[usrsp], sp_usr"
          "\n  mrs %[usrlr], lr_usr"
          : [usrsp] "=r" (caller->banked_sp_usr)
          , [usrlr] "=r" (caller->banked_lr_usr)
        );
  }

  if (!interrupt_safe_swi( number & ~Xbit )
   && !owner_of_slot_svc_stack( caller )) {
    // Not the owner of the slot's SVC stack, but caller needs to be...
  
    // Legacy SWIs can do all sorts of terrible things, including
    // enabling interrupts.
    // If we are interrupted, the state of the caller will be stored,
    // including the stack pointer and it will eventually return to
    // here as if nothing had happened, but we cannot allow the stack
    // to be corrupted...

    if (0 == change_word_if_equal( (uint32_t*) &slot->svc_stack_owner, 0, (uint32_t) caller )) {
      // Now the owner of the slot specific svc stack
      svc_registers *task_regs = (&svc_stack_top)-1;
      *task_regs = *regs;
      asm volatile ( "mov sp, %[newsp]" : : [newsp] "r" (task_regs) );
      regs = task_regs;
      resume_sp = regs + 1;
    }
    else {
      // Block until the stack is free
      // Put task into list of waiting tasks in slot
      regs->lr -= 4; // Resume at the SVC instruction, not after it
      workspace.task_slot.running = caller->next;
      dll_detatch_Task( caller );
      mpsafe_insert_at_tail( &slot->waiting_for_slot_stack, caller );
    }
  }

  Task *resume = workspace.task_slot.running;

  if (resume == caller) {
    assert( interrupt_safe_swi( number & ~Xbit ) || owner_of_slot_svc_stack( caller ) );

    // SWI can be executed by this Task, so go ahead
    execute_swi( regs, number );

    resume = workspace.task_slot.running;
  }

  if (resume == caller) {
    kick_debug_handler_thread();

    resume = workspace.task_slot.running;
  }

  if (resume != caller) {
    // Swapping out the current task, store its state
    caller->regs = *regs;
    // TODO floating point context, etc.

    if (owner_of_slot_svc_stack( caller )) {
      slot->svc_sp_when_unmapped = (uint32_t*) resume_sp;
    }

    assert( slot == caller->slot );

    regs = &resume->regs;

    if (slot != resume->slot) {
      // Note that there will be writable space on the stack for this value
      // because we just increased the stack pointer by sizeof( svc_registers )

      // Set the stack to the top of the core's SVC stack, which is mapped
      // globally for the core and won't be mapped out by MMU_switch_to.

      register TaskSlot *new_slot asm( "r0" ) = resume->slot;

      asm volatile (
        "\n  mov sp, %[new]"
        "\n  bl MMU_switch_to"
        "\n  mov sp, #3" // Invalid value, will be overwritten
        :
        : [new] "r" (core_svc_stack_top())
        , "r" (new_slot)
        : "r1", "r2", "r3", "r12", "lr" ); // Function call!
    
      assert( ((uint32_t) resume >> 16) == 0xfff8 );
    }

    slot = resume->slot;

    // Now the resumed task's slot is mapped in, with the word at the
    // restored stack pointer being the old value for svc_sp_when_unmapped
    // If the task owns the stack, restore the stack pointer, otherwise
    // leave it pointing at the core's svc_stack

    if (owner_of_slot_svc_stack( resume )) {
      resume_sp = (void*) slot->svc_sp_when_unmapped;
    }
    else {
      resume_sp = core_svc_stack_top();
    }
  }

  assert( slot == resume->slot );

  // At this point, one of these must be true:
  // 1. The running task has not changed, in which case
  //  a) the slot (and therefore the memory map) has not changed
  //  b) the svc_registers stored below the stack are still available
  //  c) the stack pointer is where it was before the SVC instruction was called
  //
  // 2. The running task has changed, but it is in the same slot as the caller.
  // 2.1 The running task is the owner of the svc stack, in which case
  //  a) The stack pointer is the value it had when this task was swapped out last time

  if (resume_sp == &svc_stack_top) {
    // Done with slot's svc_stack

    slot->svc_stack_owner = mpsafe_detatch_head( &slot->waiting_for_slot_stack );
    slot->svc_sp_when_unmapped = (uint32_t*) &svc_stack_top;
    if (slot->svc_stack_owner != 0) {
      //dll_attach_Task( slot->svc_stack_owner, &workspace.task_slot.running->next );
      Task *tail = workspace.task_slot.running->next;
      dll_attach_Task( slot->svc_stack_owner, &tail );
      //workspace.task_slot.running = workspace.task_slot.running->next;
      //regs = &workspace.task_slot.running->regs;
    }

    resume_sp = core_svc_stack_top();
  }

  if (0 == (regs->spsr & 0xf)) {
    asm (
      "\n  msr sp_usr, %[usrsp]"
      "\n  msr lr_usr, %[usrlr]"
      :
      : [usrsp] "r" (resume->banked_sp_usr)
      , [usrlr] "r" (resume->banked_lr_usr)
    );
  }

  asm volatile ( "mov sp, %[top]" : : [top] "r" (resume_sp) );

  register svc_registers *lr asm ( "lr" ) = regs;
  asm (
      "\n  ldm lr!, {r0-r12}"
      "\n  rfeia lr // Restore execution and SPSR"
      :
      : "r" (lr) );

  __builtin_unreachable();
}

Task *__attribute__(( noinline )) c_run_irq_tasks( Task *running )
{
  assert( running->regs.lr == (uint32_t) ResumeSVC
      || (running->regs.spsr & 0x1f) == 0x10 );
  // The state of the running task is safely stored

  Task *irq_task = next_irq_task();

  // This will be a problem if there are spurious interrupts, which are
  // sometimes acceptable. FIXME
  if (irq_task != 0) {

    // Not in a list
    assert( irq_task->next == irq_task && irq_task->prev == irq_task );

    // Block the running task, but keep it on this core
    dll_attach_Task( irq_task, &workspace.task_slot.running );
  }
  // TODO count spurious interrupts, I seem to be getting one in QEMU!

  return workspace.task_slot.running;
}

/*
When a SVC instruction is executed, the running task is in one of these
states:

1. Owner of slot's svc stack
2. Not owner of slot's svc stack, which is
  a) owned by another task, or
  b) not owned by another task

1. May be detected by seeing if the stored state is the only data
on the core's svc stack (in which case it is not the owner)

2. a) or b) can be determined by checking svc_stack_owner, in a
multi-processing safe way.

Case 2a requires the running task to be blocked and queued for when
the owner task releases the slot's svc stack.

Case 2b requires the context to be stored on the newly owned slot's
svc stack.

The context stored in the Task structure is only updated when the
task is swapped out, and ...
*/
void __attribute__(( naked, noreturn )) Kernel_default_svc()
{
  asm volatile (
      // This should detect if the C portion of this function gets
      // complex enough that the code generator needs stack.
      "\n.ifne .-Kernel_default_svc"
      "\n  .error \"Kernel_default_svc compiled code includes instructions before srsdb\""
      "\n.endif"

      "\n  srsdb sp!, #0x13 // Store return address and SPSR (SVC mode)"
      "\n  push { r0-r12 }  // and all the non-banked registers"
  );

  svc_registers *regs;
  asm volatile ( "mov %[regs], sp" : [regs] "=r" (regs) );

  c_execute_swi( regs );

  __builtin_unreachable();
}

#if 0
static void RunTask( char const *command )
{
  register const char *c asm( "r0" ) = command;
  register error_block *result asm( "r0" );
  asm volatile ( "svc 0x20005" : : "r" (c) : "cc" );
}

/* Swaps out the calling task until this task completes or calls
 * Wimp_Poll, the first time.
 */
void StartTask( char const *command )
{
  WriteS( "Start task: " ); Write0( command ); NewLine;

  Task *creator = workspace.task_slot.running;

  WriteS( "\"Wimp\"_StartTask " ); WriteNum( command ); NewLine;
  WriteS( "Creator: " ); WriteNum( creator ); NewLine;
  TaskSlot *child = TaskSlot_new( command );

  assert( 0 == child->wimp_task_handle );
  assert( 0 == child->wimp_poll_block );

  Task *new_task = workspace.task_slot.running;

  WriteS( "New task: " ); WriteNum( new_task ); NewLine;

  assert( creator != new_task );

  new_task->regs.lr = RunTask;
  new_task->regs.spsr = 0x10;
  new_task->banked_lr_usr = (uint32_t) task_exit;
  new_task->banked_sp_usr = 0; // No default stack
  new_task->regs.r[0] = TaskSlot_Command( child );
}
#endif

void Wimp_Polling()
{
  Task *running = workspace.task_slot.running;
  Task *creator = running->slot->creator;
  TaskSlot *slot = running->slot;

  assert( 0 != slot->wimp_task_handle );
  assert( 0 != slot->wimp_poll_block );

  if (creator != 0) {
    slot->creator = 0;
    creator->regs.r[0] = slot->wimp_task_handle;

    Task *tail = running->next;
    dll_attach_Task( creator, &tail );
  }
}

void Wimp_Initialised( uint32_t handle )
{
  Task *running = workspace.task_slot.running;
  TaskSlot *slot = running->slot;

  assert( 0 == slot->wimp_task_handle );

  slot->wimp_task_handle = handle;

  assert( 0 == slot->wimp_poll_block );

  slot->wimp_poll_block = rma_allocate( 256 );

  assert( 0 != slot->wimp_poll_block );
}

bool do_OS_AMBControl( svc_registers *regs )
{
  enum { AMB_Allocate, AMB_Deallocate, AMB_Size, AMB_MapSlot, AMB_Info };

  // Undocumented SWI!
  // I don't even know what AMB stands for!
  // It's application memory blocks.
  // RiscOS/Sources/Kernel/Docs/AMBControl
  switch (regs->r[0] & 7) {
  case AMB_Allocate:
    {
      TaskSlot *slot = TaskSlot_new( "AMB" );
      TaskSlot_adjust_app_memory( slot, regs->r[1] << 12 );
      regs->r[2] = (uint32_t) slot;
      return true;
    }
  case AMB_Deallocate:
    {
      WriteS( "AMB_Deallocate TODO\n\r" );
      // asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
  case AMB_Size:
    {
      int32_t change_in_pages = regs->r[1];
      TaskSlot *slot = (TaskSlot*) regs->r[2];
      TaskSlot_adjust_app_memory( slot, change_in_pages << 12 );
      WriteS( "AMBControl 2 - change size " ); WriteNum( change_in_pages ); NewLine;
      return true;
    }
  case AMB_MapSlot:
    {
      WriteS( "AMB_MapSlot TODO\n\r" );
      asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
  default:
    {
    WriteS( "AMBControl " ); WriteNum( regs->r[0] ); Space; WriteNum( regs->lr ); NewLine;
    }
    break;
  }

  return true;
}

