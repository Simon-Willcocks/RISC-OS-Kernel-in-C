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
  bool allocated;
  uint32_t lock;
  physical_memory_block blocks[10];
  handler handlers[17];
  Task *creator; // creator's slot is parent slot
  char const *command;
  char const *name;
  char const *tail;
  uint64_t start_time;
  Task *waiting;       // 0 or more tasks waiting for locks
};

extern TaskSlot task_slots[];
extern Task tasks[];

// #ifdef DEBUG__SHOW_TASK_SWITCHES
static void show_task( Task *task )
{
  Write0( "task " ); WriteNum( task ); NewLine;
  for (int  i = 0; i < 13; i++) {
    WriteNum( task->regs.r[i] ); if (i != 7) Space; else NewLine;
  }
  WriteNum( task->regs.banked_sp ); Space; WriteNum( task->regs.banked_lr ); Space; WriteNum( task->regs.pc ); 
  NewLine ; WriteS( "Slot " ); WriteNum( task->slot ); Space; WriteNum( task->regs.psr ); 
  WriteS( " next: " ); WriteNum( task->next ); NewLine;
}
// #endif

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
    asm( "bkpt 1" );
  }

  handler *h = &slot->handlers[regs[0]];
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
  Write0( "Changed environment " ); WriteNum( regs[0] ); NewLine;
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

  if (running->next == 0) {
    bool reclaimed = claim_lock( &shared.mmu.lock );

    for (int i = 0; i < 4096/sizeof( Task ); i++) {
      if (0 == (tasks[i].regs.pc & 1)) {
        NewLine;
        Write0( "Task: " ); 
        WriteNum( i );
        Space;
        WriteNum( tasks[i].regs.pc );
        Space;
        WriteNum( tasks[i].next );
        if (running == &tasks[i]) {
          WriteS( " *" );
        }
      }
    }

    if (!reclaimed) release_lock( &shared.mmu.lock );
  }
  assert( running->next != 0 );

  physical_memory_block result = { 0, 0, 0 }; // Fail

  TaskSlot *slot = running->slot;

  claim_lock( &slot->lock );

// WriteS( "Searching slot " ); WriteNum( (uint32_t) slot ); WriteS( " for address " ); WriteNum( va ); NewLine;
  if (slot != 0) {
    for (int i = 0; i < number_of( slot->blocks ) && slot->blocks[i].size != 0 && slot->blocks[i].virtual_base <= va; i++) {
// WriteS( "Block: " ); WriteNum( slot->blocks[i].virtual_base ); WriteS( ", " ); WriteNum( slot->blocks[i].size ); NewLine;
      if (slot->blocks[i].virtual_base <= va && slot->blocks[i].virtual_base + slot->blocks[i].size > va) {
        result = slot->blocks[i];
        goto found;
      }
    }
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
  task->regs.pc = 1; // Never a valid pc, so unallocated
}

static void free_task_slot( TaskSlot *slot )
{
  slot->allocated = 0;
}

static void binary_to_decimal( int number, char *buffer, int size )
{
  register int n asm ( "r0" ) = number;
  register char *b asm ( "r1" ) = buffer;
  register int s asm ( "r2" ) = size;
  asm ( "svc %[swi]" : : [swi] "i" (OS_BinaryToDecimal), "r" (n), "r" (b), "r" (s) );
}

static void allocate_taskslot_memory()
{
  // Only called with lock acquired
  bool first_core = (shared.task_slot.slots_memory == 0);

  if (first_core) {
    shared.task_slot.slots_memory = Kernel_allocate_pages( 4096, 4096 );
    shared.task_slot.tasks_memory = Kernel_allocate_pages( 4096, 4096 );
    if (shared.task_slot.slots_memory == 0) asm ( "bkpt 128" );
    if (shared.task_slot.tasks_memory == 0) asm ( "bkpt 129" );
  }

  // No lazy address decoding for the kernel
  // At least, not initially

  MMU_map_shared_at( (void*) &tasks, shared.task_slot.slots_memory, 4096 );
  MMU_map_shared_at( (void*) &task_slots, shared.task_slot.tasks_memory, 4096 );

  workspace.task_slot.memory_mapped = true;

  if (first_core) {
    bzero( task_slots, 4096 );
    bzero( tasks, 4096 );
    for (int i = 0; i < 4096/sizeof( TaskSlot ); i++) {
      free_task_slot( &task_slots[i] );
    }
    for (int i = 0; i < 4096/sizeof( Task ); i++) {
      free_task( &tasks[i] );
    }
  }
}

static void __attribute__(( noinline, naked )) ignore_event()
{
  asm ( "bx lr" );
}

void __attribute__(( noinline, noreturn )) do_Exit( uint32_t *regs )
{
  // FIXME: What to do with any existing threads?
  // FIXME: Free the TaskSlot
  // FIXME: Free the Task
  // resume the code in the parent slot? Where? After OS_CLI?
  // Yes, I think so.
  Task *task = workspace.task_slot.running;
  TaskSlot *slot = task->slot;

  slot->creator->next = task->next;
  task->next = 0; // TODO delete the task!
  workspace.task_slot.running = slot->creator;
  MMU_switch_to( slot->creator->slot );

  WriteS( "Exiting slot " ); WriteNum( (uint32_t) slot );
  WriteS( " returning to " ); WriteNum( (uint32_t) slot->creator->slot ); NewLine;

  show_task( slot->creator );

  // FIXME memory leak!

  {
    register Task **running asm ( "lr" ) = &workspace.task_slot.running;

    asm volatile (
          "  ldr r0, [lr]"
        "\n  add lr, r0, %[sp]"
        "\n  ldm lr!, {r1, r2} // Load sp, banked lr, point lr at pc/psr"
        "\n  ldr r3, [lr, #4] // PSR"
        "\n  tst r3, #0x0f // Never from Aarch64! (Don't need original value.)"
        "\n  bne 0f"
        "\n  msr sp_usr, r1"
        "\n  msr lr_usr, r2"
        "\n  ldm r0, {r0-r12}"
        "\n  rfeia lr // Restore execution and SPSR"
        "\n0:"
        "\n  msr cpsr, r3"
        "\n  ldm r0, {r0-r13}"
        "\n  ldr pc, [lr]"
        : 
        : "r" (running)
        , [sp] "i" ((char*)&((integer_registers*)0)->banked_sp) );
  }

  __builtin_unreachable();
}

