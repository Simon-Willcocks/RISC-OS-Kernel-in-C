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


bool Kernel_Error_UnknownSWI( svc_registers *regs )
{
  static error_block error = { 0x1e6, "Unknown SWI" }; // Could be "SWI name not known", or "SWI &3333 not known"
  regs->r[0] = (uint32_t) &error;
  return false;
}

bool Kernel_Error_UnimplementedSWI( svc_registers *regs )
{
  static error_block error = { 0x999, "Unimplemented SWI" };
  regs->r[0] = (uint32_t) &error;

  WriteS( "Unimplemented SWI" ); NewLine;
  return false;
}

bool Kernel_Error_TooManyDevicePages( svc_registers *regs )
{
  static error_block error = { 0x555, "Too many device pages have been requested" }; // FIXME allocated number
  regs->r[0] = (uint32_t) &error;
  return false;
}

bool Kernel_Error_NonMatchingDevicePagingRequest( svc_registers *regs )
{
  static error_block error = { 0x555, "The device memory has been previously assigned, but with a different size" }; // FIXME allocated number
  regs->r[0] = (uint32_t) &error;
  return false;
}

bool Kernel_Error_BufferOverflow( svc_registers *regs )
{
  static error_block error = { 0x1e4, "Buffer overflow" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static uint32_t word_align( void *p )
{
  return (((uint32_t) p) + 3) & ~3;
}

static void run_interruptable_swi( uint32_t regs_ptr, uint32_t svc )
{
  extern uint32_t JTABLE;
  uint32_t *jtable = &JTABLE;
  svc_registers *regs = (void*) regs_ptr;

  register uint32_t non_kernel_code asm( "r10" ) = jtable[svc];
  register uint32_t swi asm( "r11" ) = svc;
  register svc_registers *regs_in asm( "r12" ) = regs;
  register uint32_t result asm( "r0" );

  // Legacy kernel SWI functions expect the flags to be stored in lr
  // and the return address on the stack.

  asm volatile (
      "\n  push { r12 }"
      "\n  ldm r12, { r0-r9 }"
      "\n  adr lr, return_from_legacy_swi"
      "\n  push { lr } // return address, popped by SLVK"

      // Which SWIs use flags in r12 for input?
      "\n  ldr r12, [r12, %[spsr]]"
      "\n  bic lr, r12, #(1 << 28) // Clear V flags leaving original flags in r12"

      "\n  bx r10"
      "\nreturn_from_legacy_swi:"
      "\n  cpsid i // FIXME: is this necessary, are SWIs required to restore interrupt state?"
      "\n  pop { r12 } // regs"
      "\n  stm r12, { r0-r9 }"
      "\n  ldr r0, [r12, %[spsr]]"
      "\n  bic r0, #0xf0000000"
      "\n  and r2, lr, #0xf0000000"
      "\n  orr r0, r0, r2"
      "\n  str r0, [r12, %[spsr]]"
      : "=r" (result)
      : "r" (regs_in)
      , "r" (non_kernel_code)
      , [spsr] "i" (4 * (&regs->spsr - &regs->r[0]))
      , "r" (swi)
      : "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "lr", "memory" );
}

// This routine is for SWIs implemented in the legacy kernel, 0-511, not in
// modules, in ROM or elsewhere. (i.e. routines that return using SLVK.)
// TODO: Have a module flag to indicate its SWIs don't enable interrupts.
bool run_risos_code_implementing_swi( svc_registers *regs, uint32_t svc )
{
  TempTaskDo2( run_interruptable_swi, (uint32_t) regs, svc );

  return (regs->spsr & VF) == 0;
}

static bool do_OS_WriteS( svc_registers *regs )
{
  char *s = (void*) regs->lr;
  uint32_t r0 = regs->r[0];
  bool result = true;
  uint32_t *old_sp;
  asm volatile ( "mov %[sp], sp" : [sp] "=r" (old_sp) );

  while (*s != '\0') {
    regs->r[0] = *s++;
    // We have to work through the whole string, or returning an error is meaningless
    if (!do_OS_WriteC( regs )) {
      result = false;
      r0 = regs->r[0];
    }
  }

  uint32_t *new_sp;
  asm volatile ( "mov %[sp], sp" : [sp] "=r" (new_sp) );
  if (new_sp != old_sp) {
    asm ( "bkpt 14" );
  }
  regs->lr = word_align( s );
  regs->r[0] = r0;

  return result;
}

static bool do_OS_Write0( svc_registers *regs )
{
  const char *s = (void*) regs->r[0];
  bool result = true;

  while (*s != '\0' && result) {
    regs->r[0] = *s++;
    result = do_OS_WriteC( regs );
  }
  if (result) {
    regs->r[0] = (uint32_t) s+1;
  }

  return result;
}

static bool do_OS_NewLine( svc_registers *regs )
{
  bool result;
  uint32_t r0 = regs->r[0];
  regs->r[0] = '\r';
  result = do_OS_WriteC( regs );
  if (result) {
    regs->r[0] = '\n';
    result = do_OS_WriteC( regs );
  }
  if (result) {
    regs->r[0] = r0;
  }

  return result;
}

static bool do_OS_WriteN( svc_registers *regs )
{
  const char *string = (void*) regs->r[0];
  int n = regs->r[1];

  bool result = true;
  for (int i = 0; i < n && result; i++) {
    regs->r[0] = string[i];
    result = do_OS_WriteC( regs );
  }

  if (result) {
    regs->r[0] = (uint32_t) string;
  }
  regs->r[1] = n;

  return result;
}


static bool do_OS_Control( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_SetEnv( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_IntOn( svc_registers *regs )
{
  //Write0( __func__ ); NewLine;
  regs->spsr = (regs->spsr & ~0x80);
  return true;
}

static bool do_OS_IntOff( svc_registers *regs )
{
  //Write0( __func__ ); NewLine;
  regs->spsr = (regs->spsr & ~0x80) | 0x80;
  return true;
}

static bool do_OS_CallBack( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_EnterOS( svc_registers *regs )
{
  //Write0( __func__ ); NewLine;
  regs->spsr = (regs->spsr & ~15) | 0x1f; // System state: using sp_usr and lr_usr
  return true;
}

static bool do_OS_LeaveOS( svc_registers *regs )
{
  // Write0( __func__ ); NewLine;
  regs->spsr = (regs->spsr & ~0xf);
  return true;
}

static bool do_OS_BreakPt( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_BreakCtrl( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_UnusedSWI( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_UpdateMEMC( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SetCallBack( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReadUnsigned( svc_registers *regs )
{
  uint32_t base = regs->r[0] & 0x7f;
  if (base < 2 || base > 36) base = 10; // Can this really be a default?

WriteS( "Task " ); WriteNum( workspace.task_slot.running ); NewLine;

  bool maybe_reading_base = true;
  uint32_t result = 0;

  uint32_t limit = 0xffffffff;
  if (0 != (regs->r[0] & (1 << 29)))
    limit = regs->r[2];
  else if (0 != (regs->r[0] & (1 << 30)))
    limit = 0xff;

  const char *c = (const char*) regs->r[1];
  const char *first_character = c;

  if (*c == '&') {
    base = 16;
    c++;
    first_character = c; // First character of the number part
    maybe_reading_base = false;
  }
  for (;;) {
    for (;;) {
      unsigned char d = *c;
      int n;
      switch (d) {
      case '0' ... '9': n = d - '0'; break;
      case 'a' ... 'z': n = d - 'a' + 10; break;
      case 'A' ... 'Z': n = d - 'A' + 10; break;
      default: n = -1;
      }
      if (n >= base) break;
      if (n == -1) break;
      uint32_t new_value = (result * base) + n;
      if (new_value < result) { // overflow
        static const error_block error = { 0x16b, "Bad number" };
        regs->r[0] = (uint32_t) &error;
        return false;
      }
      if (new_value > limit) {
        static const error_block error = { 0x16c, "Number too big" };
        regs->r[0] = (uint32_t) &error;
        return false;
      }
      result = new_value;
      c++;
    }

    if (*c == '_' && maybe_reading_base) {
      if (result >= 2 && result <= 36) {
        maybe_reading_base = false;
        base = result;
        result = 0;
        c++;
        first_character = c; // Fail if there's no number
      }
      else {
        static const error_block error = { 0x16a, "Bad base" };
        regs->r[0] = (uint32_t) &error;
        return false;
      }
    }
    else {
      break;
    }
  }

  if ((0 != (regs->r[0] & (1 << 31)) && *c >= ' ')
   || (c == first_character)) {
    static const error_block error = { 0x16b, "Bad number" };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

  regs->r[1] = (uint32_t) c;
  regs->r[2] = result;

  return true;
}

#if 0
static bool gs_space_is_terminator( uint32_t flags )
{
  return (0 != (flags & (1 << 29)));
}

static bool gs_quotes_are_special( uint32_t flags )
{
  return (0 == (flags & (1 << 31)));
}

static bool gs_translate_control_codes( uint32_t flags )
{
  return (0 == (flags & (1 << 30)));
}

static bool terminator( char c, uint32_t flags )
{
  switch (c) {
  case   0: return true;
  case  10: return true;
  case  13: return true;
  case ' ': return gs_space_is_terminator( flags );
  default: return false;
  }
}

static bool do_OS_GSInit( svc_registers *regs )
{
  regs->spsr &= ~CF;
  const char *string = (void*) regs->r[0];
  uint32_t flags = regs->r[2];
  while (!terminator( *string, flags ) && *string == ' ') {
    string++;
  }
  regs->r[0] = (uint32_t) string;
  regs->r[1] = *string;
  regs->r[2] = flags & 0xe0000000;
  return true;
}

static bool Error_BadString( svc_registers *regs )
{
  static error_block error = { 0xfd, "String not recognised" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool do_OS_GSRead( svc_registers *regs )
{
  // This routine is entered with two registers, r0 and r2, containging
  // the result of the previous OS_GSRead or initial OS_GSInit.
  // There is necessarily some nesting of calls, because variables may
  // contain references to other variables, which may, in turn, do the same.

  // The kernel workspace contains a stack to accommodate this.

  // God knows what would happen if a code variable started using these routines.

  // "I am currently processing a string (next character pointed to by r0)"
  // "I am currently processing a variable, from a parent string (the character after the closing >)."
  // Numbers are one character, decoded in a single call.

  // r2 = flags, type, digit, parent
  // r0 = pointer
  uint32_t flags = regs->r[2] & 0xe0000000;
  //const char *var = regs->r[2] & ~0xe0000000;
  const char *string = (void*) regs->r[0];

  bool set_top_bit = false;
  char c;
  do {
    c = *string++;
    if (c == '"' && gs_quotes_are_special( flags )) {
      // Remove double quotes. Should I check for pairs? Do <> values in quotes not get expanded? FIXME
      c = *string++;
    }
    if (c == '|' && gs_translate_control_codes( flags )) {
      c = *string++;
      if (terminator( c, flags )) {
        return Error_BadString( regs );
      }
      else if (c >= '@' && c <= 'Z') c -= '@';
      else if (c >= 'a' && c <= 'z') c -= '`';
      else if (c == '?') c = 127;
      else if (c == '!') set_top_bit = true;
      else if (c == '[' || c == '{') c = 27;
      else if (c == '\\') c = 28;
      else if (c == ']' || c == '}') c = 29;
      else if (c == '^' || c == '~') c = 30;
      else if (c == '_' || c == '`') c = 31;
    }
    else if (c == '<') {
      for (;;) { asm ( "bkpt 77\nwfi" ); }
    }
  } while (set_top_bit);

  if (set_top_bit) c = c | 0x80;

  regs->r[1] = c;

  if (terminator( c, flags )) {
    regs->spsr |= CF;
  }
  regs->r[0] = (uint32_t) string;
  return true;
}

bool do_OS_GSTrans( svc_registers *regs )
{
  char *buffer = (void*) regs->r[1];
  uint32_t maxsize = regs->r[2] & ~0xe0000000;
  uint32_t translated = 0;
  regs->r[2] &= 0xe0000000;     // flags
  bool result = do_OS_GSInit( regs );
  while (result && 0 == (regs->spsr & CF)) {
    result = do_OS_GSRead( regs );

    if (result && buffer != 0 && translated < maxsize) {
      buffer[translated] = regs->r[1];
    }

    translated++;
  }

  regs->r[1] = (uint32_t) buffer;
  regs->r[2] = translated;

  if (translated >= maxsize) {
    regs->spsr |=  CF;
    translated = maxsize;
  }
  else
    regs->spsr &= ~CF;

  return result;
}
#else

// GSInit, from Kernel/s/Arthur2 can be found using 'push\>[^\n]*\n[^\n]*ldrb\tr1, \[r0], #1[^\n]*\n[^\n]*cmp'
// fc0206c4
// GSRead, 'bne\>[^\n]*\n[^\n]*ldrb\tr1, \[r0], #1[^\n]*\n[^\n]*cmp' (then look back to the cpsie before the bic
// fc02073c
// GSTrans, the following 'bic\tlr, lr, #.*0x20000000'
// fc020a50

bool do_OS_GSTrans( svc_registers *regs )
{
  WriteNum( regs->r[0] ); WriteNum( regs->lr );
  WriteS( "GSTrans (in) \"" ); Write0( (char*) regs->r[0] ); WriteS( "\"\n\r" );
  bool result = run_risos_code_implementing_swi( regs, OS_GSTrans );
  WriteS( "GSTrans (out) \"" );
  if (regs->r[1] != 0) {
    Write0( (char*) regs->r[1] );
  } else {
    WriteS( "NULL" );
  }
  WriteS( "\"\n\r" );
  return result;
}

// They access memory around faff3364, as do a number of modules.
// See hack in ./memory/simple/memory_manager.c
// Kernel/Docs/HAL/Notes has a memory map:
/*
00000000 16K        Kernel workspace
00004000 16K        Scratch space
00008000 Mem-32K    Application memory
0xxxxxxx 3840M-Mem  Dynamic areas
F0000000 160M       I/O space (growing downwards if necessary)
FA000000 1M         HAL workspace
FA100000 8K         IRQ stack
FA200000 32K        SVC stack
FA300000 8K         ABT stack
FA400000 8K         UND stack
FAE00000 1M         Reserved for physical memory accesses
FAF00000 256k       reserved for DCache cleaner address space (eg. StrongARM)
FAF40000 64k        kernel buffers (for long command lines, size defined by KbuffsMaxSize)
FAFE8000 32K        HAL workspace
FAFF0000 32K        "Cursor/System/Sound" block (probably becoming just "System")
FAFF8000 32K        "Nowhere"
FB000000 4M         L2PT
FB400000 16K        L1PT
FB404000 4M-16K     System heap
FB800000 8M         Soft CAM
FC000000 64M        ROM
*/


#endif

//static bool do_OS_BinaryToDecimal( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReadEscapeState( svc_registers *regs )
{
  // This can be called from interrupt routines, should probably make it more urgent.
  regs->spsr &= ~(1 << 29); // Clear CC, no escape FIXME
  return true;
}

//static bool do_OS_EvaluateExpression( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
//static bool do_OS_ReadPalette( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ValidateAddress( svc_registers *regs )
{
  // FIXME (not all memory checks are going to pass!)
  regs->spsr &= ~CF;
  return true;
}

// Ticker events

// Future possibility: Store the TaskSlot associated with the callback
// (transient callbacks, too), and swap it in and out again as needed.
static ticker_event *allocate_ticker_event()
{
  ticker_event *result = workspace.kernel.ticker_event_pool;
  if (result == 0) {
    result = rma_allocate( sizeof( transient_callback ) );
  }
  else {
    workspace.kernel.ticker_event_pool = result->next;
  }
  return result;
}

static void find_place_in_queue( ticker_event *new )
{
  ticker_event **queue = &workspace.kernel.ticker_queue;
  while (*queue != 0 && (*queue)->remaining >= new->remaining) {
    new->remaining -= (*queue)->remaining;
    queue = &(*queue)->next;
  }
  new->next = (*queue)->next;
  *queue = new;
}

static void run_handler( uint32_t code, uint32_t private )
{
  // Very trustingly, run module code
  register uint32_t p asm ( "r12" ) = private;
  register uint32_t c asm ( "r14" ) = code;
  asm volatile ( "blx r14" : : "r" (p), "r" (c) : "cc", "memory" );
}

static void run_ticker_events()
{
  asm ( "push { r0-r12, r14 }" );
  while (workspace.kernel.ticker_queue->remaining == 0) {
    ticker_event *e = workspace.kernel.ticker_queue;
    workspace.kernel.ticker_queue = e->next;
    run_handler( e->code, e->private_word );
    if (e->reload != 0) {
      e->remaining = e->reload;
      find_place_in_queue( e );
    }
    else {
      e->next = workspace.kernel.ticker_event_pool;
      workspace.kernel.ticker_event_pool = e;
    }
  }
  asm ( "pop { r0-r12, pc }" );
}

static void __attribute__(( naked )) TickerV_handler();

static void release_TickerV()
{
  register uint32_t vector asm( "r0" ) = 0x1c;
  register void *code asm( "r1" ) = TickerV_handler;
  register uint32_t private asm( "r2" ) = 0;
  // Private word not used
  asm ( "svc %[swi]"
           :
           : [swi] "i" (OS_Release | 0x20000)
           , "r" (vector)
           , "r" (code)
           , "r" (private)
           : "lr", "cc", "memory" );
}

static void claim_TickerV()
{
  register uint32_t vector asm( "r0" ) = 0x1c;
  register void *code asm( "r1" ) = TickerV_handler;
  register uint32_t private asm( "r2" ) = 0;
  // Private word not used
  asm ( "svc %[swi]"
           :
           : [swi] "i" (OS_Claim | 0x20000)
           , "r" (vector)
           , "r" (code)
           , "r" (private) 
           : "lr", "cc", "memory" );
}

static void __attribute__(( noinline )) C_TickerV_handler()
{
  if (workspace.kernel.ticker_queue != 0) {
    workspace.kernel.ticker_queue->remaining--;
    if (workspace.kernel.ticker_queue->remaining == 0) {
      run_ticker_events();
    }
  }

  if (workspace.kernel.ticker_queue == 0) {
    release_TickerV();
  }
}

static void __attribute__(( naked )) TickerV_handler()
{
  // C will ensure the callee saved registers are preserved.
  // We don't care about the private word.
  asm ( "push { "C_CLOBBERED", lr }" ); // Not intercepting vector, so storing return address
  C_TickerV_handler();
  asm ( "pop { "C_CLOBBERED", pc }" );
}

static bool insert_into_timer_queue( uint32_t code, uint32_t private, uint32_t timeout, uint32_t reload )
{
  if (workspace.kernel.ticker_queue == 0) {
    claim_TickerV();
  }

  ticker_event *new = allocate_ticker_event();
  if (new == 0)
    return false;

  new->remaining = timeout;
  new->reload = reload;
  new->code = code;
  new->private_word = private;

  find_place_in_queue( new );

  return true;
}

static bool do_OS_CallAfter( svc_registers *regs ) 
{
  if (!insert_into_timer_queue( regs->r[1], regs->r[2], regs->r[0], 0 ))
    return error_nomem( regs );
  return true;
}

static bool do_OS_CallEvery( svc_registers *regs )
{
  if (!insert_into_timer_queue( regs->r[1], regs->r[2], regs->r[0], regs->r[0] ))
    return error_nomem( regs );
  return true;
}

static bool do_OS_RemoveTickerEvent( svc_registers *regs )
{
  ticker_event **queue = &workspace.kernel.ticker_queue;

  uint32_t code = regs->r[0];
  uint32_t private_word = regs->r[1];

  while (*queue != 0) {
    ticker_event *e = *queue;
    if (e->code == code && e->private_word == private_word) {
      if (e->next != 0) {
        e->next->remaining += e->remaining;
      }
      *queue = e->next;
      e->next = workspace.kernel.ticker_event_pool;
      workspace.kernel.ticker_event_pool = e;
      break;
    }
    queue = &e->next;
  }

  if (workspace.kernel.ticker_queue == 0) {
    release_TickerV();
  }

  return true;
}

mode_selector_block const only_one_mode = { .mode_selector_flags = 1, .xres = only_one_mode_xres, .yres = only_one_mode_yres, .log2bpp = 5, .frame_rate = 60, { { -1, 0 } } };


static bool do_OS_InstallKeyHandler( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_CheckModeValid( svc_registers *regs )
{
  if (regs->r[0] != (uint32_t) &only_one_mode) {
    regs->spsr |= CF;
    regs->r[0] = -1;
    regs->r[1] = (uint32_t) &only_one_mode;
  }
  else {
    regs->spsr &= ~CF;
  }
  return true;
}


static bool do_OS_ClaimScreenMemory( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_MSTime( svc_registers *regs )
{
  uint32_t lo;
  uint32_t hi;
  uint64_t time;
  asm ( "mrrc p15, 0, %[lo], %[hi], c14" : [lo] "=r" (lo), [hi] "=r" (hi) );
  time = (((uint64_t) hi) << 32) | lo;
  regs->r[0] = lo >> 10; // FIXME: Inaccurate, but doesn't need __aeabi_uldivmod
  // Optimiser wants a function for uint64_t / uint32_t : __aeabi_uldivmod
  return true;
}

static bool do_OS_ReadMonotonicTime( svc_registers *regs )
{
  uint32_t lo;
  uint32_t hi;
  uint64_t time;
  asm ( "mrrc p15, 0, %[lo], %[hi], c14" : [lo] "=r" (lo), [hi] "=r" (hi) );
  time = (((uint64_t) hi) << 32) | lo;
  regs->r[0] = time >> 16; // FIXME completely made up, just to make sure qemu supports it!
  // Optimiser wants a function for uint64_t / uint32_t : __aeabi_uldivmod
  return true;
}

static bool do_OS_SubstituteArgs( svc_registers *regs )
{
  // The implementation in the RISC OS source doesn't pass on the flag
  uint32_t r0 = regs->r[0];
  uint32_t r5 = regs->r[5];
  regs->r[5] = regs->r[0] & 0x80000000;
  regs->r[0] = regs->r[0] & ~0x80000000;
  bool result = do_OS_SubstituteArgs32( regs );
  if (result) {
    regs->r[0] = r0;
  }
  regs->r[5] = r5;
  return result;
}

static bool do_OS_PrettyPrint( svc_registers *regs )
{
  const char *s = (void*) regs->r[0];
  const char *dictionary = (void*) regs->r[1];
  if (dictionary == 0) {
    static const char internal[] =
        "Syntax: *\x1b\0"; // FIXME
    dictionary = internal;
  }

  uint32_t r0 = regs->r[0];
  bool result = true;

  while (*s != '\0' && result) {
    if (*s == '\x1b') {
      s++;
      regs->r[0] = (uint32_t) "PrettyPrint needs implementing";
      result = do_OS_WriteS( regs );
    }
    else {
      regs->r[0] = *s++;
      result = do_OS_WriteC( regs );
    }
  }

  if (result) regs->r[0] = r0;

  return result;
}

static bool do_OS_WriteEnv( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadArgs( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadRAMFsLimits( svc_registers *regs )
{
  regs->r[0] = 5;
  if (do_OS_ReadDynamicArea( regs )) {
    regs->r[1] = regs->r[1] + regs->r[1] + 1;
    return true;
  }
  else {
    return false;
  }
}

static bool do_OS_ClaimDeviceVector( svc_registers *regs )
{
Write0( __func__ ); Space; WriteNum( regs->r[0] ); Space; WriteNum( regs->lr ); NewLine;
  asm ( "bkpt 1" );
  // TODO Emulate the traditional mechanism by creating a Task that will call the desired
  // vector.

  uint32_t device = regs->r[0];
  void (*code) = (void*) regs->r[1];
  uint32_t r12 = regs->r[2];

  if (device == 8 || device == 13) {
    // No expansion cards supported, whoever ports this to RiscPC (or wants to use this mechanism for USB?) can fix it.
    return Kernel_Error_UnimplementedSWI( regs );
  }

  // FIXME workspace or shared?
  if (device > number_of( workspace.interrupts.handlers )) {
    // FIXME Proper error
    return Kernel_Error_UnimplementedSWI( regs );
  }

  InterruptHandler *h = &workspace.interrupts.handlers[device];

  error_block const *err = 0;
  bool reclaimed = claim_lock( &workspace.interrupts.lock );
  assert( !reclaimed );
  if (h->code != 0) {
    static const error_block error = { 0x999, "Device already claimed" };
    err = &error;
  }
  else {
    h->code = code;
    h->r12 = r12;
    h->slot = TaskSlot_now();
  }
  release_lock( &workspace.interrupts.lock );

  if (err != 0) {
    regs->r[0] = (uint32_t) err;
    return false;
  }

  return true;
}

static bool do_OS_ReleaseDeviceVector( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReadMemMapInfo( svc_registers *regs )
{
  regs->r[0] = 4096;
  regs->r[1] = 64 << 8; // 256MiB FIXME Lying, but why is this being used?
  // Called from FontManager Init routine, which is only interested in the page size.
  return true;
}

static bool do_OS_ReadMemMapEntries( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SetMemMapEntries( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_AddCallBack( svc_registers *regs )
{
#ifdef DEBUG__SHOW_TRANSIENT_CALLBACKS
  WriteS( "New transient callback: " ); WriteNum( regs->r[0] ); WriteS( ", " ); WriteNum( regs->r[1] ); NewLine;
#endif
  transient_callback *callback = workspace.kernel.transient_callbacks_pool;
  if (callback == 0) {
    callback = rma_allocate( sizeof( transient_callback ) );
  }
  else {
    workspace.kernel.transient_callbacks_pool = callback->next;
  }
  // Most recently requested gets called first, I don't know if that's right or not.
  callback->next = workspace.kernel.transient_callbacks;
  workspace.kernel.transient_callbacks = callback;
  callback->code = regs->r[0];
  callback->private_word = regs->r[1];
  return true;
}

// OS_ReadSysInfo 6 values
// Not all of these will be needed or supported.
enum OS_ReadSysInfo_6 {
OSRSI6_CamEntriesPointer                       = 0,
OSRSI6_MaxCamEntry                             = 1,
OSRSI6_PageFlags_Unavailable                   = 2,
OSRSI6_PhysRamTable                            = 3,
OSRSI6_ARMA_Cleaner_flipflop                   = 4, // Unused in HAL kernels
OSRSI6_TickNodeChain                           = 5,
OSRSI6_ROMModuleChain                          = 6,
OSRSI6_DAList                                  = 7,
OSRSI6_AppSpaceDANode                          = 8,
OSRSI6_Module_List                             = 9,
OSRSI6_ModuleSHT_Entries                       = 10,
OSRSI6_ModuleSWI_HashTab                       = 11,
OSRSI6_IOSystemType                            = 12,
OSRSI6_L1PT                                    = 13,
OSRSI6_L2PT                                    = 14,
OSRSI6_UNDSTK                                  = 15,
OSRSI6_SVCSTK                                  = 16,
OSRSI6_SysHeapStart                            = 17,

// These are used by ROL, but conflict with our allocations

OSRSI6_ROL_KernelMessagesBlock                 = 18,
OSRSI6_ROL_ErrorSemaphore                      = 19,
OSRSI6_ROL_MOSdictionary                       = 20,
OSRSI6_ROL_Timer_0_Latch_Value                 = 21,
OSRSI6_ROL_FastTickerV_Counts_Per_Second       = 22,
OSRSI6_ROL_VecPtrTab                           = 23,
OSRSI6_ROL_NVECTORS                            = 24,
OSRSI6_ROL_IRQSTK                              = 25,
OSRSI6_ROL_SWIDispatchTable                    = 26, // JTABLE-SWIRelocation?
OSRSI6_ROL_SWIBranchBack                       = 27, // DirtyBranch?

// Our allocations which conflict with the above

OSRSI6_Danger_SWIDispatchTable                 = 18, // JTABLE-SWIRelocation (Relocated base of OS SWI dispatch table)
OSRSI6_Danger_Devices                          = 19, // Relocated base of IRQ device head nodes
OSRSI6_Danger_DevicesEnd                       = 20, // Relocated end of IRQ device head nodes
OSRSI6_Danger_IRQSTK                           = 21,
OSRSI6_Danger_SoundWorkSpace                   = 22, // workspace (8K) and buffers (2*4K)
OSRSI6_Danger_IRQsema                          = 23,

// Safe versions of the danger allocations
// Only supported by OS 5.17+, so if backwards compatability is required code
// should (safely!) fall back on the danger versions

OSRSI6_SWIDispatchTable                        = 64, // JTABLE-SWIRelocation (Relocated base of OS SWI dispatch table)
OSRSI6_Devices                                 = 65, // Relocated base of IRQ device head nodes
OSRSI6_DevicesEnd                              = 66, // Relocated end of IRQ device head nodes
OSRSI6_IRQSTK                                  = 67,
OSRSI6_SoundWorkSpace                          = 68, // workspace (8K) and buffers (2*4K)
OSRSI6_IRQsema                                 = 69,

// New ROOL allocations

OSRSI6_DomainId                                = 70, // current Wimp task handle
OSRSI6_OSByteVars                              = 71, // OS_Byte vars (previously available via OS_Byte &A6/VarStart)
OSRSI6_FgEcfOraEor                             = 72,
OSRSI6_BgEcfOraEor                             = 73,
OSRSI6_DebuggerSpace                           = 74,
OSRSI6_DebuggerSpace_Size                      = 75,
OSRSI6_CannotReset                             = 76,
OSRSI6_MetroGnome                              = 77, // OS_ReadMonotonicTime
OSRSI6_CLibCounter                             = 78,
OSRSI6_RISCOSLibWord                           = 79,
OSRSI6_CLibWord                                = 80,
OSRSI6_FPEAnchor                               = 81,
OSRSI6_ESC_Status                              = 82,
OSRSI6_ECFYOffset                              = 83,
OSRSI6_ECFShift                                = 84,
OSRSI6_VecPtrTab                               = 85,
OSRSI6_NVECTORS                                = 86,
OSRSI6_CAMFormat                               = 87, // 0 = 8 bytes per entry, 1 = 16 bytes per entry
OSRSI6_ABTSTK                                  = 88,
OSRSI6_PhysRamtableFormat                      = 89  // 0 = addresses are in byte units, 1 = addresses are in 4KB units
};

extern uint32_t undef_stack_top;

// Testing. Is this read-only?
// I don't think so, we need to update MetroGnome, don't we? Still, this will do as the initial values.
// I just spent ages combing through code until I worked out that this was where the strange address came
// from. Make it more obvious.
static const uint32_t SysInfo[] = {
  [OSRSI6_CamEntriesPointer]                       = 0xbaad0000 | 0,
  [OSRSI6_MaxCamEntry]                             = 0xbaad0000 | 1,
  [OSRSI6_PageFlags_Unavailable]                   = 0xbaad0000 | 2,
  [OSRSI6_PhysRamTable]                            = 0xbaad0000 | 3,
  [OSRSI6_ARMA_Cleaner_flipflop]                   = 0xbaad0000 | 4, // Unused in HAL kernels
  [OSRSI6_TickNodeChain]                           = 0xbaad0000 | 5,
  [OSRSI6_ROMModuleChain]                          = 0xbaad0000 | 6,
  [OSRSI6_DAList]                                  = 0xbaad0000 | 7,
  [OSRSI6_AppSpaceDANode]                          = 0xbaad0000 | 8,
  [OSRSI6_Module_List]                             = 0xbaad0000 | 9,
  [OSRSI6_ModuleSHT_Entries]                       = 0xbaad0000 | 10,
  [OSRSI6_ModuleSWI_HashTab]                       = 0xbaad0000 | 11,
  [OSRSI6_IOSystemType]                            = 0xbaad0000 | 12,
  [OSRSI6_L1PT]                                    = 0xbaad0000 | 13,
  [OSRSI6_L2PT]                                    = 0xbaad0000 | 14,
  [OSRSI6_UNDSTK]                                  = (uint32_t) &undef_stack_top,
  [OSRSI6_SVCSTK]                                  = 0xbaad0000 | 0x73273273, // A trap! Why does FileSwitch need to know this?
        //sizeof( workspace.kernel.svc_stack ) + (uint32_t) &workspace.kernel.svc_stack,
  [OSRSI6_SysHeapStart]                            = 0xbaad0000 | 17,

// Safe versions of the danger allocations
// Only supported by OS 5.17+, so if backwards compatability is required code
// should (safely!) fall back on the danger versions

  [OSRSI6_SWIDispatchTable]                        = 0xbaad0000 | 64, // JTABLE-SWIRelocation (Relocated base of OS SWI dispatch table)
  [OSRSI6_Devices]                                 = 0xbaad0000 | 65, // Relocated base of IRQ device head nodes
  [OSRSI6_DevicesEnd]                              = 0xbaad0000 | 66, // Relocated end of IRQ device head nodes
  [OSRSI6_IRQSTK]                                  = 0xbaad0000 | 67,
  [OSRSI6_SoundWorkSpace]                          = 0xbaad0000 | 68, // workspace (8K) and buffers (2*4K)
  [OSRSI6_IRQsema]                                 = (uint32_t) &workspace.vectors.zp.IRQsema,

// New ROOL allocations

  [OSRSI6_DomainId]                                = (uint32_t) &workspace.vectors.zp.DomainId, // current Wimp task handle
  [OSRSI6_OSByteVars]                              = 0xbaad0000 | 71, // OS_Byte vars (previously available via OS_Byte &A6/VarStart)
  [OSRSI6_FgEcfOraEor]                             = (uint32_t) &workspace.vectors.zp.vdu_drivers.ws.FgEcfOraEor, // Used by SpriteExtend
  [OSRSI6_BgEcfOraEor]                             = (uint32_t) &workspace.vectors.zp.vdu_drivers.ws.BgEcfOraEor, // Used by SpriteExtend
  [OSRSI6_DebuggerSpace]                           = 0xbaad0000 | 74,
  [OSRSI6_DebuggerSpace_Size]                      = 0xbaad0000 | 75,
  [OSRSI6_CannotReset]                             = 0xbad00000 | 76, // Used by FileCore
  [OSRSI6_MetroGnome]                              = 0xbaad0000 | 77, // OS_ReadMonotonicTime
  [OSRSI6_CLibCounter]                             = (uint32_t) &workspace.vectors.zp.CLibCounter,
  [OSRSI6_RISCOSLibWord]                           = (uint32_t) &workspace.vectors.zp.RISCOSLibWord,
  [OSRSI6_CLibWord]                                = (uint32_t) &workspace.vectors.zp.CLibWord,
  [OSRSI6_FPEAnchor]                               = 0xbaad0000 | 81,
  [OSRSI6_ESC_Status]                              = 0xbaad0000 | 82,
  [OSRSI6_ECFYOffset]                              = (uint32_t) &workspace.vectors.zp.vdu_drivers.ws.ECFYOffset, // Used by SpriteExtend
  [OSRSI6_ECFShift]                                = (uint32_t) &workspace.vectors.zp.vdu_drivers.ws.ECFShift, // Used by SpriteExtend
  [OSRSI6_VecPtrTab]                               = 0xbaad0000 | 85,
  [OSRSI6_NVECTORS]                                = 0xbaad0000 | 86,
  [OSRSI6_CAMFormat]                               = 0xbaad0000 | 87, // 0 = 8 bytes per entry, 1 = 16 bytes per entry
  [OSRSI6_ABTSTK]                                  = 0xbaad0000 | 88,
  [OSRSI6_PhysRamtableFormat]                      = 0xbaad0000 | 89  // 0 = addresses are in byte units, 1 = addresses are in 4KB units
};

bool read_kernel_value( svc_registers *regs )
{
  static error_block error = { 0x333, "ReadSysInfo 6 unknown code" };

  if (regs->r[1] == 0) {
    // Single value, number in r2, result to r2
    regs->r[2] = SysInfo[regs->r[2]];

    // Fail early, fail hard! (Then make a note of what uses it and fix it here or there.)
    if ((regs->r[2] & 0xffff0000) == 0xbaad0000) asm ( "bkpt 1" );

    return true;
  }

  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool do_OS_ReadSysInfo( svc_registers *regs )
{
  static error_block error = { 0x1ec, "Unknown OS_ReadSysInfo call" };

  switch (regs->r[0]) {
  case 0:
    {
      regs->r[0] = 8 << 20; // FIXME
      return true;
    }
  case 1:
    {
      regs->r[0] = (uint32_t) &only_one_mode;
      regs->r[1] = 7;
      regs->r[2] = 0;
      return true;
    }
  case 6: return read_kernel_value( regs );
  case 8:
    {
      regs->r[0] = 5;
      regs->r[1] = 0x14; // Multiple processors supported, OS runs from RAM
      regs->r[2] = 0;
      return true;
    }
    break;

  default: { Write0( "OS_ReadSysInfo: " ); WriteNum( regs->r[0] ); NewLine; }
  }

  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool do_OS_Confirm( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_CRC( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_PrintChar( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ChangeRedirection( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_RemoveCallBack( svc_registers *regs )
{
  // This is not at all reentrant, and I'm not sure how you could make it so...
  transient_callback **cp = &workspace.kernel.transient_callbacks;
  while (*cp != 0 && ((*cp)->code != regs->r[0] || (*cp)->private_word != regs->r[1])) {
    cp = &(*cp)->next;
  }
  if ((*cp) != 0) {
    transient_callback *callback = (*cp);
    *cp = callback->next;
    callback->next = workspace.kernel.transient_callbacks_pool;
    workspace.kernel.transient_callbacks_pool = callback;
  }
  return true;
}


static bool do_OS_FindMemMapEntries( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

typedef union {
  struct {
    uint32_t action:3; // Set, OR, AND, EOR, Invert, Unchanged, AND NOT, OR NOT.
    uint32_t use_transparency:1;
    uint32_t background:1;
    uint32_t ECF_pattern:1; // Unlikely to be supported
    uint32_t text_colour:1; // As opposed to graphics colour
    uint32_t read_colour:1; // As opposed to setting it
  };
  uint32_t raw;
} OS_SetColour_Flags;

typedef union {
  struct {
    uint32_t r:8;
    uint32_t g:8;
    uint32_t b:8;
    uint32_t q:8; // Unused, I think
  };
  uint32_t raw;
} OS_SetColour_Colour; // &00BBGGRR

static void SetColours( uint32_t bpp, uint32_t fore, uint32_t back )
{
  switch (bpp) {
  }
}

static bool do_OS_SetColour( svc_registers *regs )
{
Write0( __func__ ); Space; WriteNum( regs->r[0] ); Space; WriteNum( regs->r[1] ); NewLine;

#ifdef DEBUG__EXAMINE_SET_COLOUR
  VduDriversWorkspace before = workspace.vectors.zp.vdu_drivers.ws;
  extern uint32_t *vduvarloc[];
  uint32_t bc = *vduvarloc[154 - 128];
#endif
  bool result = run_risos_code_implementing_swi( regs, OS_SetColour );

#ifdef DEBUG__EXAMINE_SET_COLOUR
  if (bc != *vduvarloc[154 - 128]) {
    WriteS( "GFCOL changed from " ); WriteNum( bc ); WriteS( " to " ); WriteNum( *vduvarloc[154 - 128] ); NewLine;
  }

  // Findings:
  // changes: VduDriverWorkSpace.ws.FgEcf, VduDriverWorkSpace.ws.FgEcfOraEor, VduDriverWorkSpace.ws.FgPattern

  uint32_t *pa = (void*) &workspace.vectors.zp.vdu_drivers.ws;
  uint32_t *pb = (void*) &before;
  while (pb - ((uint32_t*) &before) < sizeof( before ) / 4) {
    if (*pb != *pa) {
      WriteS( "Changed " ); WriteNum( pa ); WriteS( " from " ); WriteNum( *pb ); WriteS( " to " ); WriteNum( *pa ); NewLine;
    }
    pb ++;
    pa ++;
  }
#endif

#if 0
  OS_SetColour_Flags flags = { .raw = regs->r[0] };

  if (flags.read_colour) {
    if (flags.text_colour) {
      if (flags.background)
        regs->r[1] = workspace.vectors.zp.vdu_drivers.ws.TextBgColour;
      else
        regs->r[1] = workspace.vectors.zp.vdu_drivers.ws.TextFgColour;
    }
    else {
      uint32_t *pattern_data = (void*) regs->r[1];
      uint32_t *to_read;

      if (flags.background)
        to_read = &workspace.vectors.zp.vdu_drivers.ws.BgPattern[0];
      else
        to_read = &workspace.vectors.zp.vdu_drivers.ws.FgPattern[0];

      for (int i = 0; i < 8; i++) {
        pattern_data[i] = to_read[i];
      }
    }

    return true;
  }

  if (flags.text_colour) {
    if (flags.background)
      workspace.vectors.zp.vdu_drivers.ws.TextBgColour = regs->r[1];
    else
      workspace.vectors.zp.vdu_drivers.ws.TextFgColour = regs->r[1];

    return true;
  }

  extern uint32_t *vduvarloc[];

  EcfOraEor *ecf;
  uint32_t *pattern;

  if (flags.background) {
    workspace.vectors.zp.vdu_drivers.ws.GPLBMD = flags.action | 0x60;
    pattern = &workspace.vectors.zp.vdu_drivers.ws.BgPattern[0];
    ecf = &workspace.vectors.zp.vdu_drivers.ws.BgEcfOraEor;
    *vduvarloc[154 - 128] = regs->r[1];
  }
  else {
    workspace.vectors.zp.vdu_drivers.ws.GPLFMD = flags.action | 0x60;
    pattern = &workspace.vectors.zp.vdu_drivers.ws.FgPattern[0];
    ecf = &workspace.vectors.zp.vdu_drivers.ws.FgEcfOraEor;
    *vduvarloc[153 - 128] = regs->r[1];
  }

  if (flags.ECF_pattern) {
    uint32_t *new_pattern = (void*) regs->r[1];
    for (int i = 0; i < 8; i++) {
      pattern[i] = new_pattern[i];
    }
  }
  else {
    uint32_t colour = regs->r[1];
    uint32_t log2bpp = workspace.vectors.zp.vdu_drivers.ws.Log2BPP;
    uint32_t bits = 1 << log2bpp;
    uint32_t mask = (1 << bits) - 1;
    colour = colour & mask;
    while (bits != 32) {
      colour = colour | (colour << bits);
      mask = mask | (mask << bits);
      bits = bits * 2;
    }
    for (int i = 0; i < 8; i++) {
      pattern[i] = colour;
    }
  }

/* This is not done yet, but it's not what's stopping the Wimp_Initialise call from returning
  SetColour();
    for (int i = 0; i < number_of( ecf->line ); i++) {
      // orr + eor allows you to set bits from the original pixel, or clear ones that you've set by the orr.
      // orr => ignore these bits in the original pixel
      // eor => invert these bits, afterwards.
      ecf->line[i].orr = 0xffffffff;
      ecf->line[i].eor = ~regs->r[1];
    }
  }
*/
#endif
  return result;
}

static bool do_OS_Pointer( svc_registers *regs ) { Write0( __func__ ); NewLine; return true; }

static bool do_OS_ScreenMode( svc_registers *regs )
{
#ifdef DEBUG__SHOW_SCREEN_MODE_CALLS
  Write0( __func__ ); WriteNum( regs->r[0] ); NewLine;
#endif

  enum { SelectMode, CurrentModeSpecifier, EnumerateModes, SetMonitorType, ConfigureAcceleration, FlushScreenCache, ForceFlushCache,
  RegisterGraphicsVDriver = 64, StartGraphicsVDriver, StopGraphicsVDriver, DeregisterGraphicsVDriver, EnumerateGraphicsVDriver };

  switch (regs->r[0]) {
  case SelectMode: 
    if (regs->r[1] == (uint32_t) &only_one_mode) {
      return true;
    }
    else {
      return Kernel_Error_UnimplementedSWI( regs );
    }
  case CurrentModeSpecifier: 
    regs->r[1] = (uint32_t) &only_one_mode;
    return true;
  case EnumerateModes: 
    if (regs->r[6] == 0) {
      regs->r[7] = -(4 + sizeof( only_one_mode ));
      return true;
    }
    else {
      return Kernel_Error_UnimplementedSWI( regs );
    }
  case FlushScreenCache: asm ( "svc 0xff" : : : "lr" ); return true;

  case RegisterGraphicsVDriver: regs->r[0] = 1; return true;
  case StartGraphicsVDriver: return true;
  case StopGraphicsVDriver: return Kernel_Error_UnimplementedSWI( regs );
  case DeregisterGraphicsVDriver: return Kernel_Error_UnimplementedSWI( regs );
  case EnumerateGraphicsVDriver: return Kernel_Error_UnimplementedSWI( regs );
  default: return Kernel_Error_UnimplementedSWI( regs );
  }
}

static bool do_OS_ClaimProcessorVector( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_Reset( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_MMUControl( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ResyncTime( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_PlatformFeatures( svc_registers *regs )
{
  static error_block error = { 999, "Unknown PlatformFeature" };

  if (0 == regs->r[0]) {
    regs->r[0] = 0x80103ff9; // Good enough for SpriteExt module?
    return true;
  }
  else if (34 == regs->r[0]) {
    // FIXME: Make this a 64-bit bitmap from processor and extract the appropriate bit?
    regs->r[0] = 1; // Everything supported
    return true;
  }

  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool do_OS_AMBControl( svc_registers *regs ) { Write0( __func__ ); NewLine; asm ( "bkpt 1" ); return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_SpecialControl( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_EnterUSR32( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_EnterUSR26( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_VIDCDivider( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_NVMemory( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_Hardware( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  WriteNum( regs->r[8] ); NewLine; // R8?!
  WriteNum( regs->r[9] ); NewLine; // R8?!
  return Kernel_Error_UnimplementedSWI( regs );
}

static bool do_OS_IICOp( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadLine32( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool terminator( char c )
{
  return c == 13 || c == 10 || c == 0;
}

bool do_OS_SubstituteArgs32( svc_registers *regs )
{
  // Re-write in C of RISC OS code, simply commenting out the following line results in "SWI &7e not known"
  // [OS_SubstituteArgs32] = do_OS_SubstituteArgs32

  char const *args = (void*) regs->r[0];
  bool append_remaining_args = 0 == (regs->r[5] & 0x80000000);

  char const *start[11]; // 0-9 + rest of line
  char const *end[11]; // 0-9 + rest of line

  for (int parameter = 0; parameter < 11; parameter++) {
    // Skip intermediate spaces
    while (' ' == *args) { args++; }

    start[parameter] = args;

    char c = *args;

    if (c == '"') {
      while (!terminator( c = *args )) {
        args++;
        if (c == '"') {
          if (*args == '"') args++; else break;
        }
      }
      if (c != '"') {
        asm ( "bkpt 1" ); // Mismatched quote
      }
      args++; // Include the '"'
    }
    else if (parameter < 10) {
      while (!terminator( c = *args ) && c != ' ') {
        args++;
        if (c == '"') {
          if (*args == '"') args++; else break;
        }
      }
    }
    else {
      while (!terminator( c )) {
        c = *++args;
      } 
    }

    end[parameter] = args;
  }

  char *buffer = (void*) regs->r[1];
  int length = regs->r[2];
  char const *template = (void*) regs->r[3];
  int template_length = regs->r[4];

  int highest = 0;

  char const *t = template;
  char const *template_end = template + template_length;

  char *d = buffer;
  char *end_of_buffer = buffer + length - 1; // Always allow for terminator

  while (t < template_end && d < end_of_buffer) {
    char c = *t++;
    if (c == '%') {
      bool all_from = *t == '*';
      if (all_from) t++;
      if (*t >= '0' && *t <= '9') {
        int p = *t++ - '0';
        if (p > highest) highest = p;
        char const *a = start[p];
        char const *e = end[p];
        if (all_from) {
          highest = 10;
          e = end[10];
        }

        while (d < end_of_buffer && a < e) {
          *d++ = *a++;
        }

        if (a < e) {
          break; // Buffer overflow
        }
        continue;
      }
      if (all_from) t--; // %*X where X is not a digit, go back to the *
    }
    *d++ = c;
  }

  if (append_remaining_args && highest < 10) {
    char const *a = start[highest+1];
    char const *e = end[10];
    while (d < end_of_buffer && a < e) {
      *d++ = *a++;
    }
  }

  if (d == end_of_buffer) {
    return Kernel_Error_BufferOverflow( regs );
  }

  *d++ = '\0'; // Terminator

  regs->r[2] = d - buffer;

  return true;
}

// static bool do_OS_HeapSort32( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_SynchroniseCodeAreas( svc_registers *regs )
{
  // WriteS( "OS_SynchroniseCodeAreas" );

  // FIXME: too much?
  clean_cache_to_PoC();
  clean_cache_to_PoU();
  asm volatile ( "isb sy" );

  return true;
}

static bool buffer_too_small(  svc_registers *regs )
{
  static error_block error = { 0x1e4, "Buffer overflow" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool write_converted_character( svc_registers *regs, char c )
{
  *((char *) regs->r[1]++) = c;
  regs->r[2] --;
  if (regs->r[2] == 0) return buffer_too_small( regs );
  return true;
}

// This is a lot of work for little gain, and could be fixed by a Convert module, which can use existing code.
static bool do_OS_ConvertStandardDateAndTime( svc_registers *regs )
{
  for (const char *p = "No ConvertStandardDateAndTime"; *p != 0; p++) {
    if (!write_converted_character( regs, *p )) return false;
  }
  if (!write_converted_character( regs, '\0' )) return false;
  return true;
}

static bool do_OS_ConvertDateAndTime( svc_registers *regs )
{
  for (const char *p = "No ConvertDateAndTime"; *p != 0; p++) {
    if (!write_converted_character( regs, *p )) return false;
  }
  if (!write_converted_character( regs, '\0' )) return false;
  return true;
}

const char hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

static bool hex_convert( svc_registers *regs, int digits )
{
  uint32_t n = regs->r[0];

  regs->r[0] = regs->r[1];

  for (int i = digits-1; i >= 0; i--) {
    if (!write_converted_character( regs, hex[(n >> (4*i))&0xf] )) return false;
  }

  return write_converted_character( regs, '\0' );
}

static bool do_OS_ConvertHex1( svc_registers *regs )
{
  return hex_convert( regs, 1 );
}

static bool do_OS_ConvertHex2( svc_registers *regs )
{
  return hex_convert( regs, 2 );
}

static bool do_OS_ConvertHex4( svc_registers *regs )
{
  return hex_convert( regs, 4 );
}

static bool do_OS_ConvertHex6( svc_registers *regs )
{
  return hex_convert( regs, 6 );
}

static bool do_OS_ConvertHex8( svc_registers *regs )
{
  return hex_convert( regs, 8 );
}

static bool recursive_convert_decimal( svc_registers *regs, uint32_t n )
{
  uint32_t d = n / 10;
  bool result = true;

  if (d > 0)
    result = recursive_convert_decimal( regs, d );

  if (result) {
    if (!write_converted_character( regs, '0' + n % 10 )) return false;
  }

  return result;
}

static bool convert_decimal( svc_registers *regs, uint32_t mask )
{
  uint32_t n = regs->r[0] & mask;
  regs->r[0] = regs->r[1];

  if (recursive_convert_decimal( regs, n )) {
    *(char*) regs->r[1] = '\0';
    return true;
  }

  return false;
}

static bool do_OS_ConvertCardinal1( svc_registers *regs )
{
  return convert_decimal( regs, 0xff );
}

static bool do_OS_ConvertCardinal2( svc_registers *regs )
{
  return convert_decimal( regs, 0xffff );
}

static bool do_OS_ConvertCardinal3( svc_registers *regs )
{
  return convert_decimal( regs, 0xffffff );
}

static bool do_OS_ConvertCardinal4( svc_registers *regs )
{
  return convert_decimal( regs, 0xffffffff );
}

static bool convert_signed_decimal( svc_registers *regs, uint32_t sign_bit )
{
  uint32_t n = regs->r[0] & (sign_bit - 1);

  if (0 != (regs->r[0] & sign_bit)) {
    if (!write_converted_character( regs, '-' )) return false;
    n = sign_bit - n;
  }

  return convert_decimal( regs, n );
}

static bool do_OS_ConvertInteger1( svc_registers *regs )
{
  return convert_signed_decimal( regs, (1 << 7) );
}

static bool do_OS_ConvertInteger2( svc_registers *regs )
{
  return convert_signed_decimal( regs, (1 << 15) );
}

static bool do_OS_ConvertInteger3( svc_registers *regs )
{
  return convert_signed_decimal( regs, (1 << 23) );
}

static bool do_OS_ConvertInteger4( svc_registers *regs )
{
  return convert_signed_decimal( regs, (1ul << 31) );
}


static bool do_OS_ConvertBinary1( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary2( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary3( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary4( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertSpacedCardinal1( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal2( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal3( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal4( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertSpacedInteger1( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger2( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger3( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger4( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertFixedNetStation( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertNetStation( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertFixedFileSize( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

static inline int32_t GraphicsWindow_ec_Left()
{
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  return (ws->GWLCol << ws->XEigFactor) + ws->OrgX;
}

static inline int32_t GraphicsWindow_ec_Bottom()
{
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  return (ws->GWBRow << ws->YEigFactor) + ws->OrgY;
}

static inline int32_t GraphicsWindow_ec_Right()
{
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  return (ws->GWRCol << ws->XEigFactor) + ws->OrgX;
}

static inline int32_t GraphicsWindow_ec_Top()
{
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  return (ws->GWTRow << ws->YEigFactor) + ws->OrgY;
}

static inline int32_t GraphicsWindow_ic_Left()
{
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  return ws->GWLCol;
}

static inline int32_t GraphicsWindow_ic_Bottom()
{
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  return ws->GWBRow;
}

static inline int32_t GraphicsWindow_ic_Right()
{
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  return ws->GWRCol;
}

static inline int32_t GraphicsWindow_ic_Top()
{
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  return ws->GWTRow;
}

static bool CLG( svc_registers *regs )
{
  // Was using Plot, but this is not allowed to affect the graphics cursor.
  // Ugly, ignores many aspects of colour management FIXME.
  // Good enough for only_one_mode

  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;

  int32_t x = GraphicsWindow_ic_Left();
  int32_t y = GraphicsWindow_ic_Top();

  uint32_t bg_colour = ws->BgEcfOraEor.line[0].eor ^ ws->BgEcfOraEor.line[0].orr;
  // Write0( "CLG" ); WriteNum( ws->BgEcfOraEor.line[0].orr ); Space; WriteNum( ws->BgEcfOraEor.line[0].eor ); NewLine;
  
  uint32_t left = ws->ScreenStart + (ws->YWindLimit - y) * ws->LineLength + (x << 2);

  int32_t rows = GraphicsWindow_ic_Top() - GraphicsWindow_ic_Bottom();

  for (; y > GraphicsWindow_ic_Bottom(); y--) {
    uint32_t *p = (uint32_t*) left;
    left += ws->LineLength;
    for (int xx = x; xx < GraphicsWindow_ic_Right(); xx++) {
      *p++ = bg_colour;
    }
  }

  return true;
}

static bool SetTextColour( svc_registers *regs )
{
  Write0( __func__ );
  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  asm ( "bkpt 1" );

  return true;
}

static bool SetGraphicsColour( svc_registers *regs )
{
  Write0( __func__ );
  asm ( "bkpt 1" );
  return true;
}

static bool SetPalette( svc_registers *regs )
{
  Write0( __func__ );
  return true;
}

static bool SetMode( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  uint8_t *p = (void*) regs->r[1];
  WriteNum( *p ); NewLine;
  return true;
}

static bool SetCursorMode( svc_registers *regs )
{
  Write0( __func__ );
  return true;
}

bool run_vector( svc_registers *regs, int vec );

static bool VDU23( svc_registers *regs )
{
  Write0( __func__ );
  uint8_t *params = (void*) regs->r[1];
  for (int i = 0; i < 9; i++) {
    WriteS( " " ); WriteNum( params[i] );
  }
  NewLine;

  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;

  switch (params[0]) {
  case 1: return SetCursorMode( regs );
  case 16: ws->CursorFlags = (ws->CursorFlags & 0xffffff00) | ((ws->CursorFlags & params[2])^params[3]); break;
  case 18 ... 25:
  case 28 ... 31:
    {
      uint32_t r0 = regs->r[0];

      regs->r[0] = params[0]; // The vector expects the code in r0 as well as the first parameter
      if (!run_vector( regs, 0x17 )) { // UKVDU23V
        return false;
      }

      regs->r[0] = r0;
    }
    break;
  case 32 ... 255: break; // Should redefine character. Wimp does 131, 132, 136-139
  default: break; // Do nothing
  }

  return true;
}

static int32_t int16_at( uint8_t *p )
{
  int32_t result = p[1];
  result = (result << 8) | p[0];
  return result;
}

static bool DefineGraphicsWindow( svc_registers *regs )
{
  uint8_t *params = (void*) regs->r[1];

  int32_t l = int16_at( &params[0] );
  int32_t b = int16_at( &params[2] );
  int32_t r = int16_at( &params[4] );
  int32_t t = int16_at( &params[6] );

  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  ws->GWLCol = l >> ws->XEigFactor;
  ws->GWBRow = b >> ws->YEigFactor;
  ws->GWRCol = r >> ws->XEigFactor;
  ws->GWTRow = t >> ws->YEigFactor;

  Write0( __func__ ); WriteS( " " ); WriteNum( l ); WriteS( ", " ); WriteNum( b ); WriteS( ", " ); WriteNum( r ); WriteS( ", " ); WriteNum( t ); NewLine;

  return true;
}

static bool Plot( svc_registers *regs )
{
  uint8_t *params = (void*) regs->r[1];

  uint8_t type = params[0];
  int32_t x = int16_at( &params[1] );
  int32_t y = int16_at( &params[3] );

  register uint32_t rt asm( "r0" ) = type;
  register uint32_t rx asm( "r1" ) = x;
  register uint32_t ry asm( "r2" ) = y;
  asm ( "svc %[swi]" : : [swi] "i" (0x20045), "r" (rt), "r" (rx), "r" (ry) : "lr", "cc", "memory" );

  // FIXME Handle errors!
  return true;
}

static bool RestoreDefaultWindows( svc_registers *regs )
{
  Write0( __func__ ); NewLine;

  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;

  ws->GWLCol = 0;
  ws->GWBRow = 0;
  ws->GWRCol = only_one_mode.xres-1; // Internal units.
  ws->GWTRow = only_one_mode.yres-1;

  ws->OrgX = 0;
  ws->OrgY = 0;

  return true;
}

static bool SetGraphicsOrigin( svc_registers *regs )
{
  uint8_t *params = (void*) regs->r[1];
  int32_t x = int16_at( &params[0] );
  int32_t y = int16_at( &params[2] );

  VduDriversWorkspace *ws = &workspace.vectors.zp.vdu_drivers.ws;
  ws->OrgX = x >> ws->XEigFactor;
  ws->OrgY = y >> ws->YEigFactor;

  return true;
}

static bool Bell()
{
  Write0( __func__ ); NewLine;
  return true;
}

/* This is a half-way house to having a per-thread graphics context
   approach. */
static bool do_OS_VduCommand( svc_registers *regs )
{
  // Always called with the right number of parameter bytes, honest!

  switch (regs->r[0]) {
  case 0: break; // do nothing, surely shouldn't be called
  case 1: WriteNum( regs->lr ); asm ( "bkpt 1" ); break; // Send next character to printer if enabled, ignore next char otherwise
  case 2: asm ( "bkpt 1" ); break; // "enable printer"
  case 3: return true; break; // do nothing, "disable printer"
  case 4: workspace.vectors.zp.vdu_drivers.ws.CursorFlags |= ~(1 << 30); return true;
  case 5: workspace.vectors.zp.vdu_drivers.ws.CursorFlags |= (1 << 30); return true;
  case 7: return Bell();
  case 16: return CLG( regs );
  case 17: return SetTextColour( regs );
  case 18: return SetGraphicsColour( regs );
  case 19: return SetPalette( regs );
  case 22: return SetMode( regs );
  case 23: return VDU23( regs );
  case 24: return DefineGraphicsWindow( regs );
  case 25: return Plot( regs );
  case 26: return RestoreDefaultWindows( regs );
  case 29: return SetGraphicsOrigin( regs );
  default:
    {
      static error_block error = { 0x111, "Unimplemented VDU code..." };
      Write0( error.desc ); WriteNum( regs->r[0] ); NewLine;
      regs->r[0] = (uint32_t) &error;
      return false;
    }
  }

  __builtin_unreachable();
}

static bool duplicate_page( uint32_t pa, uint32_t i )
{
  // This physical address has already been mapped (possibly by a different core, or even a different module)
  return (pa >> 12) == shared.memory.device_pages[i].page_number;
}

// FIXME Move to TaskSlot
static bool do_OS_ReleaseDMALock( svc_registers *regs )
{
  // On entry
  //    R0 = Virtual address that was locked
  //    R1 = Number of bytes that were locked

  // On exit
  //    R0,R1  preserved

  // Use
  //    For use by the HAL and other modules to communicate with devices that can directly access memory.
  //
  //    While there is a DMA lock in place in a TaskSlot, the memory manager will not be permitted to
  //    resize or rearrange the slot for efficiency.

  // FIXME FIXME FIXME Implement this! (TaskSlot related.)

  return true;
}

// FIXME Move to TaskSlot
// Or Pipes?
static bool do_OS_LockForDMA( svc_registers *regs )
{
  // On entry
  //    R0 = Virtual address to be locked
  //    R1 = Number of bytes to be locked

  // On exit
  //    R0 = Physical address of locked memory
  //    R1   preserved

  // Use
  //    For use by the HAL and other modules to communicate with devices that can directly access memory.
  //
  //    Virtual addresses will be allocated first come first served, multiple requests for the same physical
  //    address will receive the same virtual address, independent of the active core.

  // FIXME FIXME FIXME Needs a proper implementation! At the moment, the RMA is a single block of contiguous
  // memory, and that's the only area that will be used for this purpose until a proper implementation is put
  // in place for TaskSlots.
  // Check number of bytes to ensure it doesn't go over into another page.
  // Worst case for this is that a two (or four) megabyte block of memory will have to replace two smaller ones, and
  // all the memory copied from one to the other... Or we could just report an error, and they'll have to allocate
  // different areas of memory until one is practical.

  regs->r[0] = regs->r[0] - (uint32_t) &rma_heap + shared.memory.rma_memory;

  return true;
}

bool do_OS_MapDevicePages( svc_registers *regs )
{
  // On entry
  //    R0 = Physical address to be mapped, must be on a page boundary
  //    R1 = Number of pages to be mapped

  // On exit
  //    R0 = Virtual address of device memory, accessible only in privileged modes, on the current core
  //    R1   preserved

  // Use
  //    For use by the HAL and other modules to map devices to virtual addresses for use by drivers.
  //
  //    This should be used in the initialisation routine of a device driver module, it may be repeated for
  //    each core, if access is required from more than one core (it is up to the module to protect the device
  //    from simultaneous accesses).
  //
  //    Virtual addresses will be allocated first come first served, multiple requests for the same physical
  //    address will receive the same virtual address, independent of the active core.

  extern uint32_t devices; // Linker symbol

  bool reclaimed = claim_lock( &shared.memory.device_page_lock );
  assert( !reclaimed );
  uint32_t va = (uint32_t) &devices;
  uint32_t pa = regs->r[0];

  uint32_t i = 0;
  while (i < number_of( shared.memory.device_pages )
      && !duplicate_page( pa, i )
      && shared.memory.device_pages[i].pages != 0) {
    va += 4096 * shared.memory.device_pages[i].pages;
    i++;
  }

  if (i >= number_of( shared.memory.device_pages )) {
    release_lock( &shared.memory.device_page_lock );
    return Kernel_Error_TooManyDevicePages( regs );
  }

  if (duplicate_page( pa, i )) {
    if (shared.memory.device_pages[i].pages != regs->r[1]) {
      release_lock( &shared.memory.device_page_lock );
      return Kernel_Error_NonMatchingDevicePagingRequest( regs );
    }
  }
  else {
    shared.memory.device_pages[i].pages = regs->r[1];
    shared.memory.device_pages[i].page_number = pa >> 12;
  }

  MMU_map_device_at( (void*) va, pa, 4096 * shared.memory.device_pages[i].pages );

  regs->r[0] = va;

  release_lock( &shared.memory.device_page_lock );

  return true;
}

static bool do_OS_FlushCache( svc_registers *regs )
{
  bool reclaimed = claim_lock( &shared.mmu.lock );
  clean_cache_to_PoC(); // FIXME
  if (!reclaimed)
    release_lock( &shared.mmu.lock );
  return true;
}

static bool do_OS_ConvertFileSize( svc_registers *regs ) { Write0( __func__ ); NewLine; return Kernel_Error_UnimplementedSWI( regs ); }

bool do_OS_Heap( svc_registers *regs )
{
  // Note: This could possibly be improved by having a lock per heap,
  // one bit in the header, say.
  // I would hope this is never called from an interrupt handler, but 
  // if so, we should probably return an error, if shared.memory.os_heap_lock 
  // is non-zero. Masking interrupts is no longer a guarantee of atomicity.
  // OS_Heap appears to call itself, even without interrupts...
  bool reclaimed = claim_lock( &shared.memory.os_heap_lock );

  if (reclaimed) { WriteS( "OS_Heap, recursing: " ); WriteNum( regs->lr ); NewLine; }
  // assert( !reclaimed );

  bool result = run_risos_code_implementing_swi( regs, OS_Heap );
  //Write0( "OS_Heap returns " ); WriteNum( regs->r[3] ); NewLine;

  if (!reclaimed) release_lock( &shared.memory.os_heap_lock );
  return result;
}

typedef bool (*swifn)( svc_registers *regs );

static swifn os_swis[256] = {
  [OS_WriteC] =  do_OS_WriteC,
  [OS_WriteS] =  do_OS_WriteS,
  [OS_Write0] =  do_OS_Write0,
  [OS_NewLine] =  do_OS_NewLine,

  [OS_ReadC] =  do_OS_ReadC,
  [OS_CLI] =  do_OS_CLI,
  [OS_Byte] =  do_OS_Byte,
  [OS_Word] =  do_OS_Word,

  [OS_File] =  do_OS_File,
  [OS_Args] =  do_OS_Args,
  [OS_BGet] =  do_OS_BGet,
  [OS_BPut] =  do_OS_BPut,

  [OS_GBPB] =  do_OS_GBPB,
  [OS_Find] =  do_OS_Find,
  [OS_ReadLine] =  do_OS_ReadLine,
  [OS_Control] =  do_OS_Control,

  [OS_GetEnv] =  do_OS_GetEnv,
  [OS_Exit] =  do_OS_Exit,
  [OS_SetEnv] =  do_OS_SetEnv,
  [OS_IntOn] =  do_OS_IntOn,

  [OS_IntOff] =  do_OS_IntOff,
  [OS_CallBack] =  do_OS_CallBack,
  [OS_EnterOS] =  do_OS_EnterOS,
  [OS_BreakPt] =  do_OS_BreakPt,

  [OS_BreakCtrl] =  do_OS_BreakCtrl,
  [OS_UnusedSWI] =  do_OS_UnusedSWI,
  [OS_UpdateMEMC] =  do_OS_UpdateMEMC,
  [OS_SetCallBack] =  do_OS_SetCallBack,

  [OS_Mouse] =  do_OS_Mouse,
  [OS_Heap] =  do_OS_Heap,
  [OS_Module] =  do_OS_Module,
  [OS_Claim] =  do_OS_Claim,

  [OS_Release] =  do_OS_Release,
  [OS_ReadUnsigned] =  do_OS_ReadUnsigned,
  [OS_GenerateEvent] =  do_OS_GenerateEvent,

  [OS_ReadVarVal] =  do_OS_ReadVarVal,
  [OS_SetVarVal] =  do_OS_SetVarVal,
/*
  Using existing RISC OS code for the time being
  [OS_GSInit] =  do_OS_GSInit,
  [OS_GSRead] =  do_OS_GSRead,
  // Except Trans, which will output the initial string...
*/
#ifdef DEBUG__SHOW_GSTRANS
  [OS_GSTrans] =  do_OS_GSTrans,
#endif

//  [OS_BinaryToDecimal] =  do_OS_BinaryToDecimal,
  [OS_FSControl] =  do_OS_FSControl,
  [OS_ChangeDynamicArea] =  do_OS_ChangeDynamicArea,
  [OS_GenerateError] =  do_OS_GenerateError,

  [OS_ReadEscapeState] =  do_OS_ReadEscapeState,
//  [OS_EvaluateExpression] =  do_OS_EvaluateExpression,
  [OS_SpriteOp] =  do_OS_SpriteOp,
//  [OS_ReadPalette] =  do_OS_ReadPalette,

  [OS_ServiceCall] =  do_OS_ServiceCall,
  [OS_ReadVduVariables] =  do_OS_ReadVduVariables,
  [OS_ReadPoint] =  do_OS_ReadPoint,
  [OS_UpCall] =  do_OS_UpCall,

  [OS_CallAVector] =  do_OS_CallAVector,
  [OS_ReadModeVariable] =  do_OS_ReadModeVariable,
  [OS_RemoveCursors] =  do_OS_RemoveCursors,
  [OS_RestoreCursors] =  do_OS_RestoreCursors,

  [OS_SWINumberToString] =  do_OS_SWINumberToString,
  [OS_SWINumberFromString] =  do_OS_SWINumberFromString,
  [OS_ValidateAddress] =  do_OS_ValidateAddress,
  [OS_CallAfter] =  do_OS_CallAfter,

  [OS_CallEvery] =  do_OS_CallEvery,
  [OS_RemoveTickerEvent] =  do_OS_RemoveTickerEvent,
  [OS_InstallKeyHandler] =  do_OS_InstallKeyHandler,
  [OS_CheckModeValid] =  do_OS_CheckModeValid,


  [OS_ChangeEnvironment] =  do_OS_ChangeEnvironment,
  [OS_ClaimScreenMemory] =  do_OS_ClaimScreenMemory,
  [OS_ReadMonotonicTime] =  do_OS_ReadMonotonicTime,
  [OS_SubstituteArgs] =  do_OS_SubstituteArgs,

  [OS_PrettyPrint] =  do_OS_PrettyPrint,
  // [OS_Plot] =  do_OS_Plot,
  [OS_WriteN] =  do_OS_WriteN,
  [OS_AddToVector] =  do_OS_AddToVector,

  [OS_WriteEnv] =  do_OS_WriteEnv,
  // [OS_ReadArgs] =  do_OS_ReadArgs,
  [OS_ReadRAMFsLimits] =  do_OS_ReadRAMFsLimits,
  [OS_ClaimDeviceVector] =  do_OS_ClaimDeviceVector,

  [OS_ReleaseDeviceVector] =  do_OS_ReleaseDeviceVector,
  [OS_DelinkApplication] =  do_OS_DelinkApplication,
  [OS_RelinkApplication] =  do_OS_RelinkApplication,
  // [OS_HeapSort] =  do_OS_HeapSort,

  [OS_ExitAndDie] =  do_OS_ExitAndDie,
  [OS_ReadMemMapInfo] =  do_OS_ReadMemMapInfo,
  [OS_ReadMemMapEntries] =  do_OS_ReadMemMapEntries,
  [OS_SetMemMapEntries] =  do_OS_SetMemMapEntries,

  [OS_AddCallBack] =  do_OS_AddCallBack,
  [OS_ReadDefaultHandler] =  do_OS_ReadDefaultHandler,
  // [OS_SetECFOrigin] =  do_OS_SetECFOrigin,
  [OS_SerialOp] =  do_OS_SerialOp,


  [OS_ReadSysInfo] =  do_OS_ReadSysInfo,
  [OS_Confirm] =  do_OS_Confirm,
  //[OS_ChangedBox] =  do_OS_ChangedBox,
  [OS_CRC] =  do_OS_CRC,

  [OS_ReadDynamicArea] =  do_OS_ReadDynamicArea,
  [OS_PrintChar] =  do_OS_PrintChar,
  [OS_ChangeRedirection] =  do_OS_ChangeRedirection,
  [OS_RemoveCallBack] =  do_OS_RemoveCallBack,


  [OS_FindMemMapEntries] =  do_OS_FindMemMapEntries,
#ifdef DEBUG__EXAMINE_SET_COLOUR
  [OS_SetColour] =  do_OS_SetColour,
#endif
  [OS_Pointer] =  do_OS_Pointer,
  [OS_ScreenMode] =  do_OS_ScreenMode,

  [OS_DynamicArea] =  do_OS_DynamicArea,
  [OS_Memory] =  do_OS_Memory,
  [OS_ClaimProcessorVector] =  do_OS_ClaimProcessorVector,
  [OS_Reset] =  do_OS_Reset,

  [OS_MMUControl] =  do_OS_MMUControl,
  [OS_ResyncTime] = do_OS_ResyncTime,
  [OS_PlatformFeatures] = do_OS_PlatformFeatures,
  [OS_SynchroniseCodeAreas] = do_OS_SynchroniseCodeAreas,
  // [OS_CallASWI] = do_OS_CallASWI, Special case
  [OS_AMBControl] = do_OS_AMBControl,
  // [OS_CallASWIR12] = do_OS_CallASWIR12, Special case
  [OS_SpecialControl] = do_OS_SpecialControl,
  [OS_EnterUSR26] = do_OS_EnterUSR26,
  [OS_VIDCDivider] = do_OS_VIDCDivider,
  [OS_NVMemory] = do_OS_NVMemory,
  [OS_EnterUSR32] = do_OS_EnterUSR32,
  [OS_Hardware] = do_OS_Hardware,
  [OS_IICOp] = do_OS_IICOp,
  [OS_LeaveOS] = do_OS_LeaveOS,
  [OS_ReadLine32] = do_OS_ReadLine32,
  [OS_SubstituteArgs32] = do_OS_SubstituteArgs32,
//  [OS_HeapSort32] = do_OS_HeapSort32,


  [OS_ConvertStandardDateAndTime] =  do_OS_ConvertStandardDateAndTime,
  [OS_ConvertDateAndTime] =  do_OS_ConvertDateAndTime,

  [OS_ConvertHex1] =  do_OS_ConvertHex1,
  [OS_ConvertHex2] =  do_OS_ConvertHex2,
  [OS_ConvertHex4] =  do_OS_ConvertHex4,
  [OS_ConvertHex6] =  do_OS_ConvertHex6,

  [OS_ConvertHex8] =  do_OS_ConvertHex8,
  [OS_ConvertCardinal1] =  do_OS_ConvertCardinal1,
  [OS_ConvertCardinal2] =  do_OS_ConvertCardinal2,
  [OS_ConvertCardinal3] =  do_OS_ConvertCardinal3,

  [OS_ConvertCardinal4] =  do_OS_ConvertCardinal4,
  [OS_ConvertInteger1] =  do_OS_ConvertInteger1,
  [OS_ConvertInteger2] =  do_OS_ConvertInteger2,
  [OS_ConvertInteger3] =  do_OS_ConvertInteger3,

  [OS_ConvertInteger4] =  do_OS_ConvertInteger4,
  [OS_ConvertBinary1] =  do_OS_ConvertBinary1,
  [OS_ConvertBinary2] =  do_OS_ConvertBinary2,
  [OS_ConvertBinary3] =  do_OS_ConvertBinary3,

  [OS_ConvertBinary4] =  do_OS_ConvertBinary4,
  [OS_ConvertSpacedCardinal1] =  do_OS_ConvertSpacedCardinal1,
  [OS_ConvertSpacedCardinal2] =  do_OS_ConvertSpacedCardinal2,
  [OS_ConvertSpacedCardinal3] =  do_OS_ConvertSpacedCardinal3,

  [OS_ConvertSpacedCardinal4] =  do_OS_ConvertSpacedCardinal4,
  [OS_ConvertSpacedInteger1] =  do_OS_ConvertSpacedInteger1,
  [OS_ConvertSpacedInteger2] =  do_OS_ConvertSpacedInteger2,
  [OS_ConvertSpacedInteger3] =  do_OS_ConvertSpacedInteger3,

  [OS_ConvertSpacedInteger4] =  do_OS_ConvertSpacedInteger4,
  [OS_ConvertFixedNetStation] =  do_OS_ConvertFixedNetStation,
  [OS_ConvertNetStation] =  do_OS_ConvertNetStation,
  [OS_ConvertFixedFileSize] =  do_OS_ConvertFixedFileSize,

  [OS_MSTime] = do_OS_MSTime,
  [OS_ThreadOp] = do_OS_ThreadOp,
  [OS_PipeOp] = do_OS_PipeOp,

  [OS_VduCommand] = do_OS_VduCommand,
  [OS_LockForDMA] = do_OS_LockForDMA,
  [OS_ReleaseDMALock] = do_OS_ReleaseDMALock,
  [OS_MapDevicePages] = do_OS_MapDevicePages,
  [OS_FlushCache] = do_OS_FlushCache,

  [OS_ConvertFileSize] =  do_OS_ConvertFileSize
};

static bool __attribute__(( noinline )) Kernel_go_svc( svc_registers *regs, uint32_t svc )
{
  switch (svc & ~Xbit) {
  case 0 ... 255:
    if (os_swis[svc & ~Xbit] != 0) {
      return os_swis[svc & ~Xbit]( regs );
    }
    else {
      return run_risos_code_implementing_swi( regs, svc & ~Xbit );
    }
  case OS_WriteI ... OS_WriteI+255:
    {
      uint32_t r0 = regs->r[0];
      bool result;
      regs->r[0] = svc & 0xff;
      result = do_OS_WriteC( regs );
      if (result) {
        regs->r[0] = r0;
      }
      return result;
    }
  };

  return do_module_swi( regs, svc );
}

// This routine will be moved to a more sensible place (TaskSlot?) asap
/* Default behaviour: 
 * Swap out the calling task until this task completes?
 * If the task calls Wimp_Initialise, resume the caller.
 */
static void StartTask( svc_registers *regs )
{
  WriteS( "Start task: " ); Write0( regs->r[0] ); 
  if (regs->r[1] != 0) {
    WriteS( " " );
    Write0( regs->r[1] );
  }

  NewLine;

  // I think this will call Wimp_Initialise, which can be intercepted and the result returned from this routine
  OSCLI( (char const *) regs->r[0] );

  { register char *s asm ( "r0" ) = "Returned"; asm ( "svc 2" : : "r" (s) ); }
}

static void trace_wimp_calls_in( svc_registers *regs, uint32_t number )
{
  char buffer[64] = { 0 };
  register uint32_t swi asm ( "r0" ) = number + 0x400c0;
  register void *buf asm ( "r1" ) = buffer;
  register uint32_t size asm ( "r2" ) = sizeof( buffer );

  register uint32_t written asm ( "r2" );

  asm ( "svc %[swi]"
      : "=r" (written)
      : [swi] "i" (OS_SWINumberToString), "r" (swi), "r" (buf), "r" (size)
      : "lr", "memory" );

  WriteN( buffer, written ); Space; WriteNum( 0x400c0 + number );

    if (0x32 == number) {
      Space; WriteNum( regs->r[0] );
    }
    else if (0x2f == number) {
      Space; if (regs->r[0] != 0xffffffff && regs->r[0] > 1) Write0( regs->r[0] ); else WriteNum( regs->r[0] );
    }
  NewLine;
}

static void trace_wimp_calls_out( svc_registers *regs, uint32_t number )
{
    WriteS( "Wimp OUT " ); WriteNum( 0x400c0 + number );
    if (0x32 == number) {
      Space; WriteNum( regs->r[0] );
    }
    NewLine;
}

static bool special_case( svc_registers *regs, uint32_t number )
{
  if (OS_ValidateAddress == (number & ~Xbit)) { // FIXME
    regs->spsr &= ~CF;
    return true;
  }

  switch (number & ~Xbit) { // FIXME
  case 0x406c0 ... 0x406ff: return true; // Hourglass
  case 0x400c0 ... 0x400ff:
    trace_wimp_calls_in( regs, number & 0x3f );
    if ((number & 0x3f) == 0x1e) { StartTask( regs ); regs->r[0] = 0x66666666; return true; } // FIXME: should be handle returned from Wimp_Initialise, and return only when Wimp_Poll is called...
    break;
  case 0x80146: regs->r[0] = 0; return true; // PDriver_CurrentJob (called from Desktop?!)
  // case 0x41506: WriteS( "Translating error " ); Write0( regs->r[0] + 4 ); NewLine; break;
  case 0x487c0: regs->r[0] = (uint32_t) "HD Monitor"; return true;
  }

  return false;
}

void run_transient_callback( transient_callback *callback )
{
  run_handler( callback->code, callback->private_word );
}

static inline void run_transient_callbacks()
{
  if (workspace.kernel.transient_callbacks == 0) return;

  while (workspace.kernel.transient_callbacks != 0) {
    transient_callback *callback = workspace.kernel.transient_callbacks;

    // In case the callback registers a callback, make a private copy of the
    // callback details and sort out the list before making the call. 
    transient_callback latest = *callback;

    callback->next = workspace.kernel.transient_callbacks_pool;
    workspace.kernel.transient_callbacks_pool = callback;
    workspace.kernel.transient_callbacks = latest.next;

#ifdef DEBUG__SHOW_TRANSIENT_CALLBACKS
  WriteS( "Call transient callback: " ); WriteNum( latest.code ); WriteS( ", " ); WriteNum( latest.private_word ); NewLine;
#endif
    TempTaskDo( run_transient_callback, &latest );
  }
}

void __attribute__(( noinline )) execute_swi( svc_registers *regs )
{
  uint32_t number = get_swi_number( regs->lr );

  // FIXME What should happen if you call CallASWI using CallASWI?
  if ((number & ~Xbit) == OS_CallASWI) number = regs->r[9];
  else if ((number & ~Xbit) == OS_CallASWIR12) number = regs->r[12];

  regs->spsr &= ~VF;

  if (special_case( regs, number )) return;
  bool read_var_val_for_length = ((number & ~Xbit) == 0x23 && regs->r[2] == -1);


  bool result = Kernel_go_svc( regs, number );

  if (result) {
    // Worked
    regs->spsr &= ~VF;
  }
  else if ((number & Xbit) != 0) {
    // Error, should be returned to caller, no GenerateError
    error_block *e = (void*) regs->r[0];

    if (e == 0) {
      WriteS( "Error indicated, but NULL error block\\n\\r" );
      asm ( "bkpt 15" );
    }
    else {
      switch (number) {
      case 0x61500 ... 0x6153f: // MessageTrans
      case 0x63040 ... 0x6307f: // Territory
      case 0x606c0 ... 0x606ff: // Hourglass
        break;
      default:
        if (e->code != 0x1e4 && e->code != 0x124 && !read_var_val_for_length) {
svc_registers copy = *regs;
          NewLine;
          WriteS( "Error: " );
          WriteNum( number );
          Space;
          WriteNum( *(uint32_t *)(regs->r[0]) );
          Space;
          Write0( (char *)(regs->r[0] + 4 ) );
          Space;
          WriteNum( regs );
          Space;
          WriteNum( regs->r[0] );
          NewLine;
if (copy.r[0] != regs->r[0]) asm ( "bkpt 77" );
        }
        break;
      }
    }

    if (0x999 == e->code || 0x1e6 == e->code) {
      switch (number) {
      case 0x61500 ... 0x6153f: // MessageTrans
      case 0x63040 ... 0x6307f: // Territory
      case 0x606c0 ... 0x606ff: // Hourglass
      case 0x62fc0 ... 0x62fcf: // Portable
        break;
      default:
        WriteS( "Unimplemented!" ); NewLine;
        WriteNum( number ); NewLine;
        assert( false );
      }
    }
    regs->spsr |= VF;
  }
  else {
    // Call error handler
    WriteS( "Error from SWI " );
    WriteNum( number );
    WriteS( ", block: " );
    WriteNum( regs->r[0] ); Space;
    WriteNum( *(uint32_t*) regs->r[0] ); Space;
    Write0( (char *)(regs->r[0] + 4 ) ); NewLine;
    WriteNum( regs->lr );
    {
    regs->r[0] = 3;
    regs->r[1] = 10000;
    do_OS_ThreadOp( regs );
    }
  }

  switch (number & ~Xbit) {
  case 0x400c0 ... 0x400ff:
    trace_wimp_calls_out( regs, number & 0x3f );
    break;
  }

  if (0x10 == (regs->spsr & 0x1f)) {
    run_transient_callbacks();
  }
}
