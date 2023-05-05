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

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing

// Explicitly no SWIs provided (it's the default, anyway)
#define MODULE_CHUNK "0"

#include "module.h"

//NO_start;
//NO_init;
NO_finalise;
//NO_service_call;
//NO_title;
NO_help;
//NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char __attribute__(( section( ".text" ) )) title[] = "MultiTaskingWindowManager";

struct workspace {
  uint32_t poll_block[64];
  uint32_t wimp_handle;
  uint32_t poll_word;
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

void c_start_wimp_task( struct workspace *workspace )
{
  WriteS( "MTWimp Initialising Wimp..." ); NewLine;

  workspace->wimp_handle = WimpInitialise( "Multi-Tasking Window Manager", 0 );
  WriteS( "MTWimp Looping..." ); NewLine;

  for (;;) {
    switch (WimpPoll( 0, workspace->poll_block, &workspace->poll_word )) {
    case 0: WriteS( "Idle" ); NewLine; Sleep( 1 ); break;
    default: asm ( "bkpt 4" ); break;
    }
  }
}

void __attribute__(( naked )) start( char const *command )
{
  asm ( "push { "C_CLOBBERED", lr }"
    "\n  ldr r12, [r12]"
    "\n  add sp, r12, %[size]"
    "\n  mov r0, r12"
    "\n  bl c_start_wimp_task"
    "\n  pop { "C_CLOBBERED", pc }" : : [size] "i" (sizeof( struct workspace ) ) );
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

void __attribute__(( naked, section( ".text.init" ) )) in_text_init_section()
{
// If this assembler is at the top level, it gets placed before file_start,
// screwing up the module header.
  asm ( 
   "\nkeywords:"
   "\n  .asciz \"MTWimpStart\""
   "\n  .align"
   "\n  .word mt_wimp_start - header"
   "\n  .word 0x00000000"
   "\n  .word 0"
   "\n  .word 0"
  
   // End of list
   "\n  .word 0" );
}

void start_wimp( uint32_t *regs, struct workspace *workspace )
{
  // This is called in SVC mode
  // WriteS might not be a good idea...

  switch (regs[1]) {
  case 0x49: // Service_StartWimp
    if (0 && workspace->wimp_handle == 0) {
      workspace->wimp_handle = -1;
      regs[0] = (uint32_t) "MTWimpStart";
      regs[1] = 0; // Claim service
      // FIXME deal with Wimp exiting StartedWimp/Reset
    }
    break;
  case 0x4a: // Service_WimpStarted
    if (workspace->wimp_handle == -1) {
      workspace->wimp_handle = 0;
    }
    break;
  }
}

void __attribute__(( naked )) service_call()
{
  asm ( "teq r1, #0x49" // Service_StartWimp
    "\n  teqne r1, #0x4a" // Service_StartedWimp
    "\n  movne pc, lr" );

  asm ( 
    "\n  push { "C_CLOBBERED", lr }"
    "\n  mov r0, sp"
    "\n  mov r1, r12 // workspace"
    "\n  bl start_wimp"
    "\n  pop { "C_CLOBBERED", pc }" );
}

