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

#include "varvals.h"
#include "inkernel.h"

static void __attribute__(( naked, noreturn )) environment_vars_task( uint32_t handle, uint32_t queue )
{
  register uint32_t memory_top asm( "r0" ) = top;
  asm volatile ( "svc %[swi]" : "=r" (memory_top) : [swi] "i" (OSTask_AppMemoryTop), "r" (memory_top) );
  asm volatile ( "mov sp, %[sp]" : : [sp] "ir" (stack_top) );

  c_environment_vars_task( handle, queue );

  __builtin_unreachable();
}

static uint32_t make_command_queue()
{
  queue_result result = Task_QueueCreate();
  return result.handle;
}

void initialise_commands_queue()
{
  if (0 == shared.kernel.commands_queue) {
    if (mpsafe_initialise( &shared.kernel.commands_queue, make_command_queue )) {
      register void *task asm( "r0" ) = environment_vars_task;
      register uint32_t sp asm( "r1" ) = 0;
      register uint32_t queue asm( "r2" ) = shared.kernel.commands_queue;

      register uint32_t handle asm( "r0" );

      asm volatile ( "svc %[swi]"  // volatile because we don't use the output value
                   : "=r" (handle)
                   : [swi] "i" (OSTask_CreateTaskSeparate | Xbit)
                   , "r" (task)
                   , "r" (sp)
                   , "r" (queue)
                   : "lr", "cc" );
    }
    else {
      uint32_t volatile *p = &shared.kernel.commands_queue;
      while (*p == 0) {}
    }
  }
}

#define DELEGATED( SWI, Q ) bool do_##SWI( svc_registers *regs ) { \
  assert( 0 != regs->lr ); \
  error_block *error = queue_running_Task( regs, Q, SWI ); \
  if (error != 0) regs->r[0] = (uint32_t) error; \
  return error == 0; \
}

DELEGATED( OS_ReadVarVal, shared.kernel.commands_queue )
DELEGATED( OS_SetVarVal, shared.kernel.commands_queue )
//DELEGATED( OS_CLI, shared.kernel.commands_queue )
DELEGATED( OS_EvaluateExpression, shared.kernel.commands_queue )
DELEGATED( OS_GSInit, shared.kernel.commands_queue )
DELEGATED( OS_GSTrans, shared.kernel.commands_queue )
DELEGATED( OS_SubstituteArgs, shared.kernel.commands_queue )
DELEGATED( OS_SubstituteArgs32, shared.kernel.commands_queue )

// OS_GSRead reads characters from the pipe created by GSInit

bool do_OS_GSRead( svc_registers *regs )
{
  uint32_t pipe = regs->r[0];
  uint32_t index = regs->r[2];

  if (regs->r[2] == 0xffffffff) return false; // Error already in r0

  // This is quite inefficient, but rarely used
  // Note: the GSInit puts the resultant string into the pipe, PLUS
  // the final terminator (copied from the input string)
  PipeSpace space = PipeOp_WaitForData( pipe, 0 );

  regs->r[1] = ((char*) space.location)[index];
  regs->r[2] ++;

  if (space.available - 1 == index) {
    regs->spsr |= CF;
    assert( regs->r[1] == '\n' || regs->r[1] == '\r' || regs->r[1] == ' ' || regs->r[1] == 0 );
    PipeOp_NotListening( pipe ); // Free up pipe.
    // TODO call PipeNotListening or PipeNoMoreData on all pipes associated with a slot on exit. (Not here!)
  }
  else
    regs->spsr &= ~CF;

  return true;
}

