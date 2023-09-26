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

#include "common.h"

static inline os_pipe *pipe_from_handle( uint32_t handle )
{
  return (os_pipe *) handle;
}

static inline uint32_t handle_from_pipe( os_pipe *pipe )
{
  return (uint32_t) pipe;
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

bool this_is_debug_receiver()
{
  Task *running = workspace.task_slot.running;
  os_pipe *pipe = (void*) workspace.kernel.debug_pipe;
  return running == pipe->receiver;
}

static bool in_range( uint32_t value, uint32_t base, uint32_t size )
{
  return (value >= base && value < (base + size));
}

uint32_t debug_pipe_sender_va()
{
  extern uint32_t debug_pipe; // Ensure the size and the linker script match
  os_pipe *pipe = (void*) workspace.kernel.debug_pipe;
  uint32_t va = (uint32_t) &debug_pipe;
  if (pipe->sender_va != 0) {
    assert( pipe->sender_va == va );
    return pipe->sender_va;
  }
  pipe->sender_va = va;
asm ( "udf #2" );
  MMU_map_at( (void*) va, pipe->physical, pipe->max_block_size );
  MMU_map_at( (void*) (va + pipe->max_block_size), pipe->physical, pipe->max_block_size );
  return va;
}

// TODO: Get rid of this, the receiver isn't really a special case
static uint32_t debug_pipe_receiver_va()
{
  extern uint32_t debug_pipe; // Ensure the size and the linker script match
  os_pipe *pipe = (void*) workspace.kernel.debug_pipe;
  uint32_t va = 2 * pipe->max_block_size + (uint32_t) &debug_pipe;
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
  static error_block error = { 0x888, "Invalid Pipe handle" };
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
  uint32_t max_block_size = regs->r[1];
  uint32_t max_data = regs->r[2];
  uint32_t allocated_mem = regs->r[3];

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

  regs->r[0] = handle_from_pipe( pipe );

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

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipeWaitForSpace( svc_registers *regs, os_pipe *pipe )
{
  uint32_t amount = regs->r[1];
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
      pipe->sender_va = debug_pipe_sender_va();
    else
      pipe->sender_va = allocate_virtual_address( slot, pipe );
  }

  uint32_t available = space_in_pipe( pipe );

  if (available >= amount) {
    regs->r[1] = available;
    regs->r[2] = write_location( pipe, slot );

#ifdef DEBUG__PIPEOP
    // WriteS( "Space immediately available: " ); WriteNum( amount ); WriteS( ", total: " ); WriteNum( space_in_pipe( pipe ) ); WriteS( ", at " ); WriteNum( write_location( pipe, slot ) ); NewLine;
#endif
  }
  else {
    pipe->sender_waiting_for = amount;

    assert( running != next );

    save_task_context( running, regs );
    workspace.task_slot.running = next;
    regs->r[1] = 0xb00b00b0;

    // Blocked, waiting for data.
    dll_detach_Task( running );
  }

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

  return true;
}

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipeSpaceFilled( svc_registers *regs, os_pipe *pipe )
{
  error_block *error = 0;

  uint32_t amount = regs->r[1];
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
    regs->r[1] = available - amount;
    regs->r[2] = write_location( pipe, slot );

#ifdef DEBUG__PIPEOP
    // WriteS( "Filled " ); WriteNum( amount ); WriteS( ", remaining: " ); WriteNum( regs->r[1] ); WriteS( ", at " ); WriteNum( regs->r[2] ); NewLine;
#endif

    Task *receiver = pipe->receiver;

    // If there is no receiver, there's nothing to wait for data.
    assert( receiver != 0 || (pipe->receiver_waiting_for == 0) );
    // If the receiver is running, it is not waiting for data.
    assert( (receiver != running) || (pipe->receiver_waiting_for == 0) );

    // Special case: the debug_pipe is sometimes filled from the reader task
    // FIXME Is it really, any more? I don't think so.

    if (pipe->receiver_waiting_for > 0
     && pipe->receiver_waiting_for <= data_in_pipe( pipe )) {
#ifdef DEBUG__PIPEOP
      // WriteS( "Data finally available: " ); WriteNum( pipe->receiver_waiting_for ); WriteS( ", remaining: " ); WriteNum( data_in_pipe( pipe ) ); WriteS( ", at " ); WriteNum( read_location( pipe, slot ) ); NewLine;
#endif

      pipe->receiver_waiting_for = 0;

      receiver->regs.r[1] = data_in_pipe( pipe );
      receiver->regs.r[2] = read_location( pipe, slot );

      // Make the receiver ready to run when the sender blocks (likely when
      // the pipe is full).
      dll_attach_Task( receiver, &workspace.task_slot.running );
      workspace.task_slot.running = workspace.task_slot.running->next;

      assert( workspace.task_slot.running == running );
      // At least two runnble tasks, now
      assert( workspace.task_slot.running->next != workspace.task_slot.running );

      assert( receiver->next = running );
      assert( running->prev == receiver );
    }
  }

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

  return error == 0;
}

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipePassingOver( svc_registers *regs, os_pipe *pipe )
{
  pipe->sender = task_from_handle( regs->r[1] );
  pipe->sender_va = 0; // FIXME unmap and free the virtual area for re-use

  return true;
}

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipeUnreadData( svc_registers *regs, os_pipe *pipe )
{
  regs->r[1] = data_in_pipe( pipe );

  return true;
}

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipeNoMoreData( svc_registers *regs, os_pipe *pipe )
{
  // Write0( __func__ ); NewLine;
  return Kernel_Error_UnimplementedSWI( regs );
}

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipeWaitForData( svc_registers *regs, os_pipe *pipe )
{
  uint32_t amount = regs->r[1];
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
    regs->r[1] = available;
    regs->r[2] = read_location( pipe, slot );

    asm ( "svc 0xff" : : : "lr" ); // Flush whole cache FIXME flush less (by ASID of the sender?)
    assert( (regs->spsr & VF) == 0 );
  }
  else {
    pipe->receiver_waiting_for = amount;

#ifdef DEBUG__PIPEOP
  // WriteS( "Blocking receiver" ); NewLine;
#endif
    save_task_context( running, regs );
    workspace.task_slot.running = next;

    assert( workspace.task_slot.running != running );

    // Blocked, waiting for data.
    dll_detach_Task( running );

    regs->r[1] = 0x22002200; // FIXME remove; just something to look for in the qemu log
    regs->r[2] = 0x33003300; // FIXME remove; just something to look for in the qemu log
  }

  if (!reclaimed) release_lock( &shared.kernel.pipes_lock );

  return true;
}

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipeDataConsumed( svc_registers *regs, os_pipe *pipe )
{
  uint32_t amount = regs->r[1];
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

    regs->r[1] = available - amount;
    regs->r[2] = read_location( pipe, slot );

#ifdef DEBUG__PIPEOP
    // WriteS( "Consumed " ); WriteNum( amount ); WriteS( ", remaining: " ); WriteNum( regs->r[1] ); WriteS( ", at " ); WriteNum( regs->r[2] ); NewLine;
#endif

    if (pipe->sender_waiting_for > 0
     && pipe->sender_waiting_for <= space_in_pipe( pipe )) {
      Task *sender = pipe->sender;

#ifdef DEBUG__PIPEOP
      // WriteS( "Space finally available: " ); WriteNum( pipe->sender_waiting_for ); WriteS( ", remaining: " ); WriteNum( space_in_pipe( pipe ) ); WriteS( ", at " ); WriteNum( write_location( pipe, slot ) ); NewLine;
#endif

      asm ( "svc 0xff" : : : "lr" ); // Flush whole cache FIXME Invalidate cache for updated area, only if sender on a different core
      pipe->sender_waiting_for = 0;

      sender->regs.r[1] = space_in_pipe( pipe );
      sender->regs.r[2] = write_location( pipe, slot );

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

assert( 0x2a2a2a2a != regs->r[2] );
  return true;
}

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipePassingOff( svc_registers *regs, os_pipe *pipe )
{
  pipe->receiver = task_from_handle( regs->r[1] );
  pipe->receiver_va = 0; // FIXME unmap and free the virtual area for re-use

  // TODO Unmap from virtual memory (if new receiver not in same slot)

  return true;
}

#ifdef NOT_DEBUGGING
static inline
#endif
bool PipeNotListening( svc_registers *regs, os_pipe *pipe )
{
  // Write0( __func__ ); NewLine;
  return Kernel_Error_UnimplementedSWI( regs );
}


bool do_PipeOp( svc_registers *regs, uint32_t op )
{
#ifdef DEBUG__PIPEOP
  // Write0( __func__ ); WriteS( " " ); WriteNum( op ); NewLine;
#endif
  /* FIXME!
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

  if (op != OP( OSTask_PipeCreate )) {
    pipe = pipe_from_handle( regs->r[0] );
    if (pipe == 0) {
      return PipeOp_InvalidPipe( regs );
    }
  }

  switch (op) {
  case OP( OSTask_PipeCreate ): return PipeCreate( regs );
  case OP( OSTask_PipeWaitForSpace ): return PipeWaitForSpace( regs, pipe );
  case OP( OSTask_PipeSpaceFilled ): return PipeSpaceFilled( regs, pipe );
  case OP( OSTask_PipePassingOver ): return PipePassingOver( regs, pipe );
  case OP( OSTask_PipeUnreadData ): return PipeUnreadData( regs, pipe );
  case OP( OSTask_PipeNoMoreData ): return PipeNoMoreData( regs, pipe );
  case OP( OSTask_PipeWaitForData ): return PipeWaitForData( regs, pipe );
  case OP( OSTask_PipeDataConsumed ): return PipeDataConsumed( regs, pipe );
  case OP( OSTask_PipePassingOff ): return PipePassingOff( regs, pipe );
  case OP( OSTask_PipeNotListening ): return PipeNotListening( regs, pipe );
  case OP( OSTask_PipeWaitUntilEmpty ): return Kernel_Error_UnknownSWI( regs ); // TODO
  }
  return Kernel_Error_UnknownSWI( regs );
}

// The debug handler pipe is the special case, where every task
// can send to it, and the receiver is scheduled whenever there's
// text in the buffer and this routine is called.

// (The receiver end does not have to be special, FIXME)

// Looking from the outside!
#include "include/pipeop.h"

void kick_debug_handler_thread( svc_registers *regs )
{
  // Push any debug text written in SVC mode to the pipe.
  // No need to lock the pipes in this routine since:
  //   The debug pipe, if it exists, exists forever
  //   The debug pipe is associated with just one core
  //   The core is running with interrupts disabled.

  assert( (regs->spsr & 0x8f) == 0 );
  assert( (regs->spsr & 0x80) == 0 );

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
    // dll_detach_Task( receiver );
    // dll_attach_Task( receiver, &workspace.task_slot.running );
  }
  else {
    workspace.kernel.debug_written = 0;
    workspace.kernel.debug_space = PipeOp_SpaceFilled( pipe, written );

    if (workspace.task_slot.running->prev == receiver) {
      // Rather than wait for the debug pipe to fill up, we'll yield to
      // the receiver.
      assert( p->receiver_waiting_for == 0 ); // Woken by above SpaceFilled

      // About to swap out this, the sender, task
      // (Not needed when the pipe is not the debug pipe.)
      save_task_context( running, regs );

      workspace.task_slot.running = receiver;
    }
  }
}

