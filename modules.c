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
#include "include/callbacks.h"

static bool Kernel_Error_NoMoreModules( svc_registers *regs )
{
  static error_block error = { 0x107, "No more modules" };

  regs->r[0] = (uint32_t) &error;

  return false;
}

static bool Kernel_Error_NoMoreIncarnations( svc_registers *regs )
{
  static error_block error = { 0x109, "No more incarnations of that module" };

  regs->r[0] = (uint32_t) &error;

  return false;
}

static bool Kernel_Error_SWINameNotKnown( svc_registers *regs )
{
  static error_block error = { 486, "SWI name not known" };

  regs->r[0] = (uint32_t) &error;

  return false;
}

static inline void Sleep( uint32_t centiseconds )
{
  register uint32_t request asm ( "r0" ) = TaskOp_Sleep;
  register uint32_t time asm ( "r1" ) = centiseconds; // Shift down a lot for testing!

  asm volatile ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      , "r" (time)
      : "lr", "memory" );
}

static inline void Yield()
{
  asm volatile ( "mov r0, #3 // Sleep"
             "\n  mov r1, #0 // For no time - yield"
             "\n  svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      : "r0", "r1", "lr", "memory" );
}

// Linker generated:
extern uint32_t _binary_AllMods_start;
extern uint32_t rma_base;
extern uint32_t rma_heap;

typedef struct {
  uint32_t offset_to_start;
  uint32_t offset_to_initialisation;
  uint32_t offset_to_finalisation;
  uint32_t offset_to_service_call_handler;
  uint32_t offset_to_title_string;
  uint32_t offset_to_help_string;
  uint32_t offset_to_help_and_command_keyword_table;
  uint32_t swi_chunk;
  uint32_t offset_to_swi_handler;
  uint32_t offset_to_swi_decoding_table;
  uint32_t offset_to_swi_decoding_code;
  uint32_t offset_to_messages_file_name;
  uint32_t offset_to_flags;
} module_header;

struct module {
  module_header *header;
  uint32_t *private_word;       // Points to either the local_private_word, below, or the shared local_private_word
  uint32_t local_private_word;
  module *next;         // Simple singly-linked list
  module *instances;    // Simple singly-linked list of instances. Instance number is how far along the list the module is, not a constant.
  module *base;
  char postfix[];
};

static void *pointer_at_offset_from( void *base, uint32_t off )
{
  return (off == 0) ? 0 : ((uint8_t*) base) + off;
}

static inline void *start_code( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_start );
}

static inline void *init_code( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_initialisation );
}

static inline uint32_t mp_aware( module_header *header )
{
  uint32_t flags = *(uint32_t *) (((char*) header) + header->offset_to_flags);
  return 0 != (2 & flags);
}

static inline bool run_initialisation_code( const char *env, module *m, uint32_t instance )
{
  uint32_t *code = init_code( m->header );

  register uint32_t *non_kernel_code asm( "r14" ) = code;
  register uint32_t *private_word asm( "r12" ) = m->private_word;
  register uint32_t _instance asm( "r11" ) = instance;
  register const char *environment asm( "r10" ) = env;

  // These will be passed to old-style modules as well, but they'll ignore them
  register uint32_t this_core asm( "r0" ) = workspace.core_number;
  register uint32_t number_of_cores asm( "r1" ) = processor.number_of_cores;

  error_block *error;

  asm volatile (
        "  blx r14"
      "\n  movvs %[error], r0"
      "\n  movvc %[error], #0"
      : [error] "=r" (error)
      : "r" (non_kernel_code)
      , "r" (private_word)
      , "r" (_instance)
      , "r" (environment)
      , "r" (this_core)
      , "r" (number_of_cores)
      : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9" );

  // No changes to the registers by the module are of any interest,
  // so avoid corrupting any by simply not storing them

  // FIXME return error_block * instead of bool
  if (error != 0) {
    NewLine;
    WriteS( "Module initialisation returned error: " );
    Write0( error->desc );
    NewLine;
  }

  return error == 0;
}

static inline uint32_t finalisation_code( module_header *header )
{
  return header->offset_to_finalisation + (uint32_t) header;
}

static bool __attribute__(( noinline )) run_service_call_handler_code( svc_registers *regs, module *m )
{
  register uint32_t non_kernel_code asm( "r14" ) = m->header->offset_to_service_call_handler + (uint32_t) m->header;
  register uint32_t *private_word asm( "r12" ) = m->private_word;

  asm goto (
        "  push { %[regs] }"
      "\n  ldm %[regs], { r0-r8 }"
      "\n  blx r14"
      "\n  pop { r14 }"
      "\n  stm r14, { r0-r8 }"
      "\n  bvs %l[failed]"
      :
      : [regs] "r" (regs)
      , "r" (non_kernel_code)
      , "r" (private_word)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8" 
      : failed );

  return true;

failed:
  return false;
}

static error_block *run_command( module *m, uint32_t code_offset, const char *params, uint32_t count )
{
  error_block *error = 0;
  register uint32_t non_kernel_code asm( "r14" ) = code_offset + (uint32_t) m->header;
  register uint32_t *private_word asm( "r12" ) = m->private_word;
  register const char *p asm( "r0" ) = params;
  register uint32_t c asm( "r1" ) = count;

  asm (
      "\n  blx r14"
      "\n  strvs r0, %[error]"
      :
      : "r" (p)
      , "r" (c)
      , "r" (non_kernel_code)
      , "r" (private_word)
      , [error] "m" (error)
      : "r2", "r3", "r4", "r5", "r6" );

  return error;
}

static bool run_swi_handler_code( svc_registers *regs, uint32_t svc, module *m )
{
#ifdef DEBUG__SHOW_RESOURCE_FILES
if (svc == 0x41b40 || svc == 0x61b40) {
  struct rfs {
    uint32_t offset;
    uint32_t load;
    uint32_t exec;
    uint32_t size;
    uint32_t attr;
    char name[];
  } *rf = (void*) regs->r[0];
  do {
    WriteS( "New file: " );
    Write0( rf->name );
    WriteS( " " ); WriteNum( rf->offset );
    rf = (void*) (((char*)rf)+rf->offset);
    NewLine;
  } while (rf->offset != 0);
}
#endif
  clear_VF();

  register uint32_t non_kernel_code asm( "r14" ) = m->header->offset_to_swi_handler + (uint32_t) m->header;
  register uint32_t *private_word asm( "r12" ) = m->private_word;
  register uint32_t svc_index asm( "r11" ) = svc & 0x3f;

  asm (
      "\n  push { %[regs] }"
      "\n  ldm %[regs], { r0-r9 }"
      "\n  blx r14"
      "\nreturn:"
      "\n  pop { %[regs] }"
      "\n  stm %[regs], { r0-r9 }"
      "\n  ldr r1, [%[regs], %[spsr]]"
      "\n  bic r1, #0xf0000000"
      "\n  mrs r2, cpsr"
      "\n  and r2, r2, #0xf0000000"
      "\n  orr r1, r1, r2"
      "\n  str r1, [%[regs], %[spsr]]"
      :
      : [regs] "r" (regs)
      , "r" (private_word)
      , "r" (svc_index)
      , "r" (non_kernel_code)
      , [spsr] "i" (4 * (&regs->spsr - &regs->r[0]))
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9" );

  return (regs->spsr & VF) == 0;
}

#ifdef DEBUG__SHOW_VECTORS_VERBOSE
#define DEBUG__SHOW_VECTORS
#endif
#ifdef DEBUG__SHOW_VECTORS
vector do_nothing;
#endif

static void run_interruptable_vector( svc_registers *regs, vector *v )
{
  // "If your routine passes the call on, you can deliberately alter some of
  // the registers values to change the effect of the call, however, you must
  // arrange for control to return to your routine again to restore to those
  // that the original routine would have returned. It should then return
  // control back to the calling program."
  // The only way I can see this working is if the code:
  //    Stores the intercept return point (and its private word?)
  //    Replaces it with an address in its own code
  //    Returns with mov pc, lr (allowing other handlers to execute)
  // AND the final, default, action of every vector handler is pop {pc}.

  register vector *vec asm( "r10" ) = v;

  // Code always exits via intercepted.
  asm volatile (
      "\n  adr r0, intercepted  // Interception address, to go onto stack"
      "\n  push { r0, %[regs] } // Save location of register storage at sp+4"
      "\n  ldm %[regs], { r0-r9 }"
      "\n0:"
      "\n  ldr r14, [%[v], %[code]]"
      "\n  ldr r12, [%[v], %[private]]"
      "\n  blx r14"
      "\n  ldr %[v], [%[v], %[next]]"
      "\n  cmp %[v], #0 // I don't think this should happen, but carry on if it does"
      "\n  bne 0b"
      "\n  pop {lr} // intercepted"
      "\nintercepted:"
      "\n  pop { r14 } // regs (intercepted already popped)"
      "\n  stm r14, { r0-r9 }"
      "\n  ldr r1, [r14, %[spsr]] // Update spsr with cpsr flags"
      "\n  mrs r2, cpsr"
      "\n  bic r1, #0xf0000000"
      "\n  and r2, r2, #0xf0000000"
      "\n  orr r1, r1, r2"
      "\n  str r1, [r14, %[spsr]]"
      : "=r" (v) // Updated by code
      , "=r" (regs) // Corrupted by DrawV, I think
      : [regs] "r" (regs)
      , [v] "r" (vec)

      , [next] "i" ((char*) &((vector*) 0)->next)
      , [private] "i" ((char*) &((vector*) 0)->private_word)
      , [code] "i" ((char*) &((vector*) 0)->code)
      , [spsr] "i" (4 * (&regs->spsr - &regs->r[0]))

      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r12", "r14" );
}

bool run_vector( svc_registers *regs, int vec )
{
#ifdef DEBUG__SHOW_VECTORS_VERBOSE
  // Ignore WrchV, TickerV
  if (vec != 3 && vec != 0x1c)
  {
    WriteS( "Running vector " ); WriteNum( vec ); NewLine;
    vector *v = workspace.kernel.vectors[vec];
    assert( v != 0 );
    do {
      WriteNum( v->code ); WriteS( " " ); WriteNum( v->private_word ); WriteS( " " ); WriteNum( v->next ); NewLine;
      v = v->next;
    } while (v != workspace.kernel.vectors[vec]);
    NewLine;
    for (int i = 0; i < 10; i++) { WriteNum( regs->r[i] ); WriteS( " " ); }
    WriteNum( regs->lr );
    NewLine;
  }
#endif
  run_interruptable_vector( regs, workspace.kernel.vectors[vec] );
#ifdef DEBUG__SHOW_VECTORS_VERBOSE
  if (vec != 3 && vec != 0x1c && workspace.kernel.vectors[3] != &do_nothing)
  {
    WriteS( "Vector " ); WriteNum( vec ); NewLine;
  }
#endif
  return (regs->spsr & VF) == 0;
}

static inline char const *swi_decoding_table( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_swi_decoding_table );
}

static inline uint32_t swi_decoding_code( module_header *header )
{
  return (uint32_t) pointer_at_offset_from( header, header->offset_to_swi_decoding_code );
}

static inline const char *title_string( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_title_string );
}

static inline const char *help_string( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_help_string );
}

static inline const char *module_commands( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_help_and_command_keyword_table );
}

bool do_module_swi( svc_registers *regs, uint32_t svc )
{
  uint32_t chunk = svc & ~Xbit & ~0x3f;

  module *m = workspace.kernel.module_list_head;
  while (m != 0 && m->header->swi_chunk != chunk) {
    m = m->next;
  }
  if (m == 0) {
    return Kernel_Error_UnknownSWI( regs );
  }

  return run_swi_handler_code( regs, svc, m );
}

// Case insensitive, nul, cr, lf, space, or % terminates.
// So "abc" matches "abc def", "abc%ghi", etc.
bool module_name_match( char const *left, char const *right )
{
  int diff = 0;

  while (diff == 0) {
    char l = *left++;
    char r = *right++;

    if ((l == 0 || l == 10 || l == 13 || l == ' ' || l == '%')
     && (r == 0 || r == 10 || r == 13 || r == ' ' || r == '%')) {
      return true;
    }

    diff = l - r;

    if (diff == 'a'-'A') {
      if (l >= 'a' && l <= 'z') diff = 0;
    }
    else if (diff == 'A'-'a') {
      if (r >= 'a' && r <= 'z') diff = 0;
    }
  }

  return false;
}

// Case insensitive, nul, cr, lf, or space terminate
static inline bool riscoscmp( char const *left, char const *right )
{
  int diff = 0;

  while (diff == 0) {
    char l = *left++;
    char r = *right++;

    if ((l == 0 || l == 10 || l == 13 || l == ' ')
     && (r == 0 || r == 10 || r == 13 || r == ' ')) return true;

    diff = l - r;
    if (diff == 'a'-'A') {
      if (l >= 'a' && l <= 'z') diff = 0;
    }
    else if (diff == 'A'-'a') {
      if (r >= 'a' && r <= 'z') diff = 0;
    }
  }

  return false;
}