static void __attribute__(( naked )) ExitHandler()
{
  register uint32_t *regs;
  asm ( "push { r0-r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  do_Exit( regs );
}

static void __attribute__(( naked )) ErrorHandler()
{
  WriteS( "Error Handler" ); NewLine; asm ( "bkpt 1" );
  register uint32_t *regs;
  asm ( "push { r0-r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  do_Exit( regs );
}

void __attribute__(( noinline )) save_context( Task *running, svc_registers *regs )
{
  // Save task context (integer only), including the usr stack pointer and link register
  // The register values when the task is resumed
  for (int i = 0; i < 13; i++) {
    running->regs.r[i] = regs->r[i];
  }

  running->regs.pc = regs->lr;
  running->regs.psr = regs->spsr;
  asm volatile ( "mrs %[sp], sp_usr" : [sp] "=r" (running->regs.banked_sp) );
  asm volatile ( "mrs %[lr], lr_usr" : [lr] "=r" (running->regs.banked_lr) );
#ifdef DEBUG__SHOW_TASK_SWITCHES
  Write0( "Saved for later " ); show_task( running ); Space; WriteNum( regs ); NewLine;
#endif
if (running->regs.banked_sp > 0x80000000 && (running->regs.psr & 0xf) == 0) {
  WriteS( "Saved context has kernel stack but user mode\n" );

  for (int i = 0; i < 13; i++) {
    WriteNum( running->regs.r[i] ); if (i == 7) NewLine; else Space;
  }
  WriteNum( running->regs.banked_sp ); Space;
  WriteNum( running->regs.banked_lr ); Space;
  WriteNum( running->regs.pc ); Space;
  WriteNum( running->regs.psr ); NewLine;
}
}

static const handler default_handlers[17] = {
  { 0, 0, 0 },                // RAM Limit for program (0x8000 + amount of RAM)
  { 0xbadf00d1, 0, 0 },       // Undefined instruction
  { 0xbadf00d2, 0, 0 },       // Prefetch abort
  { 0xbadf00d3, 0, 0 },       // Data abort
  { 0xbadf00d4, 0, 0 },       // Address exception
  { 0xbadf00d5, 0, 0 },       // Other exceptions
  { ErrorHandler, 0, 0 },       // Error
  { 0xbadf00d7, 0, 0 },       // CallBack
  { 0xbadf00d8, 0, 0 },       // Breakpoint
  { 0xbadf00d9, 0, 0 },       // Escape
  { 0xbadf00da, 0, 0 },       // Event
  { ExitHandler, 0, 0 },       // Exit
  { 0xbadf00dc, 0, 0 },       // Unused SWI
  { 0xbadf00dd, 0, 0 },       // Exception registers
  { 0, 0, 0 },                // Application space (When does this not = RAM Limit?)
  { 0xbadf00df, 0, 0 },       // Currently Active Object
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

// Which comes first, the slot or the task? Privileged (module?) tasks don't need a slot.
TaskSlot *TaskSlot_new( char const *command_line, svc_registers *regs )
{
  TaskSlot *result = 0;

  bool reclaimed = claim_lock( &shared.mmu.lock );

  if (!workspace.task_slot.memory_mapped) allocate_taskslot_memory();

  // FIXME: make this a linked list of free objects
  for (int i = 0; i < 4096/sizeof( TaskSlot ) && result == 0; i++) {
    if (!task_slots[i].allocated) {
#ifdef DEBUG__WATCH_TASK_SLOTS
WriteS( "Allocated TaskSlot " ); Write0( command_line ); WriteNum( i ); NewLine;
#endif
      result = &task_slots[i];
      result->allocated = true;
      asm ( "dsb" );
      flush_location( &result->allocated );
    }
  }

  if (!reclaimed) release_lock( &shared.mmu.lock );

  if (result == 0) for (;;) { asm ( "bkpt 32" ); } // FIXME: expand

  Task *new_task = Task_new( result );

  Task *task = workspace.task_slot.running;
  if (task != 0) {
    // When resumed, it will return to the point of the SWI that caused this call
    Write0( "Saving creator: " );
    save_context( task, regs ); 

    // Not the first task in the first slot for this core
    result->creator = task;
    new_task->next = task->next;
    // No need to save_and_resume, the new task becomes the current
  }

  workspace.task_slot.running = new_task;

  for (int i = 0; i < number_of( result->handlers ); i++) {
    assert( i < number_of( default_handlers ) );

    result->handlers[i] = default_handlers[i];
  }

  // CAO unique to each TaskSlot, with luck, this should stop the Wimp from messing with Application memory space.
  result->handlers[15].code = (void*) result;

  // Remove leading spaces and *'s
  while (command_line[0] == ' ' || command_line[0] == '*') command_line++;

  uint32_t length = strlen( command_line );
  // Allocate space for a copy of the whole command line, then
  // a second copy which will be spilt into command name and
  // command tail (parameters).
  // FIXME: redirections?
  char *copy = rma_allocate( length * 2 + 2 );
  strcpy( copy, command_line );
  char *command_name = copy+length+1;
  strcpy( command_name, command_line );
  char *command_tail = command_name;
  while (command_tail[0] > ' ') command_tail++;
  bool has_tail = '\0' != command_tail[0];
  command_tail[0] = '\0'; // Terminate the command_name
  if (has_tail) command_tail++; // Otherwise leave it pointing at the nul
  while (command_tail[0] == ' ') command_tail++;

  result->command = copy;
  result->name = command_name;
  result->tail = command_tail;
  result->start_time = 0; // cs since Jan 1st 1900 TODO
  result->lock = 0;
  result->waiting = 0;

#ifdef DEBUG__WATCH_TASK_SLOTS
  Write0( "TaskSlot_new " ); WriteNum( (uint32_t) result ); NewLine;
  Write0( "Command " ); Write0( result->command ); NewLine;
  Write0( "Name " ); Write0( result->name ); NewLine;
  Write0( "Tail " ); Write0( result->tail ); NewLine;
#endif

  return result;
}

void TaskSlot_new_application( char const *command, char const *args )
{
  Task *task = workspace.task_slot.running;
  TaskSlot *slot = task->slot;

#ifdef DEBUG__WATCH_TASK_SLOTS
  WriteS( "TaskSlot_new_application " ); WriteNum( (uint32_t) slot ); NewLine;
  WriteS( "Command " ); Write0( slot->command ); NewLine;
  WriteS( "Old Name \"" ); Write0( slot->name ); WriteS( "\"" ); NewLine;
  WriteS( "Old Tail \"" ); Write0( slot->tail ); WriteS( "\"" ); NewLine;
  WriteS( "New Name \"" ); Write0( command ); WriteS( "\"" ); NewLine;
  WriteS( "New Tail \"" ); Write0( args ); WriteS( "\"" ); NewLine;
#endif

  uint32_t command_length = strlen( command );
  uint32_t args_length = strlen( args );

  char *copy = rma_allocate( command_length * 2 + args_length + 4 ); // Three string terminators and a space

  char *space = copy + command_length;
  char *tail = copy + command_length + 1;
  char *name = tail + args_length + 1;

  slot->command = copy;
  slot->name = name;
  slot->tail = tail;

  strcpy( copy, command );
  *space = ' ';
  strcpy( tail, args );
  strcpy( name, command );

  slot->start_time = 0; // cs since Jan 1st 1900 TODO

#ifdef DEBUG__WATCH_TASK_SLOTS
  Write0( "TaskSlot_new_application " ); WriteNum( (uint32_t) slot ); NewLine;
  Write0( "Command " ); Write0( slot->command ); NewLine;
  Write0( "Name \"" ); Write0( slot->name ); Write0( "\"" ); NewLine;
  Write0( "Tail \"" ); Write0( slot->tail ); Write0( "\"" ); NewLine;
#endif
}

Task *Task_new( TaskSlot *slot )
{
  Task *result = 0;

  bool reclaimed = claim_lock( &shared.mmu.lock );

  if (!workspace.task_slot.memory_mapped) allocate_taskslot_memory();

  // FIXME: make this a linked list of free objects
  for (int i = 0; i < 4096/sizeof( Task ) && result == 0; i++) {
    if (1 == tasks[i].regs.pc) {
      result = &tasks[i];
      result->regs.pc = 3; // Allocated, but still invalid address
    }
  }

  if (!reclaimed) release_lock( &shared.mmu.lock );

  if (result == 0) for (;;) { asm ( "bkpt 33" ); } // FIXME: expand

  result->slot = slot;
  result->resumes = 0;
  result->next = 0;

  Write0( "New Task: " ); WriteNum( result ); NewLine;

  return result;
}

void TaskSlot_add( TaskSlot *slot, physical_memory_block memory )
{
  bool reclaimed = claim_lock( &shared.mmu.lock );
  for (int i = 0; i < number_of( slot->blocks ); i++) {
    if (slot->blocks[i].size == 0) {
      slot->blocks[i] = memory;
#ifdef DEBUG__WATCH_TASK_SLOTS
  Write0( "TaskSlot_add " ); WriteNum( (uint32_t) slot ); Write0( " " ); WriteNum( slot->blocks[i].virtual_base ); Write0( " " ); WriteNum( slot->blocks[i].size ); NewLine;
#endif

      break;
    }
  }

  // FIXME: This is a massive assumption, that there's only one memory section
  slot->handlers[0].code = memory.virtual_base + memory.size;
  slot->handlers[14].code = memory.virtual_base + memory.size;


  if (!reclaimed) release_lock( &shared.mmu.lock );
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
  bool reclaimed = claim_lock( &shared.mmu.lock );

#ifdef DEBUG__WATCH_TASK_SLOTS
  Write0( "TaskSlot_Himem " ); WriteNum( (uint32_t) slot ); Write0( " " ); WriteNum( slot->blocks[0].virtual_base ); Write0( " " ); WriteNum( slot->blocks[0].size ); NewLine;
#endif

  result = slot->blocks[0].size + 0x8000; // slot->blocks[0].virtual_base;
  if (!reclaimed) release_lock( &shared.mmu.lock );
  return result;
}

TaskSlot *TaskSlot_now()
{
  return workspace.task_slot.running->slot;
}

void *TaskSlot_Time( TaskSlot *slot )
{
  return &slot->start_time;
}

char const *TaskSlot_Command( TaskSlot *slot )
{
  return slot->command;
}

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
  CallHandler( regs, 16 );
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

static void __attribute__(( noinline )) c_default_ticker()
{
  // Interrupts disabled, core-specific
  if (workspace.task_slot.sleeping != 0) {
    if (0 == --workspace.task_slot.sleeping->regs.r[1]) {
      // FIXME: Choice to make: newly woken more important than running
      // task, or put at the head of the tail of tasks or the tail of the tail?
      // Going for more important, so that resource-hogs get pre-empted.

      // Since this is called from an interrupt, the running task's context has 
      // already been stored and the task stored in running will be resumed.
      // BUT! The running task might be in a SWI
      // Solution(?): queue them after the irq_task and wait for the SWI to complete
      // (or call OS_LeaveOS?)

      Task *first_woken = workspace.task_slot.sleeping;
      Task *still_sleeping = first_woken;
      Task *last_resume = first_woken;

      WriteS( "Waking " );

      while (still_sleeping != 0 && still_sleeping->regs.r[1] == 0) {
        WriteNum( (uint32_t) still_sleeping ); Space;
        last_resume = still_sleeping;
        still_sleeping = still_sleeping->next;
      }
      NewLine;
      WriteS( "Next: " ); WriteNum( (uint32_t) still_sleeping ); NewLine;

      assert( still_sleeping == 0 || still_sleeping->regs.r[1] != 0 );
      assert( last_resume != 0 );

      // Some (maybe all) have woken...
      last_resume->next = workspace.task_slot.running->next;
      workspace.task_slot.running->next = first_woken;

      // Remove them (all) from sleeping list
      workspace.task_slot.sleeping = still_sleeping;
    }
  }
}

void __attribute__(( naked )) default_ticker()
{
  asm ( "push { "C_CLOBBERED" }" ); // Intend to intercept the vector
  c_default_ticker();
  asm ( "pop { "C_CLOBBERED", pc }" );
}

// FIXME make the following static, as soon as they work!
// Save the running task into running,
// restore the resume task into regs,
// set resume as the running task,
// does NOT affect resume->next.
void __attribute__(( noinline )) save_and_resume( Task *running, Task *resume, svc_registers *regs )
{
#ifdef DEBUG__SHOW_TASK_SWITCHES
  WriteS( "Saving " ); WriteNum( running ); WriteS( ", resuming " ); WriteNum( resume ); NewLine;
#endif
  assert( running != 0 );
  assert( resume != 0 );

  workspace.task_slot.running = resume;

  save_context( running, regs );

if (resume->regs.banked_sp > 0x80000000 && (resume->regs.psr & 0xf) == 0) {
  WriteS( "Returning to usr32 mode but with kernel stack\n" );

  for (int i = 0; i < 13; i++) {
    WriteNum( resume->regs.r[i] ); if (i == 7) NewLine; else Space;
  }
  WriteNum( resume->regs.banked_sp ); Space;
  WriteNum( resume->regs.banked_lr ); Space;
  WriteNum( resume->regs.pc ); Space;
  WriteNum( resume->regs.psr ); NewLine;
}
  // Replace the calling task with the resume task
  regs->lr = resume->regs.pc;
  regs->spsr = resume->regs.psr;
  asm volatile ( "msr sp_usr, %[sp]" : : [sp] "r" (resume->regs.banked_sp) );
  asm volatile ( "msr lr_usr, %[lr]" : : [lr] "r" (resume->regs.banked_lr) );

  for (int i = 0; i < 13; i++) {
    regs->r[i] = resume->regs.r[i];
  }

  // FIXME: do something clever with floating point

  if (resume->slot != running->slot) {
    MMU_switch_to( resume->slot );
  }

#ifdef DEBUG__SHOW_TASK_SWITCHES
  show_task( running ); show_task( resume ); NewLine;
#endif
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
  Task *raw;
  struct {
    uint32_t wanted:1;
    uint32_t task:31;
  };
} TaskLock;

static error_block * __attribute__(( noinline )) Claim( svc_registers *regs )
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

  TaskLock code = { .raw = running };
  assert( !code.wanted );

  // Despite this lock, we will still be competing for the lock word with
  // tasks that haven't claimed the lock yet or one waiting to release it.

  TaskLock latest_read;

  do {
    asm volatile ( "ldrex %[value], [%[lock]]"
                   : [value] "=&r" (latest_read.raw)
                   : [lock] "r" (lock) );

    if (code.task == latest_read.task) {
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
// Note: as part of testing, I put debug output between the LDREX and the STREX.
// That was a mistake, since returning from an exception does a CLREX
    }
    else {
      // Another task owns it, add to blocked list for task slot, block...

      save_and_resume( running, next, regs );

      // Find last pointer in list (may be head pointer)
      // FIXME have a pointer to the last pointer in the list?
      // The list will only be as long as the number of tasks
      // in the slot.
      Task **p = &slot->waiting;
      while ((*p) != 0) p = &(*p)->next;

      *p = running;
      running->next = 0;

      return 0;
    }
  } while (failed);

  return error;
}

static error_block * __attribute__(( noinline )) Release( svc_registers *regs )
{
  error_block *error = 0;

  static error_block not_owner = { 0x888, "Don't try to release locks you don't own!" };

  // TODO check valid address for task
  uint32_t *lock = (void *) regs->r[1];

  Task *running = workspace.task_slot.running;
  TaskSlot *slot = running->slot;

  TaskLock code = { .raw = running };
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
    if (latest_read.task == code.task) {
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

        new_code.raw = waiting;

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

          if (latest_read.task != code.task || latest_read.wanted == 0) {
            asm ( "bkpt 1" ); // Someone's broken the contract
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
  // It is expected that the HAL will have claimed this vector and will return
  // the number of the device the interrupt is for.
  register vector *v asm( "r10" ) = workspace.kernel.vectors[2]; // IrqV - resurrected!

// DEBUG ONLY
  extern uint32_t irq_stack_top;
  uint32_t *p = &irq_stack_top;

  register uint32_t interrupt_address asm( "r0" ) = p[-1];
// END

  register uint32_t device asm( "r0" ); // Device ID returned by HAL

  asm volatile (
      "\n  adr r0, 1f  // Interception address, to go onto stack"
      "\n  push { r0 }"
      "\n  mov r0, #0"
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

#ifdef DEBUG__SHOW_TASK_SWITCHES
  Write0( __func__ ); Space; WriteNum( workspace.task_slot.running ); Space; WriteNum( handler ); Space; WriteNum( handler->next ); NewLine;
#endif
  }

  return handler;
}

static error_block *wait_for_interrupt( svc_registers *regs )
{
  uint32_t device = regs->r[1];
  // Write0( "Wait for Interrupt " ); WriteNum( device ); NewLine;

  if (device >= shared.task_slot.number_of_interrupt_sources) {
    static error_block err = { 0x888, "Requested IRQ out of range" };
    return &err;
  }

  if (workspace.task_slot.irq_tasks == 0) {
    int count = shared.task_slot.number_of_interrupt_sources;
    workspace.task_slot.irq_tasks = rma_allocate( sizeof( Task * ) * count );
    // FIXME failure?
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

  Task *next = running->next;

  running->next = 0; // The running task is out of the running queue

  workspace.task_slot.irq_tasks[device] = running;

  // Interrupts will be disabled when the task is resumed, until InterruptIsOff is called.
  regs->spsr |= 0x80;

  // Any interrupts outstanding?
  Task *irq_task = next_irq_task();

  if (irq_task != 0) {
    // Insert the task before the next task
    assert( irq_task->next == 0 );
    irq_task->next = next;
    next = irq_task;
  }

  if (running != next) // No need to save if this task is still the running task
    save_and_resume( running, next, regs );

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
bool __attribute__(( optimize( "O4" ) )) do_OS_ThreadOp( svc_registers *regs )
{
#ifdef DEBUG__SHOW_TASK_SWITCHES
  WriteS( "ThreadOp " ); WriteNum( regs->r[0] ); NewLine;
  WriteS( "Running: " ); WriteNum( (uint32_t) workspace.task_slot.running ); NewLine;
  for (int i = 0; i < 10; i++) {
    if (0 == (tasks[i].regs.pc & 1)) {
      WriteNum( (uint32_t) &tasks[i] ); WriteS( " -> " ); WriteNum( (uint32_t) tasks[i].next ); NewLine;
    }
  }
#endif
  enum { Start, Exit, WaitUntilWoken, Sleep, Resume, GetHandle, LockClaim, LockRelease,
         WaitForInterrupt = 32, InterruptIsOff, NumberOfInterruptSources,
         DebugString = 48, DebugNumber,
         CoreNumber = 64 };

  error_block *error = 0;

  Task *running = workspace.task_slot.running;
  assert ( running != 0 );
  Task *next = running->next;

#if 0
Write0( "Running: " ); WriteNum( running ); 
while (next != 0) { Space; WriteNum( next ); next = next->next; }
next = running->next;
NewLine;
#endif

  bool svc_caller = (regs->spsr & 0x1f) == 0x13;

  if (regs->r[0] == NumberOfInterruptSources) {
    // Allowed from any mode, but only once.
    assert( shared.task_slot.number_of_interrupt_sources == 0 );
    shared.task_slot.number_of_interrupt_sources = regs->r[1];
    return true;
  }

  if ((regs->spsr & 0x1f) != 0x10                       // Not usr32 mode
   && regs->r[0] != Start                               // Start user task
   && regs->r[0] != CoreNumber                          // Returns the current core number as a string
   && regs->r[0] != DebugString
   && regs->r[0] != DebugNumber
   && !(regs->r[0] == Sleep && regs->r[1] == 0)) {      // yield
    WriteNum( regs->lr ); Space; WriteNum( regs->spsr ); NewLine;
    static error_block error = { 0x888, "Blocking OS_ThreadOp only supported from usr mode." };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

  if (next == 0 && regs->r[0] == Sleep && regs->r[1] == 0) {
    return true; // Yield, but no other threads on this core.
  }

  TaskSlot *slot = running->slot;

  if (slot == 0) {
    Write0( "No slot! " ); WriteNum( regs->r[0] ); NewLine;
  }
  assert ( slot != 0 );
  claim_lock( &slot->lock );

  // Start a new thread
  // Exit a thread (last one out turns out the lights for the slot)
  // Wait until woken
  // Sleep (microseconds)
  // Wake thread (setting registers?)
  // Get the handle of the current thread
  // Set interrupt handler (not strictly thread related)
  switch (regs->r[0]) {
  case Start:
    {
      Task *new_task = Task_new( slot );

      assert( new_task->slot == workspace.task_slot.running->slot );

      // The creating task gets to continue, although its code should
      // not assume that the new task will wait for it to yield.
      Task *running = workspace.task_slot.running;
      new_task->next = running->next;
      running->next = new_task;

      new_task->regs.psr = 0x10; // Tasks always start in usr32 mode
      new_task->regs.pc = regs->r[1];
      new_task->regs.banked_lr = (uint32_t) task_exit;
      new_task->regs.banked_sp = regs->r[2];
      new_task->regs.r[0] = handle_from_task( new_task );
      new_task->regs.r[1] = regs->r[3];
      new_task->regs.r[2] = regs->r[4];
      new_task->regs.r[3] = regs->r[5];
      new_task->regs.r[4] = regs->r[6];
      new_task->regs.r[5] = regs->r[7];
      new_task->regs.r[6] = regs->r[8];

      regs->r[0] = handle_from_task( new_task );

#ifdef DEBUG__WATCH_TASK_SLOTS
      WriteS( "Task created, may or may not start immediately " ); WriteNum( new_task ); Space; WriteNum( slot ); NewLine;
#endif
    }
    break;
  case Sleep:
    {
      Task *running = workspace.task_slot.running;
      assert( running != 0 );

      Task *resume = next;

      assert( resume != 0 || regs->r[1] == 0 ); // Idle thread never sleeps (or otherwise gets removed from the running queue)

#ifdef DEBUG__SHOW_TASK_SWITCHES
Write0( "Sleeping " ); WriteNum( running ); Write0( ", waking " ); WriteNum( resume ); NewLine;
#endif

      if (regs->r[1] == 0) {
        if (resume == 0) break; // Nothing to do, only one thread running

        // Yield
        if (svc_caller) {
          // This thread must be resumed as soon as the other thread relinquishes control.
          // This is in order to maintain a valid SVC stack
          running->next = next->next;
          next->next = running;
        }
        else {
          // Let every other task have a go before resuming
          // TODO maintain a pointer to the last task?
          Task *last = running;
          while (last->next != 0) {
            last = last->next;
          }
          last->next = running;
          running->next = 0;
        }
      }
      else {
        Task **sleeper = &workspace.task_slot.sleeping;

#ifdef DEBUG__SHOW_TASK_SWITCHES
        WriteS( "Sleeping: " ); WriteNum( running ); NewLine;
#endif

        // Subtract the times of the tasks that will be woken before this one
        while (*sleeper != 0 && regs->r[1] >= (*sleeper)->regs.r[1]) {
          regs->r[1] -= (*sleeper)->regs.r[1];
          sleeper = &(*sleeper)->next;
        }

        // Subtract the remaining time for this task from the next to be woken (if any)
        if (0 != *sleeper)
          (*sleeper)->regs.r[1] -= regs->r[1];

        running->next = (*sleeper);
        *sleeper = running;
      }

      save_and_resume( running, resume, regs );
    }
    break;

  case WaitUntilWoken:
    {
      // TODO: Perhaps return the number of resumes instead of making the Task repeatedly call this?
      // FIXME Lock, in case tasks are on separate cores
      running->resumes--;
      if (running->resumes < 0) {
        assert( next != 0 );
        running->next = 0;
        save_and_resume( running, next, regs );
        // running is now detatched
      }
    }
    break;

  case Resume:
    {
      // FIXME validate input
      // TODO Is this right? The task is returned to runnable status, but won't execute until the caller blocks.
      // This behaviour is necessary for interrupt handling tasks prodding second/third level handlers.
      // FIXME Lock, in case tasks are on separate cores
      Task *waiting = task_from_handle( regs->r[1] );
      waiting->resumes++;
      if (waiting->resumes == 0) {
        // Is waiting, detatched from the running list
        Task *task = task_from_handle( regs->r[1] );
        assert( task->next == 0 );
        task->next = running->next;
        running->next = task;
      }
    }
    break;

  case LockClaim:
    {
      error = Claim( regs );
    }
    break;

  case LockRelease:
    {
      error = Release( regs );
    }
    break;

  case WaitForInterrupt:
    {
      error = wait_for_interrupt( regs );
    }
    break;

  case InterruptIsOff:
    {
      // Continue with interrupts enabled, but only when all IRQs have been dealt with
      // The interrupt task must have ensured that there won't be any more interrupts
      // from its source until WaitForInterrupt is called again. It should ensure that
      // interrupts are turned off using OS_IntOff during that time. An interrupt for
      // a source that doesn't have a task waiting is a fatal error (or will be ignored?).
      Write0( "Interrupt is off" ); NewLine;
      regs->spsr &= ~0x80; // Enable interrupts

      // Before we resume the caller with interrupts enabled, handle any outstanding
      // interrupts without.
      Task *irq_task = next_irq_task();
      if (irq_task != 0) {
        irq_task->next = running;

        save_and_resume( running, irq_task, regs );
      }
    }
    break;

  case DebugString:
    WriteN( (char const*) regs->r[1], regs->r[2] );
    break;

  case DebugNumber:
    WriteNum( regs->r[1] );
    break;

  case CoreNumber:
    if (workspace.task_slot.core_number_string[0] == '\0') {
      binary_to_decimal( workspace.core_number,
                       workspace.task_slot.core_number_string,
                       sizeof( workspace.task_slot.core_number_string ) );

      WriteS( "Core number string: " ); Write0( workspace.task_slot.core_number_string ); NewLine;
    }
    regs->r[0] = workspace.task_slot.core_number_string;
    regs->r[2] = strlen( workspace.task_slot.core_number_string );
    break;

  default:
    {
      static error_block unknown_code = { 0x888, "Unknown code" };
      error = &unknown_code;
    }
  }

  release_lock( &slot->lock );

  if (error != 0) { regs->r[0] = (uint32_t) error; }

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
Write0( "local_sender_va " ); WriteNum( pipe->sender );
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

  asm ( "bkpt 64" );
  claim_lock( &shared.kernel.pipes_lock );

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

  release_lock( &shared.kernel.pipes_lock );

#ifdef DEBUG__PIPEOP
  Write0( __func__ ); 
  Write0( " " ); WriteNum( result.virtual_base ); 
  Write0( " " ); WriteNum( result.physical_base ); 
  Write0( " " ); WriteNum( result.size );  NewLine;
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
  // Write0( "Physical memory: " ); WriteNum( pipe->physical ); NewLine;

  // The following will be updated on the first blocking calls 
  // to WaitForSpace and WaitForData, respectively.
  pipe->sender_waiting_for = 0;
  pipe->receiver_waiting_for = 0;

  pipe->write_index = allocated_mem & 0xfff;
  pipe->read_index = allocated_mem & 0xfff;

  claim_lock( &shared.kernel.pipes_lock );

  pipe->next = shared.kernel.pipes;
  shared.kernel.pipes = pipe;

  release_lock( &shared.kernel.pipes_lock );

  regs->r[1] = handle_from_pipe( pipe );

  return true;
}

extern uint32_t pipes_top;

static uint32_t allocate_virtual_address( TaskSlot *slot, os_pipe *pipe )
{
  // Proof of concept locates pipes at the top of the first megabyte of virtual RAM
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

  // Write0( "Allocated pipe VA " ); WriteNum( va - 2 * pipe->max_block_size ); NewLine;

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

  claim_lock( &shared.kernel.pipes_lock );

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

    release_lock( &shared.kernel.pipes_lock );

#ifdef DEBUG__PIPEOP
    // Write0( "Space immediately available: " ); WriteNum( amount ); Write0( ", total: " ); WriteNum( space_in_pipe( pipe ) ); Write0( ", at " ); WriteNum( write_location( pipe, slot ) ); NewLine;
#endif

    return true;
  }

  pipe->sender_waiting_for = amount;

  save_and_resume( running, next, regs );
  running->next = 0;

  release_lock( &shared.kernel.pipes_lock );

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

  // TODO: Flush only that area, and only as far as necessary (are the two slots
  // only single core and running on the same core?)
  asm ( "svc 0xff" : : : "lr" ); // Flush whole cache FIXME

  claim_lock( &shared.kernel.pipes_lock );

  uint32_t available = space_in_pipe( pipe );

  if (available < amount) {
    static error_block err = { 0x888, "Overfilled pipe" };
    error = &err;
  }
  else {
    pipe->write_index += amount;

    regs->r[2] = available - amount;
    regs->r[3] = write_location( pipe, slot );

#ifdef DEBUG__PIPEOP
    // Write0( "Filled " ); WriteNum( amount ); Write0( ", remaining: " ); WriteNum( regs->r[2] ); Write0( ", at " ); WriteNum( regs->r[3] ); NewLine;
#endif

    if (pipe->receiver_waiting_for > 0
     && pipe->receiver_waiting_for <= data_in_pipe( pipe )) {
      Task *receiver = pipe->receiver;

#ifdef DEBUG__PIPEOP
      // Write0( "Data finally available: " ); WriteNum( pipe->receiver_waiting_for ); Write0( ", remaining: " ); WriteNum( data_in_pipe( pipe ) ); Write0( ", at " ); WriteNum( read_location( pipe, slot ) ); NewLine;
#endif

asm ( "svc 0xff" : : : "lr" ); // Flush whole cache FIXME
      pipe->receiver_waiting_for = 0;

      receiver->regs.r[2] = data_in_pipe( pipe );
      receiver->regs.r[3] = read_location( pipe, slot );

      // "Returns" from SWI next time running task (the sender) blocks
      // Special case: the debug_pipe is sometimes filled from the reader task
      if (receiver != running) {
        receiver->next = running->next;
        running->next = receiver;
      }
      else asm ( "bkpt 256" );
    }
  }

  release_lock( &shared.kernel.pipes_lock );

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

  claim_lock( &shared.kernel.pipes_lock );

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
  }
  else {
    pipe->receiver_waiting_for = amount;

#ifdef DEBUG__PIPEOP
  // Write0( "Blocking receiver" ); NewLine;
#endif
    save_and_resume( running, next, regs );
    running->next = 0;
  }

  release_lock( &shared.kernel.pipes_lock );

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

  claim_lock( &shared.kernel.pipes_lock );

  uint32_t available = data_in_pipe( pipe );

  if (available >= amount) {
    pipe->read_index += amount;

    regs->r[2] = available - amount;
    regs->r[3] = read_location( pipe, slot );

#ifdef DEBUG__PIPEOP
    // Write0( "Consumed " ); WriteNum( amount ); Write0( ", remaining: " ); WriteNum( regs->r[2] ); Write0( ", at " ); WriteNum( regs->r[3] ); NewLine;
#endif

    if (pipe->sender_waiting_for > 0
     && pipe->sender_waiting_for <= space_in_pipe( pipe )) {
      Task *sender = pipe->sender;

#ifdef DEBUG__PIPEOP
      // Write0( "Space finally available: " ); WriteNum( pipe->sender_waiting_for ); Write0( ", remaining: " ); WriteNum( space_in_pipe( pipe ) ); Write0( ", at " ); WriteNum( write_location( pipe, slot ) ); NewLine;
#endif

      asm ( "svc 0xff" : : : "lr" ); // Flush whole cache FIXME Invalidate cache for updated area, only if sender on a different core
      pipe->sender_waiting_for = 0;

      sender->regs.r[2] = space_in_pipe( pipe );
      sender->regs.r[3] = write_location( pipe, slot );

      // "Returns" from SWI next time scheduled
      if (sender != running) {
        sender->next = running->next;
        running->next = sender;
      }
    }

    release_lock( &shared.kernel.pipes_lock );

    return true;
  }
  else {
    asm ( "bkpt 1" ); // Consumed more than available?
  }

  release_lock( &shared.kernel.pipes_lock );

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
  // Write0( __func__ ); Write0( " " ); WriteNum( regs->r[0] ); NewLine;
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
         NotListening   // I don't want any more data, thanks
         };
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
  default:
    asm( "bkpt 1" );
  }
  return PipeOp_InvalidCode( regs );
}

// Default action of IrqV is not to disable the interrupt, it's to throw a wobbly.
// The HAL must ensure that IrqV never gets this far!

void default_irq()
{
  asm ( "bkpt 1" );
}

void __attribute__(( naked, noreturn )) Kernel_default_irq()
{
  asm volatile (
        "  sub lr, lr, #4"
      "\n  srsdb sp!, #0x12 // Store return address and SPSR (IRQ mode)" );

  {
    // This is essentially save_context, but without storing much on the stack first.
    assert( 0 == (void*) &((Task*) 0)->regs );

    // Need to be careful with this, that the compiler doesn't insert any code to
    // set up lr using another register, corrupting it.
    register Task **running asm ( "lr" ) = &workspace.task_slot.running;

    asm volatile (
          "  ldr lr, [lr]"
        "\n  stm lr!, {r0-r12}  // lr -> banked_sp"
        "\n  pop { r2, r3 }     // Resume address, SPSR"
        "\n  ands r4, r3, #0x0f // Never comes from Aarch64!"
        "\n  mrseq r0, sp_usr"
        "\n  mrseq r1, lr_usr"
        "\n  mrsne r0, sp_svc"
        "\n  mrsne r1, lr_svc"
        "\n  stm lr, {r0-r3}"
        : 
        : "r" (running) );
  }

Task *running = workspace.task_slot.running;
if (running->regs.banked_sp > 0x80000000 && (running->regs.psr & 0xf) == 0) {
  asm ( "bkpt 3000" );
}
#ifdef DEBUG__SHOW_INTERRUPTS
{
Task *task = workspace.task_slot.running;
Write0( "Real IRQ: " ); Write0( "task " ); WriteNum( task ); Space; WriteNum( task->regs.pc ); Space; WriteNum( task->regs.psr ); Space; WriteNum( task->slot ); NewLine;
}
#endif

  // We're now running without a task. Find all the interrupt tasks that should be
  // resumed and put them in a queue (first at the head, since the HAL will return
  // them in order of priority)
  // OR
  // Take the first one, run it until it is done with the immediate hardware problem
  // (calls WaitForInterrupt or InterruptIsOff), at which point ask the HAL for the
  // next IRQ.

  // The latter is better, because it will allow the HAL to report higher priority
  // interrupts that occur during interrupt handling.
  // Interrupt Tasks that call InterruptIsOff will be scheduled after the last Task
  // that's calling a SWI. (Can there be more than one?)

  // We're going to deal with the interrupt(s) now, with interrupts disabled,
  // generally resuming a task that will take care of anything time-consuming
  // wrt the device. Resuming tasks from the irq_task means putting them in
  // the queue *after* the irq_task, so that they get run as soon as all the
  // interrupt routines have completed.

  if (0 != (workspace.task_slot.running->regs.psr & 0xf)
   && 3 != (workspace.task_slot.running->regs.psr & 0xf)) {
    // We don't enable interrupts while dealing with undefined instructions or
    // aborts, do we?
    asm ( "bkpt 1" );
  }

  Task *irq_task = next_irq_task();

  if (irq_task != 0) {
    irq_task->next = workspace.task_slot.running;
    workspace.task_slot.running = irq_task;
  }

  {
    register Task *running asm ( "r0" ) = workspace.task_slot.running;

    asm volatile (
        "\n  add lr, r0, %[sp]"
        "\n  ldm lr!, {r1, r2} // Load sp, banked lr, point lr at pc/psr"
        "\n  ldr r3, [lr, #4] // PSR"
        "\n  ands r3, r3, #0x0f // Never from Aarch64! (Don't need original value.)"
        "\n  msreq sp_usr, r1"
        "\n  msreq lr_usr, r2"
        "\n  msrne sp_svc, r1 // Works because in irq32 mode"
        "\n  msrne lr_svc, r2 // Works because in irq32 mode"
        "\n  ldm r0, {r0-r12}"
        "\n  rfeia lr // Restore execution and SPSR"
        : 
        : "r" (running)
        , [sp] "i" ((char*)&((integer_registers*)0)->banked_sp) );
  }

  __builtin_unreachable();
}

// File operations

// Legacy code will call these SWIs, this code will translate them to PipeOp
// and calls to the legacy FileCore/FileSwitch fileing systems (using CallAVector)
// Only one task thread will access the legacy filing systems at a time.

bool run_vector( svc_registers *regs, int vec );

bool delegate_operation( svc_registers *regs, int operation )
{
  // This is a temporary solution. The tasks should block until woken, but
  // they will instead busy-wait for the lock while yielding.
  // Unfortunately, this tries to block callers from SVC modes, which isn't supported.
  // In that case, simply loop, and allow interrupts to schedule other tasks?
retry: {}

  Task *running = workspace.task_slot.running;
  uint32_t handle = (uint32_t) running;

  uint32_t old = change_word_if_equal( &shared.task_slot.filesystem_lock, 0, handle );
  bool reclaimed = old == handle;

  if (old == 0 || reclaimed) {
    if (operation == 13 && (regs->r[0] >= 64 && regs->r[0] < 256)) {
      WriteS( "Open file \"" ); Write0( regs->r[1] ); WriteS( "\"" ); NewLine;
    }
#ifdef DEBUG__SHOW_ALL_FS_VECTOR_CALLS
    Write0( "Claimed lock" ); Space; if (reclaimed) Write0( " (reclaimed) " ); NewLine;
    Write0( "Running vector " ); WriteNum( operation ); NewLine;
    WriteNum( regs->r[0] ); Space;
    WriteNum( regs->r[1] ); Space;
    WriteNum( regs->r[2] ); Space;
    WriteNum( regs->r[3] ); NewLine;
    WriteNum( regs->r[4] ); Space;
    WriteNum( regs->r[5] ); Space;
    WriteNum( regs->r[6] ); Space;
    WriteNum( regs->r[7] ); NewLine;
#endif
    run_vector( regs, operation );

#ifdef DEBUG__SHOW_ALL_FS_VECTOR_CALLS
    NewLine;
    WriteNum( regs->r[0] ); Space;
    WriteNum( regs->r[1] ); Space;
    WriteNum( regs->r[2] ); Space;
    WriteNum( regs->r[3] ); NewLine;
    WriteNum( regs->r[4] ); Space;
    WriteNum( regs->r[5] ); Space;
    WriteNum( regs->r[6] ); Space;
    WriteNum( regs->r[7] ); NewLine;
#endif

    if (operation == 13) {
      WriteS( "OS_Find: " ); WriteNum( regs->r[0] ); NewLine;
    }

    if (!reclaimed) {
      // There's no tasks queued, no need for anything clever with STREX etc.
      shared.task_slot.filesystem_lock = 0;
#ifdef DEBUG__SHOW_ALL_FS_VECTOR_CALLS
      Write0( "Released lock" ); NewLine;
#endif
    }
    else {
#ifdef DEBUG__SHOW_ALL_FS_VECTOR_CALLS
      Write0( "Keeping lock" ); NewLine;
#endif
    }
    return true;
  }
  else if ((regs->spsr & 0xf) == 0) { // usr32 mode caller, allowed to be switched out
    Write0( "Lock is held by " ); WriteNum( old ); NewLine;

    regs->lr -= 4; // Yield in such a way that the task will re-try the SWI

    Task *next = running->next;

    assert( next != 0 ); // There's always the idle task

    // Remove task from running queue
    save_and_resume( running, next, regs );

    Task *last = next;
    while (last->next != 0) {
      last = last->next;
    }
    last->next = running;
    running->next = 0;
  }
  else {
    goto retry;
  }

  return 0 == (regs->spsr & VF); // Result of the last operation, in the current thread
}

// FIXME: Return false if V flag set on exit from vector?

bool do_OS_File( svc_registers *regs )
{
// Write0( __func__ ); WriteS( " " ); WriteNum( regs->r[0] ); WriteS( " " ); WriteNum( regs->r[1] ); NewLine;
// if (regs->r[0] == 255) { Write0( regs->r[1] ); NewLine; }
  return delegate_operation( regs, 8 );
}

bool do_OS_Args( svc_registers *regs )
{
  return delegate_operation( regs, 9 );
}

bool do_OS_BGet( svc_registers *regs )
{
  return delegate_operation( regs, 10 );
}

bool do_OS_BPut( svc_registers *regs )
{
  return delegate_operation( regs, 11 );
}

bool do_OS_GBPB( svc_registers *regs )
{
  return delegate_operation( regs, 12 );
}

bool do_OS_Find( svc_registers *regs )
{
  return delegate_operation( regs, 13 );
}

bool do_OS_ReadLine( svc_registers *regs )
{
  return delegate_operation( regs, 14 );
}

bool do_OS_FSControl( svc_registers *regs )
{
  return delegate_operation( regs, 15 );
}

bool __attribute__(( noreturn )) do_OS_Exit( svc_registers *regs )
{
  Task *running = workspace.task_slot.running;
  TaskSlot *slot = running->slot;

  handler *h = &slot->handlers[11];

  register uint32_t r12 asm ( "r12" ) = h->private_word;
  register void (*code)() asm ( "r1" ) = h->code;

  asm ( "mrs r0, cpsr"
    "\n  bic r0, #0xcf" 
    "\n  msr cpsr, r0"
    "\n  bx r1" 
    : : "r" (r12), "r" (code) );

  __builtin_unreachable();
}

bool __attribute__(( noreturn )) do_OS_ExitAndDie( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  asm ( "bkpt 1" );
  __builtin_unreachable();
}

#include "include/pipeop.h"

void swi_returning_to_usr_mode( svc_registers *regs )
{
  // No need to lock the pipes in this routine since:
  //   The debug pipe, if it exists, exists forever
  //   The debug pipe is associated with just one core
  //   The core is running with interrupts disabled.

  uint32_t written = workspace.kernel.debug_written;

  if (written == 0) return;

  uint32_t pipe = workspace.kernel.debug_pipe;

  if (pipe == 0) return;

  os_pipe *p = (void*) pipe;
  Task *receiver = p->receiver;
  Task *running = workspace.task_slot.running;

  if (receiver == 0 || p->receiver_waiting_for == 0) {
    // Receiver is running, or not yet started.
    // Nothing we can do?
  }
  else if (running == receiver) {
    // Receiver is current task
    asm ( "bkpt 2" );
  }
  else {
    workspace.kernel.debug_written = 0;
    workspace.kernel.debug_space = PipeOp_SpaceFilled( pipe, written );
    //assert( p->receiver_waiting_for == 0 ); // Woken
    assert( running->next == receiver );

    // Yield to the receiver, only
    running->next = receiver->next;
    receiver->next = running;
    save_and_resume( running, receiver, regs );
  }
}

void SVCWriteN( char const *s, int len ) 
{
  os_pipe *pipe = (os_pipe*) workspace.kernel.debug_pipe;

  if (pipe == 0 || workspace.task_slot.running == pipe->receiver) {
    return; // Daren't do anything
  }

  if (workspace.kernel.debug_written + len < workspace.kernel.debug_space.available) {
    char *location = workspace.kernel.debug_space.location + workspace.kernel.debug_written;

    for (int i = 0; i < len; i++) { location[i] = s[i]; };

    workspace.kernel.debug_written += len;
  }
}

void SVCWriteNum( uint32_t n )
{
  os_pipe *pipe = (os_pipe*) workspace.kernel.debug_pipe;

  if (pipe == 0 || workspace.task_slot.running == pipe->receiver) {
    return; // Daren't do anything
  }

  if (workspace.kernel.debug_space.available - workspace.kernel.debug_written < 8) {
    return;
  }

  char *location = workspace.kernel.debug_space.location + workspace.kernel.debug_written;
  for (int i = 7; i >= 0; i--) {
    location[i] = hex[n & 0xf]; n = n >> 4;
  }

  workspace.kernel.debug_written += 8;
}

void __attribute__(( noreturn )) assertion_failed()
{
  NewLine; WriteS( "Failed task: " ); WriteNum( workspace.task_slot.running ); NewLine;

  // if (workspace.task_slot.running == 0) return; // OS_Heap recursing before tasks initialised?

  show_task( workspace.task_slot.running );

  // Store the first event that led to this failure, at least.
  uint32_t sp;
  asm ( "mov %[sp], sp" : [sp] "=r" (sp) );
  svc_registers *regs = (void*) ((sp | 0xfffff) - 0xffff - (15 * 4));

  save_context( workspace.task_slot.running, regs );

    for (int i = 0; i < 4096/sizeof( Task ); i++) {
      if (1 == tasks[i].regs.pc) {
        WriteS( "Free task: " ); WriteNum( &tasks[i] ); NewLine;
      }
      else if (3 == tasks[i].regs.pc) {
        WriteS( "Allocated task: " ); WriteNum( &tasks[i] ); NewLine;
      }
      else {
        show_task( &tasks[i] );
      }
    }

  workspace.task_slot.running = workspace.task_slot.running->next;

  // This allows the OS to stagger on a while before collapsing in a heap
  {
    register Task **running asm ( "lr" ) = &workspace.task_slot.running;
    register uint32_t current_stack asm ( "sp" );
    // FIXME Remove? Has knowledge of memory layout
    register uint32_t old_sp asm ( "r0" ) = (0xfff00000 & current_stack) | 0x000f0000;

    asm volatile (
          "  mov sp, r0"
        "\n  ldr r0, [lr]"
        "\n  add lr, r0, %[sp]"
        "\n  ldm lr!, {r1, r2} // Load sp, banked lr, point lr at pc/psr"
        "\n  ldr r3, [lr, #4] // PSR"
        "\n  ands r3, r3, #0x0f // Never from Aarch64! (Don't need original value.)"
        "\n  bne 0f"
        "\n  msr sp_usr, r1"
        "\n  msr lr_usr, r2"
        "\n  ldm r0, {r0-r12}"
        "\n  rfeia lr // Restore execution and SPSR"
        "\n0:"
        "\n  msr cpsr, r3"
        "\n  ldm r0, {r0-r13}"
        "\n  ldr pc, [lr]"
        : 
        : "r" (running)
        , "r" (old_sp)
        , [sp] "i" ((char*)&((integer_registers*)0)->banked_sp) );
  }
  __builtin_unreachable();
}

