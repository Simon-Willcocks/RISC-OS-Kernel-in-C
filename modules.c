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

static void Sleep( uint32_t microseconds )
{
  register uint32_t request asm ( "r0" ) = 3; // Sleep
  register uint32_t time asm ( "r1" ) = microseconds; // Shift down a lot for testing!

  asm volatile ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      , "r" (time)
      : "lr" );
}

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
  uint32_t instance;
  module *next;  // Simple singly-linked list
  module *next_instance;  // Simple singly-linked list
  char postfix[];
};

static void *pointer_at_offset_from( void *base, uint32_t off )
{
  return ((uint8_t*) base) + off;
}

static inline uint32_t start_code( module_header *header )
{
  return header->offset_to_start + (uint32_t) header;
}

static inline uint32_t mp_aware( module_header *header )
{
  uint32_t flags = *(uint32_t *) (((char*) header) + header->offset_to_flags);
  return 0 != (2 & flags);
}

static inline bool run_initialisation_code( const char *env, module *m )
{
  uint32_t init_code = m->header->offset_to_initialisation + (uint32_t) m->header;

  register uint32_t non_kernel_code asm( "r14" ) = init_code;
  register uint32_t *private_word asm( "r12" ) = m->private_word;
  register uint32_t instance asm( "r11" ) = m->instance;
  register const char *environment asm( "r10" ) = env;

  // These will be passed to old-style modules as well, but they'll ignore them
  register uint32_t this_core asm( "r0" ) = workspace.core_number;
  register uint32_t number_of_cores asm( "r1" ) = processor.number_of_cores;

  asm goto (
        "  blx r14"
      "\n  bvs %l[failed]"
      :
      : "r" (non_kernel_code)
      , "r" (private_word)
      , "r" (instance)
      , "r" (environment)
      , "r" (this_core)
      , "r" (number_of_cores)
      : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9"
      : failed );

  // No changes to the registers by the module are of any interest,
  // so avoid corrupting any by simply not storing them

  return true;

failed:
  NewLine;
  Write0( "\005Failed\005" );
  NewLine;

  return false;
}

static inline uint32_t finalisation_code( module_header *header )
{
  return header->offset_to_finalisation + (uint32_t) header;
}

static bool run_service_call_handler_code( svc_registers *regs, module *m )
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
    Write0( "New file: " );
    Write0( rf->name );
    Write0( " " ); WriteNum( rf->offset );
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

#ifdef DEBUG__SHOW_VECTORS
vector do_nothing;
#endif

static bool run_vector( svc_registers *regs, int vec )
{
#ifdef DEBUG__SHOW_VECTORS_VERBOSE
  if (vec != 3 && workspace.kernel.vectors[3] != &do_nothing)
  {
    Write0( "Running vector " ); WriteNum( vec ); NewLine;
    vector *v = workspace.kernel.vectors[vec];
    while (v != 0) {
      WriteNum( v->code ); Write0( " " ); WriteNum( v->private_word ); Write0( " " ); WriteNum( v->next ); NewLine;
      v = v->next;
    }
    NewLine;
    for (int i = 0; i < 10; i++) { WriteNum( regs->r[i] ); Write0( " " ); }
    WriteNum( regs->lr );
    NewLine;
  }
#endif

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
  register vector *v asm( "r10" ) = workspace.kernel.vectors[vec];
  register uint32_t flags asm( "r1" );

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
      "\n  b 0b"
      "\nintercepted:"
      "\n  pop { r14 } // regs (intercepted already popped)"
      "\n  stm r14, { r0-r9 }"
      "\n  ldr r1, [r14, %[spsr]] // Update spsr with cpsr flags"
      "\n  mrs r2, cpsr"
      "\n  bic r1, #0xf0000000"
      "\n  and r2, r2, #0xf0000000"
      "\n  orr r1, r1, r2"
      "\n  str r1, [r14, %[spsr]]"
      : "=r" (flags)

      : [regs] "r" (regs)
      , [v] "r" (v)

      , [next] "i" ((char*) &((vector*) 0)->next)
      , [private] "i" ((char*) &((vector*) 0)->private_word)
      , [code] "i" ((char*) &((vector*) 0)->code)
      , [spsr] "i" (4 * (&regs->spsr - &regs->r[0]))

      : "r0", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r12", "r14" );

#ifdef DEBUG__SHOW_VECTORS_VERBOSE
  if (vec != 3 && workspace.kernel.vectors[3] != &do_nothing)
  {
    Write0( "Vector " ); WriteNum( vec ); asm ( "svc 0x120" ); WriteNum( flags ); NewLine;
  }
#endif
  return (flags & VF) == 0;
}

static inline uint32_t swi_decoding_table_code( module_header *header )
{
  return header->offset_to_swi_decoding_table + (uint32_t) header;
}

static inline uint32_t swi_decoding_code( module_header *header )
{
  return header->offset_to_swi_decoding_code + (uint32_t) header;
}

static inline const char *title_string( module_header *header )
{
  return (const char *) header->offset_to_title_string + (uint32_t) header;
}

