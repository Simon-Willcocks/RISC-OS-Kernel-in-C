/* Copyright 2022 Simon Willcocks
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

/* There can only be one task at a time that is between polls.
   Probably.
   From testing:
     Wimp_StartTask may be called before any Wimp_Polls
     Wimp_StartTask doesn't return until the child exits or calls Wimp_Poll
  
   OK, so my StartTask should run the CLI in a new TaskSlot, Relinquish control
   to the new Task, which will resume it when it Polls or exits. (This
   hopefully being equivalent to shifting the current task out of the way
   before running the command.)
  
   OSCLI another program and the current Wimp Task exits.
 */
const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing

#define MODULE_CHUNK "0x300"

#include "module.h"
#include "include/taskop.h"

//NO_start;
//NO_init;
NO_finalise;
//NO_service_call;
//NO_title;
NO_help;
//NO_keywords;
//NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char __attribute__(( section( ".text" ) )) title[] = "MultiTaskingWindowManager";

struct workspace {
  uint32_t poll_block[64];
  uint32_t wimp_handle;
  uint32_t poll_word;
  uint32_t task;
  uint32_t stack[62]; // Stack must be at the end, or change start
};

static struct workspace *new_workspace( uint32_t number_of_cores )
{
  uint32_t required = sizeof( struct workspace );

  struct workspace *memory = rma_claim( required );

  memset( memory, 0, required );

  return memory;
}

void __attribute__(( noinline )) c_init( uint32_t this_core, uint32_t number_of_cores, struct workspace **private, char const *args )
{
WriteS( "MTWimp init" ); NewLine;
  bool first_entry = (*private == 0);

  struct workspace *workspace;

  if (first_entry) {
    *private = new_workspace( number_of_cores );
  }

  workspace = *private;
  workspace = workspace; // Warning

WriteS( "MTWimp init done" ); NewLine;

  clear_VF();
}

void __attribute__(( naked )) init( uint32_t this_core, uint32_t number_of_cores )
{
  struct workspace **private;
  char const *args;

  // Move r12, r10 into argument registers
  asm volatile (
          "push { lr }"
      "\n  mov %[private_word], r12"
      "\n  mov %[args_ptr], r10" : [private_word] "=r" (private), [args_ptr] "=r" (args) );

  c_init( this_core, number_of_cores, private, args );
  asm ( "pop { pc }" );
}


enum { Wimp_Initialise = 0x400c0, 
       Wimp_CreateWindow, Wimp_CreateIcon,
       Wimp_DeleteWindow, Wimp_DeleteIcon,
       Wimp_OpenWindow, Wimp_CloseWindow,
       Wimp_Poll,
       Wimp_RedrawWindow, Wimp_UpdateWindow,
       Wimp_GetRectangle,
       Wimp_GetWindowState, Wimp_GetWindowInfo,
       Wimp_SetWindowState,
       Wimp_GetIconState,
       Wimp_GetPointerInfo,
       Wimp_DragBox,
       Wimp_ForceRedraw,
       Wimp_SetCaretPosition, Wimp_GetCaretPosition,
       Wimp_CreateMenu,
       Wimp_DecodeMenu,
       Wimp_WhichIcon,
       Wimp_SetExtent,
       Wimp_SetPointerShape,
       Wimp_OpenTemplate, Wimp_CloseTemplate, Wimp_LoadTemplate,
       Wimp_ProcessKey,
       Wimp_CloseDown,
       Wimp_StartTask,
       Wimp_ReportError,
       Wimp_GetWindowOutline,
       Wimp_PollIdle,
       Wimp_PlotIcon,
       Wimp_SetMode,
       Wimp_SetPalette,
       Wimp_ReadPalette,
       Wimp_SetColour,
       Wimp_SendMessage,
       Wimp_CreateSubMenu,
       Wimp_SpriteOp,
       Wimp_BaseOfSprites,
       Wimp_BlockCopy,
       Wimp_SlotSize,
       Wimp_ReadPixTrans,
       Wimp_ClaimFreeMemory,
       Wimp_CommandWindow,
       Wimp_TextColour,
       Wimp_TransferBlock,
       Wimp_ReadSysInfo,
       Wimp_SetFontColours,
       Wimp_GetMenuState,
       Wimp_RegisterFilter,
       Wimp_AddMessages, Wimp_RemoveMessages,
       Wimp_SetColourMapping,
       Wimp_400f9 };

static uint32_t WimpInitialise( char const *desc, uint32_t *messages )
{
  register uint32_t vers asm( "r0" ) = 310;
  register uint32_t task asm( "r1" ) = 0x4b534154;
  register char const *title asm( "r2" ) = desc;
  register uint32_t *msgs asm( "r3" ) = messages;
  register uint32_t handle asm( "r1" );
  asm volatile ( "svc %[swi]"
         : "=r" (handle)
         , "+r" (vers)
         : [swi] "i" (Xbit | Wimp_Initialise)
         , "r" (vers)
         , "r" (task)
         , "r" (title)
         , "r" (msgs)
         : "lr", "cc", "memory" );

  return handle;
}

