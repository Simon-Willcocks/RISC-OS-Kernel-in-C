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

// Copy of the registers stored for an SVC instruction; doesn't include
// the user stack pointer, or link registers, which will be preserved
// automatically (except if the SVC performs a task switch).
typedef struct __attribute__(( packed )) svc_registers {
  uint32_t r[13];
  uint32_t lr;
  uint32_t spsr;
} svc_registers;

#include "include/kernel_swis.h"

// OS SWIs implemented or used other than in swis.c:

bool do_OS_GSTrans( svc_registers *regs );
bool do_OS_SubstituteArgs32( svc_registers *regs );


// Implemented in os_heap.c:
bool do_OS_Heap( svc_registers *regs );

// modules.c:
bool do_OS_Module( svc_registers *regs );
bool do_OS_ServiceCall( svc_registers *regs );

bool do_OS_CallAVector( svc_registers *regs );
bool do_OS_Claim( svc_registers *regs );
bool do_OS_Release( svc_registers *regs );
bool do_OS_AddToVector( svc_registers *regs );
bool do_OS_DelinkApplication( svc_registers *regs );
bool do_OS_RelinkApplication( svc_registers *regs );
bool do_OS_SWINumberFromString( svc_registers *regs );

// Vectored SWIs (do nothing but call the appropriate vectors)
bool do_OS_GenerateError( svc_registers *regs );
bool do_OS_WriteC( svc_registers *regs );
bool do_OS_ReadC( svc_registers *regs );
bool do_OS_CLI( svc_registers *regs );
bool do_OS_Byte( svc_registers *regs );
bool do_OS_Word( svc_registers *regs );

// These file-related SWIs will be serialised before accessing any file systems
bool do_OS_File( svc_registers *regs );
bool do_OS_Args( svc_registers *regs );
bool do_OS_BGet( svc_registers *regs );
bool do_OS_BPut( svc_registers *regs );
bool do_OS_GBPB( svc_registers *regs );
bool do_OS_Find( svc_registers *regs );
bool do_OS_ReadLine( svc_registers *regs );
bool do_OS_FSControl( svc_registers *regs );

bool do_OS_GenerateEvent( svc_registers *regs );
bool do_OS_Mouse( svc_registers *regs );
bool do_OS_UpCall( svc_registers *regs );
bool do_OS_ChangeEnvironment( svc_registers *regs );
bool do_OS_SpriteOp( svc_registers *regs );
bool do_OS_SerialOp( svc_registers *regs );

// TaskSlot
void __attribute__(( naked )) default_os_changeenvironment();
void __attribute__(( naked )) default_ticker();
void __attribute__(( naked )) default_irq();
bool do_OS_GetEnv( svc_registers *regs );
bool do_OS_Exit( svc_registers *regs );
bool do_OS_ExitAndDie( svc_registers *regs );
bool do_OS_ThreadOp( svc_registers *regs );
bool do_OS_PipeOp( svc_registers *regs ); // because it blocks tasks
bool do_OS_ReadDefaultHandler( svc_registers *regs );
void swi_returning_to_usr_mode( svc_registers *regs );

// memory/

bool do_OS_ChangeDynamicArea( svc_registers *regs );
bool do_OS_ReadDynamicArea( svc_registers *regs );
bool do_OS_DynamicArea( svc_registers *regs );
bool do_OS_Memory( svc_registers *regs );

// swis/vdu.c
void default_os_writec( uint32_t r0, uint32_t r1, uint32_t r2 );

void SetInitialVduVars();
bool do_OS_ChangedBox( svc_registers *regs );
bool do_OS_ReadVduVariables( svc_registers *regs );
bool do_OS_ReadPoint( svc_registers *regs );
bool do_OS_ReadModeVariable( svc_registers *regs );
bool do_OS_RemoveCursors( svc_registers *regs );
bool do_OS_RestoreCursors( svc_registers *regs );

// swis/plot.c
bool do_OS_Plot( svc_registers *regs );

// swis/varvals.c
enum VarTypes { VarType_String = 0,
                VarType_Number,
                VarType_Macro,
                VarType_Expanded,
                VarType_LiteralString,
                VarType_Code = 16 };

bool do_OS_ReadVarVal( svc_registers *regs );
bool do_OS_SetVarVal( svc_registers *regs );

// Find a module that provides this SWI
bool do_module_swi( svc_registers *regs, uint32_t svc );

bool Kernel_Error_UnknownSWI( svc_registers *regs );
bool Kernel_Error_UnimplementedSWI( svc_registers *regs );

extern uint32_t rma_base; // Linker generated
extern uint32_t rma_heap; // Linker generated

static inline void rma_free( uint32_t block )
{
  // FIXME
}

static inline void *rma_allocate( uint32_t size )
{
  svc_registers regs;

  void *result = 0;

  regs.r[0] = 2;
  regs.r[1] = (uint32_t) &rma_heap;
  regs.r[3] = size;
  regs.spsr = 0; // V flag set on entry results in failure

  claim_lock( &shared.memory.lock );

  if (do_OS_Heap( &regs )) {
    result = (void*) regs.r[2];
  }

  release_lock( &shared.memory.lock );

  return result;
}

static inline bool error_nomem( svc_registers *regs )
{
asm ( "bkpt 12" );
    static const error_block nomem = { 0x101, "The area of memory reserved for relocatable modules is full" };
    regs->r[0] = (uint32_t) &nomem;
    return false;
}

typedef struct {
  uint32_t mode_selector_flags;
  uint32_t xres;
  uint32_t yres;
  uint32_t log2bpp;
  uint32_t frame_rate;
  struct {
    uint32_t variable;
    uint32_t value;
  } mode_variables[];
} mode_selector_block;

// This is the only mode supported at the moment. Search for it to find
// places to modify to cope with more. It's referenced in swis.c and
// modules.c.
static const uint32_t only_one_mode_xres = 1920;
static const uint32_t only_one_mode_yres = 1080;
extern mode_selector_block const only_one_mode;

// From swis.c, to allow veneers on OS_ SWIs.
bool run_risos_code_implementing_swi( svc_registers *regs, uint32_t svc );