static inline void describe_service_call( svc_registers *regs )
{
NewLine; WriteS( "*** ServiceCall_" );
switch (regs->r[1]) {

case 0x00: WriteS( "CallClaimed" ); break;
case 0x04: WriteS( "UKCommand" ); break;
case 0x06: WriteS( "Error" ); break;
case 0x07: WriteS( "UKByte" ); break;
case 0x08: WriteS( "UKWord" ); break;
case 0x09: WriteS( "Help" ); break;
case 0x0B: WriteS( "ReleaseFIQ" ); break;
case 0x0C: WriteS( "ClaimFIQ" ); break;
case 0x11: WriteS( "Memory" ); break;
case 0x12: WriteS( "StartUpFS" ); break;
case 0x18: WriteS( "PostHelp?" ); break;
case 0x27: WriteS( "PostReset" ); break;
case 0x28: WriteS( "UKConfig" ); break;
case 0x29: WriteS( "UKStatus" ); break;
case 0x2A: WriteS( "NewApplication" ); break;
case 0x40: WriteS( "FSRedeclare" ); break;
case 0x41: WriteS( "Print" ); break;
case 0x42: WriteS( "LookupFileType" ); break;
case 0x43: WriteS( "International" ); break;
case 0x44: WriteS( "KeyHandler" ); break;
case 0x45: WriteS( "PreReset" ); break;
case 0x46: WriteS( "ModeChange" ); break;
case 0x47: WriteS( "ClaimFIQinBackground" ); break;
case 0x48: WriteS( "ReAllocatePorts" ); break;
case 0x49: WriteS( "StartWimp" ); break;
case 0x4A: WriteS( "StartedWimp" ); break;
case 0x4B: WriteS( "StartFiler" ); break;
case 0x4C: WriteS( "StartedFiler" ); break;
case 0x4D: WriteS( "PreModeChange" ); break;
case 0x4E: WriteS( "MemoryMoved" ); break;
case 0x4F: WriteS( "FilerDying" ); break;
case 0x50: WriteS( "ModeExtension" ); break;
case 0x51: WriteS( "ModeTranslation" ); break;
case 0x52: WriteS( "MouseTrap" ); break;
case 0x53: WriteS( "WimpCloseDown" ); break;
case 0x54: WriteS( "Sound" ); break;
case 0x55: WriteS( "NetFS" ); break;
case 0x56: WriteS( "EconetDying" ); break;
case 0x57: WriteS( "WimpReportError" ); break;
case 0x58: WriteS( "MIDI" ); break;
case 0x59: WriteS( "ResourceFSStarted" ); break;
case 0x5A: WriteS( "ResourceFSDying" ); break;
case 0x5B: WriteS( "CalibrationChanged" ); break;
case 0x5C: WriteS( "WimpSaveDesktop" ); break;
case 0x5D: WriteS( "WimpPalette" ); break;
case 0x5E: WriteS( "MessageFileClosed" ); break;
case 0x5F: WriteS( "NetFSDying" ); break;
case 0x60: WriteS( "ResourceFSStarting" ); break;
case 0x61: WriteS( "NFS?" ); break;
case 0x62: WriteS( "DBoxModuleDying?" ); break;
case 0x63: WriteS( "DBoxModuleStarting?" ); break;
case 0x64: WriteS( "TerritoryManagerLoaded" ); break;
case 0x65: WriteS( "PDriverStarting" ); break;
case 0x66: WriteS( "PDumperStarting" ); break;
case 0x67: WriteS( "PDumperDying" ); break;
case 0x68: WriteS( "CloseFile: " ); Write0( (char*) regs->r[2] ); break;
case 0x69: WriteS( "IdentifyDisc" ); break;
case 0x6A: WriteS( "EnumerateFormats" ); break;
case 0x6B: WriteS( "IdentifyFormat" ); break;
case 0x6C: WriteS( "DisplayFormatHelp" ); break;
case 0x6D: WriteS( "ValidateAddress" ); break;
case 0x6E: WriteS( "FontsChanged" ); break;
case 0x6F: WriteS( "BufferStarting" ); break;
case 0x70: WriteS( "DeviceFSStarting" ); break;
case 0x71: WriteS( "DeviceFSDying" ); break;
case 0x72: WriteS( "SwitchingOutputToSprite" ); break;
case 0x73: WriteS( "PostInit" ); break;
case 0x74: WriteS( "BASICHelp?" ); break;
case 0x75: WriteS( "TerritoryStarted" ); break;
case 0x76: WriteS( "MonitorLeadTranslation" ); break;
case 0x77: WriteS( "SerialDevice?" ); break;
case 0x78: WriteS( "PDriverGetMessages" ); break;
case 0x79: WriteS( "DeviceDead" ); break;
case 0x7A: WriteS( "ScreenBlanked" ); break;
case 0x7B: WriteS( "ScreenRestored" ); break;
case 0x7C: WriteS( "DesktopWelcome" ); break;
case 0x7D: WriteS( "DiscDismounted" ); break;
case 0x7E: WriteS( "ShutDown" ); break;
case 0x7F: WriteS( "PDriverChanged" ); break;
case 0x80: WriteS( "ShutdownComplete" ); break;
case 0x81: WriteS( "DeviceFSCloseRequest" ); break;
case 0x82: WriteS( "InvalidateCache" ); break;
case 0x83: WriteS( "ProtocolDying" ); break;
case 0x84: WriteS( "FindNetworkDriver" ); break;
case 0x85: WriteS( "WimpSpritesMoved" ); break;
case 0x86: WriteS( "WimpRegisterFilters" ); break;
case 0x87: WriteS( "FilterManagerInstalled" ); break;
case 0x88: WriteS( "FilterManagerDying" ); break;
case 0x89: WriteS( "ModeChanging" ); break;
case 0x8A: WriteS( "Portable" ); break;
case 0x8B: WriteS( "NetworkDriverStatus" ); break;
case 0x8C: WriteS( "SyntaxError" ); break;
case 0x8D: WriteS( "EnumerateScreenModes" ); break;
case 0x8E: WriteS( "PagesUnsafe" ); break;
case 0x8F: WriteS( "PagesSafe" ); break;
case 0x90: WriteS( "DynamicAreaCreate" ); break;
case 0x91: WriteS( "DynamicAreaRemove" ); break;
case 0x92: WriteS( "DynamicAreaRenumber" ); break;
case 0x93: WriteS( "ColourPickerLoaded" ); break;
case 0x94: WriteS( "ModeFileChanged" ); break;
case 0x95: WriteS( "FreewayStarting" ); break;
case 0x96: WriteS( "FreewayTerminating" ); break;
case 0x97: WriteS( "ShareDStarting?" ); break;
case 0x98: WriteS( "ShareDTerminating?" ); break;
case 0x99: WriteS( "ModulePostInitialisation?" ); break;
case 0x9A: WriteS( "ModulePreFinalisation?" ); break;
case 0x9B: WriteS( "EnumerateNetworkDrivers?" ); break;
case 0x9C: WriteS( "PCMCIA?" ); break;
case 0x9D: WriteS( "DCIDriverStatus" ); break;
case 0x9E: WriteS( "DCIFrameTypeFree" ); break;
case 0x9F: WriteS( "DCIProtocolStatus" ); break;
case 0xA7: WriteS( "URI?" ); break;
case 0xB0: WriteS( "InternetStatus" ); break;
case 0xB7: WriteS( "UKCompression" ); break;
case 0xB9: WriteS( "ModulePreInit" ); break;
case 0xC3: WriteS( "PCI" ); break;
case 0xD2: WriteS( "USB" ); break;
case 0xD9: WriteS( "Hardware" ); break;
case 0xDA: WriteS( "ModulePostInit" ); break;
case 0xDB: WriteS( "ModulePostFinal" ); break;
case 0xDD: WriteS( "RTCSynchronised" ); break;
case 0xDE: WriteS( "DisplayChanged" ); break;
case 0xDF: WriteS( "DisplayStatus" ); break;
case 0xE0: WriteS( "NVRAM?" ); break;
case 0xE3: WriteS( "PagesUnsafe64" ); break;
case 0xE4: WriteS( "PagesSafe64" ); break;


case 0x10800: WriteS( "ADFSPodule" ); break;
case 0x10801: WriteS( "ADFSPoduleIDE" ); break;
case 0x10802: WriteS( "ADFSPoduleIDEDying" ); break;
case 0x20100: WriteS( "SCSIStarting" ); break;
case 0x20101: WriteS( "SCSIDying" ); break;
case 0x20102: WriteS( "SCSIAttached" ); break;
case 0x20103: WriteS( "SCSIDetached" ); break;
case 0x400C0: WriteS( "ErrorStarting?" ); break;
case 0x400C1: WriteS( "ErrorButtonPressed?" ); break;
case 0x400C2: WriteS( "ErrorEnding?" ); break;
case 0x41580: WriteS( "FindProtocols" ); break;
case 0x41581: WriteS( "FindProtocolsEnd" ); break;
case 0x41582: WriteS( "ProtocolNameToNumber" ); break;
case 0x45540: WriteS( "DrawObjectDeclareFonts" ); break;
case 0x45541: WriteS( "DrawObjectRender" ); break;
case 0x4D480: WriteS( "SafeAreaChanged?" ); break;
case 0x81080: WriteS( "TimeZoneChanged" ); break;
case 0x810C0: WriteS( "BootBootVarsSet?" ); break;
case 0x810C1: WriteS( "BootResourcesVarsSet?" ); break;
case 0x810C2: WriteS( "BootChoicesVarsSet?" ); break;
case 0x81100: WriteS( "IIC" ); break;

default: WriteNum( regs->r[1] );
}
NewLine;
}

static inline void show_module_commands( module_header *header )
{
  const char *cmd = module_commands( header );
  while (cmd[0] != '\0') {
    NewLine; Write0( cmd );
    int len = strlen( cmd );
    len = (len + 4) & ~3;
    cmd = &cmd[len + 16];
  }
  NewLine;
}

bool do_OS_ServiceCall( svc_registers *regs )
{
  bool result = true;
  module *m = workspace.kernel.module_list_head;
  uint32_t call = regs->r[1];

#ifdef DEBUG__SHOW_SERVICE_CALLS
int count = 0;
describe_service_call( regs );
WriteNum( call );
if (m == 0) {
  WriteS( "No modules initialised\n" ); NewLine;
}
#endif

  uint32_t r12 = regs->r[12];
  while (m != 0 && regs->r[1] != 0 && result) {
    regs->r[12] = (uint32_t) m->private_word;
    if (0 != m->header->offset_to_service_call_handler) {
#if DEBUG__SHOW_SERVICE_CALLS
//if (regs->r[1] == 0x46 || regs->r[1] == 0x73) {
{
Space; Write0( title_string( m->header ) ); Space; WriteNum( pointer_at_offset_from( m->header, m->header->offset_to_service_call_handler ) );
count++;
}
#endif
      result = run_service_call_handler_code( regs, m );

      assert( regs->r[1] == 0 || regs->r[1] == call );

      if (regs->r[1] == 0) {
#if DEBUG__SHOW_SERVICE_CALLS
        WriteS( "Claimed" );
#endif
        break;
      }
    }
    else {
#if DEBUG__SHOW_SERVICE_CALLS
      Space; Write0( title_string( m->header ) ); Space; WriteS( "No handler" );
#endif
    }
    m = m->next;
  }
#ifdef DEBUG__SHOW_SERVICE_CALLS
  NewLine; WriteS( "Passed to " ); WriteNum( count ); WriteS( " modules" ); NewLine;
#endif

  regs->r[12] = r12;

  return result;
}

static bool Unknown_OS_Module_call( svc_registers *regs )
{
  static error_block error = { 0x105, "Unknown OS_Module call" };
  regs->r[0] = (uint32_t)&error;
  return false;
}

static module *find_module( char const *name )
{
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
WriteS( "Looking for " ); Write0( name );
#endif
  module *m = workspace.kernel.module_list_head;
  int number = 0;
  while (m != 0 && !module_name_match( title_string( m->header ), name )) {
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
WriteS( ", not " ); Write0( title_string( m->header ) );
#endif
    m = m->next;
    number++;
  }

#ifdef DEBUG__SHOW_MODULE_LOOKUPS
if (m) { WriteS( ", FOUND " ); Write0( title_string( m->header ) ); NewLine; }
#endif
  return m;
}

static bool do_Module_Run( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

// Warning: Returns 0 if no file
static uint32_t read_file_size( const char *name )
{
  register uint32_t os_file_code asm( "r0" ) = 17;
  register const char *filename asm( "r1" ) = name;
  register uint32_t object_type asm( "r0" );
  register uint32_t word1 asm( "r2" );
  register uint32_t word2 asm( "r3" );
  register uint32_t file_size asm( "r4" );
  register uint32_t attributes asm( "r5" );
  asm volatile ( "svc %[swi]" 
      : "=r" (file_size) 
      , "=r" (object_type)
      , "=r" (word1)
      , "=r" (word2)
      , "=r" (attributes)
      : [swi] "i" (OS_File | Xbit) // FIXME Error return?
      , "r" (os_file_code)
      , "r" (filename)
      : "lr", "memory" );
  return object_type == 1 ? file_size : 0;
}

// Reads the size, allocates space, then loads the file after a word containing the size plus 4
uint32_t *read_module_into_memory( const char *name )
{
  uint32_t file_size = read_file_size( name );
WriteS( "File size = " ); WriteNum( file_size ); NewLine;

  if (file_size == 0) {
    return 0;
  }

  void *mem = rma_allocate( file_size + 4 );
WriteS( "Memory = " ); WriteNum( (uint32_t) mem ); NewLine;

  if (mem != 0)
  {
    uint32_t *words = mem;
    words[0] = file_size + 4;

    // FIXME: what happens if the file expands between asking its size and loading it?

    register uint32_t os_file_code asm( "r0" ) = 16;
    register const char *filename asm( "r1" ) = name;
    register void *load_address asm( "r2" ) = words + 1;
    register uint32_t load_at_r2 asm( "r3" ) = 0;
    asm volatile ( "svc %[swi]" : : [swi] "i" (OS_File), "r" (os_file_code), "r" (filename), "r" (load_address), "r" (load_at_r2) : "r4", "r5", "lr", "memory" );
  }

  return mem;
}

static module *new_module( module *base, module_header *m, const char *postfix )
{
  int len = postfix == 0 ? 1 : strlen( postfix ) + 1;
  module *instance = rma_allocate( sizeof( module ) + len );

  assert( base == 0 || base->header == m );

  if (instance != 0) {
    instance->header = m;
    instance->private_word = &instance->local_private_word;
    instance->local_private_word = 0;

    instance->next = 0;
    instance->base = base;
    instance->instances = 0;

    if (base != 0) {
      module **p = &base->instances;
      while (*p != 0) { p = &(*p)->next; }
      *p = instance;
    }

    if (len == 1) {
      instance->postfix[0] = 0;
    }
    else {
      for (int i = 0; i < len; i++) {
        instance->postfix[i] = postfix[i];
      }
    }
  }

  return instance;
}

static void pre_init_service( uint32_t *size_ptr )
{
  module_header *m = (void*) (size_ptr+1);
  uint32_t size = *size_ptr;

  // Service_MemoryMoved
  register module_header* header asm( "r0" ) = m;
  register uint32_t code asm( "r1" ) = 0xb9;
  register uint32_t length_plus_four asm( "r2" ) = size;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ServiceCall | Xbit)
      , "r" (code), "r" (header), "r" (length_plus_four)
      : "lr", "cc", "memory" );
}

static inline uint32_t bcd( char c )
{
  if (c < '0' || c > '9') c = '0';
  return c - '0';
}

static inline void Send_Service_ModulePostInit( uint32_t *memory, char const *postfix )
{
  module_header *m = (void*) (memory+1);
  char const *module_title = title_string( m );
  uint32_t bcd_version = 0;
  char const *vers = help_string( m );
  WriteS( "Post Init " ); Write0( vers ); NewLine; return;

  if (postfix[0] == '\0') postfix = 0;

  while (*vers != '\t' && *vers != '\0') vers++;
  while (*vers == '\t') vers++;
  while (*vers == ' ' && *vers != '\0') vers++;
  if (vers[1] == '.') {
    bcd_version = (bcd(vers[0]) << 8) | (bcd(vers[2]) << 4) | (bcd(vers[3]));
  }
  else asm ( "bkpt 1" );

  // Service_MemoryMoved
  register module_header* header asm( "r0" ) = m;
  register uint32_t code asm( "r1" ) = 0xda;
  register char const *title asm( "r2" ) = module_title;
  register char const *module_postfix asm( "r3" ) = postfix;
  register uint32_t bcd asm( "r4" ) = bcd_version;

  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ServiceCall | Xbit)
      , "r" (code), "r" (header), "r" (title), "r" (module_postfix), "r" (bcd)
      : "lr", "cc", "memory" );
}