static uint32_t WimpPoll( uint32_t mask, uint32_t *block, uint32_t *poll_word )
{
  register uint32_t m asm( "r0" ) = mask;
  register uint32_t *b asm( "r1" ) = block;
  register uint32_t *w asm( "r3" ) = poll_word;
  register uint32_t code asm( "r0" );
  register uint32_t *out asm( "r1" );
  asm volatile ( "svc %[swi]"
         : "=r" (code)
         , "=r" (out)
         : [swi] "i" (Xbit | Wimp_Poll)
         , "r" (m)
         , "r" (b)
         , "r" (w)
         : "lr", "cc", "memory" );

  assert( out == block );

  return code;
}

/* This Task controls access to the Window Manager.
 * It claims idle events and holds on to the Wimp until one of the following
 * occurs:
 *
 *  A program calls Wimp_StartTask (which causes this Task to do it for the
 *  caller.
 *  A HID event occurs
 *  A poll word becomes non-zero (checked regularly, but not constantly)
 *  ...?
 *  
 */
void c_start_wimp_task( struct workspace *workspace )
{
  WriteS( "MTWimp Initialising Wimp..." ); NewLine;

  workspace->wimp_handle = WimpInitialise( "Multi-Tasking Window Manager", 0 );
  WriteS( "MTWimp Looping... " ); WriteNum( workspace->wimp_handle ); NewLine;

  for (;;) {
    // claim lock?
    int event = WimpPoll( 0, workspace->poll_block, &workspace->poll_word );
    // release lock
    switch (event) {
    case 0: WriteS( "Idle" ); NewLine; break; // Task_WaitUntilWoken(); break;
    default: asm ( ".word 0xffffffff\n  bkpt 4" ); break;
    }
  }
}

C_SWI_HANDLER( start_task );

bool start_task( struct workspace *ws, SWI_regs *regs )
{
  // claim lock

  uint32_t handle = 0; // Wimp_StartTask( ... "MTW" );
  // Allow the  AMB code to create the new TaskSlot and the Wimp to execute the program...
  Task_WakeTask( ws->task );
  // release lock
  regs->r[0] = handle;
  return true;
}

void __attribute__(( naked )) start( char const *command )
{
  // Ain't no stack to start with...
  asm ( 
    "\n  ldr r12, [r12]"
    "\n  add sp, r12, %[size]"
    "\n  mov r0, r12"
    "\n  bl c_start_wimp_task"
    "\n0:  b 0b"
    : : [size] "i" (sizeof( struct workspace ) ) );
}



void __attribute__(( naked )) mt_wimp_start()
{
  asm ( "push { "C_CLOBBERED", lr }"
    "\n  mov r2, r0"
    "\n  adr r1, title"
    "\n  mov r0, #2"
    "\n  svc %[swi]"
    "\n  pop { "C_CLOBBERED", pc }"
    :
    : [swi] "i" (OS_Module | Xbit)
    : "lr", "cc", "memory" );
}

void __attribute__(( naked, section( ".text.init" ) )) keywords()
{
// If this assembler is at the top level, it gets placed before file_start,
// screwing up the module header.
  asm ( 
   "\n  .asciz \"MTWimpStart\""
   "\n  .align"
   "\n  .word mt_wimp_start - header"
   "\n  .byte 0 // Min params"
   "\n  .byte 0 // GSTrans map (params 1-8("
   "\n  .byte 0 // Max params"
   "\n  .byte 0 // Flags"
   "\n  .word 0"
   "\n  .word 0"
  
   // End of list
   "\n  .word 0" );
}

void start_wimp( uint32_t *regs, uint32_t service, struct workspace *ws )
{
  // This is called in SVC mode
  // WriteS might not be a good idea...

  switch (service) {
  case 0x49: // Service_StartWimp
    if (ws->wimp_handle == 0) {
      ws->wimp_handle = -1;
      regs[0] = (uint32_t) "MTWimpStart";
      regs[1] = 0; // Claim service
      // FIXME deal with Wimp exiting
    }
    break;
  case 0x4a: // Service_WimpStarted
    if (ws->wimp_handle == -1) {
      ws->wimp_handle = 0;
    }
    break;
  }
}

void wimp_close_down( uint32_t *regs, uint32_t service, struct workspace *ws )
{
  if (regs[0] == 0) {
    // Wimp_Initialise called from my domain
  }
  else if (regs[2] == ws->wimp_handle) {
    // My Wimp Task being shut down (why?)
    static const error_block error = { 0x99, "Wimp is currently active" };
    regs[0] = (uint32_t) &error;
    // regs[1] = 0; // Claimed
    // PRM 3-73 "The call should not be claimed"
  }
}

void __attribute__(( naked )) service_call()
{
  asm ( "teq r1, #0x49" // Service_StartWimp
    "\n  teqne r1, #0x4a" // Service_StartedWimp
    "\n  teqne r1, #0x53" // Service_WimpCloseDown
    "\n  movne pc, lr" );

  asm ( 
    "\n  push { "C_CLOBBERED", lr }"
    "\n  mov r0, sp"
    // r1 = service on entry
    "\n  mov r2, r12 // workspace"
    "\n  cmp r1, #0x53"
    "\n  bne 0f"
    "\n  bl wimp_close_down"
    "\n  pop { "C_CLOBBERED", pc }"
    "\n0:"
    "\n  bl start_wimp"
    "\n  pop { "C_CLOBBERED", pc }" );
}