static inline const char *help_string( module_header *header )
{
  return (const char *) header->offset_to_help_string + (uint32_t) header;
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

static inline int riscoscmp( const char *left, const char *right, bool space_terminates )
{
  int result = 0;
  while (result == 0) {
    char l = *left++;
    char r = *right++;

    if ((l == 0 || l == 10 || l == 13 || (space_terminates && l == ' '))
     && (r == 0 || r == 10 || r == 13 || (space_terminates && r == ' '))) break;

    result = l - r;
    if (result == 'a'-'A') {
      if (l >= 'a' && l <= 'z') result = 0;
    }
    else if (result == 'A'-'a') {
      if (r >= 'a' && r <= 'z') result = 0;
    }
  }
  return result;
}

static inline void describe_service_call( svc_registers *regs )
{
Write0( "*** ServiceCall_" );
switch (regs->r[1]) {

case 0x00: Write0( "CallClaimed" ); break;
case 0x04: Write0( "UKCommand" ); break;
case 0x06: Write0( "Error" ); break;
case 0x07: Write0( "UKByte" ); break;
case 0x08: Write0( "UKWord" ); break;
case 0x09: Write0( "Help" ); break;
case 0x0B: Write0( "ReleaseFIQ" ); break;
case 0x0C: Write0( "ClaimFIQ" ); break;
case 0x11: Write0( "Memory" ); break;
case 0x12: Write0( "StartUpFS" ); break;
case 0x18: Write0( "PostHelp?" ); break;
case 0x27: Write0( "PostReset" ); break;
case 0x28: Write0( "UKConfig" ); break;
case 0x29: Write0( "UKStatus" ); break;
case 0x2A: Write0( "NewApplication" ); break;
case 0x40: Write0( "FSRedeclare" ); break;
case 0x41: Write0( "Print" ); break;
case 0x42: Write0( "LookupFileType" ); break;
case 0x43: Write0( "International" ); break;
case 0x44: Write0( "KeyHandler" ); break;
case 0x45: Write0( "PreReset" ); break;
case 0x46: Write0( "ModeChange" ); break;
case 0x47: Write0( "ClaimFIQinBackground" ); break;
case 0x48: Write0( "ReAllocatePorts" ); break;
case 0x49: Write0( "StartWimp" ); break;
case 0x4A: Write0( "StartedWimp" ); break;
case 0x4B: Write0( "StartFiler" ); break;
case 0x4C: Write0( "StartedFiler" ); break;
case 0x4D: Write0( "PreModeChange" ); break;
case 0x4E: Write0( "MemoryMoved" ); break;
case 0x4F: Write0( "FilerDying" ); break;
case 0x50: Write0( "ModeExtension" ); break;
case 0x51: Write0( "ModeTranslation" ); break;
case 0x52: Write0( "MouseTrap" ); break;
case 0x53: Write0( "WimpCloseDown" ); break;
case 0x54: Write0( "Sound" ); break;
case 0x55: Write0( "NetFS" ); break;
case 0x56: Write0( "EconetDying" ); break;
case 0x57: Write0( "WimpReportError" ); break;
case 0x58: Write0( "MIDI" ); break;
case 0x59: Write0( "ResourceFSStarted" ); break;
case 0x5A: Write0( "ResourceFSDying" ); break;
case 0x5B: Write0( "CalibrationChanged" ); break;
case 0x5C: Write0( "WimpSaveDesktop" ); break;
case 0x5D: Write0( "WimpPalette" ); break;
case 0x5E: Write0( "MessageFileClosed" ); break;
case 0x5F: Write0( "NetFSDying" ); break;
case 0x60: Write0( "ResourceFSStarting" ); break;
case 0x61: Write0( "NFS?" ); break;
case 0x62: Write0( "DBoxModuleDying?" ); break;
case 0x63: Write0( "DBoxModuleStarting?" ); break;
case 0x64: Write0( "TerritoryManagerLoaded" ); break;
case 0x65: Write0( "PDriverStarting" ); break;
case 0x66: Write0( "PDumperStarting" ); break;
case 0x67: Write0( "PDumperDying" ); break;
case 0x68: Write0( "CloseFile: " ); Write0( (char*) regs->r[2] ); break;
case 0x69: Write0( "IdentifyDisc" ); break;
case 0x6A: Write0( "EnumerateFormats" ); break;
case 0x6B: Write0( "IdentifyFormat" ); break;
case 0x6C: Write0( "DisplayFormatHelp" ); break;
case 0x6D: Write0( "ValidateAddress" ); break;
case 0x6E: Write0( "FontsChanged" ); break;
case 0x6F: Write0( "BufferStarting" ); break;
case 0x70: Write0( "DeviceFSStarting" ); break;
case 0x71: Write0( "DeviceFSDying" ); break;
case 0x72: Write0( "SwitchingOutputToSprite" ); break;
case 0x73: Write0( "PostInit" ); break;
case 0x74: Write0( "BASICHelp?" ); break;
case 0x75: Write0( "TerritoryStarted" ); break;
case 0x76: Write0( "MonitorLeadTranslation" ); break;
case 0x77: Write0( "SerialDevice?" ); break;
case 0x78: Write0( "PDriverGetMessages" ); break;
case 0x79: Write0( "DeviceDead" ); break;
case 0x7A: Write0( "ScreenBlanked" ); break;
case 0x7B: Write0( "ScreenRestored" ); break;
case 0x7C: Write0( "DesktopWelcome" ); break;
case 0x7D: Write0( "DiscDismounted" ); break;
case 0x7E: Write0( "ShutDown" ); break;
case 0x7F: Write0( "PDriverChanged" ); break;
case 0x80: Write0( "ShutdownComplete" ); break;
case 0x81: Write0( "DeviceFSCloseRequest" ); break;
case 0x82: Write0( "InvalidateCache" ); break;
case 0x83: Write0( "ProtocolDying" ); break;
case 0x84: Write0( "FindNetworkDriver" ); break;
case 0x85: Write0( "WimpSpritesMoved" ); break;
case 0x86: Write0( "WimpRegisterFilters" ); break;
case 0x87: Write0( "FilterManagerInstalled" ); break;
case 0x88: Write0( "FilterManagerDying" ); break;
case 0x89: Write0( "ModeChanging" ); break;
case 0x8A: Write0( "Portable" ); break;
case 0x8B: Write0( "NetworkDriverStatus" ); break;
case 0x8C: Write0( "SyntaxError" ); break;
case 0x8D: Write0( "EnumerateScreenModes" ); break;
case 0x8E: Write0( "PagesUnsafe" ); break;
case 0x8F: Write0( "PagesSafe" ); break;
case 0x90: Write0( "DynamicAreaCreate" ); break;
case 0x91: Write0( "DynamicAreaRemove" ); break;
case 0x92: Write0( "DynamicAreaRenumber" ); break;
case 0x93: Write0( "ColourPickerLoaded" ); break;
case 0x94: Write0( "ModeFileChanged" ); break;
case 0x95: Write0( "FreewayStarting" ); break;
case 0x96: Write0( "FreewayTerminating" ); break;
case 0x97: Write0( "ShareDStarting?" ); break;
case 0x98: Write0( "ShareDTerminating?" ); break;
case 0x99: Write0( "ModulePostInitialisation?" ); break;
case 0x9A: Write0( "ModulePreFinalisation?" ); break;
case 0x9B: Write0( "EnumerateNetworkDrivers?" ); break;
case 0x9C: Write0( "PCMCIA?" ); break;
case 0x9D: Write0( "DCIDriverStatus" ); break;
case 0x9E: Write0( "DCIFrameTypeFree" ); break;
case 0x9F: Write0( "DCIProtocolStatus" ); break;
case 0xA7: Write0( "URI?" ); break;
case 0xB0: Write0( "InternetStatus" ); break;
case 0xB7: Write0( "UKCompression" ); break;
case 0xB9: Write0( "ModulePreInit" ); break;
case 0xC3: Write0( "PCI" ); break;
case 0xD2: Write0( "USB" ); break;
case 0xD9: Write0( "Hardware" ); break;
case 0xDA: Write0( "ModulePostInit" ); break;
case 0xDB: Write0( "ModulePostFinal" ); break;
case 0xDD: Write0( "RTCSynchronised" ); break;
case 0xDE: Write0( "DisplayChanged" ); break;
case 0xDF: Write0( "DisplayStatus" ); break;
case 0xE0: Write0( "NVRAM?" ); break;
case 0xE3: Write0( "PagesUnsafe64" ); break;
case 0xE4: Write0( "PagesSafe64" ); break;


case 0x10800: Write0( "ADFSPodule" ); break;
case 0x10801: Write0( "ADFSPoduleIDE" ); break;
case 0x10802: Write0( "ADFSPoduleIDEDying" ); break;
case 0x20100: Write0( "SCSIStarting" ); break;
case 0x20101: Write0( "SCSIDying" ); break;
case 0x20102: Write0( "SCSIAttached" ); break;
case 0x20103: Write0( "SCSIDetached" ); break;
case 0x400C0: Write0( "ErrorStarting?" ); break;
case 0x400C1: Write0( "ErrorButtonPressed?" ); break;
case 0x400C2: Write0( "ErrorEnding?" ); break;
case 0x41580: Write0( "FindProtocols" ); break;
case 0x41581: Write0( "FindProtocolsEnd" ); break;
case 0x41582: Write0( "ProtocolNameToNumber" ); break;
case 0x45540: Write0( "DrawObjectDeclareFonts" ); break;
case 0x45541: Write0( "DrawObjectRender" ); break;
case 0x4D480: Write0( "SafeAreaChanged?" ); break;
case 0x81080: Write0( "TimeZoneChanged" ); break;
case 0x810C0: Write0( "BootBootVarsSet?" ); break;
case 0x810C1: Write0( "BootResourcesVarsSet?" ); break;
case 0x810C2: Write0( "BootChoicesVarsSet?" ); break;
case 0x81100: Write0( "IIC" ); break;

default: WriteNum( regs->r[1] );
}
NewLine;
}

bool do_OS_ServiceCall( svc_registers *regs )
{
  bool result = true;
  module *m = workspace.kernel.module_list_head;

#ifdef DEBUG__SHOW_SERVICE_CALLS
describe_service_call( regs );
if (m == 0) {
  Write0( "No modules initialised\n" );
}
#endif

  uint32_t r12 = regs->r[12];
  while (m != 0 && regs->r[1] != 0 && result) {
    regs->r[12] = (uint32_t) m->private_word;
    if (0 != m->header->offset_to_service_call_handler) {
#if DEBUG__SHOW_SERVICE_CALLS
Write0( title_string( m->header ) ); Write0( " " ); WriteNum( m->header->offset_to_service_call_handler + (uint32_t) m->header ); NewLine;
#endif
      result = run_service_call_handler_code( regs, m );
    }
    m = m->next;
  }
#ifdef DEBUG__SHOW_SERVICE_CALLS
  NewLine;
#endif

  regs->r[12] = r12;

  return result;
}

static bool Unknown_OS_Module_call( svc_registers *regs )
{
  static error_block error = { 0x105, "Unknown OS_Module call" };
  Write0( "OS_Module Unknown call: " ); WriteNum( regs->r[0] ); NewLine;
  regs->r[0] = (uint32_t)&error;
  return false;
}

static module *find_module( char const *name )
{
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
Write0( "Looking for " ); Write0( name );
#endif
  module *m = workspace.kernel.module_list_head;
  int number = 0;
  while (m != 0 && 0 != riscoscmp( title_string( m->header ), name, true )) {
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
Write0( ", not " ); Write0( title_string( m->header ) );
#endif
    m = m->next;
    number++;
  }

#ifdef DEBUG__SHOW_MODULE_LOOKUPS
if (m) { Write0( ", FOUND " ); Write0( title_string( m->header ) ); NewLine; }
#endif
  return m;
}

#define OSMERR( f, l ) do { Write0( f l ); for (;;) {}; } while (0)

static bool do_Module_Run( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_Load( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  return Unknown_OS_Module_call( regs );
}

static bool do_Module_Enter( svc_registers *regs )
{
Write0( __func__ ); NewLine;
  // This routine should follow the procedure described in 1-235 including 
  // making the upcall OS_FSControl 2 2-85

  // At the moment, we're just starting the WindowManager (Wimp), via
  // Desktop.

  char const *module_name = (void*) regs->r[1];
  // char const *parameters = (void*) regs->r[2]; FIXME
  module *m = find_module( module_name );

  if (m == 0) {
    static error_block error = { 0x185, "Module not found (TODO)" };
    regs->r[0] = (uint32_t)&error;
    return false;
  }

  // Found it.

  if (m->header->offset_to_start == 0) {
    return true;
  }

  // Remember: eret is unpredictable in System mode
  // TODO: This does not yet reset the SVC stack.
  register void *private asm ( "r12" ) = m->private_word;
  asm ( "isb"
    "\n\tmsr spsr, %[usermode]"
    "\n\tmov lr, %[usr]"
    "\n\tmsr sp_usr, %[stacktop]"
    "\n\tisb"
    "\n\teret" 
    : 
    : [stacktop] "r" (0xffffffff)       // Dummy. It's up to the module to allocate stack if it needs it
    , [usr] "r" (pointer_at_offset_from( m->header, m->header->offset_to_start ))
    , [usermode] "r" (0x10)
    , "r" (private) );

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
    Write0( "Allocated RMA memory at " ); WriteNum( regs->r[2] ); Write0( " @" ); WriteNum( regs->lr ); NewLine;
#endif
  }
  else {
    asm ( "bkpt 88" ); // TODO stuff with extending and r2
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
  uint32_t r1 = regs->r[1];
  regs->r[0] = 3; // Free
  regs->r[1] = (uint32_t) &rma_heap;

#ifdef DEBUG__RMA_ALLOCATIONS
  Write0( "Free RMA memory at " ); WriteNum( regs->r[2] ); Write0( " @" ); WriteNum( regs->lr ); NewLine;
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

static void pre_init_service( module_header *m, uint32_t size_plus_4 )
{
  svc_registers serviceregs = { .r = { [0] = (uint32_t) m, [1] = 0xb9, [2] = size_plus_4, [7] = 0x11111111 } };
  do_OS_ServiceCall( &serviceregs );
}

static void post_init_service( module_header *m, uint32_t size_plus_4 )
{
  svc_registers serviceregs = { .r = { [0] = (uint32_t) m, [1] = 0xda, [2] = (uint32_t) title_string( m ), [7] = 0x22222222 } };
  do_OS_ServiceCall( &serviceregs );
}

static module *new_instance( module_header *m, svc_registers *regs, const char *postfix )
{
  int len = postfix == 0 ? 1 : strlen( postfix ) + 1;
  module *instance = rma_allocate( sizeof( module ) + len, regs );

  if (instance != 0) {
    instance->header = m;
    instance->private_word = &instance->local_private_word;
    instance->local_private_word = 0;
    instance->instance = 0;
    instance->next = 0;
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

static bool do_Module_InsertFromMemory( svc_registers *regs )
{
  module_header *new_mod = (void*) regs->r[1];

  // "During initialisation, your module is not on the active module list, and
  // so you cannot call SWIs in your own SWI chunk."

  module *instance;
  module *shared_instance = 0;
  bool success = true;

  if (0 != new_mod->offset_to_initialisation) {
    // FIXME Does this still make sense? Does anyone patch ROM modules any more?
    pre_init_service( new_mod, ((uint32_t*) new_mod)[-1] );
  }
  bool mp_module = mp_aware( new_mod );

  if (mp_module) {
    claim_lock( &shared.kernel.mp_module_init_lock );

    Write0( "MP" );
    shared_instance = shared.kernel.module_list_head;
    while (shared_instance != 0 && shared_instance->header != new_mod) {
      shared_instance = shared_instance->next;
    }

    if (shared_instance == 0) {
      // No core has initialised this module, yet.
      // Store a copy in the shared list.
      shared_instance = new_instance( new_mod, regs, 0 );

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
    instance = new_instance( new_mod, regs, 0 );
    success = instance != 0; 

    if (success && shared_instance != 0) {
      instance->private_word = shared_instance->private_word;
      while (instance->private_word != &shared_instance->local_private_word) {
        asm ( "bkpt 86" );
      }
    }

    if (success && 0 != new_mod->offset_to_initialisation) {
      success = run_initialisation_code( "", instance );
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
    post_init_service( new_mod, ((uint32_t*) new_mod)[-1] );
  }

  return success;
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
  while (instance-- > 0 && m != 0) {
    m = m->next_instance;
  }

  if (m == 0) {
    return Kernel_Error_NoMoreIncarnations( regs );
  }

  regs->r[1] ++;
  if (regs->r[2] != 0) {
    regs->r[2] ++;
  }

  regs->r[3] = (uint32_t) m->header;
  regs->r[4] = *m->private_word;
  regs->r[5] = (uint32_t) m->postfix;

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
  // I have a feeling these will have to be MP aware modules...
#if 0
  char const *name = (void*) regs->r[1];
  while (*name != '%' && *name > ' ') name++;
  if (*name != '%') {
    asm ( "bkpt 1" );
  }
  new_instance( m, regs, ++name );
#endif
Write0( __func__ ); 
Write0( regs->r[1] );
for (;;) {};
  return Unknown_OS_Module_call( regs );
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
Write0( __func__ ); Write0( " " ); Write0( regs->r[1] );
#endif

  // Initially called by Wimp during init, just to find ROM location

  const char *name = (void*) regs->r[1];
  // Not calling find_module, want the number as well...
  module *m = workspace.kernel.module_list_head;
  int number = 0;
  while (m != 0 && 0 != riscoscmp( title_string( m->header ), name, true )) {
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
Write0( ", not " ); Write0( title_string( m->header ) );
#endif
    m = m->next;
    number++;
  }

  if (m == 0) {
    // TODO personalised error messages will have to be stored associated with a task
Write0( __func__ ); Write0( " " ); Write0( regs->r[1] );
    static error_block error = { 258, "Module not found" }; // FIXME "Module %s not found"
    regs->r[0] = (uint32_t) &error;
    return false;
  }
  else {
    regs->r[1] = number;
    regs->r[2] = m->instance;
    regs->r[3] = (uint32_t) m->header;
    regs->r[4] = (uint32_t) m->private_word;
    regs->r[5] = m->postfix[0] == '\0' ? 0 : (uint32_t) &m->postfix;
#ifdef DEBUG__SHOW_MODULE_LOOKUPS
Write0( ", found: " ); Write0( title_string( m->header ) ); NewLine;
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
    asm ( "bkpt 1" );
  }
  return run_vector( regs, regs->r[9] );
}

static bool error_InvalidVector( svc_registers *regs )
{
  static error_block error = { 0x998, "Invalid vector number #" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static callback *get_a_callback()
{
  callback *result = workspace.kernel.callbacks_pool;
  if (result != 0) {
    workspace.kernel.callbacks_pool = result->next;
  }
  else {
    svc_registers regs;
    result = rma_allocate( sizeof( callback ), &regs );
  }
  return result;
}

bool do_OS_Claim( svc_registers *regs )
{
#ifdef DEBUG__SHOW_VECTORS
  Write0( "New vector claim " ); WriteNum( regs->r[0] );
  Write0( " Code " ); WriteNum( regs->r[1] );
  Write0( " Private " ); WriteNum( regs->r[2] ); NewLine;
#endif
  int number = regs->r[0];
  if (number > number_of( workspace.kernel.vectors )) {
    return error_InvalidVector( regs );
  }

  vector **p = &workspace.kernel.vectors[number];
  vector *v = *p;

  while (v != 0) {
    if (v->code == regs->r[1] && v->private_word == regs->r[2]) {
      // Duplicate to be removed, except we'll just move it up to the head instead,
      // without having to allocate new space.
#ifdef DEBUG__SHOW_VECTORS
  Write0( "Raising vector to top" ); NewLine;
#endif
      *p = v->next; // Removed from list
      v->next = workspace.kernel.vectors[number];
      workspace.kernel.vectors[number] = v; // Added at head;
      return true;
    }

    p = &v->next;
    v = v->next;
  }

  vector *new = get_a_callback();
  if (new == 0) {
    return error_nomem( regs );
  }

#ifdef DEBUG__SHOW_VECTORS
  Write0( "New new vector" ); NewLine;
#endif

  new->code = regs->r[1];
  new->private_word = regs->r[2];
  new->next = workspace.kernel.vectors[number];

  workspace.kernel.vectors[number] = new;

  return true;
}

bool do_OS_Release( svc_registers *regs )
{
  int number = regs->r[0];
  if (number > number_of( workspace.kernel.vectors )) {
    return error_InvalidVector( regs );
  }

  vector **p = &workspace.kernel.vectors[number];
  vector *v = *p;

  while (v != 0) {
    if (v->code == regs->r[1] && v->private_word == regs->r[2]) {
      // Duplicate to be removed
      *p = v->next; // Removed from list
      v->next = workspace.kernel.callbacks_pool;
      workspace.kernel.callbacks_pool = v;
      return true;
    }

    p = &v->next;
    v = v->next;
  }

  static error_block not_there = { 0x1a1, "Bad vector release" };
  regs->r[0] = (uint32_t) &not_there;
  return false;
}

bool do_OS_AddToVector( svc_registers *regs )
{
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

bool do_OS_GetEnv( svc_registers *regs )
{
  Task *task = workspace.task_slot.running;

  if (task->slot != 0) {
    regs->r[0] = (uint32_t) TaskSlot_Command( task->slot );
    regs->r[1] = TaskSlot_Himem( task->slot );
    regs->r[2] = (uint32_t) TaskSlot_Time( task->slot );
  }
  else {
    regs->r[0] = (uint32_t) "ModuleTask";
    regs->r[1] = 0x8000;
    regs->r[2] = 0;
  }

  return true;
}

static inline void show_module_commands( module_header *header )
{
  const char *cmd = (void*) (header->offset_to_help_and_command_keyword_table + (uint32_t) header);
  while (cmd[0] != '\0') {
    NewLine; Write0( cmd );
    int len = strlen( cmd );
    len = (len + 4) & ~3;
    cmd = &cmd[len + 16];
  }
  NewLine;
}

// Transient callbacks are usually called when returning to USR mode,
// but it's important to call them when a module has just been initialised
// as well.
// This could also be managed by initialising them from a script, since it
// runs in USR mode.
extern void run_transient_callbacks();

static module_header *find_rom_module( const char *name )
{
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  while (0 != *rom_module) {
    module_header *header = (void*) (rom_module+1);
    register const char *title = title_string( header );
    if (0 == strcmp( title, name )) {
      return (void*) (rom_module+1); // Header without size
    }
    rom_module += (*rom_module)/4; // Includes size of length field
  }

  return 0;
}

void init_module( const char *name )
{
  workspace.kernel.env = name;
  workspace.kernel.start_time = 0x0101010101ull;

#ifdef DEBUG__SHOW_MODULE_INIT
  NewLine;
  Write0( "INIT: " );
  Write0( name );
#endif

  module_header *header = find_rom_module( name );

  if (0 != header) {
#ifdef DEBUG__SHOW_MODULE_COMMANDS_ON_INIT
    show_module_commands( header );
#endif

    register uint32_t code asm( "r0" ) = 10;
    register module_header *module asm( "r1" ) = header;
    asm volatile ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "lr", "cc" );

    // Not in USR mode, but we are idling
    run_transient_callbacks();
  }
}

#define REPLACEMENT( modname ) \
  if (0 == strcmp( name, #modname )) { \
    extern uint32_t _binary_Modules_##modname##_start; \
    module_header *header = find_rom_module( #modname ); \
    register uint32_t code asm( "r0" ) = 10; \
    register uint32_t *module asm( "r1" ) = &_binary_Modules_##modname##_start; \
    register module_header *original asm ( "r2" ) = header; \
 \
    asm volatile ( "svc %[os_module]" \
       : \
       : "r" (code) \
       , "r" (module) \
       , "r" (original) \
       , [os_module] "i" (OS_Module) \
       : "lr", "cc" ); \
    Write0( "Replacement " ); Write0( #modname ); NewLine; \
    return true; \
  }

bool excluded( const char *name )
{
  // These modules fail on init, at the moment.
  static const char *excludes[] = { "PCI"               // Data abort fc01ff04 prob. pci_handles

                                  , "Debugger"
                                  , "BCMSupport"        // Unknown dynamic area
                                  , "Portable"          // Uses OS_MMUControl
                                  , "RTSupport"         // Unknown dynamic area
                                  , "USBDriver"         //  "
                                  , "DWCDriver"         //  "
                                  , "XHCIDriver"        //  "
                                  , "VCHIQ"             //  "
                                  , "BCMSound"          // ???

                                  , "DeviceFS"          // Calls ChangeEnvironment before there's a TaskSlot

// Probably don't work, I can't be bothered to see if their problems are solved already
                                  , "SoundDMA"          // Uses OS_Memory
                                  , "SoundChannels"     // ???
                                  , "SoundScheduler"    // Sound_Tuning
                                  , "TaskManager"       // Initialisation returns an error
                                  , "BCMVideo"          // Tries to use OS_MMUControl
                                  , "FilterManager"     // Uses Wimp_ReadSysInfo 
                                  , "WaveSynth"         // throws exception
                                  , "StringLib"         // ?
                                  , "Percussion"         // ?
                                  , "IIC"         // ? 0xe200004d
                                  , "SharedSound"       // 0xe200004d
                                  , "DOSFS"             // 0x8600003f
                                  , "SCSIDriver"        // 0x8600003f
                                  , "SCSISoftUSB"       // 0x8600003f
                                  , "SCSIFS"            // 0xe2000001
                                  , "SDIODriver"        // 0x8600003f
                                  , "SDFS"              // 0x8600003f
                                  , "SDCMOS"              // 0x8600003f
                                  //, "ColourPicker"      // 0x8600003f
                     //             , "BootCommands"      // 0x8600003f
                                  , "WindowScroll"      // 0x8600003f OS_Pointer not yet supported
                                  , "Internet"          // 0x8600003f
                                  , "Resolver"          // 0x8600003f
                                  , "Net"               // 0x8600003f

// Not checked:
                                  , "BootNet"
                                  , "Freeway"
                                  , "ShareFS"
                                  , "MimeMap"
                                  , "LanManFS"
                                  , "EtherGENET"
                                  , "EtherUSB"
                                  , "DHCP"
                     //             , "!Edit"
                     //             , "!Draw"
                     //             , "!Paint"
                     //             , "!Alarm"
                     //             , "!Chars"
                     //             , "!Help"
                                  , "Toolbox"           // Tries to RMLoad System:FilterManager
                                  , "Window"            // Requires Toolbox
                                  , "ToolAction"        // Requires Window
                                  , "Menu"
                                  , "Iconbar"
                                  , "ColourDbox"
                                  , "ColourMenu"
                                  , "DCS"
                                  , "FileInfo"
                                  , "FontDbox"
                                  , "FontMenu"
                                  , "PrintDbox"
                                  , "ProgInfo"
                                  , "SaveAs"
                                  , "Scale"
                                  , "TextGadgets"
                                  , "CDFSDriver"
                                  , "CDFSSoftSCSI"
                                  , "CDFS"
                                  , "CDFSFiler"
                                  , "UnSqueezeAIF"
                                  , "GPIO"

                                  , "DMAManager"        // Calls OS_Hardware
                                  , "BBCEconet"         // Data abort
                                  , "RamFS"             // Unknown dynamic area
                                  , "FSLock"            // Writes CMOS not yet supported
                                  , "FPEmulator"        // OS_ClaimProcessorVector

                                  , "MbufManager"       // 0xe200004d

                                  , "DragASprite"       // Doesn't return, afaics
                                  , "RamFS"             // Tries to use OS_MMUControl
                                  , "Filer"             // Doesn't return, afaics
                                  , "VFPSupport"        // Tries to claim processor vector
                                  , "Hourglass"        // OS_ReadPalette
                                  , "InternationalKeyboard" // Probably because there isn't one?
                                  , "NetFS"             // Doesn't return
                                  , "NetPrint"             // Doesn't return
                                  , "NetStatus"             // Doesn't return
                                  , "PipeFS"             // OS_ClaimProcessorVector
                                  , "RTC"               // No ticks? No hardware?
                                  , "ScreenBlanker"        // Doesn't return, afaics
                                  , "ScrSaver"        // Doesn't return, afaics
                                  , "Serial"        // "esources$Path{,_Message} not found
                                  , "SerialDeviceSupport"        // "esources$Path{,_Message} not found
                                  , "ShellCLI"        // "esources$Path{,_Message} not found
                                  , "SoundControl"          // No return
                                  , "BootFX"                    // No return
                                  , "SystemDevices"             // No return
                                  , "TaskWindow"             // Data abort, fc339bc4 -> 01f0343c
/**/
  };

  // C Modules that replace ROM modules (experimental)
  //REPLACEMENT( FontManager );
  REPLACEMENT( Portable );
  REPLACEMENT( VFPSupport );

  for (int i = 0; i < number_of( excludes ); i++) {
    if (0 == strcmp( name, excludes[i] ))
      return true;
  }
  return false;
}

void init_modules()
{
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  workspace.kernel.start_time = 0x0101010101ull;

  while (0 != *rom_module) {
    module_header *header = (void*) (rom_module+1);

    workspace.kernel.env = title_string( header );

#ifdef DEBUG__SHOW_MODULE_INIT
    NewLine;
    Write0( "INIT: " );
    WriteNum( rom_module ); Write0( " " );
    Write0( workspace.kernel.env );
#endif
    if (!excluded( workspace.kernel.env )) {
#ifdef DEBUG__SHOW_MODULE_INIT
      {
      if (header->offset_to_service_call_handler != 0) {
        Write0( " services " );
        uint32_t *p = pointer_at_offset_from( header, header->offset_to_service_call_handler );
        if (0xe1a00000 == p[0]) {
          Write0( " with table" );
          uint32_t table_offset = p[-1];

          uint32_t *p = pointer_at_offset_from( header, table_offset );

          NewLine; Write0( "Flags: " ); WriteNum( *p++ ); NewLine; p++; // Skip handler offset
          do {
            NewLine; Write0( "Expects service: " ); WriteNum( *p++ );
          } while (*p != 0);
        }
      }
      NewLine;
      }
#endif

      register uint32_t code asm( "r0" ) = 10;
      register module_header *module asm( "r1" ) = header;

      asm volatile ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "lr", "cc" );

      // Not in USR mode, but we are idling
      run_transient_callbacks();
    }
    else {
#ifdef DEBUG__SHOW_MODULE_INIT
      Write0( " - excluded" );
      NewLine;
#endif
    }
    rom_module += (*rom_module)/4; // Includes size of length field
  }
}

static inline void set_var( const char *name, const char *value )
{
  svc_registers regs;
  regs.r[0] = (uint32_t) name;
  regs.r[1] = (uint32_t) value;
  regs.r[2] = strlen( value );
  regs.r[3] = 0;
  regs.r[4] = 0;

  do_OS_SetVarVal( &regs );
}

static inline void Plot( uint32_t type, uint32_t x, uint32_t y )
{
  register uint32_t Rtype asm( "r0" ) = type;
  register uint32_t Rx asm( "r1" ) = x * 2; // pixel units to OS units, just for the tests
  register uint32_t Ry asm( "r2" ) = y * 2;
  asm volatile ( "svc %[swi]" : : [swi] "i" (OS_Plot), "r" (Rtype), "r" (Rx), "r" (Ry) );
}

static inline void Draw_Fill( uint32_t *path, int32_t *transformation_matrix )
{
  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = 0;
  register  int32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 0;
  asm ( "swi 0x60702"
        : 
        : "r" (draw_path)
        , "r" (fill_style)
        , "r" (matrix)
        , "r" (flatness)
        : "lr" );
}

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

static inline void SetColour( uint32_t flags, uint32_t colour )
{
  register uint32_t in_flags asm( "r0" ) = flags;
  register uint32_t in_colour asm( "r1" ) = colour;
  asm ( "swi %[swi]" : 
        : "r" (in_colour)
        , "r" (in_flags)
        , [swi] "i" (OS_SetColour)
        : "lr" );
}

static inline void SetGraphicsFgColour( uint32_t colour )
{
  Write0( "Setting graphics foreground colour with ColourTrans... " );
  register uint32_t pal asm( "r0" ) = colour;
  register uint32_t Rflags asm( "r3" ) = 0; // FG, no ECFs
  register uint32_t action asm( "r4" ) = 0; // set
  asm volatile ( "svc %[swi]" : : [swi] "i" (0x60743), "r" (pal), "r" (Rflags), "r" (action) : "lr", "cc" );
}

static inline void SetGraphicsBgColour( uint32_t colour )
{
  Write0( "Setting graphics background colour with ColourTrans... " );
  register uint32_t pal asm( "r0" ) = colour;
  register uint32_t Rflags asm( "r3" ) = 0x80;
  register uint32_t action asm( "r4" ) = 0; // set
  asm volatile ( "svc %[swi]" : : [swi] "i" (0x60743), "r" (pal), "r" (Rflags), "r" (action) : "lr", "cc" );
}

void Draw_Stroke( uint32_t *path, uint32_t *transformation_matrix )
{
  // Keep this declaration before the first register variable declaration, or
  // -Os will cause the compiler to forget the preceding registers.
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101422
  uint32_t cap_and_join_style[4] =  { 0, 0xa0000, 0, 0 };

  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = 0;
  register uint32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 0;
  register uint32_t thickness asm( "r4" ) = 0x1000;
  register uint32_t *cap_and_join asm( "r5" ) = cap_and_join_style;
  register uint32_t dashes asm( "r6" ) = 0;
  asm ( "swi 0x60704" : 
        : "r" (draw_path)
        , "r" (fill_style)
        , "r" (matrix)
        , "r" (flatness)
        , "r" (thickness)
        , "r" (cap_and_join)
        , "r" (dashes)
        , "m" (cap_and_join_style) // Without this, the array is not initialised
        : "lr" );
}

// Warning: does not return error status (although a "handle" > 255 is certainly an error)
static inline uint32_t Font_FindFont( const char *name, uint32_t xpoints, uint32_t ypoints, uint32_t xdpi, uint32_t ydpi )
{
  register uint32_t result asm( "r0" );
  register const char *rname asm( "r1" ) = name;
  register uint32_t rxpoints asm( "r2" ) = xpoints;
  register uint32_t rypoints asm( "r3" ) = ypoints;
  register uint32_t rxdpi asm( "r4" ) = xdpi;
  register uint32_t rydpi asm( "r5" ) = ydpi;

  asm volatile ( "swi %[swi]"
        : "=r" (result)
        : "r" (rname)
        , "r" (rxpoints)
        , "r" (rypoints)
        , "r" (rxdpi)
        , "r" (rydpi)
        , [swi] "i" (0x40081)
        : "lr" );

  return result;
}

static inline void ColourTrans_SetFontColours( uint32_t font, uint32_t fg, uint32_t bg, uint32_t maxdiff )
{
  register uint32_t Rfont asm( "r0" ) = font;
  register uint32_t Rfg asm( "r1" ) = fg;
  register uint32_t Rbg asm( "r2" ) = bg;
  register uint32_t Rmaxdiff asm( "r3" ) = maxdiff;

  asm volatile ( "swi %[swi]"
        :
        : "r" (Rfont)
        , "r" (Rfg)
        , "r" (Rbg)
        , "r" (Rmaxdiff)
        , [swi] "i" (0x20000 | 0x4074F)
        : "lr" );
}

static inline void usr_OS_ConvertCardinal4( uint32_t number, char *buffer, uint32_t buffer_size, char *old_buffer, char **terminator, uint32_t *remaining_size )
{
  // Do any calculations or variable initialisations here, before
  // declaring any register variables.
  // This is important because the compiler may insert function calls
  // like memcpy or memset, which will corrupt already declared registers.

  // The inputs to the SWI
  register uint32_t n asm( "r0" ) = number;
  register char *buf asm( "r1" ) = buffer;
  register uint32_t s asm( "r2" ) = buffer_size;

  // The outputs from the SWI
  register uint32_t oldbuf asm( "r0" );
  register char *term asm( "r1" );
  register uint32_t rem asm( "r2" );

  // Call the SWI
  asm volatile ( "svc %[swi]"
    : "=r" (oldbuf) // List all the output variables
    , "=r" (term)   // or they will be optimised away.
    , "=r" (rem)
    : [swi] "i" (OS_ConvertCardinal4) // Can use an enum for the SWI number
    , "r" (n)       // List all the input register variables,
    , "r" (buf)     // or they will be optimised away.
    , "r" (s)
    : // If the SWI corrupts any registers, list them here
      // If the function is to be called in a priviledged mode, include "lr"
      "memory"
  );

  // Store the output values
  // Don't worry about the apparent inefficiency, the compiler will
  // optimise out unused values.
  // Again, don't do anything other than simple storage or assignments
  // to non-register variables.
  if (old_buffer != 0) *old_buffer = oldbuf;
  if (terminator != 0) *terminator = term;
  if (remaining_size != 0) *remaining_size = rem;
}

void Font_Paint( uint32_t font, const char *string, uint32_t type, uint32_t startx, uint32_t starty, uint32_t length )
{
  register uint32_t rHandle asm( "r0" ) = font;
  register const char *rString asm( "r1" ) = string;
  register uint32_t rType asm( "r2" ) = type;
  register uint32_t rx asm( "r3" ) = startx;
  register uint32_t ry asm( "r4" ) = starty;
  register uint32_t rBlankArea asm( "r5" ) = 0;
  register uint32_t rMatrix asm( "r6" ) = 0;
  register uint32_t rLength asm( "r7" ) = length;
  asm ( "swi 0x60086" : 
        : "r" (rHandle)
        , "r" (rString)
        , "r" (rType)
        , "r" (rx)
        , "r" (ry)
        , "r" (rMatrix)
        , "r" (rBlankArea)
        , "r" (rLength)
        : "lr" );
}

static void __attribute__(( noinline )) default_os_byte_c( uint32_t *regs )
{
#ifdef DEBUG__SHOW_OS_BYTE
  Write0( "OS_Byte " ); WriteNum( regs[0] ); NewLine;
#endif

  switch (regs[0]) {
  case 0x00: // Display OS version or return machine type
    {
      static error_block version = { 0, "RISC OS, C Kernel 0.01 (1 May 2022)" };
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
      Write0( "Write Cursor Key State " ); WriteNum( regs[1] );
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
        asm ( "bkpt 1" );
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
        asm ( "bkpt 1" );
      }
    }
    break;
  case 0x15: // Clear selected buffer
    {
      Write0( "Flush buffer " ); WriteNum( regs[1] );
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
      Write0( "Setting alphabet/keyboard not supported" );
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
    Write0( "Read CMOS " ); WriteNum( regs[1] );
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
    default: Write0( " CMOS byte " ); WriteNum( regs[1] ); asm ( "bkpt 61" );
    }
#ifdef DEBUG__SHOW_OS_BYTE
    Write0( " = " ); WriteNum( regs[2] );
#endif
    }
    break;
  case 0xa2:
    {
#ifdef DEBUG__SHOW_OS_BYTE
    Write0( "Write CMOS " ); WriteNum( regs[1] ); Write0( " " ); WriteNum( regs[2] );
#endif
    switch (regs[1]) {
    case 0x10: Write0( "Misc flags" ); break;
    default: asm ( "bkpt 71" );
    }
    }
    break;
  case 0xa8 ... 0xff:
    {
#ifdef DEBUG__SHOW_OS_BYTE
    if (regs[1] == 0 && regs[2] == 255) {
      Write0( " read " );
    }
    else if (regs[2] == 0) {
      Write0( " write " ); WriteNum( regs[1] );
    }
    else {
      Write0( " " ); WriteNum( regs[1] );
      Write0( " " ); WriteNum( regs[2] );
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
    case 0xc6: Write0( " Exec handle" ); break;
    case 0xc7: Write0( " Spool handle" ); break;

    // Called by Wimp02 fn: resetkeycodes *fx 221,2 - fx 228,2, etc.
    // TODO make this the default and provide a compatibility layer for old code
    case 0xdb: Write0( " Tab key code" ); break;
    case 0xdc: Write0( " Escape character" ); break;
    case 0xdd ... 0xe4: Write0( " input values interpretation" ); break;
    case 0xe5: Write0( " Escape key status" ); break;
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
          Write0( "OS Version number" ); NewLine;
          regs[1] = 171;
        }
        else if (regs[1] <= 0x7f) {
          Write0( "Scan for range of keys " ); WriteNum( regs[1] ); NewLine;
          regs[1] = 0xff;       // No key (no keyboard!)
        }
        else {
          Write0( "Scan for particular key " ); WriteNum( regs[1] ); NewLine;
          regs[1] = 0xff;       // No key (no keyboard!)
        }
      }
      else if (regs[2] <= 0x7f) {
        Write0( "Scan keyboard with timeout." ); NewLine;
        WriteNum( 10 * ((regs[2] << 8) | regs[1]) ); // FIXME This needs to start a sleep, or the caller needs to be fixed, somehow... Wimp calls this regularly
        regs[2] = 0xff;       // Timeout (no keyboard!)
      }
      else {
        Write0( "Unknown OS_Byte option!" ); NewLine;
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

#ifdef DEBUG__SHOW_VECTOR_CALLS
#define WriteFunc do { Write0( __func__ ); NewLine; for (int i = 0; i < 13; i++) { WriteNum( regs->r[i] ); asm volatile ( "svc 0x100+' '" ); } WriteNum( regs->lr ); asm volatile ( "svc 0x100+' '" ); WriteNum( regs->spsr ); NewLine; } while (false)
#else
#define WriteFunc
#endif

bool do_OS_GenerateError( svc_registers *regs )
{
WriteFunc;
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
WriteFunc;
  // Check stack space TODO
  // Check command length TODO (still 256?)
  // /SetECF

  return run_vector( regs, 5 );
}

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

bool do_OS_File( svc_registers *regs )
{
#ifdef DEBUG__SHOW_FILES
do { Write0( __func__ ); NewLine; for (int i = 0; i < 13; i++) { WriteNum( regs->r[i] ); asm volatile ( "svc 0x100+' '" ); } WriteNum( regs->lr ); asm volatile ( "svc 0x100+' '" ); WriteNum( regs->spsr ); NewLine; } while (false);
uint32_t code = regs->r[0];
switch (code) {
  case 5:
  case 13:
  case 15:
  case 17: Write0( "Catalogue info for: " ); Write0( regs->r[1] ); NewLine; break;
  default: break;
}
#endif
  bool result = run_vector( regs, 8 );
#ifdef DEBUG__SHOW_FILES
Write0( "OS_File vector returned" ); NewLine;
for (int i = 0; i < 6; i++) { WriteNum( regs->r[i] ); asm volatile ( "svc 0x100+' '" ); }
NewLine;
#endif
  return result;
}

bool do_OS_Args( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 9 );
}

bool do_OS_BGet( svc_registers *regs )
{
  return run_vector( regs, 10 );
}

bool do_OS_BPut( svc_registers *regs )
{
  return run_vector( regs, 11 );
}

bool do_OS_GBPB( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 12 );
}

bool do_OS_Find( svc_registers *regs )
{
#ifdef DEBUG__SHOW_FILES
uint32_t code = regs->r[0];
WriteFunc;
switch (code) {
case 0: Write0( "Close file " ); WriteNum( regs->r[1] ); NewLine; break;
case 0x40 ... 0x7f: Write0( "Open existing file for reading " ); WriteNum( regs->r[0] ); Write0( " " ); Write0( regs->r[1] ); NewLine; break;
case 0x80 ... 0xbf: Write0( "Create new file " ); WriteNum( regs->r[0] ); Write0( " " ); Write0( regs->r[1] ); NewLine; break;
case 0xc0 ... 0xff: Write0( "Open existing file for writing " ); WriteNum( regs->r[0] ); Write0( " " ); Write0( regs->r[1] ); NewLine; break;
}
#endif
  bool result = run_vector( regs, 13 );
#ifdef DEBUG__SHOW_FILES
switch (code) {
case 0x40 ... 0x7f: Write0( "Opened for reading, handle " ); WriteNum( regs->r[0] ); NewLine; break;
}
#endif
  return result;
}

bool do_OS_ReadLine( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 14 );
}

bool do_OS_FSControl( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 15 );
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
WriteFunc;
  return run_vector( regs, 30 );
}

bool do_OS_SpriteOp( svc_registers *regs )
{
WriteFunc;
if (regs->r[0] == 0x118) { Write0( "Select sprite " ); Write0( regs->r[2] ); NewLine; WriteNum( regs->lr ); NewLine; }
  return run_vector( regs, 31 );
}

bool do_OS_SerialOp( svc_registers *regs )
{
WriteFunc;
  return run_vector( regs, 36 );
}

static uint32_t screen_colour_from_os_colour( uint32_t os )
{
  union {
    struct { // BGR0
      uint32_t Z:8;
      uint32_t R:8;
      uint32_t G:8;
      uint32_t B:8;
    };
    uint32_t raw;
  } os_colour = { .raw = os };

  return (255 << 24) | (os_colour.R << 16) | (os_colour.G << 8) | os_colour.B;
}

void __attribute__(( naked )) fast_horizontal_line_draw( uint32_t left, uint32_t y, uint32_t right, uint32_t action )
{
  // FIXME needs to work in sprites as well, I think

  // FIXME These things need to be moved into some graphics context. The Kernel/s/vdu stuff accesses this directly, but the DrawMod uses the ReadVduVariables interface.
  extern uint32_t *vduvarloc[];

  asm ( "push { r0-r12, lr }" );

#ifdef DEBUG__SHOW_HLINES
  Write0( "HLine " ); NewLine;
#endif
  // EcfOraEor *ecf;
  extern uint32_t frame_buffer;
  uint32_t *screen = &frame_buffer;
  uint32_t *row = screen + (1079 - y) * 1920;
  uint32_t *l = row + left;
  uint32_t *r = row + right;
  switch (action) {
  case 1: // Foreground
    {
    uint32_t *p = l;
    uint32_t c = screen_colour_from_os_colour( *vduvarloc[153 - 128] );
    while (p <= r) {
      *p = c;
      p++;
    }
    }
    break;
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
  default: break;
  }

  asm ( "pop { r0-r12, pc }" );
}


static const uint32_t sines[] = {
  0x00000, // sin 0
  0x00477, // sin 1
  0x008ef, // sin 2
  0x00d65, // sin 3
  0x011db, // sin 4
  0x0164f, // sin 5
  0x01ac2, // sin 6
  0x01f32, // sin 7
  0x023a0, // sin 8
  0x0280c, // sin 9
  0x02c74, // sin 10
  0x030d8, // sin 11
  0x03539, // sin 12
  0x03996, // sin 13
  0x03dee, // sin 14
  0x04241, // sin 15
  0x04690, // sin 16
  0x04ad8, // sin 17
  0x04f1b, // sin 18
  0x05358, // sin 19
  0x0578e, // sin 20
  0x05bbe, // sin 21
  0x05fe6, // sin 22
  0x06406, // sin 23
  0x0681f, // sin 24
  0x06c30, // sin 25
  0x07039, // sin 26
  0x07438, // sin 27
  0x0782f, // sin 28
  0x07c1c, // sin 29
  0x07fff, // sin 30
  0x083d9, // sin 31
  0x087a8, // sin 32
  0x08b6d, // sin 33
  0x08f27, // sin 34
  0x092d5, // sin 35
  0x09679, // sin 36
  0x09a10, // sin 37
  0x09d9b, // sin 38
  0x0a11b, // sin 39
  0x0a48d, // sin 40
  0x0a7f3, // sin 41
  0x0ab4c, // sin 42
  0x0ae97, // sin 43
  0x0b1d5, // sin 44
  0x0b504, // sin 45
  0x0b826, // sin 46
  0x0bb39, // sin 47
  0x0be3e, // sin 48
  0x0c134, // sin 49
  0x0c41b, // sin 50
  0x0c6f3, // sin 51
  0x0c9bb, // sin 52
  0x0cc73, // sin 53
  0x0cf1b, // sin 54
  0x0d1b3, // sin 55
  0x0d43b, // sin 56
  0x0d6b3, // sin 57
  0x0d919, // sin 58
  0x0db6f, // sin 59
  0x0ddb3, // sin 60
  0x0dfe7, // sin 61
  0x0e208, // sin 62
  0x0e419, // sin 63
  0x0e617, // sin 64
  0x0e803, // sin 65
  0x0e9de, // sin 66
  0x0eba6, // sin 67
  0x0ed5b, // sin 68
  0x0eeff, // sin 69
  0x0f08f, // sin 70
  0x0f20d, // sin 71
  0x0f378, // sin 72
  0x0f4d0, // sin 73
  0x0f615, // sin 74
  0x0f746, // sin 75
  0x0f865, // sin 76
  0x0f970, // sin 77
  0x0fa67, // sin 78
  0x0fb4b, // sin 79
  0x0fc1c, // sin 80
  0x0fcd9, // sin 81
  0x0fd82, // sin 82
  0x0fe17, // sin 83
  0x0fe98, // sin 84
  0x0ff06, // sin 85
  0x0ff60, // sin 86
  0x0ffa6, // sin 87
  0x0ffd8, // sin 88
  0x0fff6, // sin 89
  0x10000  // sin 90
  };        // sin 90, cos 0

static inline uint32_t draw_sin( int deg )
{
  while (deg < 0) { deg += 360; }
  while (deg > 360) { deg -= 360; }
  if (deg > 180) return -draw_sin( deg - 180 );
  if (deg > 90) return draw_sin( 180 - deg );
  return sines[deg];
}

static inline uint32_t draw_cos( int deg )
{
  return draw_sin( deg + 90 );
}

static inline void fill_rect( uint32_t left, uint32_t top, uint32_t w, uint32_t h, uint32_t c )
{
  extern uint32_t frame_buffer;
  uint32_t *screen = &frame_buffer;

  for (uint32_t y = top; y < top + h; y++) {
    uint32_t *p = &screen[y * 1920 + left];
    for (int x = 0; x < w; x++) { *p++ = c; }
  }
}

static void user_mode_code();

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

static bool terminator( char c )
{
  return c == '\0' || c == '\r' || c == '\n';
}

static uint32_t count_params( const char *p )
{
  uint32_t result = 0;

  while (*p == ' ' && !terminator( *p )) p++;

  while (!terminator( *p )) {

    result ++;

    while (!terminator( *p ) && *p != ' ') {
      if ('"' == *p) {
        do {
          p ++;
        } while (!terminator( *p ) && *p != '"');
        if (*p != '"') return -1; // Mistake
      }
      p++;
    }

    while (*p == ' ' && !terminator( *p )) p++;
  }

  return result;
}

static error_block *run_module_command( const char *command )
{
  module *m = workspace.kernel.module_list_head;
#ifdef DEBUG__SHOW_ALL_COMMANDS
  char *sep = "Is it ";
#endif
  while (m != 0) {
    module_header *header = m->header;

    const char *cmd = pointer_at_offset_from( header, header->offset_to_help_and_command_keyword_table );
    while (cmd[0] != '\0') {
#ifdef DEBUG__SHOW_ALL_COMMANDS
  Write0( sep ); sep = ", "; Write0( cmd );
#endif
      int len = strlen( cmd );
      if (0 == riscoscmp( cmd, command, true )) {
        struct {
          uint32_t code_offset;
          uint32_t info_word;
          uint32_t invalid_syntax_offset;
          uint32_t help_offset;
        } *c = (void*) &cmd[(len+4)&~3]; // +4 because len is strlen, not including terminator

#ifdef DEBUG__SHOW_ALL_COMMANDS
        NewLine; Write0( "Yes! " ); WriteNum( c->code_offset ); WriteNum( c->info_word ); WriteNum( c->invalid_syntax_offset ); WriteNum( c->help_offset ); NewLine;
#endif

        if (c->code_offset != 0) {
          const char *params = command + len;
          while (*params == ' ') params++;
          uint32_t count = count_params( params );

          if (count == -1) {
            static error_block mistake = { 4, "Mistake" };
            return &mistake;
          }

#ifdef DEBUG__SHOW_COMMANDS
          Write0( "Running command " ); Write0( command ); Write0( " in " ); Write0( title_string( header ) ); Write0( " at " ); WriteNum( c->code_offset + (uint32_t) header ); NewLine;
#endif

          return run_command( m, c->code_offset, params, count );
        }
#ifdef DEBUG__SHOW_COMMANDS
        else {
          void *help = pointer_at_offset_from( header, c->help_offset );
          NewLine; Write0( "Found " ); Write0( cmd ); Write0( ", but no code!" );
          if (c->help_offset != 0) Write0( help );
        }
#endif
      }
#ifdef DEBUG__SHOW_ALL_COMMANDS
      else {
        Write0( sep ); sep = ", "; Write0( cmd );
      }
#endif

      cmd = &cmd[(len+20)&~3]; // +4 for terminator and alignment, +16 for words
    }
    m = m->next;
  }
#ifdef DEBUG__SHOW_COMMANDS
  NewLine;
#endif

  static error_block not_found = { 214, "Command not found" };

  return &not_found;
}

static void __attribute__(( noinline )) do_CLI( const char *command )
{
#ifdef DEBUG__SHOW_COMMANDS
  Write0( "CLI: " ); Write0( command ); Write0( " at " ); WriteNum( (uint32_t) command ); NewLine;
#endif
  // Max length is 1024 bytes in RO 5.28
  // PRM 1-958
  command = discard_leading_characters( command );
  if (*command == '|') return; // Comment, nothing to do
  if (*command < ' ') return; // Nothing on line, nothing to do
  bool run = (*command == '/');
  if (run) {
    command++;
  }
  else {
    run = ((command[0] == 'R' || command[0] == 'r') &&
           (command[1] == 'U' || command[1] == 'u') &&
           (command[2] == 'N' || command[2] == 'n') &&
           (command[3] == ' '  || command[3] == '\0' ||
            command[3] == '\t' || command[3] == '\n'));
    if (run) {
      command += 3;
      command = discard_leading_characters( command );
    }
  }

  if (command[0] == '%') {
    // Skip alias checking
    command++;
  }
  else {
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
    Write0( "Looking for " ); Write0( variable ); NewLine;
#endif
    char result[256];
    register const char *var_name asm( "r0" ) = variable;
    register char *value asm( "r1" ) = result;
    register uint32_t size asm( "r2" ) = sizeof( result );
    register uint32_t context asm( "r3" ) = 0;
    register uint32_t convert asm( "r4" ) = 0;
    error_block *error;
    asm volatile ( "svc 0x20023\n  movvs %[err], r0\n  movvc %[err], #0"
        : "=r" (size), [err] "=r" (error)
        : "r" (var_name), "r" (value), "r" (size), "r" (context), "r" (convert)
        : "lr", "cc" );
    if (error == 0) {
      Write0( "Alias$ variable found" ); NewLine;
      Write0( variable ); Write0( "Exists: " ); Write0( result );
      asm ( "bkpt 41" );
    }
  }

  error_block *error = run_module_command( command );

  if (error != 0 && error->code == 214) {
    // Not found in any module
#ifdef DEBUG__SHOW_COMMANDS
    Write0( "Command not found, try filesystem, then files..." );
    // WindowManager runs FontInstall but is initialised before FontManager
    // and ROMFonts. Let this one go... (and re-order the modules)
    static const char exception[] = "FontInstall";
    if (0 != riscoscmp( command, exception, true ))
      asm( "bkpt 51" );
#endif
  }
}

void __attribute__(( naked )) default_os_cli()
{
  register uint32_t *regs;
  // Return address is already on stack, ignore lr
  asm ( "push { "C_CLOBBERED" }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  do_CLI( (const char *) regs[0] );

  asm ( "pop { "C_CLOBBERED", pc }" );
}

void __attribute__(( naked )) default_os_fscontrol()
{
  register uint32_t *regs;
  // Return address is already on stack, ignore lr
  // Some FSControl commands take up to r8; store them all
  asm ( "push { "C_CLOBBERED" }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  do_FSControl( regs );

  asm ( "pop { "C_CLOBBERED", pc }" );
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

vector do_nothing = { .next = 0, .code = (uint32_t) finish_vector, .private_word = 0 };
vector default_SpriteV = { .next = &do_nothing, .code = (uint32_t) SpriteVecHandler, .private_word = 0 };
vector default_ByteV = { .next = 0, .code = (uint32_t) default_os_byte, .private_word = 0 };
vector default_ArgsV = { .next = 0, .code = (uint32_t) default_os_args, .private_word = 0 };
vector default_ChEnvV = { .next = 0, .code = (uint32_t) default_os_changeenvironment, .private_word = 0 };
vector default_CliV = { .next = 0, .code = (uint32_t) default_os_cli, .private_word = 0 };
vector default_FSControlV = { .next = 0, .code = (uint32_t) default_os_fscontrol, .private_word = 0 };
vector default_PaletteV = { .next = &do_nothing, .code = (uint32_t) MOSPaletteV, .private_word = (uint32_t) &workspace.vectors.zp.vdu_drivers.ws };
vector default_GraphicsV = { .next = &do_nothing, .code = (uint32_t) MOSGraphicsV, .private_word = (uint32_t) &workspace.vectors.zp.vdu_drivers.ws };
vector default_IrqV = { .next = 0, .code = (uint32_t) default_irq, .private_word = 0 };
vector default_TickerV = { .next = 0, .code = (uint32_t) default_ticker, .private_word = 0 };

static vector *default_handler( int number )
{
  switch (number) {
  case 0x02: return &default_IrqV;
  case 0x05: return &default_CliV;
  case 0x06: return &default_ByteV;
  case 0x09: return &default_ArgsV;
  case 0x0f: return &default_FSControlV;
  case 0x1c: return &default_TickerV;
  case 0x1e: return &default_ChEnvV;
  case 0x1f: return &default_SpriteV;
  case 0x22: return &default_GraphicsV;
  case 0x23: return &default_PaletteV;
  default:
    return &do_nothing;
  }
}

bool do_OS_ReadDefaultHandler( svc_registers *regs )
{
  vector *v = default_handler( regs->r[0] );

  regs->r[1] = v->code;
  regs->r[2] = v->private_word;
  regs->r[3] = 0; // Only relevant for Error, CallBack, BreakPoint. These will probably have to be associated with Task Slots...?
  return true;
}

static uint64_t timer_now()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 0, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo) : : "memory"  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static uint32_t timer_interrupt_time()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 2, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo)  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static void timer_interrupt_at( uint64_t then )
{
  asm volatile ( "mcrr p15, 2, %[lo], %[hi], c14" : : [hi] "r" (then >> 32), [lo] "r" (0xffffffff & then) : "memory" );
}

static void allocate_legacy_scratch_space()
{
  // DrawMod uses ScratchSpace at 0x4000
  uint32_t for_drawmod = Kernel_allocate_pages( 4096, 4096 );
  MMU_map_at( (void*) 0x4000, for_drawmod, 4096 );

  uint32_t for_eval = Kernel_allocate_pages( 4096, 4096 );
  MMU_map_at( (void*) 0x6000, for_eval, 4096 );

  uint32_t for_eval2 = Kernel_allocate_pages( 4096, 4096 );
  MMU_map_at( (void*) 0x5000, for_eval2, 4096 );

  // IDK what uses memory here, but it played havoc with my translation tables!
  // Might be Squash.
  uint32_t for_something_else = Kernel_allocate_pages( 4096, 4096 );
  MMU_map_at( (void*) 0xfff00000, for_something_else, 4096 );
}

extern mode_selector_block only_one_mode;

static void set_up_legacy_zero_page()
{
  // For default PaletteV code
  // Legacy code has this in System Heap, but whatever.
  // PalEntries*5 = 0x514, sizeof( PV ) = 0x1850
  svc_registers regs;
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
  } *palette = rma_allocate( sizeof( struct PV ), &regs );
  if (sizeof( struct PV ) != 0x1850) { asm ( "bkpt 1" ); }

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
    (1920 << eigen) - 1,
    (1080 << eigen) - 1,
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
  workspace.vectors.zp.vdu_drivers.ws.ScreenStart = (uint32_t) &frame_buffer;

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

  { // FIXME this should be system heap
    svc_registers regs;
    regs.spsr = 0; // OS_Heap fails if entered with V flag set
    workspace.vectors.zp.vdu_drivers.ws.TextExpandArea = rma_allocate( 2048, &regs );
  }

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
}

static void setup_OS_vectors()
{
  for (int i = 0; i < number_of( workspace.kernel.vectors ); i++) {
    workspace.kernel.vectors[i] = default_handler( i );
  }
}

void Boot()
{
  setup_OS_vectors();

  Task *task = Task_new( 0 );
  workspace.task_slot.running = task;

  workspace.kernel.irq_task = Task_new( 0 );

  allocate_legacy_scratch_space();

  set_up_legacy_zero_page();

  // Start the HAL, a multiprocessing-aware module that initialises essential features before
  // the boot sequence can start.
  {
    extern uint32_t _binary_Modules_HAL_start;
    register uint32_t code asm( "r0" ) = 10;
    register uint32_t *module asm( "r1" ) = &_binary_Modules_HAL_start;

    asm volatile ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "lr", "cc" );
  }

#ifdef LIMITED_MODULES
  init_module( "UtilityModule" );
  init_module( "ColourTrans" );

  init_module( "Draw" ); // needed by...
  init_module( "SpriteExtend" ); // and...

  // Order is important: FontManager and ResourceFS before ROMFonts
  init_module( "FontManager" ); // needed by ROMFonts
  init_module( "FileSwitch" ); // needed by...
  init_module( "ResourceFS" ); // needed by...
  init_module( "ROMFonts" );

  init_module( "SuperSample" ); // needed for anti-aliasing fonts

  init_module( "TerritoryManager" );
  init_module( "Messages" );
  init_module( "MessageTrans" );
  init_module( "UK" );
#else
  init_modules();
#endif

  {
    svc_registers regs = {};
    regs.r[1] = 0x73; // Service_PostInit
    do_OS_ServiceCall( &regs );
  }

  {
    svc_registers regs = {};
    regs.r[1] = 0x46; // Service_ModeChange
    regs.r[2] = (uint32_t) &only_one_mode;
    regs.r[3] = 0;
    do_OS_ServiceCall( &regs );
  }

  NewLine; Write0( "All modules initialised, starting USR mode code" ); NewLine;

  { // Environment for boot sequence
  TaskSlot *slot = TaskSlot_new( "System" );
  Write0( "Slot: " ); WriteNum( (uint32_t) slot ); NewLine;
  Task *task = Task_new( slot );
  Write0( "Task: " ); WriteNum( (uint32_t) task ); Write0( ", slot: " ); WriteNum( (uint32_t) task->slot ); NewLine;
  assert (task->slot == slot);

  // Initial state
  task->regs.r[0] = workspace.core_number;
  task->regs.pc = (uint32_t) user_mode_code;
  task->regs.psr = 0x10;
  task->regs.banked_sp = 0x9000;

  workspace.task_slot.running->next = task;

  // This will be replaced with code to load an application at 0x8000 and run it...
  uint32_t const initial_slot_size = 64 << 10;  // 64k
  physical_memory_block block = { .virtual_base = 0x8000, .physical_base = Kernel_allocate_pages( initial_slot_size, 4096 ), .size = initial_slot_size };
  TaskSlot_add( slot, block );

  MMU_switch_to( slot );

  // This appears to be necessary. Perhaps it should be in MMU_switch_to.
  clean_cache_to_PoC();
  }

  // asm ( "mov sp, %[stack]" : : [stack] "r" (sizeof( workspace.kernel.svc_stack ) + (uint32_t) &workspace.kernel.svc_stack) );

  asm volatile ( "svc %[swi]" : : [swi] "i" (OS_LeaveOS) : "lr" );
  asm volatile ( "svc %[swi]" : : [swi] "i" (OS_IntOn) : "lr" );
  for (;;) {
    // Transfer control to the boot task.
    // Don't make a function call, there's no stack.
    // (In practice it wouldn't be needed, but why take the chance?)
    asm volatile ( "mov r0, #3 // Sleep"
               "\n  mov r1, #0 // For no time - yield"
               "\n  svc %[swi]"
        :
        : [swi] "i" (OS_ThreadOp)
        : "r0", "r1", "lr" );
    // TODO: Pootle around, tidying up memory, etc.
    // Don't do any I/O!
    // Don't forget to give it some stack!
    asm volatile ( "wfi" );
  }

  __builtin_unreachable();
}

// None of the following will remain in the kernel, it is experimental user
// mode code.

/*
// Damnit, this breaks simple stuff...
#undef Write0
// ss is there in case the s parameter is even slightly complicated
#define Write0( s ) do { const char *ss = s; register const char *string asm( "r0" ) = ss; asm ( "svc 2" : : "r" (string) ); } while (false)
*/

// The following routines are designed for user mode, so they don't have to save lr.
static inline uint32_t open_file_to_read( const char *name )
{
  // OS_Find
  register uint32_t os_find_code asm( "r0" ) = 0x43 | (1 << 3);
  register const char *filename asm( "r1" ) = name;
  register uint32_t file_handle asm( "r0" );
  asm volatile ( "svc 0x0d" : "=r" (file_handle) : "r" (os_find_code), "r" (filename) ); // Doesn't corrupt lr because running usr

  return file_handle;
}

static uint32_t read_file_size( const char *name )
{
  register uint32_t os_file_code asm( "r0" ) = 17;
  register const char *filename asm( "r1" ) = name;
  register uint32_t file_size asm( "r4" );
  asm volatile ( "svc %[swi]" : "=r" (file_size) 
                     : [swi] "i" (OS_File), "r" (os_file_code), "r" (filename)
                     : "r2", "r3", "r5" );
  return file_size;
}

static void *claim_rma_memory( uint32_t size )
{
  register uint32_t reason asm( "r0" ) = 6;
  register uint32_t amount asm( "r3" ) = size;
  register void *mem asm ( "r2" );

  asm volatile ( "svc %[swi]" : "=r" (mem) : [swi] "i" (OS_Module), "r" (reason), "r" (amount) );

  return mem;
}

void *read_file_into_memory( const char *name )
{
  uint32_t file_size = read_file_size( name );
Write0( "File size = " ); WriteNum( file_size ); NewLine;
  void *mem = claim_rma_memory( file_size );
Write0( "Memory = " ); WriteNum( (uint32_t) mem ); NewLine;

  if (mem != 0)
  {
    register uint32_t os_file_code asm( "r0" ) = 16;
    register const char *filename asm( "r1" ) = name;
    register void *load_address asm( "r2" ) = mem;
    register uint32_t load_at_r2 asm( "r3" ) = 0;
    asm volatile ( "svc %[swi]" : : [swi] "i" (OS_File), "r" (os_file_code), "r" (filename), "r" (load_address), "r" (load_at_r2) : "r4", "r5" );
  }

  return mem;
}

#if 0
static void dump_zero_page()
{
  Write0( "\"Zero page\"" ); NewLine;
  // Dump "zero page"
  uint32_t *p = (void*)0xffff0000;
  while (p < (uint32_t*) 0xffff4000) {
    NewLine;
    Write0( "Address  :     3 2 1 0     7 6 5 4     B A 9 8     F E D C     3 2 1 0     7 6 5 4     B A 9 8     F E D C :            ASCII Data" );
    NewLine;
    for (int o = 0; o < 16; o++) { // 128 words, 8 to a line, 16 lines
      WriteNum( (uint32_t) p );
      Write0( " :" );
      for (int w = 0; w < 32/4; w++) {
        Write0( "    " );
        WriteNum( p[w] );
      }
      Write0( " : " );
      char *a = (void*) p;
      for (int c = 0; c < 32; c++) {
        char s[] = ".";
        if (a[c] >= ' ') s[0] = a[c];
        Write0( s );
      }
      NewLine;
      p += 8; // words
    }
  }
}
#endif

static uint32_t *core_lock()
{
  return (uint32_t *) 0x8000; // core-local lock
}

static bool claim_core_lock(  )
{
#if 1
  // The kernel should be able to do this alone without error.
  // It should be possible to avoid it in usr32 mode, but that
  // comes later...
  register uint32_t code asm( "r0" ) = 6;
  register uint32_t lock asm( "r1" ) = 0x8000;
  register bool already_owner asm( "r0" );
  // Without volatile, this is "optimized" away...
  asm volatile ( "svc %[swi]"
      : "=r" (already_owner)
      : [swi] "i" (OS_ThreadOp)
      , "r" (code)
      , "r" (lock)
      : "lr" );
  return already_owner;
#else
  uint32_t failed;
  uint32_t value;

  do {
    asm volatile ( "ldrex %[value], [%[lock]]"
                   : [value] "=&r" (value)
                   : [lock] "r" (core_lock()) );

    // Usage note: if a routine needs to claim a lock which may already
    // be claimed by the same task, it should remember the return value
    // so as not to release it prematurely.
    if (value == code) return true;

    if (value == 0) {
      // Inline asm note:
      // The failed and lock registers are not allowed to be the same, so
      // pretend to gcc that the lock may be written as well as read.
      // That requires an l-value to be passed in, hence:
      uint32_t *lock = core_lock();

      asm volatile ( "strex %[failed], %[value], [%[lock]]"
                     : [failed] "=&r" (failed)
                     , [lock] "+r" (lock)
                     : [value] "r" (code) );
    }
    else {
      asm ( "clrex" );
      Sleep( 0 ); // This is not the ideal implementation yet. Need to mark the lock as being wanted and block the thread until it is released. (The release function will have to check for the mark.)
      failed = true;
    }
  } while (failed);
  asm ( "dmb sy" );

  return false;
#endif
}

static void release_core_lock()
{
#if 1
  // The kernel should be able to do this alone without error.
  // It should be possible to avoid it in usr32 mode, but that
  // comes later...
  register uint32_t code asm( "r0" ) = 7;
  register uint32_t lock asm( "r1" ) = 0x8000;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (code)
      , "r" (lock)
      : "lr" );
#else
  // Ensure that any changes made while holding the lock are visible before the lock is seen to have been released
  asm ( "dmb sy" );
  *core_lock() = 0;
#endif
}

static void __attribute__(( noreturn )) spin( uint32_t code, uint32_t x, uint32_t y, bool clockwise )
{
  register uint32_t now asm( "r0" );
  asm ( "svc %[swi]" : "=r" (now) : [swi] "i" (OS_MSTime) );

  Sleep( (now + 0xfff) & ~0xfff );

static uint32_t path1[] = {
 0x00000002, 0x00000400, 0xffff7400,
 0x00000008, 0x00006900, 0xffff9e00,
 0x00000008, 0x00009300, 0x00000400,
 0x00000008, 0x00006900, 0x00006900,
 0x00000008, 0x00000400, 0x00009300,
 0x00000008, 0xffff9e00, 0x00006900,
 0x00000008, 0xffff7400, 0x00000400,
 0x00000008, 0xffff9e00, 0xffff9e00,
 0x00000008, 0x00000400, 0xffff7400,
 0x00000005,

 0x00000002, 0x00000300, 0xfffed000,
 0x00000006, 0xfffff900, 0xfffed000, 0xffffee00, 0xfffed100, 0xffffe400, 0xfffed200,
 0x00000008, 0xffffe100, 0xfffed200,
 0x00000008, 0xffffcd00, 0xffff3e00,
 0x00000008, 0xffffc700, 0xffff4000,
 0x00000006, 0xffffba00, 0xffff4400, 0xffffad00, 0xffff4900, 0xffffa200, 0xffff4f00,
 0x00000008, 0xffff9e00, 0xffff5100,
 0x00000008, 0xffff4400, 0xffff1300,
 0x00000008, 0xffff4000, 0xffff1600,
 0x00000006, 0xffff3100, 0xffff2300, 0xffff2300, 0xffff3100, 0xffff1600, 0xffff4000,
 0x00000008, 0xffff1300, 0xffff4400,
 0x00000008, 0xffff5100, 0xffff9e00,
 0x00000008, 0xffff4f00, 0xffffa200,
 0x00000006, 0xffff4900, 0xffffad00, 0xffff4400, 0xffffba00, 0xffff4000, 0xffffc700,
 0x00000008, 0xffff3e00, 0xffffcd00,
 0x00000008, 0xfffed200, 0xffffe100,
 0x00000008, 0xfffed200, 0xffffe400,
 0x00000006, 0xfffed100, 0xffffee00, 0xfffed000, 0xfffff900, 0xfffed000, 0x00000300,
 0x00000006, 0xfffed000, 0x00000e00, 0xfffed100, 0x00001900, 0xfffed200, 0x00002300,
 0x00000008, 0xfffed200, 0x00002600,
 0x00000008, 0xffff3e00, 0x00003a00,
 0x00000008, 0xffff4000, 0x00004000,
 0x00000006, 0xffff4400, 0x00004d00, 0xffff4900, 0x00005a00, 0xffff4f00, 0x00006500,
 0x00000008, 0xffff5200, 0x00006900,
 0x00000008, 0xffff1300, 0x0000c300,
 0x00000008, 0xffff1600, 0x0000c700,
 0x00000006, 0xffff2300, 0x0000d600, 0xffff3100, 0x0000e400, 0xffff4000, 0x0000f100,
 0x00000008, 0xffff4400, 0x0000f400,
 0x00000008, 0xffff9e00, 0x0000b600,
 0x00000008, 0xffffa200, 0x0000b800,
 0x00000006, 0xffffad00, 0x0000be00, 0xffffba00, 0x0000c300, 0xffffc700, 0x0000c700,
 0x00000008, 0xffffcd00, 0x0000c900,
 0x00000008, 0xffffe100, 0x00013500,
 0x00000008, 0xffffe400, 0x00013500,
 0x00000006, 0xffffee00, 0x00013600, 0xfffff900, 0x00013700, 0x00000300, 0x00013700,
 0x00000006, 0x00000e00, 0x00013700, 0x00001900, 0x00013600, 0x00002300, 0x00013500,
 0x00000008, 0x00002600, 0x00013500,
 0x00000008, 0x00003a00, 0x0000c900,
 0x00000008, 0x00004000, 0x0000c700,
 0x00000006, 0x00004d00, 0x0000c300, 0x00005a00, 0x0000be00, 0x00006500, 0x0000b800,
 0x00000008, 0x00006900, 0x0000b600,
 0x00000008, 0x0000c300, 0x0000f400,
 0x00000008, 0x0000c700, 0x0000f100,
 0x00000006, 0x0000d600, 0x0000e400, 0x0000e400, 0x0000d600, 0x0000f100, 0x0000c700,
 0x00000008, 0x0000f400, 0x0000c300,
 0x00000008, 0x0000b600, 0x00006900,
 0x00000008, 0x0000b800, 0x00006500,
 0x00000006, 0x0000be00, 0x00005a00, 0x0000c300, 0x00004d00, 0x0000c700, 0x00004000,
 0x00000008, 0x0000c900, 0x00003a00,
 0x00000008, 0x00013500, 0x00002600,
 0x00000008, 0x00013500, 0x00002300,
 0x00000006, 0x00013600, 0x00001900, 0x00013700, 0x00000e00, 0x00013700, 0x00000400,
 0x00000006, 0x00013700, 0xfffff900, 0x00013600, 0xffffee00, 0x00013500, 0xffffe400,
 0x00000008, 0x00013500, 0xffffe100,
 0x00000008, 0x0000c900, 0xffffcd00,
 0x00000008, 0x0000c700, 0xffffc700,
 0x00000006, 0x0000c300, 0xffffba00, 0x0000be00, 0xffffad00, 0x0000b800, 0xffffa200,
 0x00000008, 0x0000b600, 0xffff9e00,
 0x00000008, 0x0000f400, 0xffff4400,
 0x00000008, 0x0000f100, 0xffff4000,
 0x00000006, 0x0000e400, 0xffff3100, 0x0000d600, 0xffff2300, 0x0000c700, 0xffff1600,
 0x00000008, 0x0000c300, 0xffff1300,
 0x00000008, 0x00006900, 0xffff5100,
 0x00000008, 0x00006500, 0xffff4f00,
 0x00000006, 0x00005a00, 0xffff4900, 0x00004d00, 0xffff4400, 0x00004000, 0xffff4000,
 0x00000008, 0x00003a00, 0xffff3e00,
 0x00000008, 0x00002600, 0xfffed200,
 0x00000008, 0x00002300, 0xfffed200,
 0x00000006, 0x00001900, 0xfffed100, 0x00000e00, 0xfffed000, 0x00000300, 0xfffed000,
 0x00000005, 0x00000000 };

static uint32_t path2[] = {
 0x00000002, 0x00012d00, 0x00002100,
 0x00000008, 0x0000c200, 0x00003500,
 0x00000008, 0x0000d000, 0x00004100,
 0x00000008, 0x00013c00, 0x00002c00,
 0x00000008, 0x00012d00, 0x00002100,
 0x00000005,

 0x00000002, 0x00006300, 0x00006300,
 0x00000008, 0x00008b00, 0x00000300,
 0x00000008, 0x00006300, 0xffffa400,
 0x00000008, 0x00000300, 0xffff7c00,
 0x00000008, 0xffffa400, 0xffffa400,
 0x00000008, 0xffff9800, 0xffff9800,
 0x00000008, 0x00000300, 0xffff6c00,
 0x00000008, 0x00006f00, 0xffff9800,
 0x00000008, 0x00009b00, 0x00000300,
 0x00000008, 0x00006f00, 0x00006f00,
 0x00000008, 0x00006300, 0x00006300,
 0x00000005,

 0x00000002, 0x0000fe00, 0x0000c400,
 0x00000008, 0x0000eb00, 0x0000c100,
 0x00000008, 0x0000ea00, 0x0000c200,
 0x00000006, 0x0000de00, 0x0000d100, 0x0000d100, 0x0000de00, 0x0000c200, 0x0000ea00,
 0x00000008, 0x0000c100, 0x0000eb00,
 0x00000008, 0x00006700, 0x0000ad00,
 0x00000008, 0x00006100, 0x0000b000,
 0x00000008, 0x00006100, 0x0000b000,
 0x00000008, 0x00006100, 0x0000b000,
 0x00000006, 0x00005600, 0x0000b700, 0x00004a00, 0x0000bc00, 0x00003e00, 0x0000bf00,
 0x00000008, 0x00003500, 0x0000c200,
 0x00000008, 0x00004100, 0x0000d000,
 0x00000008, 0x00004300, 0x0000cf00,
 0x00000006, 0x00004d00, 0x0000cc00, 0x00005700, 0x0000c800, 0x00006000, 0x0000c400,
 0x00000008, 0x00006000, 0x0000c400,
 0x00000008, 0x00006000, 0x0000c400,
 0x00000008, 0x00006800, 0x0000bf00,
 0x00000008, 0x0000c400, 0x0000fe00,
 0x00000008, 0x0000cc00, 0x0000f700,
 0x00000006, 0x0000dc00, 0x0000ea00, 0x0000ea00, 0x0000dc00, 0x0000f700, 0x0000cc00,
 0x00000008, 0x0000fe00, 0x0000c400,
 0x00000005,

 0x00000002, 0x00002c00, 0x00013c00,
 0x00000008, 0x00002100, 0x00012d00,
 0x00000008, 0x00001300, 0x00012e00,
 0x00000006, 0x00000e00, 0x00012f00, 0x00000900, 0x00012f00, 0x00000300, 0x00012f00,
 0x00000006, 0xfffffe00, 0x00012f00, 0xfffff900, 0x00012f00, 0xfffff400, 0x00012e00,
 0x00000008, 0xffffe600, 0x00012d00,
 0x00000008, 0xffffd200, 0x0000c200,
 0x00000008, 0xffffc900, 0x0000bf00,
 0x00000006, 0xffffbd00, 0x0000bc00, 0xffffb100, 0x0000b700, 0xffffa600, 0x0000b000,
 0x00000008, 0xffffa600, 0x0000b000,
 0x00000008, 0xffffa600, 0x0000b000,
 0x00000008, 0xffffa000, 0x0000ad00,
 0x00000008, 0xffff4600, 0x0000eb00,
 0x00000008, 0xffff4500, 0x0000ea00,
 0x00000006, 0xffff3600, 0x0000de00, 0xffff2900, 0x0000d100, 0xffff1d00, 0x0000c200,
 0x00000008, 0xffff1c00, 0x0000c100,
 0x00000008, 0xffff5a00, 0x00006700,
 0x00000008, 0xffff5700, 0x00006100,
 0x00000006, 0xffff5000, 0x00005600, 0xffff4b00, 0x00004a00, 0xffff4800, 0x00003e00,
 0x00000008, 0xffff4800, 0x00003e00,
 0x00000008, 0xffff4800, 0x00003e00,
 0x00000008, 0xffff4500, 0x00003500,
 0x00000008, 0xfffeda00, 0x00002100,
 0x00000008, 0xfffed900, 0x00001300,
 0x00000006, 0xfffed800, 0x00000e00, 0xfffed800, 0x00000900, 0xfffed800, 0x00000400,
 0x00000006, 0xfffed800, 0xfffffe00, 0xfffed800, 0xfffff900, 0xfffed900, 0xfffff400,
 0x00000008, 0xfffeda00, 0xffffe600,
 0x00000008, 0xfffecb00, 0xffffdb00,
 0x00000008, 0xfffeca00, 0xffffe300,
 0x00000006, 0xfffec900, 0xffffee00, 0xfffec800, 0xfffff900, 0xfffec800, 0x00000400,
 0x00000006, 0xfffec800, 0x00000e00, 0xfffec900, 0x00001900, 0xfffeca00, 0x00002400,
 0x00000008, 0xfffecb00, 0x00002c00,
 0x00000008, 0xffff3700, 0x00004100,
 0x00000008, 0xffff3800, 0x00004300,
 0x00000008, 0xffff3800, 0x00004300,
 0x00000008, 0xffff3800, 0x00004300,
 0x00000006, 0xffff3b00, 0x00004d00, 0xffff3f00, 0x00005700, 0xffff4300, 0x00006000,
 0x00000008, 0xffff4800, 0x00006800,
 0x00000008, 0xffff0900, 0x0000c400,
 0x00000008, 0xffff1000, 0x0000cc00,
 0x00000006, 0xffff1d00, 0x0000dc00, 0xffff2b00, 0x0000ea00, 0xffff3b00, 0x0000f700,
 0x00000008, 0xffff4300, 0x0000fe00,
 0x00000008, 0xffff9f00, 0x0000bf00,
 0x00000008, 0xffffa700, 0x0000c400,
 0x00000008, 0xffffa700, 0x0000c400,
 0x00000008, 0xffffa700, 0x0000c400,
 0x00000006, 0xffffb000, 0x0000c800, 0xffffba00, 0x0000cc00, 0xffffc400, 0x0000cf00,
 0x00000008, 0xffffc600, 0x0000d000,
 0x00000008, 0xffffdb00, 0x00013c00,
 0x00000008, 0xffffe300, 0x00013d00,
 0x00000006, 0xffffee00, 0x00013f00, 0xfffff900, 0x00013f00, 0x00000300, 0x00013f00,
 0x00000006, 0x00000e00, 0x00013f00, 0x00001900, 0x00013f00, 0x00002400, 0x00013d00,
 0x00000008, 0x00002c00, 0x00013c00,
 0x00000005,

 0x00000002, 0xffff4500, 0xffffd200,
 0x00000008, 0xffff4800, 0xffffc900,
 0x00000006, 0xffff4b00, 0xffffbd00, 0xffff5000, 0xffffb100, 0xffff5700, 0xffffa600,
 0x00000008, 0xffff5700, 0xffffa600,
 0x00000008, 0xffff5700, 0xffffa600,
 0x00000008, 0xffff5a00, 0xffffa000,
 0x00000008, 0xffff1c00, 0xffff4600,
 0x00000008, 0xffff0900, 0xffff4300,
 0x00000008, 0xffff4800, 0xffff9f00,
 0x00000008, 0xffff4300, 0xffffa700,
 0x00000008, 0xffff4300, 0xffffa700,
 0x00000008, 0xffff4300, 0xffffa700,
 0x00000006, 0xffff3f00, 0xffffb000, 0xffff3b00, 0xffffba00, 0xffff3800, 0xffffc400,
 0x00000008, 0xffff3700, 0xffffc600,
 0x00000008, 0xffff4500, 0xffffd200,
 0x00000005,

 0x00000002, 0xffffd200, 0xffff4500,
 0x00000008, 0xffffe600, 0xfffeda00,
 0x00000008, 0xffffdb00, 0xfffecb00,
 0x00000008, 0xffffc600, 0xffff3700,
 0x00000008, 0xffffd200, 0xffff4500,
 0x00000005, 0x00000000 };

static uint32_t path3[] = {
 0x00000002, 0x0000c200, 0x00003500,
 0x00000008, 0x0000d000, 0x00004100,
 0x00000008, 0x0000cf00, 0x00004300,
 0x00000008, 0x0000cf00, 0x00004300,
 0x00000008, 0x0000cf00, 0x00004300,
 0x00000006, 0x0000cc00, 0x00004d00, 0x0000c800, 0x00005700, 0x0000c400, 0x00006000,
 0x00000008, 0x0000bf00, 0x00006800,
 0x00000008, 0x0000fe00, 0x0000c400,
 0x00000008, 0x0000eb00, 0x0000c100,
 0x00000008, 0x0000ad00, 0x00006700,
 0x00000008, 0x0000b000, 0x00006100,
 0x00000006, 0x0000b700, 0x00005600, 0x0000bc00, 0x00004a00, 0x0000bf00, 0x00003e00,
 0x00000008, 0x0000bf00, 0x00003e00,
 0x00000008, 0x0000bf00, 0x00003e00,
 0x00000008, 0x0000c200, 0x00003500,
 0x00000005,

 0x00000002, 0xffffa400, 0xffffa400,
 0x00000008, 0xffff7c00, 0x00000400,
 0x00000008, 0xffffa400, 0x00006300,
 0x00000008, 0x00000300, 0x00008b00,
 0x00000008, 0x00006300, 0x00006300,
 0x00000008, 0x00006f00, 0x00006f00,
 0x00000008, 0x00000300, 0x00009b00,
 0x00000008, 0xffff9800, 0x00006f00,
 0x00000008, 0xffff6c00, 0x00000400,
 0x00000008, 0xffff9800, 0xffff9800,
 0x00000008, 0xffffa400, 0xffffa400,
 0x00000005,

 0x00000002, 0xfffeda00, 0xffffe600,
 0x00000008, 0xffff4500, 0xffffd200,
 0x00000008, 0xffff3700, 0xffffc600,
 0x00000008, 0xfffecb00, 0xffffdb00,
 0x00000008, 0xfffeda00, 0xffffe600,
 0x00000005,

 0x00000002, 0xffff1c00, 0xffff4600,
 0x00000008, 0xffff1d00, 0xffff4500,
 0x00000006, 0xffff2900, 0xffff3600, 0xffff3600, 0xffff2900, 0xffff4500, 0xffff1d00,
 0x00000008, 0xffff4600, 0xffff1c00,
 0x00000008, 0xffffa000, 0xffff5a00,
 0x00000008, 0xffffa600, 0xffff5700,
 0x00000006, 0xffffb100, 0xffff5000, 0xffffbd00, 0xffff4b00, 0xffffc900, 0xffff4800,
 0x00000008, 0xffffc900, 0xffff4800,
 0x00000008, 0xffffc900, 0xffff4800,
 0x00000008, 0xffffd200, 0xffff4500,
 0x00000008, 0xffffc600, 0xffff3700,
 0x00000008, 0xffffc400, 0xffff3800,
 0x00000008, 0xffffc400, 0xffff3800,
 0x00000008, 0xffffc400, 0xffff3800,
 0x00000006, 0xffffba00, 0xffff3b00, 0xffffb000, 0xffff3f00, 0xffffa700, 0xffff4300,
 0x00000008, 0xffff9f00, 0xffff4800,
 0x00000008, 0xffff4300, 0xffff0900,
 0x00000008, 0xffff3b00, 0xffff1000,
 0x00000006, 0xffff2b00, 0xffff1d00, 0xffff1d00, 0xffff2b00, 0xffff1000, 0xffff3b00,
 0x00000008, 0xffff0900, 0xffff4300,
 0x00000008, 0xffff1c00, 0xffff4600,
 0x00000005,

 0x00000002, 0xffffe600, 0xfffeda00,
 0x00000008, 0xfffff400, 0xfffed900,
 0x00000006, 0xfffff900, 0xfffed800, 0xfffffe00, 0xfffed800, 0x00000300, 0xfffed800,
 0x00000006, 0x00000900, 0xfffed800, 0x00000e00, 0xfffed800, 0x00001300, 0xfffed900,
 0x00000008, 0x00002100, 0xfffeda00,
 0x00000008, 0x00003500, 0xffff4500,
 0x00000008, 0x00003e00, 0xffff4800,
 0x00000008, 0x00003e00, 0xffff4800,
 0x00000008, 0x00003e00, 0xffff4800,
 0x00000006, 0x00004a00, 0xffff4b00, 0x00005600, 0xffff5000, 0x00006100, 0xffff5700,
 0x00000008, 0x00006700, 0xffff5a00,
 0x00000008, 0x0000c100, 0xffff1c00,
 0x00000008, 0x0000c200, 0xffff1d00,
 0x00000006, 0x0000d100, 0xffff2900, 0x0000de00, 0xffff3600, 0x0000ea00, 0xffff4500,
 0x00000008, 0x0000eb00, 0xffff4600,
 0x00000008, 0x0000ad00, 0xffffa000,
 0x00000008, 0x0000b000, 0xffffa600,
 0x00000008, 0x0000b000, 0xffffa600,
 0x00000008, 0x0000b000, 0xffffa600,
 0x00000006, 0x0000b700, 0xffffb100, 0x0000bc00, 0xffffbd00, 0x0000bf00, 0xffffc900,
 0x00000008, 0x0000c200, 0xffffd200,
 0x00000008, 0x00012d00, 0xffffe600,
 0x00000008, 0x00012e00, 0xfffff400,
 0x00000006, 0x00012e00, 0xfffff900, 0x00012f00, 0xfffffe00, 0x00012f00, 0x00000400,
 0x00000006, 0x00012f00, 0x00000900, 0x00012e00, 0x00000e00, 0x00012e00, 0x00001300,
 0x00000008, 0x00012d00, 0x00002100,
 0x00000008, 0x00013c00, 0x00002c00,
 0x00000008, 0x00013d00, 0x00002400,
 0x00000006, 0x00013e00, 0x00001900, 0x00013f00, 0x00000e00, 0x00013f00, 0x00000400,
 0x00000006, 0x00013f00, 0xfffff900, 0x00013e00, 0xffffee00, 0x00013d00, 0xffffe300,
 0x00000008, 0x00013c00, 0xffffdb00,
 0x00000008, 0x0000d000, 0xffffc600,
 0x00000008, 0x0000cf00, 0xffffc400,
 0x00000006, 0x0000cc00, 0xffffba00, 0x0000c800, 0xffffb000, 0x0000c400, 0xffffa700,
 0x00000008, 0x0000c400, 0xffffa700,
 0x00000008, 0x0000c400, 0xffffa700,
 0x00000008, 0x0000bf00, 0xffff9f00,
 0x00000008, 0x0000fe00, 0xffff4300,
 0x00000008, 0x0000f700, 0xffff3b00,
 0x00000006, 0x0000ea00, 0xffff2b00, 0x0000dc00, 0xffff1d00, 0x0000cc00, 0xffff1000,
 0x00000008, 0x0000c400, 0xffff0900,
 0x00000008, 0x00006800, 0xffff4800,
 0x00000008, 0x00006000, 0xffff4300,
 0x00000006, 0x00005700, 0xffff3f00, 0x00004d00, 0xffff3b00, 0x00004300, 0xffff3800,
 0x00000008, 0x00004300, 0xffff3800,
 0x00000008, 0x00004300, 0xffff3800,
 0x00000008, 0x00004100, 0xffff3700,
 0x00000008, 0x00002c00, 0xfffecb00,
 0x00000008, 0x00002400, 0xfffeca00,
 0x00000006, 0x00001900, 0xfffec800, 0x00000e00, 0xfffec800, 0x00000300, 0xfffec800,
 0x00000006, 0xfffff900, 0xfffec800, 0xffffee00, 0xfffec800, 0xffffe300, 0xfffeca00,
 0x00000008, 0xffffdb00, 0xfffecb00,
 0x00000008, 0xffffe600, 0xfffeda00,
 0x00000005,

 0x00000002, 0x00004100, 0x0000d000,
 0x00000008, 0x00002c00, 0x00013c00,
 0x00000008, 0x00002100, 0x00012d00,
 0x00000008, 0x00003500, 0x0000c200,
 0x00000008, 0x00004100, 0x0000d000,
 0x00000005, 0x00000000 };

  int32_t matrix[6] = { 0, 0, 0, 0, x << 8, y << 8 };

  // Re-start after 45 degree turn (octagonal cog)
  int angle = clockwise ? 22 : 0; // Starting angle
  int step = 2;

  claim_core_lock( ); // Claimed, except while sleeping
  for (int loop = 0;; loop++) {
    register uint32_t start asm( "r0" );
    asm ( "svc %[swi]" : "=r" (start) : [swi] "i" (OS_MSTime) );

    matrix[0] =  draw_cos( angle );
    matrix[1] =  draw_sin( angle );
    matrix[2] = -draw_sin( angle );
    matrix[3] =  draw_cos( angle );

    SetColour( 0, 0x00990000 );
    Draw_Fill( path1, matrix );
    SetColour( 0, 0x00e50000 );
    Draw_Fill( path2, matrix );
    SetColour( 0, 0x004c0000 );
    Draw_Fill( path3, matrix );
    release_core_lock();

    // asm volatile ( "svc %[swi]" : : [swi] "i" (OS_FlushCache) : "cc" ); // lr is not corrupted in USR mode

    register uint32_t done asm( "r0" );
    asm ( "svc %[swi]" : "=r" (done) : [swi] "i" (OS_MSTime) );

    Sleep( 20 - (done - start) );

    claim_core_lock();
    SetColour( 0, 0x00000000 );
//    Draw_Fill( path1, matrix ); // Not needed for small changes in angle
    Draw_Fill( path2, matrix );
    Draw_Fill( path3, matrix );

    if (clockwise) {
      angle += step;
      if (angle >= 45) angle -= 45;
    }
    else {
      angle -= step;
      if (angle < 0) angle += 45;
    }
  }
  release_core_lock();
  __builtin_unreachable();
}

void user_thread( uint32_t thread, uint32_t x, uint32_t y, bool clockwise )
{
  Write0( "Task running" ); NewLine;
  uint32_t sp;
  asm volatile ( "mov %[sp], sp" : [sp] "=r" (sp) );
  WriteNum( sp ); Write0( " " ); WriteNum( thread ); Write0( " " ); WriteNum( x ); Write0( " " ); WriteNum( y ); Write0( " " ); WriteNum( clockwise ); NewLine;
  spin( thread, x, y, clockwise );
}

static void user_mode_code( int core_number )
{
  Write0( "In USR32 mode" ); NewLine;
  WriteNum( core_number ); NewLine;
  *(uint32_t*) 0x8000 = 0; // Reset the lock

  //OSCLI( "Modules" );
  if (core_number == 0) {
    OSCLI( "Desktop Resources:$.Apps.!Alarm" );
  }

  for (int i = 0; i < 3; i++) {
    // CreateThread
    // Registers 3-8 are passed to the code as arguments (r1-r6)
    // FIXME: compatible with aapcs?
    // Argument 1 is the handle for the thread
    // Write0( "Creating task " ); WriteNum( core_number ); Write0( " " ); WriteNum( i ); NewLine;
    asm ( "isb" );

    const distance = 560;
    int32_t xx = 400 + core_number * distance;
    int32_t yy = 400 + i * distance;
    bool direction = (1 & core_number) != (1 & i);
    // Write0( "Params: " ); WriteNum( xx ); Write0( " " ); WriteNum( yy ); Write0( " " ); WriteNum( direction ); NewLine;

    uint32_t stack = 0x9000 - 0x100 * i;

    register uint32_t request asm ( "r0" ) = 0; // Create Thread
    register void *code asm ( "r1" ) = user_thread;
    register uint32_t stack_top asm ( "r2" ) = stack;
    register uint32_t x asm ( "r3" ) = xx;
    register uint32_t y asm ( "r4" ) = yy;
    register bool clockwise asm ( "r5" ) = direction ? 1 : 0;
    register uint32_t handle asm ( "r0" );

    asm volatile ( "svc %[swi]"
        : "=r" (handle) 
        : [swi] "i" (OS_ThreadOp)
        , "r" (request)
        , "r" (code)
        , "r" (stack_top)
        , "r" (x), "r" (y), "r" (clockwise) );

  }

  for (int i = 0;;i++) {
    Sleep( 60000 ); // 1 minute
    WriteNum( i ); Write0( " " ); WriteNum( 0xffffffff & timer_now() ); NewLine;
  }

  __builtin_unreachable();
}