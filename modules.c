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
  uint32_t *private_word;
  uint32_t local_private_word;
  uint32_t instance;
  module *next;  // Simple singly-linked list
};

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
  WriteS( "\005Failed\005" );
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
WriteNum( code_offset + (uint32_t) m->header ); // 22504
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

static bool run_vector( int vec, svc_registers *regs )
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
  vector *v = workspace.kernel.vectors[vec];
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
      result = (l >= 'a' && l <= 'z');
    }
    else if (result == 'A'-'a') {
      result = (r >= 'a' && r <= 'z');
    }
  }
  return result;
}

static void describe_service_call( svc_registers *regs )
{
WriteS( "*** ServiceCall_" );
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

describe_service_call( regs );

  uint32_t r12 = regs->r[12];
  while (m != 0 && regs->r[1] != 0 && result) {
    regs->r[12] = (uint32_t) m->private_word;
    if (0 != m->header->offset_to_service_call_handler) {
Write0( title_string( m->header ) ); WriteS( " " );
      result = run_service_call_handler_code( regs, m );
    }
    m = m->next;
  }
  NewLine;

  regs->r[12] = r12;

  return result;
}

error_block UnknownCall = { 0x105, "Unknown OS_Module call" };

#define OSMERR( f, l ) do { WriteS( f l ); for (;;) {}; } while (0)

