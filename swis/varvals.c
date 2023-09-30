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

#include "inkernel.h"
#include "include/taskop.h"

// FIXME Move these debug functions to a header file
static inline void debug_string_with_length( char const *s, int length )
{
  register char const *string asm( "r0" ) = s;
  register int len asm( "r1" ) = length;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_DebugString)
      , "r" (string)
      , "r" (len)
      : "lr", "memory" );
}

static inline void debug_string( char const *s )
{
  int len = 0;
  char const *p = s;
  while (*p != '\0') { p++; len++; }
  debug_string_with_length( s, len );
}

static inline void debug_number( uint32_t num )
{
  register uint32_t number asm( "r0" ) = num;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_DebugNumber)
      , "r" (number)
      : "lr", "memory" );
}

#define WriteN( s, n ) debug_string_with_length( s, n )
#define Write0( s ) debug_string( s )
#define WriteS( s ) debug_string_with_length( s, sizeof( s ) - 1 )
#define NewLine WriteS( "\n" );
#define Space WriteS( " " );
#define WriteNum( n ) debug_number( n )

#define OP( c ) (0x3f & (c))

struct results {
  uint32_t regs[6];             // Match with vvv
  uint32_t psr;
  uint32_t caller;
};

// In the client Task's slot, run the legacy SWI, passed in r12,
// store the resulting registers at r11, then return control to
// the caller Task.
void __attribute__(( naked )) run_legacy()
{
  asm ( "svc %[swi]"
    "\n  stm r11!, { r0-r5 }"   // Match with ^^^
    "\n  mrs r0, cpsr"
    "\n  str r0, [r11]"
    "\n  ldr r0, [r11, #4]"
    "\n  svc %[relinquish]"
        :
        : [swi] "i" (Xbit | OSTask_CallLegacySWI)
        , [relinquish] "i" (Xbit | OSTask_RelinquishControl) );
}

// Use the legacy code until re-implemented GSTrans etc.
// Note: This works because the SWIs are all <= 0x40, there needs to be
// a separate queue for numbers outside that range.
void __attribute__(( noinline, noreturn )) c_task( uint32_t handle, uint32_t queue )
{
  queued_task task;
  error_block *error = 0;

  svc_registers copy;

  struct results *results = rma_allocate( sizeof ( struct results ) );
  results->caller = handle;

  WriteS( "Commands: Environment Task" ); NewLine;
  for (;;) {
    task = Task_QueueWait( queue );
    assert( task.error == 0 );

    svc_registers regs;
    Task_GetRegisters( task.task_handle, &regs );
    copy = regs;
    copy.r[9] = 0x11111111;
    copy.r[10] = 0x99999999;

    if (task.swi == OP( OS_GSInit )) {
      // Allocate space, GSTrans into it, create a pipe over the data
      // GSRead then simply reads a byte at a time from the pipe
      // Later.
    }
    // Run the legacy SWI in the queued task, then release it to continue
    copy.lr = (uint32_t) run_legacy;
    copy.r[11] = (uint32_t) results;
    copy.r[12] = Xbit | task.swi;
    error = Task_RunThisForMe( task.task_handle, &copy );
    regs.r[0] = results->regs[0];
    regs.r[1] = results->regs[1];
    regs.r[2] = results->regs[2];
    regs.r[3] = results->regs[3];
    regs.r[4] = results->regs[4];
    regs.r[5] = results->regs[5];
    // Copy just the flags, not the mode, etc.
    regs.spsr = (regs.spsr & ~0xf0000000) | (results->psr & 0xf0000000);

    WriteS( "Delegated SWI at " ); WriteNum( regs.lr ); NewLine;
    switch (task.swi) {
    case OP( OS_ReadVarVal ):
      WriteS( "OS_ReadVarVal" ); NewLine; Write0( copy.r[0] ); NewLine;
      break;
    case OP( OS_SetVarVal ):
      WriteS( "OS_SetVarVal" ); NewLine; Write0( copy.r[0] ); NewLine;
      break;
    case OP( OS_EvaluateExpression ):
      WriteS( "OS_EvaluateExpression" ); NewLine;
      break;  
    case OP( OS_CLI ):
      WriteS( "OS_CLI" ); NewLine;
      break;
    case OP( OS_GSTrans ):
      WriteS( "OS_GSTrans" ); NewLine;
      break;
    case OP( OS_GSInit ):
      WriteS( "OS_GSInit" ); NewLine;
      break;
    case OP( OS_SubstituteArgs ):
      WriteS( "OS_SubstituteArgs" ); NewLine;
      break;
    case OP( OS_SubstituteArgs32 ):
      WriteS( "OS_SubstituteArgs32" ); NewLine;
      break;
    default: assert( false );
    }

    error = Task_ReleaseTask( task.task_handle, &regs );
  } 

  __builtin_unreachable();
}

void __attribute__(( naked, noreturn )) environment_vars_task( uint32_t handle, uint32_t queue )
{
  register uint32_t top asm( "r0" ) = 0x9000;

  asm volatile ( "svc %[swi]"
    "\n  mov sp, r0" // New top, should be the above constant
    :
    : [swi] "i" (OSTask_AppMemoryTop)
    , "r" (top) );

  c_task( handle, queue );
}

static inline uint32_t commands_queue()
{
  if (shared.kernel.commands_queue == 0) {
    if (mpsafe_initialise( &shared.kernel.commands_queue, new_queue )) {
      register uint32_t handle asm( "r0" );

      register void *task asm( "r0" ) = environment_vars_task;
      register uint32_t sp asm( "r1" ) = 0;
      register uint32_t queue asm( "r2" ) = shared.kernel.commands_queue;
      asm volatile ( "svc %[swi]"  // volatile because we don't use the output value
                   : "=r" (handle)
                   : [swi] "i" (OSTask_CreateTaskSeparate | Xbit)
                   , "r" (task)
                   , "r" (sp)
                   , "r" (queue)
                   : "lr", "cc" );
    }
  }

  return shared.kernel.commands_queue;
}

#define DELEGATED( SWI, Q ) bool do_##SWI( svc_registers *regs ) { \
  error_block *error = queue_Task( regs, Q, Task_now(), SWI ); \
  if (error != 0) regs->r[0] = (uint32_t) error; \
  return error == 0; \
}

DELEGATED( OS_ReadVarVal, commands_queue() )
DELEGATED( OS_SetVarVal, commands_queue() )
//DELEGATED( OS_CLI, commands_queue() )
DELEGATED( OS_EvaluateExpression, commands_queue() )
DELEGATED( OS_GSInit, commands_queue() )
DELEGATED( OS_GSTrans, commands_queue() )
DELEGATED( OS_SubstituteArgs, commands_queue() )
DELEGATED( OS_SubstituteArgs32, commands_queue() )

// OS_GSRead reads characters from the pipe created by GSInit

