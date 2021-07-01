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

extern uint32_t rma_base; // Loader generated
extern uint32_t rma_heap; // Loader generated
extern uint32_t sma_lock; // Loader generated
extern uint32_t sma_heap; // Loader generated

// ROM Modules, with the length in a word before the code:
extern uint32_t _binary_AllMods_start;
extern uint32_t _binary_AllMods_end;

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
} module_header;

struct module {
  module_header *header;
  uint32_t private_word;
  uint32_t instance;
  module *next;  // Simple singly-linked list
};

static uint32_t start_code( module_header *header )
{
  return header->offset_to_start + (uint32_t) header;
}

static bool run_initialisation_code( const char *env, module *m )
{
  register uint32_t non_kernel_code asm( "r14" ) = m->header->offset_to_initialisation + (uint32_t) m->header;
  register uint32_t private_word asm( "r12" ) = (uint32_t) &m->private_word;
  register uint32_t instance asm( "r11" ) = m->instance;
  register const char *environment asm( "r10" ) = env;

  asm goto (
        "  blx r14"
      "\n  bvs %l[failed]"
      :
      : "r" (non_kernel_code)
      , "r" (private_word)
      , "r" (instance)
      , "r" (environment)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6"
      : failed );

  // No changes to the registers by the module are of any interest,
  // so avoid corrupting any by simply not storing them

  return true;

failed:
  return false;
}

static uint32_t finalisation_code( module_header *header )
{
  return header->offset_to_finalisation + (uint32_t) header;
}