static bool do_Module_Run( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Load( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Enter( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_ReInit( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Delete( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
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

static bool do_Module_Claim( svc_registers *regs )
{
  uint32_t r1 = regs->r[1];
  regs->r[0] = 2;
  regs->r[1] = (uint32_t) &rma_heap;

  bool result = do_OS_Heap( regs );
  if (result) {
    regs->r[0] = 6;
    regs->r[1] = r1;
  }
  else {
    result = error_nomem( regs );
  }

  return result;
}

static bool do_Module_Free( svc_registers *regs )
{
  uint32_t r1 = regs->r[1];
  regs->r[0] = 3; // Free
  regs->r[1] = (uint32_t) &rma_heap;

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
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Clear( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
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

static module *new_instance( module_header *m, svc_registers *regs )
{
  module *instance = rma_allocate( sizeof( module ), regs );

  if (instance != 0) {
    instance->header = m;
    instance->private_word = &instance->local_private_word;
    instance->local_private_word = 0;
    instance->instance = 0;
    instance->next = 0;
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

    WriteS( "MP" );
    shared_instance = shared.kernel.module_list_head;
    while (shared_instance != 0 && shared_instance->header != new_mod) {
      shared_instance = shared_instance->next;
    }

    if (shared_instance == 0) {
      // No core has initialised this module, yet.
      // Store a copy in the shared list.
      shared_instance = new_instance( new_mod, regs );

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
    instance = new_instance( new_mod, regs );
    success = instance != 0; 

    if (success && shared_instance != 0) {
      instance->private_word = shared_instance->private_word;
      while (instance->private_word != &shared_instance->local_private_word) {
        asm ( "wfi" );
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
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_ExtractModuleInfo( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
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
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_RenameInstantiation( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_MakePreferredInstantiation( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_AddExpansionCardModule( svc_registers *regs )
{
Write0( __func__ ); for (;;) {};
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_LookupModuleName( svc_registers *regs )
{
  // Actually Lookup Module BY Name

Write0( __func__ ); Write0( regs->r[1] ); // Initially called by Wimp during init, just to find ROM location
  const char *name = regs->r[1];
  if (name[0] == 'U' && name[7] == 'M' && name[13] == 0) { // FIXME
    WriteS( "Returning UtilityModule address (hack) \\x06 " );
    extern uint32_t va_base;
    regs->r[3] = (uint32_t) &va_base;
    return true;
  }
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
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

static bool no_more_modules( svc_registers *regs )
{
  static error_block error = { 0x107, "No more modules" };

  regs->r[0] = (uint32_t) &error;

  return false;
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
    return no_more_modules( regs );
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
    return no_more_modules( regs );
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
         FindEndOfROM_ModuleChain };

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
  default:
    regs->r[0] = (uint32_t) &UnknownCall;
    return false;
  }
}

bool do_OS_CallAVector( svc_registers *regs )
{
  return run_vector( regs->r[9], regs );
}

static bool error_InvalidVector( svc_registers *regs )
{
  static error_block error = { 0x999, "Invalid vector number #" };
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
    regs.spsr = 0; // OS_Heap fails if entered with V flag set
    result = rma_allocate( sizeof( callback ), &regs );
  }
  return result;
}

bool do_OS_Claim( svc_registers *regs )
{
  WriteS( "New vector " ); WriteNum( regs->r[0] );
  WriteS( " Code " ); WriteNum( regs->r[1] );
  WriteS( " Private " ); WriteNum( regs->r[2] ); NewLine;
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

  new->code = regs->r[1];
  new->private_word = regs->r[2];
  new->next = workspace.kernel.vectors[number];

  workspace.kernel.vectors[regs->r[0]] = new;

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

  // FIXME Error on not found?
  return true;
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
  regs->r[0] = (uint32_t) workspace.kernel.env;
  regs->r[1] = 0;
  regs->r[2] = (uint32_t) &workspace.kernel.start_time;
  return true;
}

static void show_module_commands( module_header *header )
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

void init_module( const char *name )
{
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  workspace.kernel.env = name;
  workspace.kernel.start_time = 0x0101010101ull;

#ifdef DEBUG__SHOW_MODULE_INIT
  WriteS( "\x09NIT: " );
  Write0( name );
#endif

  // UtilityModule isn't a real module
  // PCI calls XOS_Hardware (and XOS_Heap 8)
  // BASIC? - starts two other modules...
  // Obey.
  // The intention is to initialise a HAL module, which can kick off a centisecond
  // upcall and initialise the hardware, including checking for pressed buttons on
  // a keyboard or similar.

  while (0 != *rom_module) {
    module_header *header = (void*) (rom_module+1);
    register const char *title = title_string( header );
    if (0 == strcmp( title, name )) {

#ifdef DEBUG__SHOW_MODULE_COMMANDS_ON_INIT
      show_module_commands( header );
#endif

      register uint32_t code asm( "r0" ) = 10;
      register module_header *module asm( "r1" ) = header;
      asm ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "lr", "cc" );
    }
    rom_module += (*rom_module)/4; // Includes size of length field
  }
}

bool excluded( const char *name )
{
  // These modules fail on init, at the moment.
  static const char *excludes[] = { "PCI"               // Data abort fc01ff04 prob. pci_handles

                                  , "BufferManager"     // Full RMA? Something odd, anyway.
                                  , "ColourTrans"     // Full RMA? Something odd, anyway.
                                  , "WindowManager"
                                  , "Portable"          // Uses OS_MMUControl
                                  , "SoundDMA"          // Uses OS_Memory
                                  , "SoundChannels"     // ???
                                  , "SoundScheduler"    // Sound_Tuning
                                  , "SpriteExtend" // ReadSysInfo
                                  , "Debugger"
                                  , "BCMSupport"        // Doesn't return, afaics
                                  , "RTSupport"         // Doesn't return, afaics
                                  , "USBDriver"         // Doesn't return, afaics
                                  , "DWCDriver"         // Doesn't return, afaics
                                  , "XHCIDriver"        // Doesn't return, afaics
                                  , "VCHIQ"             // Doesn't return, afaics
                                  , "BCMSound"          // Initialisation returns an error
                                  , "TaskManager"       // Initialisation returns an error
                                  , "ScreenModes"       // Doesn't return, afaics
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
                                  , "ColourPicker"      // 0x8600003f
                                  , "DrawFile"          // 0x8600003f - are these SharedCLib users?
                                  , "BootCommands"      // 0x8600003f
                                  , "WindowScroll"      // 0x8600003f
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
                                  , "!Edit"
                                  , "!Draw"
                                  , "!Paint"
                                  , "!Alarm"
                                  , "!Chars"
                                  , "!Help"
                                  , "Toolbox"
                                  , "Window"
                                  , "ToolAction"
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


                                  , "MbufManager"       // 0xe200004d

                                  , "DMAManager"    // 
  //                                , "DisplayManager"    // Accesses memory at 0x210184bb - Outside RMA?
                                  , "DragASprite"       // Doesn't return, afaics
                                  , "BBCEconet"       // Doesn't return, afaics
                                  , "RamFS"             // Tries to use OS_MMUControl
                                  , "Filer"             // Doesn't return, afaics
                                  , "FSLock"             // Doesn't return, afaics
                                  //, "FontManager"        // Doesn't return, afaics
                                  , "FPEmulator"        // Undefined instruction fc277a00
                                  , "VFPSupport"        // Init returned with V set
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
                                  , "ShellCLI"        // "esources$Path{,_Message} not found
                                  , "SoundControl"          // No return
                                  , "Squash"                    // No return
                                  , "BootFX"                    // No return
                                  , "SystemDevices"             // No return
                                  , "TaskWindow"             // Data abort, fc339bc4 -> 01f0343c

  };

  for (int i = 0; i < number_of( excludes ); i++) {
    if (0 == strcmp( name, excludes[i] ))
      return true;
  }
  return false;
}

extern void run_transient_callbacks();

void init_modules()
{
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  workspace.kernel.start_time = 0x0101010101ull;

  while (0 != *rom_module) {
    module_header *header = (void*) (rom_module+1);

    workspace.kernel.env = title_string( header );

#ifdef DEBUG__SHOW_MODULE_COMMANDS_ON_INIT
    WriteS( "\x09NIT: " );
    Write0( workspace.kernel.env );
#endif
    if (!excluded( workspace.kernel.env )) {
      NewLine;
      register uint32_t code asm( "r0" ) = 10;
      register module_header *module asm( "r1" ) = header;

      asm ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "lr", "cc" );

#ifdef DEBUG__SHOW_MODULE_COMMANDS_ON_INIT
      NewLine;
#endif

      // Not in USR mode, but we are idling
      run_transient_callbacks();
    }
    else {
      Write0( workspace.kernel.env );
      WriteS( " - excluded" );
      NewLine;
    }
    rom_module += (*rom_module)/4; // Includes size of length field
  }
}

static void set_var( const char *name, const char *value )
{
  svc_registers regs;
  regs.r[0] = (uint32_t) name;
  regs.r[1] = (uint32_t) value;
  regs.r[2] = strlen( value );
  regs.r[3] = 0;
  regs.r[4] = 0;

  do_OS_SetVarVal( &regs );
}

static void Draw_Fill( uint32_t *path, int32_t *transformation_matrix )
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

static void SetColour( uint32_t flags, uint32_t colour )
{
  register uint32_t in_flags asm( "r0" ) = flags;
  register uint32_t in_colour asm( "r1" ) = colour;
  asm ( "swi %[swi]" : 
        : "r" (in_colour)
        , "r" (in_flags)
        , [swi] "i" (OS_SetColour)
        : "lr" );
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

  asm ( "swi %[swi]"
        : "=r" (result)
        : "r" (rname)
        , "r" (rxpoints)
        , "r" (rypoints)
        , "r" (rxdpi)
        , "r" (rydpi)
        , [swi] "i" (0x20000 | 0x40081)
        : "lr" );

  return result;
}

void Font_Paint( uint32_t font, const char *string, uint32_t type, uint32_t startx, uint32_t starty, uint32_t length )
{
  register uint32_t rHandle asm( "r0" ) = font;
  register uint32_t rString asm( "r1" ) = 0;
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

static void __attribute__(( naked )) default_os_byte( uint32_t r0, uint32_t r1, uint32_t r2 )
{
  // Always intercepting because there's no lower call.
  register uint32_t *regs;
  asm ( "push { r0-r11 }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  WriteS( "OS_Byte " );

  switch (r0) {
  case 0x47: // Read/Write alphabet or keyboard
    {
    switch (r1) {
    case 127: // Read alphabet
      regs[2] = 1; break;
    case 255: // Read keyboard
      regs[2] = 1; break;
    default:
      WriteS( "Setting alphabet/keyboard not supported" );
    }
    }
    break;
  case 0xa1:
    {
    WriteS( "Read CMOS " ); WriteNum( r1 ); WriteS( " " ); WriteNum( r2 );
    switch (r1) {

    // No loud beep, scrolling allowed, no boot from disc, serial data format code 0
    // Read from UK territory module
    case 0x10: regs[2] = 0; break;

    // Unplugged flags
    case 0x6:
    case 0x7:
    case 0x12 ... 0x15:
      regs[2] = 0; break;
    
    // UK Territory (encoded)
    case 0x18: regs[2] = 1 ^ 1; break;

    // Font Cache pages: 512k
    case 0x86: regs[2] = 128; break;

    // Time zone (15 mins as signed)
    case 0x8b: regs[2] = 0; break;

    // FontMax, FontMax1-5
    case 0xc8 ... 0xcd: regs[2] = 32; break;

    // Alarm flags/DST ???
    case 0xdc: regs[2] = 0; break;

    default: WriteS( " CMOS byte " ); WriteNum( r1 ); asm ( "bkpt 61" );
    }
    WriteS( " = " ); WriteNum( regs[2] );
    }
    break;
  case 0xa2:
    {
    WriteS( "Write CMOS " ); WriteNum( r1 ); WriteS( " " ); WriteNum( r2 );
    switch (r1) {
    case 0x10: WriteS( "Misc flags" ); break;
    default: asm ( "bkpt 71" );
    }
    }
    break;
  case 0xa8 ... 0xff:
    {
    if (r1 == 0 && r2 == 255) {
      WriteS( " read " );
    }
    else if (r2 == 0) {
      WriteS( " write " ); WriteNum( r1 );
    }
    else {
      WriteS( " " ); WriteNum( r1 );
      WriteS( " " ); WriteNum( r2 );
    }
    // All treated the same, a place for storing a byte.
    // "; All calls &A8 to &FF are implemented together."
    // "; <NEW VALUE> = (<OLD VALUE> AND R2 ) EOR R1"
    // "; The old value is returned in R1 and the next location is returned in R2"
    // Kernel/s/PMF/osbyte

    uint8_t *v = ((uint8_t*) &workspace.vectors.zp.OsbyteVars) - 0xa6 + r0;
    regs[1] = *v;
    *v = ((*v) & r2) ^ r1;

    switch (r0) {
    case 0xc6: WriteS( " Exec handle" ); break;
    case 0xc7: WriteS( " Spool handle" ); break;
    default: asm( "bkpt 81" ); // Catch used variables I haven't identified yet
    }
    }
    break;
  default: asm ( "bkpt 91" );
  }
  NewLine;
  asm ( "pop { r0-r11, pc }" );
}

#ifdef DEBUG__SHOW_VECTOR_CALLS
#define WriteFunc do { Write0( __func__ ); NewLine; for (int i = 0; i < 13; i++) { WriteNum( regs->r[i] ); asm ( "svc 0x100+' '" ); } WriteNum( regs->lr ); asm ( "svc 0x100+' '" ); WriteNum( regs->spsr ); NewLine; } while (false)
#else
#define WriteFunc
#endif

bool do_OS_GenerateError( svc_registers *regs )
{
WriteFunc;
  return run_vector( 1, regs );
}

bool do_OS_WriteC( svc_registers *regs )
{
if (regs->r[0] == 0) {
  register int r0 asm( "r0" ) = regs->lr;
  asm ( "bkpt 15" : : "r" (r0) );
}
  return run_vector( 3, regs );
}

bool do_OS_ReadC( svc_registers *regs )
{
WriteFunc;
  return run_vector( 4, regs );
}

bool do_OS_CLI( svc_registers *regs )
{
WriteFunc;
  return run_vector( 5, regs );
}

bool do_OS_Byte( svc_registers *regs )
{
WriteFunc;
  return run_vector( 6, regs );
}

bool do_OS_Word( svc_registers *regs )
{
WriteFunc;
  return run_vector( 7, regs );
}

bool do_OS_File( svc_registers *regs )
{
WriteFunc;
  return run_vector( 8, regs );
}

bool do_OS_Args( svc_registers *regs )
{
WriteFunc;
  return run_vector( 9, regs );
}

bool do_OS_BGet( svc_registers *regs )
{
  return run_vector( 10, regs );
}

bool do_OS_BPut( svc_registers *regs )
{
  return run_vector( 11, regs );
}

bool do_OS_GBPB( svc_registers *regs )
{
WriteFunc;
  return run_vector( 12, regs );
}

bool do_OS_Find( svc_registers *regs )
{
WriteFunc;
  return run_vector( 13, regs );
}

bool do_OS_ReadLine( svc_registers *regs )
{
WriteFunc;
  return run_vector( 14, regs );
}

bool do_OS_FSControl( svc_registers *regs )
{
WriteFunc;
  return run_vector( 15, regs );
}

bool do_OS_GenerateEvent( svc_registers *regs )
{
WriteFunc;
  return run_vector( 16, regs );
}

bool do_OS_Mouse( svc_registers *regs )
{
WriteFunc;
  return run_vector( 26, regs );
}

bool do_OS_UpCall( svc_registers *regs )
{
WriteFunc;
  return run_vector( 29, regs );
}

bool do_OS_ChangeEnvironment( svc_registers *regs )
{
WriteFunc;
  return run_vector( 30, regs );
}

bool do_OS_SpriteOp( svc_registers *regs )
{
WriteFunc;
  return run_vector( 31, regs );
}

bool do_OS_SerialOp( svc_registers *regs )
{
WriteFunc;
  return run_vector( 36, regs );
}

void __attribute__(( naked )) fast_horizontal_line_draw( uint32_t left, uint32_t y, uint32_t right, uint32_t action )
{
  asm ( "push { r0-r12, lr }" );

  extern uint32_t frame_buffer;
  uint32_t *screen = &frame_buffer;
  uint32_t *row = screen + (1079 - y) * 1920;
  uint32_t *l = row + left;
  uint32_t *r = row + right;
  switch (action) {
  case 1: // Foreground
    {
    uint32_t *p = l;
    uint32_t c = workspace.vdu.vduvars[153 - 128];
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
      *p = 0xff333333; // ~*p; // FIXME?
      p++;
    }
    }
    break;
  case 3: // Background
    {
    uint32_t *p = l;
    uint32_t c = workspace.vdu.vduvars[154 - 128];
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

static uint32_t draw_sin( int deg )
{
  while (deg < 0) { deg += 360; }
  while (deg > 360) { deg -= 360; }
  if (deg > 180) return -draw_sin( deg - 180 );
  if (deg > 90) return draw_sin( 180 - deg );
  return sines[deg];
}

static uint32_t draw_cos( int deg )
{
  return draw_sin( deg + 90 );
}

static void fill_rect( uint32_t left, uint32_t top, uint32_t w, uint32_t h, uint32_t c )
{
  extern uint32_t frame_buffer;
  uint32_t *screen = &frame_buffer;

  for (uint32_t y = top; y < top + h; y++) {
    uint32_t *p = &screen[y * 1920 + left];
    for (int x = 0; x < w; x++) { *p++ = c; }
  }
}

static void user_mode_code();

static const char *discard_leading_characters( const char *command )
{
  const char *c = command;
  while (*c == ' ' || *c == '*') c++;
  return c;
}

static bool terminator( char c )
{
  return c == '\0' || c == '\r' || c == '\n';
}

static uint32_t count_params( const char *p )
{
  uint32_t result = 0;

  while (!terminator( *p )) {
    while (*p == ' ') p++;

    result ++;
    if ('"' == *p) {
      do {
        p ++;
      } while (!terminator( *p ) && *p != '"');
      if (*p != '"') return -1; // Mistake
    }
    else {
      while (!terminator( *p ) && *p != ' ') p++;
    }
  }

  return result;
}

static error_block *run_module_command( const char *command )
{
  module *m = workspace.kernel.module_list_head;
  while (m != 0) {
    module_header *header = m->header;

    const char *cmd = (void*) (header->offset_to_help_and_command_keyword_table + (uint32_t) header);
    while (cmd[0] != '\0') {
      int len = strlen( cmd );
      if (0 == riscoscmp( cmd, command, true )) {
        struct {
          uint32_t code_offset;
          uint32_t info_word;
          uint32_t invalid_syntax_offset;
          uint32_t help_offset;
        } *c = (void*) &cmd[(len+4)&~3]; // +4 because len is strlen, not including terminator

        if (c->code_offset != 0) {
          const char *params = command + len;
          while (*params == ' ') params++;
          uint32_t count = count_params( params );

          if (count == -1) {
            static error_block mistake = { 4, "Mistake" };
            return &mistake;
          }

          WriteS( "Running command " ); Write0( command ); WriteS( " in " ); Write0( title_string( header ) ); WriteS( " at " ); WriteNum( c->code_offset ); NewLine;
          return run_command( m, c->code_offset, params, count );
        }
      }
      cmd = &cmd[(len+20)&~3]; // +4 for terminator and alignment, +16 for words
    }
    m = m->next;
  }

  static error_block not_found = { 214, "Command not found" };

  return &not_found;
}

static void __attribute__(( noinline )) do_CLI( const char *command )
{
  WriteS( "CLI: " ); Write0( command ); NewLine;
  // PRM 1-958
  command = discard_leading_characters( command );
  if (*command == '|') return; // Comment, nothing to do
  bool run = (*command == '/');
  if (run) {
    command++;
  }
  else {
    run = run ||
     ((command[0] == 'R' || command[0] == 'r') &&
      (command[1] == 'U' || command[1] == 'u') &&
      (command[2] == 'N' || command[2] == 'n') &&
      (command[3] == ' ' || command[3] == '\0' || command[3] == '\n'));
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
    WriteS( "Looking for " ); Write0( variable ); NewLine;
    char result[256];
    register const char *var_name asm( "r0" ) = variable;
    register char *value asm( "r1" ) = result;
    register uint32_t size asm( "r2" ) = sizeof( result );
    register uint32_t context asm( "r3" ) = 0;
    register uint32_t convert asm( "r4" ) = 0;
    error_block *error;
    asm ( "svc 0x20023\n  movvs %[err], r0\n  movvc %[err], #0"
        : "=r" (size), [err] "=r" (error)
        : "r" (var_name), "r" (value), "r" (size), "r" (context), "r" (convert)
        : "lr", "cc" );
    if (error == 0) {
      Write0( variable ); WriteS( "Exists: " ); Write0( result );
      asm ( "bkpt 41" );
    }
  }

  error_block *error = run_module_command( command );

  if (error != 0 && error->code == 214) {
    // Not found in any module
    WriteS( "Command not found" );
    asm( "bkpt 51" );
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

static void __attribute__(( naked )) finish_vector()
{
  asm volatile ( "pop {pc}" );
}


void Boot()
{
  static vector default_ByteV = { .next = 0, .code = (uint32_t) default_os_byte, .private_word = 0 };
  static vector default_ChEnvV = { .next = 0, .code = (uint32_t) default_os_changeenvironment, .private_word = 0 };
  static vector default_CliV = { .next = 0, .code = (uint32_t) default_os_cli, .private_word = 0 };
  static vector do_nothing = { .next = 0, .code = (uint32_t) finish_vector, .private_word = 0 };

  for (int i = 0; i < number_of( workspace.kernel.vectors ); i++) {
    workspace.kernel.vectors[i] = &do_nothing;
  }

  workspace.kernel.vectors[0x05] = &default_CliV;
  workspace.kernel.vectors[0x06] = &default_ByteV;
  workspace.kernel.vectors[0x1e] = &default_ChEnvV;

  SetInitialVduVars();

  // PMF/osinit replacement:
  // Avoid "Buffer too small" error from BufferManager, which seems not to be returned in r0
  workspace.vectors.zp.PrinterBufferAddr = 0xfaff2c98; // Where from?
  workspace.vectors.zp.PrinterBufferSize = 0x1000; // 

  // This is obviously becoming the boot sequence, to be refactored when something's happening...

  workspace.vdu.modevars[0] = 0x40; // Don't know what this means, but it's what the real thing returns
  workspace.vdu.modevars[3] = -1;
  workspace.vdu.modevars[4] = 1;
  workspace.vdu.modevars[5] = 1;
  workspace.vdu.modevars[6] = 1920 * 4;
  workspace.vdu.modevars[9] = 5;
  workspace.vdu.modevars[12] = 1080-1;
// TODO set these from information from the HAL
  workspace.vdu.vduvars[128 - 128] = 0;         // VduExt_GWLCol
  workspace.vdu.vduvars[129 - 128] = 0;         // VduExt_GWBRow
  workspace.vdu.vduvars[130 - 128] = 1920 - 1;  // VduExt_GWRCol
  workspace.vdu.vduvars[131 - 128] = 1080 - 1;  // VduExt_GWTRow
  extern uint32_t frame_buffer;
  workspace.vdu.vduvars[148 - 128] = (uint32_t) &frame_buffer;
  workspace.vdu.vduvars[149 - 128] = (uint32_t) &frame_buffer;
  workspace.vdu.vduvars[150 - 128] = 1920 * 1080 * 4;
  workspace.vdu.vduvars[153 - 128] = 0xffffffff; // FG (lines) white
  workspace.vdu.vduvars[154 - 128] = 0xffffffff; // BG (fill) white

  workspace.vdu.vduvars[166 - 128] = (uint32_t) fast_horizontal_line_draw;

  // Start the HAL, a multiprocessing-aware module that initialises essential features before
  // the boot sequence can start.
  {
    extern uint32_t _binary_Modules_HAL_start;
    register uint32_t code asm( "r0" ) = 10;
    register uint32_t *module asm( "r1" ) = &_binary_Modules_HAL_start;

    asm ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "lr", "cc" );
  }

  init_modules();
/*
  init_module( "UtilityModule" );

  init_module( "FileSwitch" );
  init_module( "ResourceFS" );
  init_module( "TerritoryManager" );
  init_module( "Messages" );
  init_module( "MessageTrans" );
  init_module( "UK" );
  init_module( "BlendTable" );
  init_module( "ColourTrans" );
  init_module( "FontManager" );
  init_module( "ROMFonts" );
  init_module( "DrawMod" );
*/
  NewLine; WriteS( "All modules initialised, starting USR mode code" ); NewLine;


  TaskSlot *slot = MMU_new_slot();
  physical_memory_block block = { .virtual_base = 0x8000, .physical_base = Kernel_allocate_pages( 4096, 4096 ), .size = 4096 };
  TaskSlot_add( slot, block );
  MMU_switch_to( slot );

  // This appears to be necessary. Perhaps it should be in MMU_switch_to.
  clean_cache_to_PoC();

  register uint32_t param asm ( "r0" ) = workspace.core_number;
  // Remember: eret is unpredictable in System mode
  asm ( "isb"
    "\n\tmsr spsr, %[usermode]"
    "\n\tmov lr, %[usr]"
    "\n\tmsr sp_usr, %[stacktop]"
    "\n\tisb"
    "\n\teret" : : [stacktop] "r" (0x9000), [usr] "r" (user_mode_code), "r" (param), [usermode] "r" (0x10) );

  __builtin_unreachable();
}

static inline void show_character( uint32_t x, uint32_t y, unsigned char c, uint32_t colour )
{
  extern uint8_t system_font[128-32][8];
  uint32_t dx = 0;
  uint32_t dy = 0;
  c = (c & 0x7f) - ' ';

  for (dy = 0; dy < 8; dy++) {
    for (dx = 0; dx < 8; dx++) {
      if (0 != (system_font[c][dy] & (0x80 >> dx)))
        set_pixel( x+dx, y+dy, colour );
      else
        set_pixel( x+dx, y+dy, Black );
    }
  }
}

void show_string( uint32_t x, uint32_t y, const char *string, uint32_t colour )
{
  while (*string != 0) {
    show_character( x, y, *string++, colour );
    x += 8;
  }
  asm ( "svc %[swi]" : : [swi] "i" (OS_FlushCache) : "lr", "cc" );
}

// None of the following will remain in the kernel, it is experimental user
// mode code.

/*
// Damnit, this breaks simple stuff...
#undef Write0
// ss is there in case the s parameter is even slightly complicated
#define Write0( s ) do { const char *ss = s; register const char *string asm( "r0" ) = ss; asm ( "svc 2" : : "r" (string) ); } while (false)
*/

static uint32_t open_file_to_read( const char *name )
{
  // OS_Find
  register uint32_t os_find_code asm( "r0" ) = 0x43 | (1 << 3);
  register const char *filename asm( "r1" ) = name;
  register uint32_t file_handle asm( "r0" );
  asm ( "svc 0x0d" : "=r" (file_handle) : "r" (os_find_code), "r" (filename) ); // Doesn't corrupt lr because running usr

  return file_handle;
}

static void user_mode_code( int core_number )
{
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

  int32_t offx = (400 << 8) + core_number * (560 << 8);
  int32_t offy = 400 << 8;
  int32_t matrix[6] = { 0, 0, 0, 0, offx, offy };

  bool odd = 0 != (core_number & 1);
  // Re-start after 45 degree turn (octagonal cog)
  int angle = odd ? 0 : 22; // Starting angle
  int step = 2;

  {
    char buffer[256];
    const char var[] = "Font$Path";
    WriteS( "Reading " ); Write0( var ); NewLine;

    register const char *var_name asm( "r0" ) = var;
    register char *value asm( "r1" ) = buffer;
    register uint32_t size asm( "r2" ) = sizeof( buffer )-1;
    register uint32_t context asm( "r3" ) = 0;
    register uint32_t convert asm( "r4" ) = 0;
    register error_block *error;
    asm ( "svc 0x20023\n  movvs %[error], r0\n  movvc %[error], #0" : [error] "=r" (error), "=r" (size) : "r" (var_name), "r" (value), "r" (size), "r" (context), "r" (convert) : "lr", "cc" );
    if (error != 0) {
      WriteS( "Error: " ); Write0( error->desc ); NewLine;
    }
    else {
      WriteS( "Value size = " ); WriteNum( size ); NewLine;
      WriteS( "Value \\\"" ); buffer[size] = '\0'; Write0( buffer ); WriteS( "\\\"" ); NewLine;
    }
  }

  if (0) { // Try a GSTrans, it fails in FindFont
    char buffer[256];
    const char var[] = "<Font$Path>";

    register const char *var_name asm( "r0" ) = var;
    register char *value asm( "r1" ) = buffer;
    register uint32_t size asm( "r2" ) = sizeof( buffer )-1;
    register error_block *error;
    asm ( "svc 0x20027\n  movvs %[error], r0\n  movvc %[error], #0" : [error] "=r" (error), "=r" (size) : "r" (var_name), "r" (value), "r" (size) : "lr", "cc" );
    if (error != 0) {
      WriteS( "Error: " ); Write0( error->desc ); NewLine;
    }
    else {
      WriteS( "Value size = " ); WriteNum( size ); NewLine;
      WriteS( "Value \\\"" ); buffer[size] = '\0'; Write0( buffer ); WriteS( "\\\"" ); NewLine;
    }
  }

OSCLI( "Echo Hello" );
// OSCLI( "Eval 1 + 1" ); Fails with data abort attempting to read from 0 
// OSCLI( "ROMModules" ); Fails with lots of Buffer overflows.

const char *filename = "Resources:$.Resources.Alarm.Messages";
uint32_t file_handle = open_file_to_read( filename );

WriteS( "Opened file " ); Write0( filename ); WriteS( " handle: " ); WriteNum( file_handle ); NewLine;

register uint32_t handle asm( "r1" ) = file_handle;
asm ( "0: svc 0x0a\n  svccc 0\n  bcc 0b" : : "r" (handle) );
asm( "bkpt 99" );
/*
register uint32_t version asm( "r0" );
register uint32_t size asm( "r2" );
register uint32_t used asm( "r3" );
asm ( "svc 0x40080" : "=r" (version), "=r" (size), "=r" (used) );
WriteS( "Read 

WriteS( "Looking for Trinity.Medium" ); NewLine;
uint32_t font = Font_FindFont( "Trinity.Medium", 12 * 16, 12 * 16, 96, 96 );
if (font > 255) {
  Write0( (const char*) (font + 4) );
}
  WriteS( "Found font " ); WriteNum( font ); NewLine;

Font_Paint( font, "Hello world", (1 << 4), 500 * core_number, 200, 0 );
*/
  for (int loop = 0;; loop++) {

    matrix[0] =  draw_cos( angle );
    matrix[1] =  draw_sin( angle );
    matrix[2] = -draw_sin( angle );
    matrix[3] =  draw_cos( angle );

    SetColour( 0, 0x990000 );
    Draw_Fill( path1, matrix );
    SetColour( 0, 0xe50000 );
    Draw_Fill( path2, matrix );
    SetColour( 0, 0x4c0000 );
    Draw_Fill( path3, matrix );

    asm ( "svc %[swi]" : : [swi] "i" (OS_FlushCache) : "cc" ); // lr is not corrupted, in USR mode

    for (int i = 0; i < 0x800000; i++) { asm ( "" ); }

    SetColour( 0, 0x000000 );
//    Draw_Fill( path1, matrix ); // Not needed for small changes in angle
    Draw_Fill( path2, matrix );
    Draw_Fill( path3, matrix );

    if (odd) {
      angle -= step;
      if (angle < 0) angle += 45;
    }
    else {
      angle += step;
      if (angle >= 45) angle -= 45;
    }
  }
  __builtin_unreachable();
}