static bool initialise_module( svc_registers *regs, uint32_t *memory, char const* parameters )
{
  // regs are only used to return errors. TODO

  // uint32_t size_plus_four = *memory;
  module_header *new_mod = (void*) (memory+1);

  TaskSlot_new_application( title_string( new_mod ), parameters );

  if (0 != (new_mod->offset_to_initialisation & (1 << 31))) {
    WriteS( "Is this module squashed? I can't cope with that." ); asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  }

  // "During initialisation, your module is not on the active module list, and
  // so you cannot call SWIs in your own SWI chunk."

  module *instance;
  module *shared_instance = 0;
  bool success = true;

  if (0 != new_mod->offset_to_initialisation) {
    // FIXME Does this still make sense? Does anyone patch ROM modules any more?
    pre_init_service( memory );
  }
  bool mp_module = mp_aware( new_mod );

  if (mp_module) {
    claim_lock( &shared.kernel.mp_module_init_lock );

    shared_instance = shared.kernel.module_list_head;
    while (shared_instance != 0 && shared_instance->header != new_mod) {
      shared_instance = shared_instance->next;
    }

    if (shared_instance == 0) {
      // No core has initialised this module, yet.
      // Store a copy in the shared list.
      shared_instance = new_module( 0, new_mod, 0 );

      if (shared_instance != 0) {
        if (shared.kernel.module_list_tail == 0) {
          shared.kernel.module_list_head = shared_instance;
        }
        else {
          shared.kernel.module_list_tail->next = shared_instance;
        }

        shared.kernel.module_list_tail = shared_instance;
      }
      else {
        success = error_nomem( regs );
      }
    }
  }

  if (success) {
    instance = new_module( 0, new_mod, 0 );
    success = instance != 0; 

    if (success && shared_instance != 0) {
      instance->private_word = shared_instance->private_word;
      while (instance->private_word != &shared_instance->local_private_word) {
        asm ( "bkpt 86" );
      }
    }

    if (success && 0 != new_mod->offset_to_initialisation) {
      success = run_initialisation_code( parameters, instance, 0 );
    }

    if (success) {
      if (workspace.kernel.module_list_tail == 0) {
        workspace.kernel.module_list_head = instance;
      }
      else {
        workspace.kernel.module_list_tail->next = instance;
      }

      workspace.kernel.module_list_tail = instance;
    }
  }

  if (mp_module) {
    release_lock( &shared.kernel.mp_module_init_lock );
  }

  if (success && 0 != new_mod->offset_to_initialisation) {
    // "This means that any SWIs etc provided by the module are available
    // (in contrast, during any service calls issued by the module’s own
    // initialisation code, the module is not yet linked into the chain)."
    Send_Service_ModulePostInit( memory, instance->postfix );
  }

  return success;
}

static bool do_Module_Load( svc_registers *regs )
{
  char const *command = (void*) regs->r[1];
  uint32_t *memory = read_module_into_memory( command );
  if (memory == 0) {
    static error_block err = { 0x888, "File not a module, or not found" };
    regs->r[0] = (uint32_t) &err;
    WriteS( "Tried to load module " ); Write0( command ); NewLine;
    return false;
  }

  char const *parameters = command;
  while (*++parameters > ' ') {}
  while (*parameters == ' ') { parameters++; }
  
  return initialise_module( regs, memory, parameters );
}

static bool do_Module_Enter( svc_registers *regs )
{
Write0( __func__ ); NewLine;
Write0( (char*) regs->r[1] ); NewLine;
Write0( (char*) regs->r[2] ); NewLine;

  // This routine should follow the procedure described in 1-235 including
  // making the upcall OS_FSControl 2 2-85

  // The caller expects to be replaced by the module

  char const *module_name = (void*) regs->r[1];
  char const *args = (void*) regs->r[2];
  module *m = find_module( module_name );

  if (m == 0) {
    static error_block error = { 0x185, "Module not found (TODO)" };
    regs->r[0] = (uint32_t)&error;
    return false;
  }

  // Found it.

  void *start_address = start_code( m->header );

WriteS( "Start address: " ); WriteNum( start_address ); NewLine;
  if (start_address == 0) {
    return true;
  }

  {
    register uint32_t reason asm ( "r0" ) = 256; // NewApplication UpCall
    register uint32_t allowed asm ( "r0" );      // Set to zero if not allowed
    asm volatile ( "svc 0x20033"
      : "=r" (allowed)
      : "r" (reason)
      : "lr" );
    if (!allowed) {
      asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); // FIXME ErrorBlock_CantStartApplication
    }
  }

  {
    register uint32_t service asm ( "r1" ) = 0x2a; // Service_NewApplication
    register uint32_t allowed asm ( "r1" );        // Set to zero if not allowed
    asm volatile ( "svc 0x20030"
      : "=r" (allowed)
      : "r" (service)
      : "lr" );
    if (!allowed) {
      asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); // FIXME ErrorBlock_CantStartApplication, but this time with feeling!
    }
  }

  TaskSlot_new_application( module_name, args );

  {
    register uint32_t handler asm ( "r0" ) = 15; // CAOPointer
    register void *address asm ( "r1" ) = start_address;
    asm volatile ( "svc 0x20040" // XOS_ChangeEnvironment
      : "=r" (address)  // clobbered, but can't go in the clobber list...
      , "=r" (handler)  // clobbered, but can't go in the clobber list...
      : "r" (handler)
      , "r" (address)
      : "r2", "r3", "lr" );
  }

  TaskSlot_enter_application( start_address, m->private_word );

  __builtin_unreachable();

  return false;
}