static bool run_service_call_handler_code( svc_registers *regs, module *m )
{
  register uint32_t non_kernel_code asm( "r14" ) = m->header->offset_to_service_call_handler + (uint32_t) m->header;
  register uint32_t private_word asm( "r12" ) = (uint32_t) &m->private_word;

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

static bool run_swi_handler_code( svc_registers *regs, uint32_t svc, module *m )
{
  register uint32_t non_kernel_code asm( "r14" ) = m->header->offset_to_swi_handler + (uint32_t) m->header;
  register uint32_t private_word asm( "r12" ) = (uint32_t) &m->private_word;
  register uint32_t svc_index asm( "r11" ) = svc & 0x3f;

  asm goto (
        "  push { %[regs] }"
      "\n  ldm %[regs], { r0-r9 }"
      "\n  blx r14"
      "\n  pop { r14 }"
      "\n  stm r14, { r0-r9 }"
      "\n  bvs %l[failed]"
      :
      : [regs] "r" (regs)
      , "r" (private_word)
      , "r" (svc_index)
      , "r" (non_kernel_code)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9"
      : failed );

  return true;

failed:
  return false;
}

static bool run_vector_code( svc_registers *regs, vector *v )
{
  register uint32_t non_kernel_code asm( "r14" ) = v->code;
  register uint32_t private_word asm( "r12" ) = (uint32_t) &v->private_word;

  asm goto (
        "  push { %[regs] }"
      "\n  ldm %[regs], { r0-r9 }"
      "\n  blx r14"
      "\n  pop { r14 }"
      "\n  stm r14, { r0-r9 }"
      "\n  bvs %l[failed]"
      :
      : [regs] "r" (regs)
      , "r" (non_kernel_code)
      , "r" (private_word)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9"
      : failed );

  return true;

failed:
  return false;
}

static uint32_t swi_decoding_table_code( module_header *header )
{
  return header->offset_to_swi_decoding_table + (uint32_t) header;
}

static uint32_t swi_decoding_code( module_header *header )
{
  return header->offset_to_swi_decoding_code + (uint32_t) header;
}

static const char *title_string( module_header *header )
{
  return (const char *) header->offset_to_title_string + (uint32_t) header;
}

static const char *help_string( module_header *header )
{
  return (const char *) header->offset_to_help_string + (uint32_t) header;
}

bool do_module_swi( svc_registers *regs, uint32_t svc )
{
  uint32_t chunk = svc & ~Xbit & ~0x3f;

  clear_VF();

  module *m = workspace.kernel.module_list_head;
  while (m != 0 && m->header->swi_chunk != chunk) {
    m = m->next;
  }
  if (m == 0) {
    regs->r[0] = Kernel_Error_UnknownSWI;
    return false;
  }
  return run_swi_handler_code( regs, svc, m );
}

bool do_OS_ServiceCall( svc_registers *regs )
{
  bool result = true;
  module *m = workspace.kernel.module_list_head;

  uint32_t r12 = regs->r[12];
  while (m != 0 && regs->r[1] != 0 && result) {
    regs->r[12] = (uint32_t) &m->private_word;
    if (0 != m->header->offset_to_service_call_handler) {
      result = run_service_call_handler_code( regs, m );
    }
    m = m->next;
  }
  regs->r[12] = r12;

  return result;
}

error_block UnknownCall = { 0x105, "Unknown OS_Module call" };

static bool do_Module_Run( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Load( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Enter( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_ReInit( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Delete( svc_registers *regs )
{
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
    static error_block nomem = { 0x101, "The area of memory reserved for relocatable modules is full" };
    regs->r[0] = (uint32_t) &nomem;
  }
  return result;
}

static bool do_Module_Free( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Tidy( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_Clear( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_InsertFromMemory( svc_registers *regs )
{
  module_header *new_mod = (void*) regs->r[1];

  module *instance = (void*) rma_allocate( sizeof( module ), regs );

  if (instance == 0) {
    for (;;) { asm ( "wfi" ); }
  }

  instance->header = new_mod;
  instance->private_word = 0;
  instance->instance = 0;
  instance->next = 0;

  // "During initialisation, your module is not on the active module list, and so you cannot call SWIs
  // in your own SWI chunk."

  if (0 != new_mod->offset_to_initialisation) {
    bool success = run_initialisation_code( "", instance );
    if (!success) {
      for (;;) { asm ( "wfi" ); }
    }
  }

  if (workspace.kernel.module_list_tail == 0) {
    workspace.kernel.module_list_head = instance;
  }
  else {
    workspace.kernel.module_list_tail->next = instance;
  }

  workspace.kernel.module_list_tail = instance;

  return true;
}

static bool do_Module_InsertAndRelocateFromMemory( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_ExtractModuleInfo( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_ExtendBlock( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_CreateNewInstantiation( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_RenameInstantiation( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_MakePreferredInstantiation( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_AddExpansionCardModule( svc_registers *regs )
{
  regs->r[0] = (uint32_t) &UnknownCall;
  return false;
}

static bool do_Module_LookupModuleName( svc_registers *regs )
{
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
  uint32_t *rom_modules_end = &_binary_AllMods_end;
  uint32_t *rom_module = rom_modules;

  for (int i = 0; i < n && rom_module < rom_modules_end; i++) {
    rom_module += 1 + (*rom_module)/4; // One word of length
  }

  if (rom_module == rom_modules_end) {
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
  uint32_t *rom_modules_end = &_binary_AllMods_end;
  uint32_t *rom_module = rom_modules;

  for (int i = 0; i < n && rom_module < rom_modules_end; i++) {
    rom_module += 1 + (*rom_module)/4; // One word of length
  }

  if (rom_module == rom_modules_end) {
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

bool do_OS_Module( svc_registers *regs )
{
  enum { Run, Load, Enter, ReInit, Delete, DescribeRMA,
         Claim, Free, Tidy, Clear, InsertFromMemory,
         InsertAndRelocateFromMemory, ExtractModuleInfo,
         ExtendBlock, CreateNewInstantiation, RenameInstantiation,
         MakePreferredInstantiation, AddExpansionCardModule,
         LookupModuleName, EnumerateROMModules, EnumerateROMModulesWithVersion };

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
  default:
    regs->r[0] = (uint32_t) &UnknownCall;
    return false;
  }
}

bool do_OS_CallAVector( svc_registers *regs )
{
  vector *v = workspace.kernel.vectors[regs->r[9]];
  bool result = true;

  uint32_t r12 = regs->r[12];
  while (v != 0 && regs->r[1] != 0 && result) {
    result = run_vector_code( regs, v );
    v = v->next;
  }
  regs->r[12] = r12;

  return result;
}

bool do_OS_Claim( svc_registers *regs )
{
  regs->r[0] = Kernel_Error_UnknownSWI;
  return false;
}
bool do_OS_Release( svc_registers *regs )
{
  regs->r[0] = Kernel_Error_UnknownSWI;
  return false;
}
bool do_OS_AddToVector( svc_registers *regs )
{
  regs->r[0] = Kernel_Error_UnknownSWI;
  return false;
}
bool do_OS_DelinkApplication( svc_registers *regs )
{
  regs->r[0] = Kernel_Error_UnknownSWI;
  return false;
}
bool do_OS_RelinkApplication( svc_registers *regs )
{
  regs->r[0] = Kernel_Error_UnknownSWI;
  return false;
}

bool do_OS_GetEnv( svc_registers *regs )
{
  regs->r[0] = workspace.kernel.env;
  regs->r[1] = 0;
  regs->r[2] = &workspace.kernel.start_time;
}

void Generate_the_SMA()
{
  // Create a Shared Module Area, and initialise a heap in it.
  // This is for multi-processing aware software, and changes to its structure
  // (allocating, freeing, etc.) will be protected by a lock at the base address.

  uint32_t SMA = Kernel_allocate_pages( natural_alignment, natural_alignment );
  uint32_t initial_sma_size = natural_alignment;

  MMU_map_at( &sma_heap, SMA, initial_sma_size );

  svc_registers regs = { .r[0] = 0, .r[1] = (uint32_t) &sma_heap, .r[3] = initial_sma_size };

  if (!do_OS_Heap( &regs )) {
    for (;;) { asm ( "wfi" ); }
  }
}

void init_module( const char *name )
{
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_modules_end = &_binary_AllMods_end;
  uint32_t *rom_module = rom_modules;

  workspace.kernel.env = name;
  workspace.kernel.start_time = 0x0101010101ull;

  // UtilityModule isn't a real module
  // PCI calls XOS_Hardware (and XOS_Heap 8)
  // BASIC? - starts two other modules...
  // Obey.
  // The intention is to initialise a HAL module, which can kick off a centisecond
  // upcall and initialise the hardware, including checking for pressed buttons on
  // a keyboard or similar.

  while (rom_module < rom_modules_end) {
    module_header *header = (void*) (rom_module+1);
    register const char *title = title_string( header );
    if (0 == strcmp( title, name )) {
      register uint32_t code asm( "r0" ) = 10;
      register module_header *module asm( "r1" ) = header;

      asm ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) );
    }
    rom_module += 1 + (*rom_module)/4; // One word of length
  }
}

void Generate_the_RMA()
{
  // Create a Relocatable Module Area, and initialise a heap in it.

  uint32_t RMA = Kernel_allocate_pages( natural_alignment, natural_alignment );
  uint32_t initial_rma_size = natural_alignment;

  MMU_map_at( &rma_heap, RMA, initial_rma_size );

  svc_registers regs = { .r[0] = 0, .r[1] = (uint32_t) &rma_heap, .r[3] = initial_rma_size };

  if (!do_OS_Heap( &regs )) {
    for (;;) { asm ( "wfi" ); }
  }

  // This is obviously becoming the boot sequence, to be refactored when something's happening...
  // Current confusion: Why does ResourceFS need to know the screen mode?

  init_module( "Obey" );
  init_module( "FileCore" );
  init_module( "FileSwitch" );
  init_module( "ResourceFS" );
}
