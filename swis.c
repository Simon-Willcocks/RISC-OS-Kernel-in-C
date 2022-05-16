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


// This is the only mode supported at the moment. Search for it to find
// places to modify to cope with more. It's also referenced in modules.c,
// at the moment.
mode_selector_block const only_one_mode = { .mode_selector_flags = 1, .xres = 1920, .yres = 1080, .log2bpp = 5, .frame_rate = 60, { { -1, 0 } } };

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

  Write0( "Unimplemented SWI" ); NewLine;
  asm ( "bkpt 77" );
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

static uint32_t word_align( void *p )
{
  return (((uint32_t) p) + 3) & ~3;
}

// This routine is for SWIs implemented in the legacy kernel, 0-511, not in
// modules, in ROM or elsewhere. (i.e. routines that return using SLVK.)
bool run_risos_code_implementing_swi( svc_registers *regs, uint32_t svc )
{
  clear_VF();

  extern uint32_t JTABLE;
  uint32_t *jtable = &JTABLE;

  register uint32_t non_kernel_code asm( "r10" ) = jtable[svc];
  register uint32_t result asm( "r0" );
  register uint32_t swi asm( "r11" ) = svc;
  register svc_registers *regs_in asm( "r12" ) = regs;

  asm (
      "\n  push { r12 }"
      "\n  ldm r12, { r0-r9 }"
      "\n  adr lr, return"
      "\n  push { lr } // return address, popped by SLVK"

      // Which SWIs use flags in r12 for input?
      "\n  ldr r12, [r12, %[spsr]]"
      "\n  bic lr, r12, #(1 << 28) // Clear V flags leaving original flags in r12"
      // TODO re-enable interrupts if enabled in caller

      "\n  bx r10"
      "\nreturn:"
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
      : "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "lr" );

  return (result & VF) == 0;
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


static bool do_OS_Control( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_Exit( svc_registers *regs )
{
  WriteS( "OS_Exit" ); NewLine;
  return Kernel_Error_UnimplementedSWI( regs );
}

static bool do_OS_SetEnv( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_IntOn( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  regs->spsr = (regs->spsr & ~0x80);
  return true;
}

static bool do_OS_IntOff( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  regs->spsr = (regs->spsr & ~0x80) | 0x80;
  return true;
}

static bool do_OS_CallBack( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_EnterOS( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  regs->spsr = (regs->spsr & ~15) | 3;
  return true;
}

static bool do_OS_LeaveOS( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  regs->spsr = (regs->spsr & ~15);
  return true;
}

static bool do_OS_BreakPt( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_BreakCtrl( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_UnusedSWI( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_UpdateMEMC( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SetCallBack( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReadUnsigned( svc_registers *regs )
{
  // FIXME, can overflow, ignores flags
  if (regs->r[4] == 0x45444957) asm ( "bkpt 80" );
  uint32_t base = regs->r[0] & 0x7f;
  if (base < 2 || base > 36) base = 10; // Can this really be a default?
  uint32_t result = 0;
  const char *c = (const char*) regs->r[1];
  for (;;) {
    unsigned char d = *c;
    if (d < '0') break;
    if (d >= 'a') d -= ('a' - 'A');
    if (d > 'Z') break;
    if (d > '9' && d < 'A') break;
    if (d > '9') d -= ('A' - '0' + 10);
    if (d > base) break;
    result = (result * base) + d;
    c++;
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
  WriteS( "GSTrans (in) \\\"" ); Write0( (char*) regs->r[0] ); WriteS( "\\\"\\n\\r" );
  bool result = run_risos_code_implementing_swi( regs, OS_GSTrans );
  WriteS( "GSTrans (out) \\\"" );
  if (regs->r[1] != 0) {
    Write0( (char*) regs->r[1] );
  } else {
    WriteS( "NULL" );
  }
  WriteS( "\\\"\\n\\r" );
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

//static bool do_OS_BinaryToDecimal( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReadEscapeState( svc_registers *regs )
{
  // This can be called from interrupt routines, should probably make it more urgent.
  regs->spsr &= ~(1 << 29); // Clear CC, no escape FIXME
  return true;
}

//static bool do_OS_EvaluateExpression( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
//static bool do_OS_ReadPalette( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_SWINumberToString( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SWINumberFromString( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
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
    svc_registers regs;
    result = rma_allocate( sizeof( transient_callback ), &regs );
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
      workspace.kernel.ticker_event_pool = e->next;
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
           : "lr", "cc" );
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
           : "lr", "cc" );
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
  asm ( "push { "C_CLOBBERED" }" );
  C_TickerV_handler();
  asm ( "pop { "C_CLOBBERED" }" );
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



static bool do_OS_InstallKeyHandler( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

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


static bool do_OS_ClaimScreenMemory( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

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

static bool do_OS_SubstituteArgs( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

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

static bool do_OS_WriteEnv( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadArgs( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
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

static bool do_OS_ClaimDeviceVector( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReleaseDeviceVector( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ExitAndDie( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadMemMapInfo( svc_registers *regs )
{
  regs->r[0] = 4096;
  regs->r[1] = 64 << 20; // FIXME Lying, but why is this being used?
  return true;
}

static bool do_OS_ReadMemMapEntries( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SetMemMapEntries( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_AddCallBack( svc_registers *regs )
{
#ifdef DEBUG__SHOW_TRANSIENT_CALLBACKS
  WriteS( "New transient callback: " ); WriteNum( regs->r[0] ); WriteS( ", " ); WriteNum( regs->r[1] ); NewLine;
#endif
  transient_callback *callback = workspace.kernel.transient_callbacks_pool;
  if (callback == 0) {
    callback = rma_allocate( sizeof( transient_callback ), regs );
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

extern vector default_SpriteV;
extern vector default_ByteV;
extern vector default_ChEnvV;
extern vector default_CliV;
extern vector do_nothing;

static bool do_OS_ReadDefaultHandler( svc_registers *regs )
{
  vector *v = &do_nothing;
  switch (regs->r[0]) {
  case 0x05: v = &default_CliV; break;
  case 0x06: v = &default_ByteV; break;
  case 0x1e: v = &default_ChEnvV; break;
  case 0x1f: v = &default_SpriteV; break;
  default:
    v = &do_nothing; break;
  }

  regs->r[1] = v->code;
  regs->r[2] = v->private_word;
  regs->r[3] = 0; // Only relevant for Error, CallBack, BreakPoint. These will probably have to be associated with Task Slots...?
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
  [OSRSI6_UNDSTK]                                  = 
        sizeof( workspace.kernel.undef_stack ) + (uint32_t) &workspace.kernel.undef_stack,
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
  [OSRSI6_FgEcfOraEor]                             = (uint32_t) &workspace.vectors.zp.VduDriverWorkSpace.ws.FgEcfOraEor, // Used by SpriteExtend
  [OSRSI6_BgEcfOraEor]                             = (uint32_t) &workspace.vectors.zp.VduDriverWorkSpace.ws.BgEcfOraEor, // Used by SpriteExtend
  [OSRSI6_DebuggerSpace]                           = 0xbaad0000 | 74,
  [OSRSI6_DebuggerSpace_Size]                      = 0xbaad0000 | 75,
  [OSRSI6_CannotReset]                             = 0xbaad0000 | 76,
  [OSRSI6_MetroGnome]                              = 0xbaad0000 | 77, // OS_ReadMonotonicTime
  [OSRSI6_CLibCounter]                             = (uint32_t) &workspace.vectors.zp.CLibCounter,
  [OSRSI6_RISCOSLibWord]                           = (uint32_t) &workspace.vectors.zp.RISCOSLibWord,
  [OSRSI6_CLibWord]                                = (uint32_t) &workspace.vectors.zp.CLibWord,
  [OSRSI6_FPEAnchor]                               = 0xbaad0000 | 81,
  [OSRSI6_ESC_Status]                              = 0xbaad0000 | 82,
  [OSRSI6_ECFYOffset]                              = (uint32_t) &workspace.vectors.zp.VduDriverWorkSpace.ws.ECFYOffset, // Used by SpriteExtend
  [OSRSI6_ECFShift]                                = (uint32_t) &workspace.vectors.zp.VduDriverWorkSpace.ws.ECFShift, // Used by SpriteExtend
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

static bool do_OS_Confirm( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_CRC( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_PrintChar( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ChangeRedirection( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
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


static bool do_OS_FindMemMapEntries( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

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

static void SetColours( uint32_t bpp, uint32_t fore, uint32_t back )
{
  switch (bpp) {
  }
}

static bool do_OS_SetColour( svc_registers *regs )
{
  OS_SetColour_Flags flags = { .raw = regs->r[0] };

  if (flags.read_colour) {
    if (flags.text_colour) {
      if (flags.background)
        regs->r[1] = workspace.vectors.zp.VduDriverWorkSpace.ws.TextBgColour;
      else
        regs->r[1] = workspace.vectors.zp.VduDriverWorkSpace.ws.TextFgColour;
    }
    else {
      uint32_t *pattern_data = (void*) regs->r[1];
      uint32_t *to_read;

      if (flags.background)
        to_read = &workspace.vectors.zp.VduDriverWorkSpace.ws.BgPattern[0];
      else
        to_read = &workspace.vectors.zp.VduDriverWorkSpace.ws.FgPattern[0];

      for (int i = 0; i < 8; i++) {
        pattern_data[i] = to_read[i];
      }
    }

    return true;
  }

  if (flags.text_colour) {
    if (flags.background)
      workspace.vectors.zp.VduDriverWorkSpace.ws.TextBgColour = regs->r[1];
    else
      workspace.vectors.zp.VduDriverWorkSpace.ws.TextFgColour = regs->r[1];

    return true;
  }

  extern uint32_t *vduvarloc[];

  EcfOraEor *ecf;
  uint32_t *pattern;

  if (flags.background) {
    workspace.vectors.zp.VduDriverWorkSpace.ws.GPLBMD = flags.action | 0x60;
    pattern = &workspace.vectors.zp.VduDriverWorkSpace.ws.BgPattern[0];
    ecf = &workspace.vectors.zp.VduDriverWorkSpace.ws.BgEcfOraEor;
    *vduvarloc[154 - 128] = regs->r[1];
  }
  else {
    workspace.vectors.zp.VduDriverWorkSpace.ws.GPLFMD = flags.action | 0x60;
    pattern = &workspace.vectors.zp.VduDriverWorkSpace.ws.FgPattern[0];
    ecf = &workspace.vectors.zp.VduDriverWorkSpace.ws.FgEcfOraEor;
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
    uint32_t log2bpp = workspace.vectors.zp.VduDriverWorkSpace.ws.Log2BPP;
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
  return true;
}

static bool do_OS_Pointer( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ScreenMode( svc_registers *regs )
{
  Write0( __func__ ); WriteNum( regs->r[0] ); NewLine;

  enum { SelectMode, CurrentModeSpecifier, EnumerateModes, SetMonitorType, ConfigureAcceleration, FlushScreenCache, ForceFlushCache }; // ...

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
  case FlushScreenCache: return true;
  default: return Kernel_Error_UnimplementedSWI( regs );
  }
}

static bool do_OS_ClaimProcessorVector( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_Reset( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_MMUControl( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ResyncTime( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

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

static bool do_OS_AMBControl( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_SpecialControl( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_EnterUSR32( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_EnterUSR26( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_VIDCDivider( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_NVMemory( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_Hardware( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_IICOp( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadLine32( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SubstituteArgs32( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
// static bool do_OS_HeapSort32( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_SynchroniseCodeAreas( svc_registers *regs )
{
  WriteS( "OS_SynchroniseCodeAreas" );

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


static bool do_OS_ConvertBinary1( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary2( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary3( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary4( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertSpacedCardinal1( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal2( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal3( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal4( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertSpacedInteger1( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger2( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger3( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger4( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertFixedNetStation( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertNetStation( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertFixedFileSize( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool CLG( svc_registers *regs )
{
  // Move bottom left
  register uint32_t code asm( "r0" ) = 4;
  register uint32_t x asm( "r1" ) = 0;
  register uint32_t y asm( "r2" ) = 0;
  asm ( "svc 0x45" : : "r" (code), "r" (x), "r" (y) : "lr" );

  // Fill rectangle top right. FIXME don't just try to draw over a random-sized screen!
  code = 96 + 7;
  x = 1920 * 4;
  y = 1080 * 4; 
  asm ( "svc 0x45" : : "r" (code), "r" (x), "r" (y) : "lr" );

  return true;
}

static bool SetTextColour( svc_registers *regs )
{
  Write0( __func__ );
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

static bool VDU23( svc_registers *regs )
{
  Write0( __func__ );
  uint8_t *params = (void*) regs->r[1];
  for (int i = 0; i < 9; i++) {
    WriteS( " " ); WriteNum( params[i] );
  }
  NewLine;

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

  workspace.vectors.zp.VduDriverWorkSpace.ws.GWLCol = l;
  workspace.vectors.zp.VduDriverWorkSpace.ws.GWBRow = b;
  workspace.vectors.zp.VduDriverWorkSpace.ws.GWRCol = r;
  workspace.vectors.zp.VduDriverWorkSpace.ws.GWTRow = t;

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
  asm ( "svc %[swi]" : : [swi] "i" (0x20045), "r" (rt), "r" (rx), "r" (ry) : "lr", "cc" );

  // FIXME Handle errors!
  return true;
}

static bool RestoreDefaultWindows( svc_registers *regs )
{
  Write0( __func__ ); NewLine;
  workspace.vectors.zp.VduDriverWorkSpace.ws.GWLCol = 0;
  workspace.vectors.zp.VduDriverWorkSpace.ws.GWBRow = 0;
  workspace.vectors.zp.VduDriverWorkSpace.ws.GWRCol = 1920;
  workspace.vectors.zp.VduDriverWorkSpace.ws.GWTRow = 1080;
  return true;
}

/* This is a half-way house to having a per-thread graphics context
   approach. */
static bool do_OS_VduCommand( svc_registers *regs )
{
  // Always called with the right number of parameter bytes, honest!

  switch (regs->r[0]) {
  case 0: asm ( "bkpt 1" ); break; // do nothing, surely shouldn't be called
  case 2: asm ( "bkpt 1" ); break; // "enable printer"
  case 3: break; // do nothing, "disable printer"
  case 4: workspace.vectors.zp.VduDriverWorkSpace.ws.CursorFlags |= ~(1 << 30); return true;
  case 5: workspace.vectors.zp.VduDriverWorkSpace.ws.CursorFlags |= (1 << 30); return true;
  case 16: return CLG( regs );
  case 17: return SetTextColour( regs );
  case 19: return SetPalette( regs );
  case 22: return SetMode( regs );
  case 23: return VDU23( regs );
  case 24: return DefineGraphicsWindow( regs );
  case 25: return Plot( regs );
  case 26: return RestoreDefaultWindows( regs );
  default:
    {
      static error_block error = { 0x111, "Unimplemented VDU code" };
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

  claim_lock( &shared.memory.device_page_lock );
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

  MMU_map_device_shared_at( (void*) va, pa, 4096 * shared.memory.device_pages[i].pages );

  regs->r[0] = va;

  release_lock( &shared.memory.device_page_lock );

  return true;
}

static bool do_OS_FlushCache( svc_registers *regs )
{
  claim_lock( &shared.mmu.lock );
  clean_cache_to_PoC(); // FIXME
  release_lock( &shared.mmu.lock );
  return true;
}

static bool do_OS_ConvertFileSize( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

bool do_OS_ThreadOp( svc_registers *regs )
{
  if ((regs->spsr & 0x1f) != 0x10) {
    WriteNum( regs->spsr ); NewLine;
    static error_block error = { 0x999, "OS_ThreadOp only supported from usr mode, so far." };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

  enum { Start, Exit, WaitUntilWoken, Sleep, Resume, GetHandle, SetInterruptHandler };
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
      Task *new_task = Task_new( workspace.task_slot.running->slot );
      Write0( "Task allocated " ); WriteNum( new_task ); NewLine;

      new_task->slot = workspace.task_slot.running->slot;

      Task *running = workspace.task_slot.running;
      new_task->next = running;
      workspace.task_slot.running = new_task;

      // Save task context (integer only), including the usr stack pointer
      // The register values when the creating task is resumed
      running->regs.r[0] = (uint32_t) new_task;
      running->regs.r[1] = regs->r[1];
      running->regs.r[2] = regs->r[2];
      running->regs.r[3] = regs->r[3];
      running->regs.r[4] = regs->r[4];
      running->regs.r[5] = regs->r[5];
      running->regs.r[6] = regs->r[6];
      running->regs.r[7] = regs->r[7];
      running->regs.r[8] = regs->r[8];
      running->regs.r[9] = regs->r[9];
      running->regs.r[10] = regs->r[10];
      running->regs.r[11] = regs->r[11];
      running->regs.r[12] = regs->r[12];
      running->regs.pc = regs->lr;
      running->regs.psr = regs->spsr;

      switch (regs->spsr & 0x1f) {
      case 0x10:
        {
        asm volatile ( "mrs %[sp], sp_usr" : [sp] "=r" (running->regs.sp) );
        }
        break;
      default:
        // FIXME
        {
          WriteNum( regs->spsr ); NewLine;
          static error_block error = { 0x999, "Don't create threads from privileged modes?" };
          regs->r[0] = (uint32_t) &error;
        }
        return false;
      }

      // Replace the calling task with the new task

      uint32_t starting_sp = regs->r[2];
      regs->lr = regs->r[1];
      // The new thread will start with the same psr as the parent

      asm volatile ( "msr sp_usr, %[sp]" : : [sp] "r" (starting_sp) );

      regs->r[0] = (uint32_t) new_task;
      regs->r[1] = regs->r[3];
      regs->r[2] = regs->r[4];
      regs->r[3] = regs->r[5];
      regs->r[4] = regs->r[6];
      regs->r[5] = regs->r[7];
      regs->r[6] = regs->r[8];

      // FIXME: do something clever with floating point

      Write0( "Task created" ); NewLine;

      return true;
    }
  case Sleep:
    {
      // No interrupts yet, just yield to another thread

      Task *running = workspace.task_slot.running;
      if (running == 0) { Write0( "Sleep, but running is zero" ); NewLine; asm( "wfi" ); }

      Task *resume = running->next;
      if (resume == 0) { Write0( "Sleep, but resume is zero" ); NewLine; asm( "wfi" ); }
      workspace.task_slot.running = resume;

      Task *last = running;
      while (last->next != 0) {
        last = last->next;
      }

      running->next = 0;
      last->next = running;     // Fakey fakey, the task should be taken 
                                // out of the list altogether and
                                // re-activated when an interrupt occurs.


      // Save task context (integer only), including the usr stack pointer
      // The register values when the task is resumed
      running->regs.r[0] = regs->r[0];
      running->regs.r[1] = regs->r[1];
      running->regs.r[2] = regs->r[2];
      running->regs.r[3] = regs->r[3];
      running->regs.r[4] = regs->r[4];
      running->regs.r[5] = regs->r[5];
      running->regs.r[6] = regs->r[6];
      running->regs.r[7] = regs->r[7];
      running->regs.r[8] = regs->r[8];
      running->regs.r[9] = regs->r[9];
      running->regs.r[10] = regs->r[10];
      running->regs.r[11] = regs->r[11];
      running->regs.r[12] = regs->r[12];
      running->regs.pc = regs->lr;
      running->regs.psr = regs->spsr;

      switch (regs->spsr & 0x1f) {
      case 0x10:
        {
          asm volatile ( "mrs %[sp], sp_usr" : [sp] "=r" (running->regs.sp) );
        }
        break;
      default:
        // FIXME
        {
          asm ( "bkpt 1" );
        }
        return false;
      }

      // Replace the calling task with the next task in the queue
      regs->lr = resume->regs.pc;
      regs->spsr = resume->regs.psr;
      asm volatile ( "msr sp_usr, %[sp]" : : [sp] "r" (resume->regs.sp) );

      regs->r[0] = resume->regs.r[0];
      regs->r[1] = resume->regs.r[1];
      regs->r[2] = resume->regs.r[2];
      regs->r[3] = resume->regs.r[3];
      regs->r[4] = resume->regs.r[4];
      regs->r[5] = resume->regs.r[5];
      regs->r[6] = resume->regs.r[6];
      regs->r[7] = resume->regs.r[7];
      regs->r[8] = resume->regs.r[8];
      regs->r[9] = resume->regs.r[9];
      regs->r[10] = resume->regs.r[10];
      regs->r[11] = resume->regs.r[11];
      regs->r[12] = resume->regs.r[12];

      // FIXME: do something clever with floating point

      return true;
    }
  default: return Kernel_Error_UnimplementedSWI( regs );
  }
}

struct os_pipe {
  os_pipe *next;
  Task *sender;
  Task *receiver;
  uint32_t physical;
  uint32_t allocated_mem;
  uint32_t virtual_receiver;
  uint32_t virtual_sender;
  uint32_t max_block_size;
  uint32_t max_data;
  uint32_t write_index;
  uint32_t read_index;
};

static bool PipeCreate( svc_registers *regs )
{
  uint32_t max_block_size = regs->r[2];
  uint32_t max_data = regs->r[3];
  uint32_t allocated_mem = regs->r[4];

  if (max_block_size == 0 || max_block_size > max_data) {
    // FIXME
    return Kernel_Error_UnimplementedSWI( regs );
  }

  if (max_data != 0) {
    // FIXME
    return Kernel_Error_UnimplementedSWI( regs );
  }

  os_pipe *pipe = rma_allocate( sizeof( os_pipe ), regs );

  if (pipe == 0) {
    asm ( "bkpt 1" );
  }

  // At the moment, the running task is the only one that knows about it.
  // If it goes away, the resource should be cleaned up.
  pipe->sender = pipe->receiver = workspace.task_slot.running;

  pipe->max_block_size = max_block_size;
  pipe->max_data = max_data;
  pipe->allocated_mem = allocated_mem;
  pipe->physical = Kernel_allocate_pages( 4096, 4096 );

  // The following will be updated on the first calls to WaitForSpace and WaitForData, respectively.
  pipe->virtual_sender = -1;
  pipe->virtual_receiver = -1;

  pipe->write_index = allocated_mem & 0xfff;
  pipe->read_index = allocated_mem & 0xfff;

  claim_lock( &shared.kernel.pipes_lock );
  pipe->next = shared.kernel.pipes;
  shared.kernel.pipes = pipe;
  release_lock( &shared.kernel.pipes_lock );

  regs->r[1] = (uint32_t) pipe;

  return true;
}

static bool PipeWaitForSpace( svc_registers *regs )
{
  return true;
}

static bool PipeSpaceFilled( svc_registers *regs )
{
  return true;
}

static bool PipePassingOver( svc_registers *regs )
{
  os_pipe *pipe = (void*) regs->r[1];

  pipe->virtual_sender = -1;

  return true;
}

static bool PipeNoMoreData( svc_registers *regs )
{
  return true;
}

static bool PipeWaitForData( svc_registers *regs )
{
  return true;
}

static bool PipeDataFreed( svc_registers *regs )
{
  return true;
}

static bool PipePassingOff( svc_registers *regs )
{
  os_pipe *pipe = (void*) regs->r[1];

  pipe->virtual_receiver = -1;

  return true;
}

static bool PipeNotListening( svc_registers *regs )
{
  return true;
}


bool do_OS_PipeOp( svc_registers *regs )
{
  enum { Create, WaitForSpace, SpaceFilled, PassingOver, NoMoreData, WaitForData, DataFreed, PassingOff, NotListening };
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
        



    PassingOver - about to ask another task to send its data to this pipe
    PassingOff - about to ask another task to handle the data from this pipe
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
  switch (regs->r[0]) {
  case Create: return PipeCreate( regs );
  case WaitForSpace: return PipeWaitForSpace( regs );
  case PassingOver: return PipePassingOver( regs );
  case SpaceFilled: return PipeSpaceFilled( regs );
  case NoMoreData: return PipeNoMoreData( regs );
  case WaitForData: return PipeWaitForData( regs );
  case DataFreed: return PipeDataFreed( regs );
  case PassingOff: return PipePassingOver( regs );
  case NotListening: return PipeNotListening( regs );
  default:
    asm( "bkpt 1" );
  }
  return false;
}

bool do_OS_Heap( svc_registers *regs )
{
  // Note: This could possibly be improved by having a lock per heap,
  // one bit in the header, say.
  // I would hope this is never called from an interrupt handler, but 
  // if so, we should probably return an error, if shared.memory.os_heap_lock 
  // is non-zero. Masking interrupts is no longer a guarantee of atomicity.
  // OS_Heap appears to call itself, even without interrupts...
  bool reclaimed = claim_lock( &shared.memory.os_heap_lock );
  bool result = run_risos_code_implementing_swi( regs, 0x1d );
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
  [OS_ReadArgs] =  do_OS_ReadArgs,
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
  [OS_SetColour] =  do_OS_SetColour,
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

  [OS_ThreadOp] = do_OS_ThreadOp,
  [OS_PipeOp] = do_OS_PipeOp,

  [OS_VduCommand] = do_OS_VduCommand,
  [OS_LockForDMA] = do_OS_LockForDMA,
  [OS_ReleaseDMALock] = do_OS_ReleaseDMALock,
  [OS_MapDevicePages] = do_OS_MapDevicePages,
  [OS_FlushCache] = do_OS_FlushCache, // This could be called by each core on centisecond interrupts, maybe?

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
  Write0( "Start task: " ); Write0( regs->r[0] ); 
  if (regs->r[1] != 0) {
    WriteS( " " );
    Write0( regs->r[1] );
  }

  NewLine;

  OSCLI( (char const *) regs->r[0] );

  asm( "bkpt 170" );
}

static void __attribute__(( noinline )) do_svc( svc_registers *regs, uint32_t number )
{
  regs->spsr &= ~VF;

if (OS_ValidateAddress == (number & ~Xbit)) { // FIXME
  regs->spsr &= ~CF;
  return;
}

      switch (number & ~Xbit) { // FIXME
      case 0x406c0 ... 0x406ff: return; // Hourglass
      case 0x400de: StartTask( regs ); return;
      case 0x80146: regs->r[0] = 0; return; // PDriver_CurrentJob (called from Desktop?!)
      }
      bool read_var_val_for_length = ((number & ~Xbit) == 0x23 && regs->r[2] == -1);

  uint32_t r0 = regs->lr;

  if (Kernel_go_svc( regs, number )) {
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
          NewLine;
          Write0( "Returned error: " );
          WriteNum( number );
          Write0( " " );
          WriteNum( r0 );
          Write0( " " );
          WriteNum( regs->r[1] );
          Write0( " " );
          WriteNum( regs->r[2] );
          Write0( " " );
          WriteNum( *(uint32_t *)(regs->r[0]) );
          Write0( " " );
          Write0( (char *)(regs->r[0] + 4 ) );
          NewLine;
        }
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
        register uint32_t swinum asm( "r0" ) = number;
        asm ( "bkpt 1" : : "r" (swinum) );
        //asm ( "bkpt 1" ); // WindowManager initialisation uses OS_DynamicArea to get free pool, and OS_Memory to allocate from it; let them continue
      }
    }
    regs->spsr |= VF;
  }
  else {
    // Call error handler
    WriteS( "SWI " );
    WriteNum( number );
    WriteS( " " );
    Write0( (char *)(regs->r[0] + 4 ) ); NewLine;
    asm ( "bkpt 16" );
  }
}

void run_transient_callbacks()
{
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

    run_handler( latest.code, latest.private_word );
  }
}

static void __attribute__(( noinline )) do_something_else( svc_registers *regs )
{
  while (0 == workspace.task_slot.running) {
    if (0 != shared.task_slot.runnable) { // Checking outside lock
      claim_lock( &shared.task_slot.lock );

      if (0 != shared.task_slot.runnable) { // Safe check
        workspace.task_slot.running->regs = shared.task_slot.runnable->regs;
        shared.task_slot.runnable = shared.task_slot.runnable->next;
        // *regs = workspace.task_slot.running->regs;
        asm( "bkpt 17" ); // Do the above properly, still untested
        MMU_switch_to( workspace.task_slot.running->slot );
      }

      release_lock( &shared.task_slot.lock );
    }
    else {
      asm ( "wfe" ); // Maybe this should be WFI?
    }
  }
}

static void __attribute__(( noinline )) bottleneck_reopened( svc_registers *regs )
{
  claim_lock( &shared.task_slot.lock );

  assert( workspace.task_slot.running == shared.task_slot.bottleneck_owner );

  Task *freed = shared.task_slot.next_to_own;

  shared.task_slot.bottleneck_owner = freed;

  if (freed != 0) {
    shared.task_slot.next_to_own = freed->next;
    if (freed->next == 0) {
      assert( shared.task_slot.last_to_own == freed );
      shared.task_slot.last_to_own = 0;
    }
    // The freed task goes to the top of the list, since it's been waiting for
    // so long, but doesn't preempt the finished task... Debatable. TODO
    freed->next = shared.task_slot.runnable;
    shared.task_slot.runnable = freed;
  }

  release_lock( &shared.task_slot.lock );
}

void __attribute__(( naked, noreturn )) Kernel_default_svc()
{
  // Some SWIs preserve all registers
  // SWIs have the potential to update the first 10 registers
  // The implementations are passed values in r11 and r12, which must not
  // be seen by the caller, and r10 may also be corrupted.
  // The SVC stack pointer should be maintained by the implementation.

  // C functions may corrupt r0-r3, r9, and r10-12, and r14 (lr)
  // Note: r9 is optionally callee-saved, use C_CLOBBERED

  // Gordian knot time.
  // Store r0-r12 on the stack, plus the exception return details (srs)
  // Call C functions to find and call the appropriate handler, storing the returned
  // r0-r9 over the original values on return (and updating the stored SPSR flags).
  // The savings of not always having to save r4-r8 (into non-shared, cached memory)
  // will be minor compared to messing about trying to avoid it.

  svc_registers *regs;
  uint32_t lr;
  asm ( "  srsdb sp!, #0x13"
      "\n  push { r0-r12 }"
      "\n  mov %[regs], sp"
      "\n  mov %[lr], lr"
      : [regs] "=r" (regs)
      , [lr] "=r" (lr)
      );

  uint32_t number = get_swi_number( lr );

  // FIXME What should happen if you call CallASWI using CallASWI?
  if ((number & ~Xbit) == OS_CallASWI) number = regs->r[9];
  else if ((number & ~Xbit) == OS_CallASWIR12) number = regs->r[12];

  switch (number & ~Xbit) {
    case OS_File:
    case OS_Args:
    case OS_BGet:
    case OS_BPut:
    case OS_GBPB:
    case OS_Find:
    case OS_ReadLine:
    case OS_FSControl:
    case 0x40080 ... 0x400bf:
      {
        // These SWIs expect only a single program running on a single processor
        Task *blocked = 0;

retry: // This is not its final form

        claim_lock( &shared.task_slot.lock );

        bool already_owner = (shared.task_slot.bottleneck_owner == workspace.task_slot.running);
        if (already_owner || shared.task_slot.bottleneck_owner == 0) {
          // We're IN! (Possibly recursing)
          if (!already_owner) {
            shared.task_slot.bottleneck_owner = workspace.task_slot.running;
            assert (shared.task_slot.next_to_own == 0);
            assert (shared.task_slot.last_to_own == 0);
          }
        }
        else {
#if 1
// Implementation assumes the owner is on another core
// Final implementation puts the current task to sleep until the bottleneck is cleared up again
release_lock( &shared.task_slot.lock );
for (int i = 0; i < 1000000; i++) { asm ( "" ); }
goto retry;
#else
          // First come, first served, and we're not first!
          blocked = workspace.task_slot.running;

          // Save context
          //workspace.task_slot.running->regs = *regs;
          asm ( "bkpt 13" ); // FIXME
          workspace.task_slot.running = 0; // we're not running any more

          if (shared.task_slot.last_to_own == 0) {
            shared.task_slot.next_to_own = blocked;
          }
          else {
            shared.task_slot.last_to_own->next = blocked;
          }

          shared.task_slot.last_to_own = blocked;
#endif
        }
        release_lock( &shared.task_slot.lock );

        if (0 == blocked) { // This task owns the filesystem
          do_svc( regs, number );
          if (!already_owner) {
            bottleneck_reopened( regs );
          }
        }
        else {
          // When this routine returns, there is a thread in the mapped in slot
          // that can be run by this core
          do_something_else( regs );
        }
      }
      break;
    default:
      do_svc( regs, number );
      break;
  }

  if ((number & ~Xbit) == 0x400c8 // Wimp_RedrawWindow
   || (number & ~Xbit) == 0x400ca) { // Wimp_GetRectangle
    if (regs->r[0] == 0) { Write0( "GetRectangle Done" ); NewLine; }
    else {
      uint32_t *block = (void*) regs->r[1];
      Write0( "GetRectangle not done." ); NewLine;
      for (int i = 0; i <= 10; i++) {
        WriteNum( block[i] ); NewLine;
      }
    }
  }

  if (0 == (regs->spsr & 0x1f)) {
    run_transient_callbacks();
  }

  asm ( "pop { r0-r12 }"
    "\n  rfeia sp!" );

  __builtin_unreachable();
}

