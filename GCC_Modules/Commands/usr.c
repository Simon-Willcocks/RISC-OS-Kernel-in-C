/* Copyright 2023 Simon Willcocks
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

// This file contains usr32 mode code

typedef unsigned long long uint64_t;
typedef unsigned        uint32_t;
typedef int             int32_t;
typedef unsigned short  uint16_t;
typedef short           int16_t;
typedef signed char     int8_t;
typedef unsigned char   uint8_t;
typedef unsigned        size_t;
typedef unsigned        bool;
#define true  (0 == 0)
#define false (0 != 0)

#define C_CLOBBERED "r0-r3,r12"

#define assert( c ) while (!(c)) { asm( "bkpt 65535" ); }

#define number_of( arr ) (sizeof( arr ) / sizeof( arr[0] ))

typedef struct {
  uint32_t code;
  char desc[];
} error_block;

#include "kernel_swis.h"
#include "taskop.h"

#define WriteN( s, n ) Task_DebugString( s, n )
#define Write0( s ) Task_DebugString( s, strlen( s ) )
#define WriteS( s ) Task_DebugString( s, sizeof( s ) - 1 )
#define NewLine Task_DebugString( "\n\r", 2 )
#define Space Task_DebugString( " ", 1 )
#define WriteNum( n ) Task_DebugNumber( (uint32_t) (n) )


void __attribute__(( noinline, noreturn )) environment_vars_task( uint32_t handle, uint32_t *queue )
{
  queued_task task;
  uint32_t queue_handle = *queue;

  WriteS( "Commands: Environment Task" ); NewLine;
  for (;;) {
    task = Task_QueueWait( queue_handle );
    assert( task.error == 0 );

    switch (task.swi) {
    case OS_ReadVarVal: WriteS( "OS_ReadVarVal" ); NewLine; break;
    case OS_SetVarVal: WriteS( "OS_SetVarVal" ); NewLine; break;
    case OS_EvaluateExpression: WriteS( "OS_EvaluateExpression" ); NewLine; break;
    case OS_CLI: WriteS( "OS_CLI" ); NewLine; break;
    case OS_GSTrans: WriteS( "OS_GSTrans" ); NewLine; break;
    default: assert( false );
    }
  }
  __builtin_unreachable();
}

// Obey command

/*
  Open file (cache if requested, point to ResourceFS memory if appropriate)
  loop until eof
    read line (change tabs to single spaces, terminated by < ' ')
    If buffer overflowed, return error
    substitute args (don't append unsubstituted)
    nul terminate
    If at eof, 
      close file (if open. if cached, release cache memory)
      Keep copy of command until application replaced
    If verbose, print Obey: command
    OS_CLI command (may replace this command)
  end
  Either OS_Exit/OS_GenerateError or return to caller


  Notes: Obey file called from Obey file is allowed, and will 

  In new kernel: open file as input pipe
*/