static bool do_Module_ReInit( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_Delete( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_DescribeRMA( svc_registers *regs )
{
  uint32_t r1 = regs->r[1];
  regs->r[0] = 1;
  regs->r[1] = (uint32_t) &rma_heap;
  bool result = do_OS_Heap( regs );
  if (result) {
    regs->r[0] = 5;
    regs->r[1] = r1;
  }
  return result;
}

static bool IntoRMAHeapOp( svc_registers *regs )
{
  uint32_t r0 = regs->r[0];
  uint32_t r1 = regs->r[1];
  uint32_t r2 = regs->r[2];
  uint32_t r3 = regs->r[3];

  regs->r[1] = (uint32_t) &rma_heap;

  bool result = do_OS_Heap( regs );
  if (result) {
    regs->r[0] = r0 == 2 ? 6 : 24; // Aligned, or not
    regs->r[1] = r1;
    regs->r[3] = r3;

#ifdef DEBUG__RMA_ALLOCATIONS
    WriteS( "Allocated RMA memory at " ); WriteNum( regs->r[2] ); WriteS( " @" ); WriteNum( regs->lr ); NewLine;
#endif
  }
  else {
    asm ( "bkpt 78" ); // TODO stuff with extending and r2
    r2 = r2;
    result = error_nomem( regs );
  }
  return result;
}

static bool do_Module_Claim( svc_registers *regs )
{
  uint32_t r1 = regs->r[1];
  uint32_t r3 = regs->r[3];

  regs->r[0] = 2;

  // s.ModHand, RMAClaim_Chunk
  // "now force size to 32*n-4 so heap manager always has 8-word aligned blocks"
  regs->r[3] = ((r3 + 31 + 4) & ~31) - 4;

  bool result = IntoRMAHeapOp( regs );
  if (result) regs->r[0] = 6;
  regs->r[1] = r1;
  regs->r[3] = r3;
  return result;
}

static bool do_Module_ClaimAligned( svc_registers *regs )
{
  if (regs->r[4] == 0 || 0 != (regs->r[4] & (regs->r[4]-1))) {
    static error_block const error = { 0x117, "Bad alignment request" };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

  uint32_t r1 = regs->r[1];
  uint32_t r4 = regs->r[4];

  regs->r[0] = 7;       // #HeapReason_GetAligned
  regs->r[2] = regs->r[4];
  regs->r[4] = 0;       // "any boundary"

  bool result = IntoRMAHeapOp( regs );
  if (result) regs->r[0] = 24;
  regs->r[1] = r1;
  regs->r[4] = r4;
  return result;
}

static bool do_Module_Free( svc_registers *regs )
{
register uint32_t reason asm( "r0" ) = 3;
register uint32_t *heap asm( "r1" ) = &rma_heap;
register uint32_t block asm( "r2" ) = regs->r[2];
asm ( "svc 0x2001d" : : "r" (reason), "r" (heap), "r" (block) : "lr" );
return true;
  uint32_t r1 = regs->r[1];
  regs->r[0] = 3; // Free
  regs->r[1] = (uint32_t) &rma_heap;

#ifdef DEBUG__RMA_ALLOCATIONS
  WriteS( "Free RMA memory at " ); WriteNum( regs->r[2] ); WriteS( " @" ); WriteNum( regs->lr ); NewLine;
#endif

  bool result = do_OS_Heap( regs );
  if (result) {
    regs->r[0] = 7;
    regs->r[1] = r1;
  }

  return result;
}

static bool do_Module_Tidy( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_Clear( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static uint32_t instance_number( module *m )
{
  if (m->base == 0) return 0;
  uint32_t result = 1;
  module *i = m->instances;
  while (i != 0 && i != m) { i = i->next; result++; }

  assert (i == 0); // Not an instance of its base?

  return result;
}

static bool do_Module_InsertFromMemory( svc_registers *regs )
{
#ifdef DEBUG__SHOW_MODULE_INIT
  module_header *header = (void*) regs->r[1];

  NewLine;
  WriteS( "INIT: " );
  Write0( title_string( header ) ); NewLine;
#ifdef DEBUG__SHOW_MODULE_COMMANDS_ON_INIT
  show_module_commands( header );
#endif
#endif

  return initialise_module( regs, (void*) (regs->r[1] - 4), "" );
}

static bool do_Module_InsertAndRelocateFromMemory( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_ExtractModuleInfo( svc_registers *regs )
{
  int mod = regs->r[1];
  module *m = workspace.kernel.module_list_head;
  while (mod-- > 0 && m != 0) {
    m = m->next;
  }

  if (m == 0) {
    return Kernel_Error_NoMoreModules( regs );
  }

  int instance = regs->r[2];

  if (m->instances != 0) {
    if (instance > 0) {
      m = m->instances;
WriteS( "Instance " ); WriteNum( instance ); Space; WriteNum( m ); NewLine;
      while (--instance > 0 && m != 0) {
        m = m->next;
WriteNum( instance ); Space; WriteNum( m ); NewLine;
      }
    }

    if (m == 0) {
      return Kernel_Error_NoMoreIncarnations( regs );
    }

    if (m->next == 0) {
      regs->r[2] = 0;
    }
    else {
      regs->r[2] ++;
    }
  }
  else {
    regs->r[1] ++;
  }

  regs->r[3] = (uint32_t) m->header;
  regs->r[4] = *m->private_word;
  regs->r[5] = (m->postfix[0] == '\0' ? (uint32_t) "Base" : (uint32_t) &m->postfix);

  return true;
}

static bool do_Module_ExtendBlock( svc_registers *regs )
{
  uint32_t r1 = regs->r[1];
  regs->r[0] = 4; // Change the size of a block
  regs->r[1] = (uint32_t) &rma_heap;

  bool result = do_OS_Heap( regs );
  if (result) {
    regs->r[0] = 13;
    regs->r[1] = r1;
  }

  return result;
}

static bool do_Module_CreateNewInstantiation( svc_registers *regs )
{
  char const *module_name = (void*) regs->r[1];

  module *m = find_module( module_name );

  if (m == 0) {
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  }
Write0( __func__ ); Space; Write0( title_string( m->header ) ); NewLine;
Write0( __func__ ); Space; Write0( module_name ); NewLine;
Write0( __func__ ); Space; WriteNum( module_name ); NewLine;

  char const *extension = module_name;
  while (*extension != '%' && *extension > ' ') extension++;

  if (*extension != '%') {
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  }

Write0( __func__ ); Space; Write0( extension ); NewLine;
Write0( __func__ ); Space; WriteNum( extension ); NewLine;
  extension ++;

  char const *parameters = extension;
  while (*parameters > ' ') { parameters++; } // FIXME Tabs?
  while (*parameters == ' ') { parameters++; } // FIXME Tabs?

Write0( __func__ ); Space; Write0( parameters ); NewLine;
Write0( __func__ ); Space; WriteNum( parameters ); NewLine;

  module *instance = new_module( m, m->header, extension );

  bool success = (instance != 0);

  if (success && 0 != instance->header->offset_to_initialisation) {
#if 1
    WriteS( "Passing parameters " ); Write0( parameters ); WriteS( " to new instance" ); NewLine;
#endif
    success = run_initialisation_code( parameters, instance, instance_number( instance ) );
  }
  else {
#if 1
    WriteS( "Not passing parameters " ); Write0( parameters ); WriteS( " to new instance" ); NewLine;
#endif
  }

  if (success) {
    if (workspace.kernel.module_list_tail == 0) {
      workspace.kernel.module_list_head = instance;
    }
    else {
      workspace.kernel.module_list_tail->next = instance;
    }

    workspace.kernel.module_list_tail = instance;
  }

  return success;
}

static bool do_Module_RenameInstantiation( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_MakePreferredInstantiation( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_AddExpansionCardModule( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_LookupModuleName( svc_registers *regs )
{
  // Actually Lookup Module BY Name
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
Write0( __func__ ); WriteS( " " ); Write0( regs->r[1] );
#endif

  // Initially called by Wimp during init, just to find ROM location
  // Checking the whole module name, including any extension.

  const char *name = (void*) regs->r[1];
  const char *extension = name;
  char c;
  do {
    c = *extension++;
  } while (c != '%' && c > ' ');
  if (c != '%') extension = 0;

  // Not calling find_module, want the number as well...
  module *m = workspace.kernel.module_list_head;
  int number = 0;
  while (m != 0 && !module_name_match( title_string( m->header ), name )) {
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
WriteS( ", not " ); Write0( title_string( m->header ) );
#endif
    m = m->next;
    number++;
  }

  uint32_t instance = 0;

  if (m != 0 && m->instances != 0) {
    if (extension != 0 && !riscoscmp( "Base", extension )) {
      instance++; // First non-base instance is number 1
      m = m->instances;
      while (m != 0 && !riscoscmp( m->postfix, extension )) {
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
WriteS( ", not " ); Write0( title_string( m->header ) ); WriteS( "%" ); Write0( m->postfix );
#endif
        m = m->next;
        instance++;
      }
    }
  }

  if (m == 0) {
    // TODO personalised error messages will have to be stored associated with a task
Write0( __func__ ); WriteS( " " ); Write0( regs->r[1] );
    static error_block error = { 258, "Module not found" }; // FIXME "Module %s not found"
    regs->r[0] = (uint32_t) &error;
    return false;
  }
  else {
    regs->r[1] = number;
    regs->r[2] = instance;
    regs->r[3] = (uint32_t) m->header;
    regs->r[4] = (uint32_t) m->private_word;
    regs->r[5] = m->postfix[0] == '\0' ? 0 : (uint32_t) &m->postfix;
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
WriteS( ", found: " ); Write0( title_string( m->header ) ); NewLine;
#endif
  }

  return true;
}

static int module_state( module_header *header )
{
  module *m = workspace.kernel.module_list_head;

  while (m != 0 && m->header != header) {
    m = m->next;
  }

  if (m != 0) {
    return 1; // FIXME: Difference between active and running?
  }

  return 0; // Dormant
}

static bool do_Module_EnumerateROMModules( svc_registers *regs )
{
  int n = regs->r[1];
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  for (int i = 0; i < n && 0 != *rom_module; i++) {
    rom_module += (*rom_module)/4; // Includes size of length field
  }

  if (0 == *rom_module) {
    return Kernel_Error_NoMoreModules( regs );
  }

  module_header *header = (void*) (rom_module+1);
  regs->r[1] = n + 1;
  regs->r[2] = -1;
  regs->r[3] = (uint32_t) title_string( header );
  regs->r[4] = (uint32_t) module_state( header );
  regs->r[5] = 0; // Chunk number

  return true;
}

static bool do_Module_EnumerateROMModulesWithVersion( svc_registers *regs )
{
  int n = regs->r[1];
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  for (int i = 0; i < n && 0 != *rom_module; i++) {
    rom_module += (*rom_module)/4; // Includes size of length field
  }

  if (0 == *rom_module) {
    return Kernel_Error_NoMoreModules( regs );
  }

  // FIXME WithVersion!
  module_header *header = (void*) (rom_module+1);
  regs->r[1] = n + 1;
  regs->r[2] = -1;
  regs->r[3] = (uint32_t) title_string( header );
  regs->r[4] = (uint32_t) module_state( header );
  regs->r[5] = 0; // Chunk number

  return true;
}

static bool do_Module_FindEndOfROM_ModuleChain( svc_registers *regs )
{
  int n = regs->r[1];
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  for (int i = 0; i < n && 0 != *rom_module; i++) {
    rom_module += (*rom_module)/4; // Includes size of length field
  }

  regs->r[2] = 4 + (uint32_t) rom_module;

  return true;
}

bool do_OS_Module( svc_registers *regs )
{
  enum { Run, Load, Enter, ReInit, Delete, DescribeRMA,
         Claim, Free, Tidy, Clear, InsertFromMemory,
         InsertAndRelocateFromMemory, ExtractModuleInfo,
         ExtendBlock, CreateNewInstantiation, RenameInstantiation,
         MakePreferredInstantiation, AddExpansionCardModule,
         LookupModuleName, EnumerateROMModules, EnumerateROMModulesWithVersion,
         FindEndOfROM_ModuleChain, 
         Enumerate_modules_with_private_word_pointer,
         Unplug_or_insert_modules,
         ClaimAligned };

  switch (regs->r[0]) {
  case Run: return do_Module_Run( regs );
  case Load: return do_Module_Load( regs );
  case Enter: return do_Module_Enter( regs );
  case ReInit: return do_Module_ReInit( regs );
  case Delete: return do_Module_Delete( regs );
  case DescribeRMA: return do_Module_DescribeRMA( regs );
  case Claim: return do_Module_Claim( regs );
  case Free: return do_Module_Free( regs );
  case Tidy: return do_Module_Tidy( regs );
  case Clear: return do_Module_Clear( regs );
  case InsertFromMemory: return do_Module_InsertFromMemory( regs );
  case InsertAndRelocateFromMemory: return do_Module_InsertAndRelocateFromMemory( regs );
  case ExtractModuleInfo: return do_Module_ExtractModuleInfo( regs );
  case ExtendBlock: return do_Module_ExtendBlock( regs );
  case CreateNewInstantiation: return do_Module_CreateNewInstantiation( regs );
  case RenameInstantiation: return do_Module_RenameInstantiation( regs );
  case MakePreferredInstantiation: return do_Module_MakePreferredInstantiation( regs );
  case AddExpansionCardModule: return do_Module_AddExpansionCardModule( regs );
  case LookupModuleName: return do_Module_LookupModuleName( regs );
  case EnumerateROMModules: return do_Module_EnumerateROMModules( regs );
  case EnumerateROMModulesWithVersion: return do_Module_EnumerateROMModulesWithVersion( regs );
  case FindEndOfROM_ModuleChain: return do_Module_FindEndOfROM_ModuleChain( regs );
  case Enumerate_modules_with_private_word_pointer:
    return Unknown_OS_Module_call( regs );
  case Unplug_or_insert_modules:
    return Unknown_OS_Module_call( regs );
  case ClaimAligned: return do_Module_ClaimAligned( regs );
  default:
    NewLine; WriteNum( regs->r[0] );
    return Unknown_OS_Module_call( regs );
  }
}

bool do_OS_CallAVector( svc_registers *regs )
{
  if (regs->r[9] > number_of( workspace.kernel.vectors )) {
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  }
  return run_vector( regs, regs->r[9] );
}

static bool error_InvalidVector( svc_registers *regs )
{
  static error_block error = { 0x998, "Invalid vector number #" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static inline bool equal_callback( callback *a, callback *b )
{
  assert( a != 0 && b != 0 );
  return (a->code == b->code) && (a->private_word == b->private_word);
}

bool do_OS_Claim( svc_registers *regs )
{
#ifdef DEBUG__SHOW_VECTORS
  WriteS( "New vector claim " ); WriteNum( regs->r[0] );
  WriteS( " Code " ); WriteNum( regs->r[1] );
  WriteS( " Private " ); WriteNum( regs->r[2] ); NewLine;
#endif

// FIXME FIXME FIXME FIXME: total internal knowlege
workspace.kernel.frame_buffer_initialised = 1;

  int number = regs->r[0];
  if (number > number_of( workspace.kernel.vectors )) {
    return error_InvalidVector( regs );
  }

  vector **p = &workspace.kernel.vectors[number];
  assert( *p != 0 );

  vector cb = { .code = regs->r[1], .private_word = regs->r[2] };
  assert( equal_callback( &cb, &cb ) );

  vector *found = mpsafe_find_and_remove_callback( p, &cb, equal_callback );

  vector *new;

  if (found != 0) {
    // Duplicate to be removed, except we'll just move it up to the head instead,
    // without having to allocate new space.
    new = found;
  }
  else {
    new = callback_new( &shared.kernel.callbacks_pool );
    if (new == 0) {
      return error_nomem( regs );
    }

    new->code = regs->r[1];
    new->private_word = regs->r[2];
  }

  assert( new->code == regs->r[1] && new->private_word == regs->r[2] );

  mpsafe_insert_callback_at_head( p, new );

  assert( *p == new );

#ifdef DEBUG__SHOW_VECTORS
  WriteS( "New new vector" ); NewLine;
#endif

  return true;
}

bool do_OS_Release( svc_registers *regs )
{
  int number = regs->r[0];
  if (number > number_of( workspace.kernel.vectors )) {
    return error_InvalidVector( regs );
  }

  vector **p = &workspace.kernel.vectors[number];

  vector cb = { .code = regs->r[1], .private_word = regs->r[2] };

  vector *found;

  do {
    found = mpsafe_find_and_remove_callback( p, &cb, equal_callback );
    if (found != 0)
      mpsafe_insert_callback_at_tail( &shared.kernel.callbacks_pool, found );
  } while (found != 0);

  if (found != 0) return true;

  static error_block not_there = { 0x1a1, "Bad vector release" };
  regs->r[0] = (uint32_t) &not_there;
  return false;
}

bool do_OS_AddToVector( svc_registers *regs )
{
  asm ( "bkpt 0x1999" );
  return Kernel_Error_UnknownSWI( regs );
}
bool do_OS_DelinkApplication( svc_registers *regs )
{
  return Kernel_Error_UnknownSWI( regs );
}
bool do_OS_RelinkApplication( svc_registers *regs )
{
  return Kernel_Error_UnknownSWI( regs );
}

static void __attribute__(( noinline )) default_os_byte_c( uint32_t *regs )
{
#ifdef DEBUG__SHOW_OS_BYTE
  WriteS( "OS_Byte " ); WriteNum( regs[0] ); NewLine;
#endif

  switch (regs[0]) {
  case 0x00: // Display OS version or return machine type
    {
      static error_block version = { 0, "RISC OS, C Kernel 0.01 ("__DATE__")" };
      if (regs[1] == 0) {
        regs[0] = (uint32_t) &version;
        set_VF();
      }
      else {
        regs[1] = 6;
      }
    }
    break;
  case 0x04: // Write cursor key status
    {
#ifdef DEBUG__SHOW_OS_BYTE
      WriteS( "Write Cursor Key State " ); WriteNum( regs[1] );
#endif
      regs[1] = 0;
    }
    break;
  case 0x09: // Duration of first colour
    {
    }
    break;
  case 0x0a: // Duration of second colour
    {
    }
    break;
  case 0x0d: // Disable Event
    {
      uint32_t event = regs[1];
      if (event < number_of( workspace.kernel.event_enabled )) {
        if (workspace.kernel.event_enabled[event] != 0)
          regs[1] = workspace.kernel.event_enabled[event] --;
        else
          regs[1] = 0;
      }
      else {
        regs[1] = 255; // Observed behaviour
        asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
      }
    }
    break;
  case 0x0e: // Enable Event
    {
      uint32_t event = regs[1];
      if (event < number_of( workspace.kernel.event_enabled )) {
        regs[1] = workspace.kernel.event_enabled[event] ++;
      }
      else {
        regs[1] = 255; // Observed behaviour
        asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
      }
    }
    break;
  case 0x15: // Clear selected buffer
    {
      WriteS( "Flush buffer " ); WriteNum( regs[1] );
    }
    break;
  case 0x47: // Read/Write alphabet or keyboard
    {
    switch (regs[1]) {
    case 127: // Read alphabet
      regs[2] = 1; break;
    case 255: // Read keyboard
      regs[2] = 1; break;
    default:
      WriteS( "Setting alphabet/keyboard not supported" );
    }
    }
    break;
  case 0x6a: // Select pointer/activate mouse
    {
    }
    break;
  case 0x72: // Set shadow state 0 = shadow, 1 = non-shadow
    {
    }
    break;
  case 0x75: // Read VDU status
    {
      regs[1] = 0;
    }
    break;
  case 0x7c: // Clear escape condition
    {
    }
    break;
  case 0xa1:
    {
#ifdef DEBUG__SHOW_OS_BYTE
    WriteS( "Read CMOS " ); WriteNum( regs[1] );
#endif
    switch (regs[1]) {

    // No loud beep, scrolling allowed, no boot from disc, serial data format code 0
    // Read from UK territory module
    case 0x10: regs[2] = 0; break;

    // Unplugged flags
    case 0x6:
    case 0x7:
    case 0x12 ... 0x15:
      regs[2] = 0; break;
    
    // WimpDoubleClickMove Limit
    case 0x16: regs[2] = 5; break; // FIXME made up!

    // WimpAutoMenuDelay time
    case 0x17: regs[2] = 50; break; // FIXME made up!

    // UK Territory (encoded)
    case 0x18: regs[2] = 1 ^ 1; break;

    // Wimp menu drag delay
    case 0x1b: regs[2] = 50; break; // FIXME made up!

    // FileSwitch options
    case 0x1c: regs[2] = 0b00000010; break; // FIXME made up!

    case 0x84: regs[2] = 0xa4; break;   // FIXME from real hardware
    case 0x85: regs[2] = 0x40; break;   // FIXME from real hardware

    // Font Cache, pages (see also 0xc8-0xcd
    case 0x86: regs[2] = 64; break; // 4KiB pages = 256KiB

    // Time zone (15 mins as signed)
    case 0x8b: regs[2] = 0; break;

    // Desktop features
    //case 0x8c: regs[2] = 0x11; break; // From real hardware
    case 0x8c: regs[2] = 0x91; break; // RO2-style, avoiding problem of not finding tile_6* sprite

    // Screen size (pages)
    case 0x8f: regs[2] = (1920 * 1080 + 4095) >> 12; break;

#ifdef USE_ROM_OSBYTE_VARS
    case 0xa6 ... 0xff:
      {
        extern uint8_t ByteVarInitTable;
        uint8_t *table = &ByteVarInitTable;
        regs[2] = table[regs[1] - 0xa6];
        break;
      }
#else
    case 0xb9: regs[2] = 11; break; // Language
    // Desktop features
    case 0xbc: regs[2] = 0x1; break; // FIXME made up: opt 4, 1

    // Wimp Flags
    case 0xc5: regs[2] = 0x6f; break; // FIXME

    // FontMax, FontMax1-5
    // case 0xc8 ... 0xcd: regs[2] = 32; break;
    case 0xc8: regs[2] = 64; break; // 4KiB pages = 256k
    case 0xc9: regs[2] = 0; break; // 0 => no x90y45?
    case 0xca: regs[2] = 36; break;
    case 0xcb: regs[2] = 36; break;
    case 0xcc: regs[2] = 16; break;
    case 0xcd: regs[2] = 12; break;

    // Alarm flags/DST ???
    case 0xdc: regs[2] = 0; break;

    // WimpDragDelayTime
    case 0xdd: regs[2] = 20; break; // FIXME made up!

    // WimpDragMoveLimit
    case 0xde: regs[2] = 20; break; // FIXME made up!

    // WimpDoubleClickDelayTime
    case 0xdf: regs[2] = 50; break; // FIXME made up!
#endif
    default: WriteS( " CMOS byte " ); WriteNum( regs[1] ); asm ( "bkpt 61" );
    }
#ifdef DEBUG__SHOW_OS_BYTE
    WriteS( " = " ); WriteNum( regs[2] );
#endif
    }
    break;
  case 0xa2:
    {
#ifdef DEBUG__SHOW_OS_BYTE
    WriteS( "Write CMOS " ); WriteNum( regs[1] ); WriteS( " " ); WriteNum( regs[2] );
#endif
    switch (regs[1]) {
    case 0x10: WriteS( "Misc flags" ); break;
    default: asm ( "bkpt 71" );
    }
    }
    break;
  case 0xa8 ... 0xff:
    {
#ifdef DEBUG__SHOW_OS_BYTE
    if (regs[1] == 0 && regs[2] == 255) {
      WriteS( " read " );
    }
    else if (regs[2] == 0) {
      WriteS( " write " ); WriteNum( regs[1] );
    }
    else {
      WriteS( " " ); WriteNum( regs[1] );
      WriteS( " " ); WriteNum( regs[2] );
    }
#endif
    // All treated the same, a place for storing a byte.
    // "; All calls &A8 to &FF are implemented together."
    // "; <NEW VALUE> = (<OLD VALUE> AND R2 ) EOR R1"
    // "; The old value is returned in R1 and the next location is returned in R2"
    // Kernel/s/PMF/osbyte

    uint8_t *v = ((uint8_t*) &workspace.vectors.zp.OsbyteVars) - 0xa6 + regs[0];
    regs[1] = *v;
    *v = ((*v) & regs[2]) ^ regs[1];

    switch (regs[0]) {
#ifdef DEBUG__SHOW_OS_BYTE
    case 0xc6: WriteS( " Exec handle" ); break;
    case 0xc7: WriteS( " Spool handle" ); break;

    // Called by Wimp02 fn: resetkeycodes *fx 221,2 - fx 228,2, etc.
    // TODO make this the default and provide a compatibility layer for old code
    case 0xdb: WriteS( " Tab key code" ); break;
    case 0xdc: WriteS( " Escape character" ); break;
    case 0xdd ... 0xe4: WriteS( " input values interpretation" ); break;
    case 0xe5: WriteS( " Escape key status" ); break;
#else
    case 0xc6: break;
    case 0xc7: break;
    case 0xdb ... 0xe5: break;
#endif
    default: asm( "bkpt 81" ); // Catch used variables I haven't identified yet
    }
    }
    break;
  case 0x81: // Scan keyboard/read OS version (two things that are made for each other!)
    {
      if (regs[2] == 0xff) {
        if (regs[1] == 0) {
          WriteS( "OS Version number" ); NewLine;
          regs[1] = 171;
        }
        else if (regs[1] <= 0x7f) {
          WriteS( "Scan for range of keys " ); WriteNum( regs[1] ); NewLine;
          regs[1] = 0xff;       // No key (no keyboard!)
        }
        else {
          WriteS( "Scan for particular key " ); WriteNum( regs[1] ); NewLine;
          regs[1] = 0xff;       // No key (no keyboard!)
        }
      }
      else if (regs[2] <= 0x7f) {
        WriteS( "Scan keyboard with timeout." ); NewLine;
        WriteNum( 10 * ((regs[2] << 8) | regs[1]) ); // FIXME This needs to start a sleep, or the caller needs to be fixed, somehow... Wimp calls this regularly
        regs[2] = 0xff;       // Timeout (no keyboard!)
      }
      else {
        WriteS( "Unknown OS_Byte option!" ); NewLine;
        asm ( "bkpt 90" );
      }
    }
    break;
  default: asm ( "bkpt 91" );
  }
#ifdef DEBUG__SHOW_OS_BYTE
  NewLine;
#endif
}

static void __attribute__(( naked )) default_os_byte()
{
  // Always intercepting because there's no lower call.
  register uint32_t *regs;
  asm ( "push { "C_CLOBBERED" }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  default_os_byte_c( regs );

  asm ( "pop { "C_CLOBBERED", pc }" );
}

static void __attribute__(( noinline )) default_os_word_c( uint32_t *regs )
{
#ifdef DEBUG__SHOW_OS_WORD
  WriteS( "OS_Word " ); WriteNum( regs[0] ); NewLine;
#endif

  switch (regs[0]) {
  case 0x00:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x01:
    {
    uint8_t *t = (void*) regs[1];
    t[0] = workspace.vectors.zp.EnvTime[0];
    t[1] = workspace.vectors.zp.EnvTime[1];
    t[2] = workspace.vectors.zp.EnvTime[2];
    t[3] = workspace.vectors.zp.EnvTime[3];
    t[4] = workspace.vectors.zp.EnvTime[4];
workspace.vectors.zp.EnvTime[0]++;
    }
    break;
  case 0x02:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x03:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x04:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x05:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x06:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x07:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x08:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x09:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x0a:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x0b:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x0c:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x0d:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x0e:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x0f:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x10:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x11:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x12:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x13:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x14:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  case 0x15:
    {
      WriteS( "Define pointer and mouse parameters: TODO\n" );
      char *block = (void*) regs[1];
      WriteNum( block[0] ); NewLine;
      switch (block[0]) {
      case 0: break; // Define pointer size, shape, and active point
      case 1: break; // Define Mouse coordinates bounding box
      case 2: break; // Define Mouse multipliers
      case 3: break; // Set Mouse position
      case 4: block[1] = 0; block[2] = 0; block[3] = 0; block[4] = 0; break; // Get unbuffered Mouse position
      case 5: break; // Set pointer position
      case 6: block[1] = 0; block[2] = 0; block[3] = 0; block[4] = 0; break; // Get pointer position
      default: asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
      }
    }
    break;
  case 0x16:
    {
    asm( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  default: asm ( "bkpt 91" );
  }
#ifdef DEBUG__SHOW_OS_WORD
  NewLine;
#endif
}

static void __attribute__(( naked )) default_os_word()
{
  // Always intercepting because there's no lower call.
  register uint32_t *regs;
  asm ( "push { "C_CLOBBERED" }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  default_os_word_c( regs );

  asm ( "pop { "C_CLOBBERED", pc }" );
}

#ifdef DEBUG__SHOW_VECTOR_CALLS
#define WriteFunc do { Write0( __func__ ); NewLine; for (int i = 0; i < 13; i++) { WriteNum( regs->r[i] ); Space; } WriteNum( regs->lr ); Space; WriteNum( regs->spsr ); NewLine; } while (false)
#else
#define WriteFunc
#endif

bool do_OS_GenerateError( svc_registers *regs )
{
WriteFunc; Write0( regs->r[0] + 4 ); NewLine;
  return run_vector( regs, 1 );
}

bool do_OS_WriteC( svc_registers *regs )
{
  return run_vector( regs, 3 );
}

bool do_OS_ReadC( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 4 );
}

bool do_OS_CLI( svc_registers *regs )
{
  return run_vector( regs, 5 );
}
/*
WriteFunc;
WriteS( "OS_CLI: " ); WriteNum( regs->r[0] ); Space; Write0( (void*) regs->r[0] ); NewLine;
WriteS( "Caller: " ); WriteNum( regs->lr ); NewLine;
  // Check stack space TODO
  // Check command length TODO (still 256?)
  // /SetECF

  return run_vector( regs, 5 );
}
*/

bool do_OS_Byte( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 6 );
}

bool do_OS_Word( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 7 );
}

bool do_OS_GenerateEvent( svc_registers *regs )
{
WriteFunc;
  uint32_t event = regs->r[0];
  if (event < number_of( workspace.kernel.event_enabled )) {
    if (workspace.kernel.event_enabled[event] != 0)
      return run_vector( regs, 16 );
  }
  return true;
}

bool do_OS_Mouse( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 26 );
}

bool do_OS_UpCall( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 29 );
}

bool do_OS_ChangeEnvironment( svc_registers *regs )
{
#ifdef DEBUG__SHOW_ENVIRONMENT_CHANGES
Write0( __func__ ); Space; WriteNum( regs->lr ); NewLine;
#endif
  return run_vector( regs, 30 );
}

bool do_OS_SpriteOp( svc_registers *regs )
{
WriteFunc;
if (regs->r[0] == 0x118) { WriteS( "Select sprite " ); Write0( regs->r[2] ); NewLine; WriteNum( regs->lr ); NewLine; }
  return run_vector( regs, 31 );
}

bool do_OS_SerialOp( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 36 );
}

static char const *const unknown = "Unknown";

static char const *const os_swi_names[256] = {
"WriteC",
"WriteS",
"Write0",
"NewLine",
"ReadC",
"CLI",
"Byte",
"Word",
"File",
"Args",
"BGet",
"BPut",
"GBPB",
"Find",
"ReadLine",
"Control",
"GetEnv",
"Exit",
"SetEnv",
"IntOn",
"IntOff",
"CallBack",
"EnterOS",
"BreakPt",
"BreakCtrl",
"UnusedSWI",
"UpdateMEMC",
"SetCallBack",
"Mouse",
"Heap",
"Module",
"Claim",

"Release",
"ReadUnsigned",
"GenerateEvent",
"ReadVarVal",
"SetVarVal",
"GSInit",
"GSRead",
"GSTrans",
"BinaryToDecimal",
"FSControl",
"ChangeDynamicArea",
"GenerateError",
"ReadEscapeState",
"EvaluateExpression",
"SpriteOp",
"ReadPalette",
"ServiceCall",
"ReadVduVariables",
"ReadPoint",
"UpCall",
"CallAVector",
"ReadModeVariable",
"RemoveCursors",
"RestoreCursors",
"SWINumberToString",
"SWINumberFromString",
"ValidateAddress",
"CallAfter",
"CallEvery",
"RemoveTickerEvent",
"InstallKeyHandler",
"CheckModeValid",

"ChangeEnvironment",
"ClaimScreenMemory",
"ReadMonotonicTime",
"SubstituteArgs",
"PrettyPrint",
"Plot",
"WriteN",
"AddToVector",
"WriteEnv",
"ReadArgs",
"ReadRAMFsLimits",
"ClaimDeviceVector",
"ReleaseDeviceVector",
"DelinkApplication",
"RelinkApplication",
"HeapSort",
"ExitAndDie",
"ReadMemMapInfo",
"ReadMemMapEntries",
"SetMemMapEntries",
"AddCallBack",
"ReadDefaultHandler",
"SetECFOrigin",
"SerialOp",

"ReadSysInfo",
"Confirm",
"ChangedBox",
"CRC",
"ReadDynamicArea",
"PrintChar",
"ChangeRedirection",
"RemoveCallBack",

"FindMemMapEntries",
"SetColour",
unknown,
unknown,
"Pointer",
"ScreenMode",
"DynamicArea",
unknown,
"Memory",
"ClaimProcessorVector",
"Reset",
"MMUControl",
"ResyncTime",
"PlatformFeatures",
"SynchroniseCodeAreas",
"CallASWI",
"AMBControl",
"CallASWIR12",
"SpecialControl",
"EnterUSR32",
"EnterUSR26",
"VIDCDivider",
"NVMemory",
unknown,
unknown,
unknown,
"Hardware",
"IICOp",
"LeaveOS",
"ReadLine32",
"SubstituteArgs32",
"HeapSort32",
 
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,

unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,

"ConvertStandardDateAndTime",
"ConvertDateAndTime",
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
"ConvertHex1",
"ConvertHex2",
"ConvertHex4",
"ConvertHex6",
"ConvertHex8",
"ConvertCardinal1",
"ConvertCardinal2",
"ConvertCardinal3",
"ConvertCardinal4",
"ConvertInteger1",
"ConvertInteger2",
"ConvertInteger3",
"ConvertInteger4",
"ConvertBinary1",
"ConvertBinary2",
"ConvertBinary3",
"ConvertBinary4",
"ConvertSpacedCardinal1",
"ConvertSpacedCardinal2",
"ConvertSpacedCardinal3",
"ConvertSpacedCardinal4",
"ConvertSpacedInteger1",
"ConvertSpacedInteger2",
"ConvertSpacedInteger3",
"ConvertSpacedInteger4",
"ConvertFixedNetStation",
"ConvertNetStation",
"ConvertFixedFileSize",
"ConvertFileSize",
unknown,
unknown,
unknown,

unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
unknown,
"MSTime",
"ThreadOp",
"PipeOp",
"VduCommand",
"LockForDMA",
"ReleaseDMALock",
"MapDevicePages",
"FlushCache"
};
 
bool do_OS_SWINumberFromString( svc_registers *regs )
{
  WriteS( __func__ ); Space; Write0( regs->r[1] ); NewLine;

  // String is terminated by any character <= ' '
  char const * const whole_string = (void*) regs->r[1];
  char const * const string = whole_string + (whole_string[0] == 'X' ? 1 : 0);
  uint32_t x_bit = (string == whole_string) ? 0 : 0x20000;
  WriteS( __func__ ); Space; Write0( string ); NewLine;
  WriteS( __func__ ); Space; Write0( whole_string ); NewLine;

  // FIXME: X prefix. Does it exclude SWI names starting with an X?

  module *m = workspace.kernel.module_list_head;

  bool kernel_swi = string[0] == 'O' && string[1] == 'S' && string[2] == '_';

  if (kernel_swi) {
    // FIXME: OS_WriteI+nn?
#if DEBUG__SWI_NUMBER_FROM_STRING
    Write0( string+3 ); NewLine;
#endif
    for (int i = 0; i < number_of( os_swi_names ); i++) {
      char const *matched = string + 3;
      char const *name = os_swi_names[i];
#if DEBUG__SWI_NUMBER_FROM_STRING
      Write0( name ); NewLine;
#endif
      while (*matched == *name && *name != '\0') {
        matched++;
        name++;
      }
      if (*matched <= ' ' && *name == '\0') {
#if DEBUG__SWI_NUMBER_FROM_STRING
        WriteS( "Match: " ); WriteNum( i ); NewLine;
#endif
        regs->r[0] = i | x_bit;
        return true;
      }
    }

    return Kernel_Error_SWINameNotKnown( regs );
  }

  // TODO SWI Decoder code
  while (m != 0) {
#if DEBUG__SWI_NUMBER_FROM_STRING
WriteNum( m->header ); Space; Write0( title_string( m->header ) ); Space;
WriteNum( swi_decoding_code( m->header ) ); Space; WriteNum( swi_decoding_table( m->header ) );
#endif
    if (0 == m->header->swi_chunk) {
      // No SWIs
    }
    else if (swi_decoding_table( m->header ) != 0) {
      char const *prefix = swi_decoding_table( m->header );
      char const *matched = string;

#if DEBUG__SWI_NUMBER_FROM_STRING
Write0( prefix ); NewLine;
#endif

      while (*prefix == *matched) { prefix++; matched++; }

      if (*prefix == '\0' && *matched == '_') {
        uint32_t swi_number = m->header->swi_chunk;

        char const *table_entry = prefix + 1;
        while (*table_entry != '\0') {
#if DEBUG__SWI_NUMBER_FROM_STRING
Write0( table_entry ); Space;
#endif
          char const *tail = matched + 1;
#if DEBUG__SWI_NUMBER_FROM_STRING
Write0( tail ); Space;
#endif
          // Case sensitive
          while (*tail == *table_entry && *table_entry != '\0') { tail++; table_entry++; }
          if (*tail <= ' ' && *table_entry == '\0') {
#if DEBUG__SWI_NUMBER_FROM_STRING
            WriteS( "Match: " ); NewLine;
#endif
            regs->r[0] = swi_number | x_bit;
            return true;
          }

#if DEBUG__SWI_NUMBER_FROM_STRING
          WriteS( "No Match: " ); Write0( table_entry ); Space; Write0( tail ); NewLine;
#endif

          while (*table_entry != '\0') { table_entry++; }
          table_entry++;
          swi_number++;
        }

        // Stop checking modules when a prefix is matched, or not?
        break; // Yes. (May be wrong choice.)
      }
    }
    else if (0 != swi_decoding_code( m->header )) {
      WriteS( "Decoding using code" ); NewLine;
      register int32_t offset asm( "r0" );
      register uint32_t text_to_number asm( "r0" ) = -1;
      register char const *string asm ( "r1" ) = string;
      asm ( "blx %[code]"
          : "=r" (offset)
          : [code] "r" (swi_decoding_code( m->header ))
          , "r" (text_to_number)
          : "lr" );
      if (offset >= 0) {
        regs->r[0] = (m->header->swi_chunk + offset) | x_bit;
        return true;
      }
    }
#if DEBUG__SWI_NUMBER_FROM_STRING
    NewLine;
#endif

    m = m->next;
  }

  return Kernel_Error_SWINameNotKnown( regs );
}

bool do_OS_SWINumberToString( svc_registers *regs )
{
  uint32_t swi = regs->r[0];
  char *buffer = (void *) regs->r[1];
  uint32_t buffer_length = regs->r[2];
  uint32_t written = 0;

  if (0 != (swi & 0x20000)) {
    if (buffer_length > 1) {
      buffer[written++] = 'X';
    }
    swi = swi & ~0x20000;
  }

  if (swi < 0x100) {
    char const *prefix = "OS_";
    while (written < buffer_length && *prefix != '\0') {
      buffer[written++] = *prefix++;
    }
    char const *name = os_swi_names[swi];
    while (written < buffer_length && *name != '\0') {
      buffer[written++] = *name++;
    }
  }
  else if (swi < 0x200) {
    char const *name = "OS_WriteI";
    while (written < buffer_length && *name != '\0') {
      buffer[written++] = *name++;
    }
  }
  else {
    module *m = workspace.kernel.module_list_head;
    uint32_t const chunk = (swi & ~0x3f);
    uint32_t const index = (swi & 0x3f);
    while (m != 0) {
      if (chunk == m->header->swi_chunk) {
        char const *name = swi_decoding_table( m->header );

        assert( name != (void*) m->header ); // FIXME: call the decoding code...

        while (written < buffer_length && *name != '\0') {
          buffer[written++] = *name++;
        }
        // name points to the terminating nul, unless the buffer has been filled, in which case it
        // points to a character in the prefix. The latter case doesn't matter, since nothing else
        // will be added to the buffer.
        if (written < buffer_length) {
          buffer[written++] = '_';
        }
        int current = 0;
        while (current <= index && written < buffer_length) {
          assert( *name == '\0' );
          name++;
          if (*name == '\0') { // "\0\0" => end of list
            // Passing end of list.
            if (written < buffer_length && index > 10) buffer[written++] = '0' + (swi & 0x3f) / 10;
            if (written < buffer_length) buffer[written++] = '0' + (swi & 0x3f) % 10;
            break;
          }
          else if (current == index) {
            while (written < buffer_length && *name != '\0') {
              buffer[written++] = *name++;
            }
            break;
          }
          else {
            // Skip next name
            while (*name != '\0') name++;
            current ++;
          }
        }
        break;
      }
      m = m->next;
    }
  }

  buffer[written] = '\0';
  regs->r[2] = written;

  return true;
}

static inline const char *discard_leading_characters( const char *command )
{
  const char *c = command;
  while (*c == ' ' || *c == '*') c++;
  return c;
}

static inline const char *discard_leading_whitespace( const char *command )
{
  const char *c = command;
  while (*c == ' ' || *c == '\t') c++;
  return c;
}

static inline bool terminator( char c )
{
  return c == '\0' || c == '\r' || c == '\n';
}

static uint32_t count_params( char const *params )
{
  uint32_t result = 0;
  char const *p = params;

  while (*p == ' ') p++;

  while (!terminator( *p )) {

    result ++;

    while (!terminator( *p ) && *p != ' ') {
      if ('"' == *p) {
        do {
          p ++;
        } while (!terminator( *p ) && *p != '"');
        if (terminator( *p )) return -1; // Mistake
        // Otherwise p points to closing quote...
      }
      p++;
    }

    while (*p == ' ') p++;
  }

NewLine; WriteS( "Counted " ); WriteNum( result ); WriteS( " parameters in \"" ); Write0( params ); WriteS( "\"\n\r" );
  return result;
}

static inline error_block *Send_Service_UKCommand( char const *command )
{
  WriteS( "UKCommand\n\r" );
  Write0( command );

  register char const *cmd asm ( "r0" ) = command;
  register uint32_t service asm ( "r1" ) = 4;

  register error_block *error;
  register bool claimed asm ( "r1" );

  asm ( "svc %[swi]"
    "\n  movvc %[error], #0"
    "\n  movvs %[error], r0"
    : [error] "=r" (error)
    , "=r" (claimed)
    : [swi] "i" (OS_ServiceCall)
    , "r" (cmd)
    , "r" (service)
    : "memory", "lr", "cc", "r2", "r3", "r4", "r5", "r6", "r7", "r8" );

  if (error != 0) {
    Write0( error->desc ); NewLine;
  }
  else if (!claimed) {
    WriteS( "UKCommand not claimed, passing to file system" ); NewLine;

    static error_block not_found = { 214, "Command not found" };

    return &not_found;
  }
  else
    WriteS( "UKCommand claimed" ); NewLine;

  return error;
}

typedef struct __attribute__(( packed, aligned( 4 ) )) {
  uint32_t code_offset;
  struct {
    uint8_t min_params;
    uint8_t gstrans;
    uint8_t max_params;
    uint8_t flags;
  } info;
  uint32_t invalid_syntax_offset;
  uint32_t help_offset;
} module_command;

static module_command *find_module_command( module_header *header, char const *command )
{
#ifdef DEBUG__SHOW_ALL_COMMANDS
  char *sep = "Is it ";
#endif

  const char *cmd = module_commands( header );

  if (cmd == 0) return 0;

  while (cmd[0] != '\0') {
#ifdef DEBUG__SHOW_ALL_COMMANDS
Write0( sep ); sep = ", "; Write0( cmd ); Space;
#endif
    int len = strlen( cmd );

    module_command *c = (void*) (((uint32_t) cmd + len + 4)&~3); // +4 because len is strlen, not including terminator

    if (riscoscmp( cmd, command )) {

#ifdef DEBUG__SHOW_ALL_COMMANDS
      NewLine; WriteS( "Yes! " ); WriteNum( c->code_offset ); Space; WriteNum( c->invalid_syntax_offset ); Space; WriteNum( c->help_offset ); NewLine;
      if (c->help_offset != 0) { Write0( pointer_at_offset_from( header, c->help_offset ) ); }
      if (c->invalid_syntax_offset != 0) { Write0( pointer_at_offset_from( header, c->invalid_syntax_offset ) ); }
#endif

      return c;
    }

    cmd = (char const *) (c + 1);
  }

  return 0;
}

static error_block *run_module_command( const char *command )
{
  module *m = workspace.kernel.module_list_head;

  while (m != 0) {
#ifdef DEBUG__SHOW_ALL_COMMANDS
    if (m->header->offset_to_help_and_command_keyword_table != 0) {
      NewLine; WriteS( "From " ); Write0( title_string( m->header ) ); NewLine;
    }
#endif

    module_header *header = m->header;

    module_command *c = find_module_command( header, command );

    if (c != 0) {
      if (c->code_offset != 0) {
        const char *params = command;
        while (*params > ' ') params++;
        while (*params == ' ') params++;
        uint32_t count = count_params( params );

        if (count < c->info.min_params || count > c->info.max_params) {
          static error_block error = { 666, "Invalid number of parameters" };
          // TODO Service_SyntaxError
          return &error;
        }
        else if (count == -1) {
          static error_block mistake = { 4, "Mistake" };
          return &mistake;
        }

        if (c->info.gstrans != 0 && count > 0) {
          // Need to copy the command, running GSTrans on some parameters
          asm ( "bkpt 1" );
        }

#ifdef DEBUG__SHOW_COMMANDS
        WriteS( "Running command " ); Write0( command ); WriteS( " in " ); Write0( title_string( header ) ); WriteS( " at " ); WriteNum( c->code_offset + (uint32_t) header ); NewLine;
#endif

        return run_command( m, c->code_offset, params, count );
      }
    }
    m = m->next;
  }
#ifdef DEBUG__SHOW_COMMANDS
  NewLine;
#endif

  return Send_Service_UKCommand( command );
}

static bool is_file_command( char const *command )
{
  char const *p = command;
  while (*p > ' ') {
    if (*p == ':' || *p == '.') return true;
    p++;
  }
  return false;
}

// In: varname
// In: buf
// InOut: len
// Out: type
static error_block *read_var_into( char const *varname, char *buf, uint32_t *len, uint32_t *type )
{
  register const char *var_name asm( "r0" ) = varname;
  register char *value asm( "r1" ) = buf;
  register uint32_t size asm( "r2" ) = *len;
  register uint32_t context asm( "r3" ) = 0;
  register uint32_t convert asm( "r4" ) = 0;

  register uint32_t var_type asm( "r4" );

  error_block *error;

  asm volatile ( "svc %[swi]"
             "\n  movvs %[err], r0"
             "\n  movvc %[err], #0"
      : "+r" (size)
      , "+r" (context)
      , "=r" (var_type)
      , [err] "=r" (error)
      : [swi] "i" (OS_ReadVarVal | Xbit)
      , "r" (var_name)
      , "r" (value)
      , "r" (size)
      , "r" (context)
      , "r" (convert)
      : "lr", "cc", "memory" );

  *len = size;
  *type = var_type;

  asm volatile ( "" : : : "r0", "r1", "r2", "r3", "r4" );

  return error;
}

static error_block *substitute_args_into( char const *command, char *buf, uint32_t *len, char const *template, uint32_t tlen )
{
  error_block *error;

  register char const *c asm ( "r0" ) = command;
  register char *r asm ( "r1" ) = buf;
  register uint32_t size asm ( "r2" ) = *len;
  register char const *t asm ( "r3" ) = template;
  register uint32_t ts asm ( "r4" ) = tlen;
  asm volatile ( "svc %[swi]"
             "\n  movvs %[err], r0"
             "\n  movvc %[err], #0"
             : [err] "=r" (error)
             , "+r" (size)
             : [swi] "i" (OS_SubstituteArgs32 | Xbit)
             , "r" (c)
             , "r" (r)
             , "r" (size)
             , "r" (t)
             , "r" (ts)
             : "lr", "cc", "memory" );

  *len = size;

  asm volatile ( "" : : : "r0", "r1", "r2", "r3", "r4" );

  return error;
}

static bool __attribute__(( noinline )) do_CLI( uint32_t *regs )
{
  char const *command = (void*) regs[0];

  error_block *error = 0;
#ifdef DEBUG__SHOW_COMMANDS
  WriteS( "CLI: " ); Write0( command ); WriteS( " at " ); WriteNum( (uint32_t) command ); NewLine;
#endif

  // Max length is 1024 bytes in RO 5.28
  // PRM 1-958
  command = discard_leading_characters( command );
  if (*command == '|') return true; // Comment, nothing to do
  if (*command < ' ') return true; // Nothing on line, nothing to do
  bool run = (*command == '/');
  bool run_free = (*command == '&');

  if (run || run_free) {
    command++;
  }
  else if ((command[0] == 'R' || command[0] == 'r') &&
           (command[1] == 'U' || command[1] == 'u') &&
           (command[2] == 'N' || command[2] == 'n') &&
           (command[3] == ' '  || command[3] == '\0' ||
            command[3] == '\t' || command[3] == '\n')) {
    command += 3;
    run = true;
  }

  if (run_free) {
    asm ( "bkpt 8" ); // This doesn't look like it ever worked...
    TaskSlot *child = TaskSlot_new( command );
    TaskSlot_detach_from_creator( child );
  }

  if (run) {
    command = discard_leading_characters( command );

    // Can't use FSControl; it probably resets the SVC stack if it doesn't return
  }

  bool is_file = is_file_command( command );
  if (is_file) { WriteS( " file command" ); NewLine; }

  if (command[0] == '%') {
    // Skip alias checking
    command++;
  }
  else if (!is_file) {
    char variable[256];
    static const char alias[] = "Alias$";
    strcpy( variable, alias );
    int i = 0;
    while (command[i] > ' ') {
      variable[i + sizeof( alias ) - 1] = command[i];
      i++;
    }
    variable[i + sizeof( alias ) - 1] = '\0';
#ifdef DEBUG__SHOW_COMMANDS
    WriteS( "Looking for " ); Write0( variable ); NewLine;
#endif
    char result[256];
    uint32_t length = sizeof( result );
    uint32_t type;
    error = read_var_into( variable, result, &length, &type );
    if (error == 0) {
      WriteS( "Alias$ variable found" ); NewLine;
      Write0( variable ); WriteS( " exists: " ); Write0( result );
      // for (;;) { Yield(); }
    }
  }

  if (!is_file) {
    error = run_module_command( command );
    if (error == 0) return true;
  }

  if (is_file || (error != 0 && error->code == 214)) {
WriteS( "Looking for file " ); Write0( command ); NewLine;

    // Not found in any module
    register void const *filename asm ( "r1" ) = command;     // File name
    struct __attribute__(( packed )) {
      uint32_t type;
      uint32_t timestamp_MSB:8;
      uint32_t filetype:12;
      uint32_t timestamped:12;
      uint32_t word2;
      uint32_t size;
      uint32_t attrs;
    } file_info;

    asm volatile (
        "mov r0, %[reason]" 
    "\n  svc %[swi]"
    "\n  movvs %[error], r0"
    "\n  movvc %[error], #0"
    "\n  stmvc %[info], { r0, r2, r3, r4, r5 }"
        : [error] "=&r" (error)
        : [swi] "i" (OS_File | Xbit)
        , [reason] "i" (5) // Read catalogue information
        , "r" (filename)
        , [info] "r" (&file_info)
        , "m" (file_info)
        : "r0", "r2", "r3", "r4", "r5", "lr", "cc", "memory" );

    if (error == 0 && file_info.type == 1) {
      char runtype[] = "Alias$@RunType_XXX";
      runtype[15] = hex[(file_info.filetype >> 8) & 0xf];
      runtype[16] = hex[(file_info.filetype >> 4) & 0xf];
      runtype[17] = hex[(file_info.filetype >> 0) & 0xf];

      WriteS( "Found file, type " ); WriteNum( file_info.filetype ); NewLine;
      WriteS( "Looking for " ); Write0( runtype ); NewLine;

      char template[256];
      uint32_t varlen = sizeof( template );
      uint32_t type;
      error_block *error = read_var_into( runtype, template, &varlen, &type );

      if (error == 0) {
        template[varlen] = '\0';
        Write0( runtype ); WriteS( " variable found \"" ); Write0( template ); WriteS( "\"" ); NewLine;
        char to_run[1024];
        uint32_t length = sizeof( to_run );
        error = substitute_args_into( command, to_run, &length, template, varlen );

        if (error != 0) {
          regs[0] = (uint32_t) error;
          return false;
        }

        to_run[length] = '\0';
        WriteS( "Command to run: " ); Write0( to_run ); NewLine;
        error = run_module_command( to_run );
        WriteS( "Command returned" ); NewLine;
      }
      else {
        WriteS( "No Alias$@RunType for this file found" ); NewLine;
        Write0( error->desc ); NewLine;
      }
    }
    else if (error == 0 && file_info.type == 2) {
      // Application directory?
      WriteS( "Run any !Run file in the directory - TODO" ); NewLine; // TODO
    }
    else {
      for (;;) {
        WriteS( "No file " ); Write0( command ); NewLine;
        Yield();
      }
    }
  }

  if (error != 0) {
    regs[0] = (uint32_t) error;
    return false;
  }

  return true;
}

void __attribute__(( naked )) default_os_cli()
{
  register uint32_t *regs;
  // Return address is already on stack, ignore lr, always claiming
  asm ( "push { "C_CLOBBERED" }\n  mov %[regs], sp" 
      : [regs] "=r" (regs) );

  if (!do_CLI( regs )) {
    set_VF();
  }
  else {
    clear_VF();
  }

  asm ( "pop { "C_CLOBBERED", pc }" );
}

void __attribute__(( naked )) default_os_fscontrol()
{
  register uint32_t *regs;
  // Return address is already on stack, ignore lr
  // Some FSControl commands take up to r8
  // Pushes too many registers, to be sure that all C_CLOBBERED registers
  // are included.
  asm ( "push { r0-r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  do_FSControl( regs );

  asm ( "pop { r0-r12, pc }" );
}

void __attribute__(( naked )) default_os_upcall()
{
  register uint32_t *regs;
  // Return address is already on stack, ignore lr
  // Pushes too many registers, to be sure that all C_CLOBBERED registers
  // are included.
  asm volatile ( "push { r0-r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  do_UpCall( regs );

  asm ( "pop { r0-r12, pc }" );
}

static void __attribute__(( naked )) default_os_args()
{
  // It's unallocated, unless something else has said it is allocated.
  asm volatile ( "mov r0, #(1 << 11)\n  pop {pc}" );
}

static void __attribute__(( naked )) finish_vector()
{
  asm volatile ( "pop {pc}" );
}

// SwiSpriteOp does BranchNotJustUs, which accesses internal kernel structures. Avoid this, by going directly
// to SpriteVecHandler. FIXME: This might no longer be necessary; I've bypassed this in another way, somewhere...
extern void SpriteVecHandler();

extern void MOSPaletteV();
extern void MOSGraphicsV();

static uint32_t screen_colour_from_os_colour( uint32_t os )
{
  union {
    struct { // BGR0
      uint32_t Z:8;
      uint32_t B:8;
      uint32_t G:8;
      uint32_t R:8;
    };
    uint32_t raw;
  } os_colour = { .raw = os };

  return (255 << 24) | (os_colour.R << 16) | (os_colour.G << 8) | os_colour.B;
}

#ifdef DEBUG__SHOW_HLINES
static void show_sprite()
{
  if (workspace.vectors.zp.vdu_drivers.ws.XWindLimit > 256
   || workspace.vectors.zp.vdu_drivers.ws.YWindLimit > 256) {
    WriteS( "Sprite too big to display" ); NewLine;
    return;
  }
  int shift = workspace.vectors.zp.vdu_drivers.ws.Log2BPP + 1;
  int mask = (1 << shift) - 1;

  WriteS( "Show sprite " ); WriteNum( workspace.vectors.zp.vdu_drivers.ws.XWindLimit ); WriteS( "x" ); WriteNum( workspace.vectors.zp.vdu_drivers.ws.YWindLimit ); Space; WriteNum( shift ); Space; WriteNum( mask ); NewLine;

  uint32_t const *start = (void*) workspace.vectors.zp.vdu_drivers.ws.ScreenStart;
  // "The image contains the rows of the sprite from top to bottom, all word-aligned" PRM 1-780

  // Reversed y, because printing top line first
  uint32_t const *pixels = start;
  for (int y = 0; y <= workspace.vectors.zp.vdu_drivers.ws.YWindLimit; y++) {
    asm volatile ( "mov r0, #3 // Sleep"
               "\n  mov r1, #0 // For no time - yield"
               "\n  svc %[swi]"
        :
        : [swi] "i" (OS_ThreadOp)
        : "r0", "r1", "lr", "memory" );
    WriteNum( pixels ); Space; WriteS( "|" );
    int word_shifted = 32;
    uint32_t word;
    for (int x = 0; x <= workspace.vectors.zp.vdu_drivers.ws.XWindLimit; x++) {
      if (word_shifted == 32) {
        word = *pixels++;
        word_shifted = 0;
      }
      if (0 != (word & mask)) WriteS( "*" ); else Space;
      word = word >> shift;
      word_shifted += shift;
    }
    if (word_shifted != 32) pixels++;
    WriteS( "|" );
    NewLine;
  }
  NewLine;
}
#endif

void __attribute__(( noinline )) c_horizontal_line_draw( uint32_t left, uint32_t y, uint32_t right, uint32_t action )
{
#ifdef DEBUG__SHOW_HLINES
  WriteS( "HLine: " ); WriteNum( left ); Space; WriteNum( right ); Space; WriteNum( y ); Space; WriteNum( action ); NewLine;
  // FIXME needs to work in sprites as well, I think. I really does, especially for fonts!

  WriteS( "HLine:  " ); WriteNum( workspace.vectors.zp.vdu_drivers.ws.GWBRow );
  Space; WriteNum( workspace.vectors.zp.vdu_drivers.ws.GWTRow );
  Space; WriteNum( workspace.vectors.zp.vdu_drivers.ws.GWLCol );
  Space; WriteNum( workspace.vectors.zp.vdu_drivers.ws.GWRCol );
  NewLine;
#endif

  if (y < workspace.vectors.zp.vdu_drivers.ws.GWBRow || y > workspace.vectors.zp.vdu_drivers.ws.GWTRow) return;
  if (left > workspace.vectors.zp.vdu_drivers.ws.GWRCol || right < workspace.vectors.zp.vdu_drivers.ws.GWLCol) return;

  switch (workspace.vectors.zp.vdu_drivers.ws.XShftFactor) {
  case 0: break;
  case 5: break;
  default: WriteS( "HLine called, shift factor " ); WriteNum( workspace.vectors.zp.vdu_drivers.ws.XShftFactor ); NewLine;
  }

#ifdef DEBUG__SHOW_HLINES
  WriteNum( workspace.vectors.zp.vdu_drivers.ws.XShftFactor ); Space;
  WriteNum( workspace.vectors.zp.vdu_drivers.ws.XWindLimit ); Space;
  WriteNum( workspace.vectors.zp.vdu_drivers.ws.YWindLimit ); Space;
  WriteNum( workspace.vectors.zp.vdu_drivers.ws.ScreenStart ); Space;
  WriteNum( workspace.vectors.zp.vdu_drivers.ws.Log2BPP ); Space;
  WriteNum( workspace.vectors.zp.vdu_drivers.ws.Log2BPC ); NewLine;
#endif

  // FIXME These things need to be moved into some graphics context. The Kernel/s/vdu stuff accesses this directly, but the DrawMod uses the ReadVduVariables interface.

  // EcfOraEor *ecf;
  uint32_t bpp = 1 << workspace.vectors.zp.vdu_drivers.ws.Log2BPP;
  uint32_t *screen = (void*) workspace.vectors.zp.vdu_drivers.ws.ScreenStart;
  uint32_t y_from_top = workspace.vectors.zp.vdu_drivers.ws.YWindLimit - y;
  int32_t stride = (((workspace.vectors.zp.vdu_drivers.ws.XWindLimit + 1) * bpp + 31) / 32);
  uint32_t *row = screen + stride * y_from_top;
  uint32_t *l = row + (left * bpp) / 32;
  uint32_t *r = row + (right * bpp) / 32;
  uint32_t lmask = 0xffffffff << ((left * bpp) & 31);
  uint32_t rmask = 0xffffffff >> (31 - ((right * bpp) & 31));
#ifdef DEBUG__SHOW_HLINES
  WriteS( "Row: " ); WriteNum( row ); Space;
  WriteS( "L: " ); WriteNum( l ); Space;
  WriteS( "R: " ); WriteNum( r ); Space;
  if (l == r) WriteNum( lmask & rmask ); else { WriteNum( lmask ); Space; WriteNum( rmask ); } NewLine;
  show_sprite();
#endif

  uint32_t c;
  switch (workspace.vectors.zp.vdu_drivers.ws.Log2BPP) {
  case 0: // 1bpp
    c = workspace.vectors.zp.vdu_drivers.ws.FgEcf[0];
    assert( c == 0xffffffff || c == 0 );
    break;
  case 5: // 32bpp
    c = screen_colour_from_os_colour( workspace.vectors.zp.vdu_drivers.ws.FgEcf[0] );
    break;
  default:
    WriteS( "Unsupported bit depth" ); NewLine;
    c = 0xffffffff;
  }

  switch (action) {
  case 1: // Foreground
    {
    // FIXME URGENTLY Only sets to one!
    if (l == r) {
      *l = ((*l) & ~(lmask & rmask)) | (c & (lmask & rmask));
    }
    else {
      if (lmask != 0xffffffff) { *l = (*l & ~lmask) | (c & lmask); l++; }
      while (l < r) {
        *l++ = c;
      }
      assert( l == r );
      if (rmask != 0xffffffff) *l |= (*l & ~rmask) | (c & rmask); else *l = 0xffffffff;
    }
    }
    break;
#if 0
  case 2: // Invert
    {
    uint32_t *p = l;
    while (p <= r) {
      *p = ~*p;
      p++;
    }
    }
    break;
  case 3: // Background
    {
    uint32_t *p = l;
    uint32_t c = screen_colour_from_os_colour( *vduvarloc[154 - 128] );
    while (p <= r) {
      *p = c;
      p++;
    }
    }
    break;
#endif
  default: break;
  }
}

// Actual "parameters":
// GWLCol,GWBRow,GWRCol,GWTRow, YWindLimit, XShftFactor,LineLength,GColAdr,ScreenStart, lefy, y, right, action
// workspace.zp.vdu_drivers.ws.GWLCol etc.

void __attribute__(( naked )) fast_horizontal_line_draw( uint32_t left, uint32_t y, uint32_t right, uint32_t action )
{
  asm ( "push { r0-r12, lr }" );
  c_horizontal_line_draw( left, y, right, action );
  asm ( "pop { r0-r12, pc }" );
}

static void set_up_legacy_zero_page()
{
  // For default PaletteV code
  // Legacy code has this in System Heap, but whatever.
  // PalEntries*5 = 0x514, sizeof( PV ) = 0x1850
  struct PV {
    uint32_t blank[256+1+3];
    uint32_t LogFirst[256+1+3];
    uint32_t LogSecond[256+1+3];
    uint32_t PhysFirst[256+1+3];
    uint32_t PhysSecond[256+1+3];
    uint8_t RTable[256];
    uint8_t GTable[256];
    uint8_t BTable[256];
    uint8_t STable[256];
  } *palette = rma_allocate( sizeof( struct PV ) );
  if (sizeof( struct PV ) != 0x1850) { asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); }

  memset( palette, 0, sizeof( struct PV ) );
  workspace.vectors.zp.vdu_drivers.ws.BlankPalAddr = (uint32_t) &palette->LogFirst;
  workspace.vectors.zp.vdu_drivers.ws.FirPalAddr = (uint32_t) &palette->LogFirst;
  workspace.vectors.zp.vdu_drivers.ws.SecPalAddr = (uint32_t) &palette->LogSecond;
  for (int i = 0; i < 256; i++) {
    palette->RTable[i] = i;
    palette->GTable[i] = i;
    palette->BTable[i] = i;
    palette->STable[i] = i;
  }

  // For sprites
  workspace.vectors.zp.vdu_drivers.ws.SpChoosePtr = 0;
  workspace.vectors.zp.vdu_drivers.ws.SpChooseName[12] = 13;

  static const int eigen = 1;

  static const uint32_t initial_mode_vars[13] = {
    0x40,
    0xef,
    0x86,
    0xffffffff,
    eigen,
    eigen,
    0x1e00,
    0x7e9000,
    0x0,
    0x5,
    0x5,
    0x77f,
    0x437
  };
  extern uint32_t *const modevarloc[13];
  for (int i = 0; i < number_of( initial_mode_vars ); i++) {
    *modevarloc[i] = initial_mode_vars[i];
  }

  // PMF/osinit replacement:
  // Avoid "Buffer too small" error from BufferManager, which seems not to be returned in r0
  workspace.vectors.zp.PrinterBufferAddr = 0xfaff2c98; // Where from?
  workspace.vectors.zp.PrinterBufferSize = 0x1000; // 

  // Kernel/s/HAL
  workspace.vectors.zp.Page_Size = 0x1000;

  // This is obviously becoming the boot sequence, to be refactored when something's happening...

  extern uint32_t frame_buffer;
  extern uint32_t *vduvarloc[];
  static const uint32_t vduvars[45] = {
    0x0,                                // 0x80
    0x0,
    only_one_mode_xres - 1, // XWindLimit
    only_one_mode_yres - 1, // YWindLimit
    0,
    0x86,
    0xef,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0,                         // 0x90
    (uint32_t) &frame_buffer, // 0x94
    (uint32_t) &frame_buffer, // 0x95
    0xfd2000,
    0x63,
    0x60,
    0xffffffff,
    0x0,
    0xffffff,
    0x0,
    0xff,
    0x0,
    0xff,
    0x0,                                // 0xa0 160
    0x35,
    0x8,
    0x10,
    0x8,
    0x10,
    (uint32_t) fast_horizontal_line_draw,
    8,
    8,
    8,
    8,
    0xffff1480,
    0 };

  // TODO set these from information from the HAL

  for (int i = 0; i < number_of( vduvars ); i++)
    *vduvarloc[i] = vduvars[i];

  // Used by SpriteOp 60 (at least)
  workspace.vectors.zp.vdu_drivers.ws.VduSaveAreaPtr = &workspace.vectors.zp.vdu_drivers.ws.VduSaveArea;

  // I know, we need to not have the frame buffer at a fixed address, 
  // and probably allow for more than one at a time...

  // In ReadModeVariable number order:
  // (Matches only_one_mode.)
  workspace.vectors.zp.vdu_drivers.ws.ModeFlags = 64;
  workspace.vectors.zp.vdu_drivers.ws.ScrRCol = 239;
  workspace.vectors.zp.vdu_drivers.ws.ScrBRow = 134;
  workspace.vectors.zp.vdu_drivers.ws.NColour = 0xffffffff; // Total number of colours - 1
  workspace.vectors.zp.vdu_drivers.ws.XEigFactor = 1;
  workspace.vectors.zp.vdu_drivers.ws.YEigFactor = 1;
  workspace.vectors.zp.vdu_drivers.ws.LineLength = 1920 * 4;
  workspace.vectors.zp.vdu_drivers.ws.ScreenSize = 1920 * 1080 * 4;
  workspace.vectors.zp.vdu_drivers.ws.YShftFactor = 0;
  workspace.vectors.zp.vdu_drivers.ws.Log2BPP = 5;
  workspace.vectors.zp.vdu_drivers.ws.Log2BPC = 5;
  workspace.vectors.zp.vdu_drivers.ws.XWindLimit = 1920 - 1; // Pixels, afaict
  workspace.vectors.zp.vdu_drivers.ws.YWindLimit = 1080 - 1;

  // These three read together in vduplot
  workspace.vectors.zp.vdu_drivers.ws.XShftFactor = 0;
  workspace.vectors.zp.vdu_drivers.ws.GColAdr = &workspace.vectors.zp.vdu_drivers.ws.FgEcfOraEor;
  workspace.vectors.zp.vdu_drivers.ws.ScreenStart = (void*) &frame_buffer;

  // What's the difference between GcolOraEorAddr and GColAdr?

  // The above is set from this, in Kernel/s/vdu/vdugrafl:
  workspace.vectors.zp.vdu_drivers.ws.DisplayScreenStart = (uint32_t) &frame_buffer;

  // From dump of active RISC OS "zero page"
  // Next task: find out where they're set and used
  for (int i = 0; i < 8; i++) {
    workspace.vectors.zp.vdu_drivers.ws.FgEcf[i] = 0;
    workspace.vectors.zp.vdu_drivers.ws.BgEcf[i] = 0x00ffffff;
  }
  // These two are double what they should be
  // workspace.vectors.zp.vdu_drivers.ws.GFCOL = 
  // workspace.vectors.zp.vdu_drivers.ws.GBCOL = 
  workspace.vectors.zp.vdu_drivers.ws.BitsPerPix = 32;
  workspace.vectors.zp.vdu_drivers.ws.BytesPerChar = 32;
  workspace.vectors.zp.vdu_drivers.ws.DisplayLineLength = 0x1e00;
  workspace.vectors.zp.vdu_drivers.ws.RowMult = 8;
  workspace.vectors.zp.vdu_drivers.ws.RowLength = 0xf000;
  workspace.vectors.zp.vdu_drivers.ws.CursorAddr = (uint32_t) &frame_buffer;
  workspace.vectors.zp.vdu_drivers.ws.InputCursorAddr = 0x33fd8000; // Clearly wrong, no idea what this is
  // workspace.vectors.zp.vdu_drivers.ws.CBWS Clear Block workspace
  // workspace.vectors.zp.vdu_drivers.ws.CBStart = 0x1f50; // ??
  // workspace.vectors.zp.vdu_drivers.ws.CBEnd = 0x0d500d00; // ??


  workspace.vectors.zp.vdu_drivers.ws.DisplayBankAddr = (uint32_t) &frame_buffer;
  workspace.vectors.zp.vdu_drivers.ws.DisplayNColour = 0xffffffff;
  workspace.vectors.zp.vdu_drivers.ws.DisplayModeFlags = 0x40;

  workspace.vectors.zp.vdu_drivers.ws.DisplayModeNo = (uint32_t) &only_one_mode; // This is read at fc031e40 - ValidateModeSelector in SanitizeSGetMode, in PreCreateHeader, in CreateHeader (for sprite), in GetSprite, having fallen through from GetSpriteUserCoords, SWI SpriteReason_GetSpriteUserCoords+256 from clipboard_mode_changed_int in Wimp/s/Clipboard due to WindowManager init.
  workspace.vectors.zp.vdu_drivers.ws.DisplayXWindLimit = 1919;
  workspace.vectors.zp.vdu_drivers.ws.DisplayYWindLimit = 1079;
  workspace.vectors.zp.vdu_drivers.ws.DisplayXEigFactor = 1;
  workspace.vectors.zp.vdu_drivers.ws.DisplayYEigFactor = 1;
  workspace.vectors.zp.vdu_drivers.ws.DisplayLog2BPP = 5;
  workspace.vectors.zp.vdu_drivers.ws.PointerXEigFactor = 1;
  *(uint64_t*) workspace.vectors.zp.vdu_drivers.ws.Ecf1 = 0xFFFEFDFCFFFEFDFCull;
  *(uint64_t*) workspace.vectors.zp.vdu_drivers.ws.Ecf2 = 0x00102030010203ull;
  *(uint64_t*) workspace.vectors.zp.vdu_drivers.ws.Ecf3 = 0x2021222320212223ull;
  *(uint64_t*) workspace.vectors.zp.vdu_drivers.ws.Ecf4 = 0x0000000000ffffffull;
  *(uint64_t*) &workspace.vectors.zp.vdu_drivers.ws.DotLineStyle = 0x9f9f9f9f9f9f9f9full;
  workspace.vectors.zp.vdu_drivers.ws.ModeNo = (uint32_t) &only_one_mode;
  workspace.vectors.zp.vdu_drivers.ws.GFTint = 0xc0;
  workspace.vectors.zp.vdu_drivers.ws.TotalScreenSize = 0x00FD2000; // Twice what I say...?
  workspace.vectors.zp.vdu_drivers.ws.MaxMode = 0x35;
  workspace.vectors.zp.vdu_drivers.ws.ScreenEndAddr = (uint32_t) &frame_buffer + 4 * 1920 * 1080;
  workspace.vectors.zp.vdu_drivers.ws.CursorFlags = 0x60007A41;
  workspace.vectors.zp.vdu_drivers.ws.ECFShift = 0x20;
  workspace.vectors.zp.vdu_drivers.ws.ECFYOffset = 4;

  // That's everything up to ffff1220, I think
  // workspace.vectors.zp.vdu_drivers.ws.

  // This is the ECF pattern to be used, 8 pairs of eor/orr values
  for (int i = 0; i < 8; i++) {
    workspace.vectors.zp.vdu_drivers.ws.FgEcfOraEor.line[i].orr = 0xffffffff;
    workspace.vectors.zp.vdu_drivers.ws.FgEcfOraEor.line[i].eor = 0;
    workspace.vectors.zp.vdu_drivers.ws.BgEcfOraEor.line[i].orr = 0xffffffff;
    workspace.vectors.zp.vdu_drivers.ws.BgEcfOraEor.line[i].eor = 0;
  }

  workspace.vectors.zp.vdu_drivers.ws.ScreenEndAddr = (uint32_t) &(&frame_buffer)[1920*1080-1];
  workspace.vectors.zp.vdu_drivers.ws.TotalScreenSize = 1920 * 1080 * 4;
  workspace.vectors.zp.vdu_drivers.ws.TrueVideoPhysAddr = (uint32_t) &frame_buffer;

  // Like VduInit, without calling internal routines. Can assume workspace already zeroed. Kernel/s/vdu/vdudriver
  workspace.vectors.zp.vdu_drivers.ws.ScreenBlankDPMSState = 255;
  workspace.vectors.zp.vdu_drivers.ws.CurrentGraphicsVDriver = ~1; // GraphicsVInvalid; this means only one display?
  workspace.vectors.zp.vdu_drivers.ws.SpriteMaskSelect = 0x23c; // =RangeC+SpriteReason_SwitchOutputToSprite
  workspace.vectors.zp.vdu_drivers.ws.CursorFlags = 0x40007a00; // From VduInit, plus VDU5
  workspace.vectors.zp.vdu_drivers.ws.WrchNbit = 0xbbadf00d; // Should be NUL (mov pc, lr), but when does this happen?
  workspace.vectors.zp.vdu_drivers.ws.HLineAddr = (uint32_t) fast_horizontal_line_draw;
  workspace.vectors.zp.vdu_drivers.ws.GcolOraEorAddr = &workspace.vectors.zp.vdu_drivers.ws.FgEcfOraEor;
  workspace.vectors.zp.vdu_drivers.ws.MaxMode = 53; // "Constant now"
  // etc...

  // FIXME this should be system heap
  workspace.vectors.zp.vdu_drivers.ws.TextExpandArea = rma_allocate( 2048 );

  // To avoid problems in SWIPlot Kernel/s/vdu/vduswis
  // Rather than doing its job, it will put a stream of characters into the WrchV queue, if:
  //  * the WrchV handler is not the default (in the unused VecPtrTab, assuming anything above 0xfc000000 is default
  //  * either WrchDest or SpoolFileH are not zero
  //  * there's anything in the VDU queue
  //  * the VduDisabled bit is set in CursorFlags (0x4000000, bit 26?)
  //  * the ModeFlag_NonGraphic bit is set in ModeFlags (1, bit 0)
  // Edited the OS to never do that.

  for (int i = 0; i < number_of( workspace.vectors.zp.VecPtrTab ); i++) {
    workspace.vectors.zp.VecPtrTab[i] = 0xffffffff;
  }

  workspace.vectors.zp.OsbyteVars.VDUqueueItems = 0; // Isn't this already zeroed?

  // Should be settable by VDU 23, 17, 7... FIXME
  workspace.vectors.zp.vdu_drivers.ws.GCharSizeX = 8;
  workspace.vectors.zp.vdu_drivers.ws.GCharSizeY = 8;
  workspace.vectors.zp.vdu_drivers.ws.GCharSpaceX = 8;
  workspace.vectors.zp.vdu_drivers.ws.GCharSpaceY = 8;
  workspace.vectors.zp.vdu_drivers.ws.TCharSizeX = 8;
  workspace.vectors.zp.vdu_drivers.ws.TCharSizeX = 8;
  workspace.vectors.zp.vdu_drivers.ws.TCharSpaceX = 8;
  workspace.vectors.zp.vdu_drivers.ws.TCharSpaceY = 8;
}

static void setup_OS_vectors()
{
  void (*code)();

  for (int i = 0; i < number_of( workspace.kernel.vectors ); i++) {
    workspace.kernel.default_vectors[i] = callback_new( &shared.kernel.callbacks_pool );
    switch (i) {
    case 0x02: code = default_irq; break;
    case 0x05: code = default_os_cli; break;
    case 0x06: code = default_os_byte; break;
    case 0x07: code = default_os_word; break;
    case 0x09: code = default_os_args; break;
    case 0x0f: code = default_os_fscontrol; break;
    case 0x1c: code = default_ticker; break;
    case 0x1d: code = default_os_upcall; break;
    case 0x1e: code = default_os_changeenvironment; break;
    case 0x1f: code = SpriteVecHandler; break;
    case 0x22: code = MOSGraphicsV; break;
    case 0x23: code = MOSPaletteV; break;
    default:
      code = finish_vector;
    }
    workspace.kernel.default_vectors[i]->code = (uint32_t) code;
    if (i == 0x22 || i == 0x23)
      workspace.kernel.default_vectors[i]->private_word = (uint32_t) &workspace.vectors.zp.vdu_drivers.ws;

    workspace.kernel.vectors[i] = workspace.kernel.default_vectors[i];
  }
}

static void __attribute__(( naked, noreturn )) idle_thread()
{
  const int reset = 10000;
  uint32_t count = reset;

  // This thread currently needs no stack!
  asm ( "mov sp, #0" ); // Avoid triggering any checks for privileged stack pointers in usr32 code 
  asm ( "cpsie aif" );  // Non-interruptable idle thread is not helpful
  for (;;) {
    // Transfer control to the boot task.
    // Don't make a function call, there's no stack.
    // (In practice it wouldn't be needed, but why take the chance?)
    asm volatile ( "mov r0, #3 // Sleep"
               "\n  mov r1, #0 // For no time - yield"
               "\n  svc %[swi]"
               "\n  bcs 0f // No other tasks running (could report being bored)"
               "\n  wfe"     // This isn't working, TODO
               "\n0:"
        :
        : [swi] "i" (OS_ThreadOp)
        , "r" (0x65656565) // Something to look out for in qemu output
        : "r0", "r1", "lr", "memory" );
    // TODO: Pootle around, tidying up memory, etc.
    // Better: wake a task that does that in an interruptable way
    // Don't do any I/O!
    // Don't forget to give it some stack!
    //asm volatile ( "wfi" );
    if (--count == 0) {
      asm volatile ( "mov r0, #255\n  svc 0xf9" : : : "r0" ); // Display status of threads
      count = reset;
    }
  }

  __builtin_unreachable();
}

extern void __attribute__(( noreturn )) UsrBoot();

#ifndef NO_DEBUG_OUTPUT
#include "include/pipeop.h"
#endif

#include "trivial_display.h"

void PreUsrBoot()
{
  Initialise_system_DAs(); // Including the RMA

  setup_OS_vectors(); // Requires the RMA

  set_up_legacy_zero_page();

  // Start the HAL, a multiprocessing-aware module that initialises 
  // essential features before the boot sequence can start.
  // It should register Resource:$.!Boot, which should perform the
  // post-ROM module boot.

  {
#ifndef NO_DEBUG_OUTPUT
    workspace.kernel.debug_pipe = PipeOp_CreateForTransfer( 4096 );
    // Guaranteed to work:
    workspace.kernel.debug_space = PipeOp_WaitForSpace( workspace.kernel.debug_pipe, 2048 );
    PipeOp_PassingOff( workspace.kernel.debug_pipe, 0 ); // The task doesn't exist yet
#endif
    WriteS( "Kernel starting HAL" ); NewLine;

    char args[] = "HAL ########";
    uint32_t tmp = workspace.kernel.debug_pipe;
    for (int i = 11; i >= 4; i--) {
      args[i] = hex[tmp & 0xf]; tmp = tmp >> 4;
    }
    Write0( args ); NewLine; NewLine;

    extern void _binary_Modules_HAL_start();
    // Can't use OS_Module because it doesn't pass arguments to init
    // I think they're supposed to use OS_ReadArgs
    svc_registers regs = { 0 };
#ifdef DEBUG__SHOW_MODULE_INIT
  NewLine;
  WriteS( "INIT HAL: " );
  Write0( title_string( (void*) (1 + (uint32_t*) &_binary_Modules_HAL_start ) ) ); NewLine;
#endif

    initialise_module( &regs, (void*) &_binary_Modules_HAL_start, args );
#ifdef DEBUG__SHOW_MODULE_INIT
  WriteS( "HAL initialised" ); NewLine;
#endif
  }
}

static uint32_t start_idle_task()
{
  register uint32_t request asm ( "r0" ) = 0; // Create Thread
  register void *code asm ( "r1" ) = idle_thread;
  register void *stack_top asm ( "r2" ) = 0;
  register uint32_t core_number asm( "r3" ) = workspace.core_number;

  register uint32_t handle asm ( "r0" );

  asm volatile ( "svc %[swi]"
      : "=r" (handle)
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      , "r" (code)
      , "r" (stack_top)
      , "r" (core_number)
      : "lr" );

  return handle;
}

// Not static, only called from inline assembler
void __attribute__(( noreturn, noinline )) BootWithFullSVCStack()
{
  start_idle_task(); // The one that's always there

  PreUsrBoot();

  // No App memory initially

  // The HAL will have ensured that no extraneous interrupts are occuring,
  // so we can reset the SVC stack, enable interrupts and drop to USR mode.
  static uint32_t const root_stack_size = 1024;
  uint32_t *stack = rma_allocate( root_stack_size * sizeof( uint32_t ) );
  extern uint32_t svc_stack_top;

  // Named registers so that no banked register is used (we're changing
  // processor mode)
  register uint32_t *stack_top asm ( "r0" ) = &svc_stack_top;
  register uint32_t *usr_stack_top asm ( "r12" ) = &stack[root_stack_size];

  asm ( "mov sp, r0"

    // This yield will release the slot's svc stack
    "\n  mov r0, %[sleep]"
    "\n  mov r1, #0"
    "\n  svc %[swi]"

    "\n  cpsie aif, #0x10"
    "\n  mov sp, r12"
    "\n  b UsrBoot"
    :
    : "r" (stack_top)
    , "r" (usr_stack_top)
    , [swi] "i" (OS_ThreadOp)
    , [sleep] "i" (TaskOp_Sleep)
    : "sp", "lr" );

  __builtin_unreachable();
}


void Boot()
{
  TaskSlot *slot = TaskSlot_first();

  assert( slot != 0 );
  assert( Task_now() != 0 );
  assert( TaskSlot_now() == slot );

  BootWithFullSVCStack();

  __builtin_unreachable();
}
