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
extern uint32_t _binary_AllMods_end;
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
} module_header;

struct module {
  module_header *header;
  uint32_t private_word;
  uint32_t instance;
  module *next;  // Simple singly-linked list
};

static inline bool error_nomem( svc_registers *regs )
{
    static error_block nomem = { 0x101, "The area of memory reserved for relocatable modules is full" };
    regs->r[0] = (uint32_t) &nomem;
    return false;
}

static inline uint32_t start_code( module_header *header )
{
  return header->offset_to_start + (uint32_t) header;
}

static inline bool run_initialisation_code( const char *env, module *m )
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

static inline uint32_t finalisation_code( module_header *header )
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

// Returns true unless intercepted by vector code
static bool run_vector_code( svc_registers *regs, vector *v )
{
  register uint32_t non_kernel_code asm( "r14" ) = v->code;
  register uint32_t private_word asm( "r12" ) = v->private_word;

  // FIXME I don't think this handles errors well, if at all

  asm goto (
      "\n  push { %[regs] }"
      "\n  adr r0, intercepted"
      "\n  push { r0 } // Push address to stack, in case vector intercepts"
      "\n  ldm %[regs], { r0-r9 }"
      "\n  blx r14"
      "\n  add sp, sp, #4 // Remove unused intercepted address from stack"
      "\n  pop { r14 }"
      "\n  stm r14, { r0-r9 }"
      "\n  b %l[pass_on]"
      "\nintercepted:"
      "\n  pop { r14 }"
      "\n  stm r14, { r0-r9 }"
      :
      : [regs] "r" (regs)
      , "r" (non_kernel_code)
      , "r" (private_word)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9"
      : pass_on );

  return false;

pass_on:
  return true;
}

static bool run_vector( int vec, svc_registers *regs )
{
  vector *v = workspace.kernel.vectors[vec];
  bool result = true;

  while (v != 0 && run_vector_code( regs, v )) {
    v = v->next;
  }

  return result;
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

  clear_VF();

  module *m = workspace.kernel.module_list_head;
  while (m != 0 && m->header->swi_chunk != chunk) {
    m = m->next;
  }
  if (m == 0) {
    return Kernel_Error_UnknownSWI( regs );
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

  module *instance = rma_allocate( sizeof( module ), regs );

  if (instance == 0) {
    return error_nomem( regs );
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
  return run_vector( regs->r[9], regs );
}

bool do_OS_Claim( svc_registers *regs )
{
  int number = regs->r[0];
  if (number > number_of( workspace.kernel.vectors )) {
    return Kernel_Error_UnknownSWI( regs );
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

  vector *new = rma_allocate( sizeof( vector ), regs );
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
  return Kernel_Error_UnknownSWI( regs );
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

void Draw_Stroke( uint32_t *path, uint32_t *transformation_matrix )
{
  // Keep this declaration before the first register variable declaration, or
  // -Os will cause the compiler to forget the preceding registers.
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101422
  uint32_t cap_and_join_style[4] =  { 0, 0xa0000, 0x3000300, 0x30000300 };

  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = 0;
  register uint32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 0;
  register uint32_t thickness asm( "r4" ) = 80*256;
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
        , "m" (cap_and_join_style) ); // Without this, the array is not initialised
}

static void __attribute__(( naked )) default_os_byte( uint32_t r0, uint32_t r1, uint32_t r2 )
{
  // Always does a simple return to caller, never intercepting because there's no lower call.
  asm ( "push { r0-r11, lr }" );
  switch (r0) {
  case 0xa1:
    {
    switch (r1) {
    case 0x18: asm ( "mov r0, %[v]\nstr r0, [sp, #8]" : : [v] "i" (1) ); break; // UK Territory
    }
    }
    break;
  }
  asm ( "pop { r0-r11, pc }" );
}

static vector default_os_byte_v = { .next = 0, .code = (uint32_t) default_os_byte, .private_word = 0 };

bool do_OS_GenerateError( svc_registers *regs )
{
  return run_vector( 1, regs );
}

bool do_OS_WriteC( svc_registers *regs )
{
  return run_vector( 3, regs );
}

bool do_OS_ReadC( svc_registers *regs )
{
  return run_vector( 4, regs );
}

bool do_OS_CLI( svc_registers *regs )
{
  return run_vector( 5, regs );
}

bool do_OS_Byte( svc_registers *regs )
{
  return run_vector( 6, regs );
}

bool do_OS_Word( svc_registers *regs )
{
  return run_vector( 7, regs );
}

bool do_OS_File( svc_registers *regs )
{
  return run_vector( 8, regs );
}

bool do_OS_Args( svc_registers *regs )
{
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
  return run_vector( 12, regs );
}

bool do_OS_Find( svc_registers *regs )
{
  return run_vector( 13, regs );
}

bool do_OS_ReadLine( svc_registers *regs )
{
  return run_vector( 14, regs );
}

bool do_OS_FSControl( svc_registers *regs )
{
  return run_vector( 15, regs );
}

bool do_OS_GenerateEvent( svc_registers *regs )
{
  return run_vector( 16, regs );
}

bool do_OS_Mouse( svc_registers *regs )
{
  return run_vector( 26, regs );
}

bool do_OS_UpCall( svc_registers *regs )
{
  return run_vector( 29, regs );
}

bool do_OS_ChangeEnvironment( svc_registers *regs )
{
  return run_vector( 30, regs );
}

bool do_OS_SpriteOp( svc_registers *regs )
{
  return run_vector( 31, regs );
}

bool do_OS_SerialOp( svc_registers *regs )
{
  return run_vector( 36, regs );
}

void __attribute__(( naked )) fast_horisontal_line_draw( uint32_t left, uint32_t y, uint32_t right, uint32_t action )
{
  asm ( "push { r0-r12, lr }" );

  extern uint32_t frame_buffer;
  uint32_t *screen = &frame_buffer;
  uint32_t *row = screen + (1079 - y) * 1920;
  uint32_t *l = row + left;
  uint32_t *r = row + right;
  switch (action) {
  case 1:
    {
    uint32_t *p = l;
    uint32_t c = workspace.vdu.vduvars[153 - 128];
    while (p < r) {
      *p++ = c;
    }
    }
  case 2:
    {
    uint32_t *p = l;
    uint32_t c = workspace.vdu.vduvars[154 - 128];
    while (p < r) {
      *p++ = c;
    }
    }
    break;
  default: break;
  }

  asm ( "pop { r0-r12, pc }" );
}

void Boot()
{
  workspace.kernel.vectors[6] = &default_os_byte_v;

  SetInitialVduVars();

  // This is obviously becoming the boot sequence, to be refactored when something's happening...

  set_var( "Run$Path", "" );
  set_var( "File$Path", "" );

  init_module( "FileSwitch" ); // Uses MessageTrans, but survives it not being there at startup
  init_module( "ResourceFS" ); // Uses TerritoryManager
  init_module( "TerritoryManager" ); // Uses MessageTrans to open file
  init_module( "Messages" );
  init_module( "MessageTrans" );
  init_module( "UK" );

  init_module( "DrawMod" );

  init_module( "SharedCLibrary" );
  init_module( "FileCore" );

/*
//  init_module( "DrawFile" );
  init_module( "SpriteExtend" );
  init_module( "SpriteUtils" );
  init_module( "DitherExt" );
  init_module( "AWRender" );
  init_module( "GDraw" );
  init_module( "GSpriteExtend" );
*/

//  extern uint32_t _binary_Files_rfs_start;
//  register uint32_t *files asm ( "r0" ) = &_binary_Files_rfs_start;
//  asm ( "svc 0x41b40" : : "r" (files) );
  //register uint32_t matrix[6] asm( r = { 1 << 16, 0, 0, 1 << 16, 0, 0 }; // Identity matrix

  workspace.vdu.modevars[6] = 1920 * 4;

  workspace.vdu.vduvars[128 - 128] = 0;
  workspace.vdu.vduvars[129 - 128] = 0;
  workspace.vdu.vduvars[130 - 128] = 1920 - 1;
  workspace.vdu.vduvars[131 - 128] = 1080 - 1;
  extern uint32_t frame_buffer;
  workspace.vdu.vduvars[148 - 128] = (uint32_t) &frame_buffer;
  workspace.vdu.vduvars[149 - 128] = (uint32_t) &frame_buffer;
  workspace.vdu.vduvars[150 - 128] = 1920 * 1080 * 4;
  workspace.vdu.vduvars[153 - 128] = 0xffffffff; // FG (lines) white
  workspace.vdu.vduvars[154 - 128] = 0xffff0000; // BG (fill) red

  workspace.vdu.vduvars[166 - 128] = (uint32_t) fast_horisontal_line_draw;
/*
  extern uint32_t _binary_DrawFile_start;
  extern uint32_t _binary_DrawFile_size;

  register uint32_t flags asm ( "r0" ) = 0;
  register uint32_t file asm ( "r1" ) = (uint32_t) &_binary_DrawFile_start;
  register uint32_t size asm ( "r2" ) = (uint32_t) &_binary_DrawFile_size;
  register uint32_t matrix asm ( "r3" ) = 0;
  register uint32_t clipping asm ( "r4" ) = 0;
  register uint32_t flatness asm ( "r5" ) = 0;
  asm ( "svc 0x45540" : : "r" (flags), "r" (file), "r" (size), "r" (matrix), "r" (clipping), "r" (flatness) );
*/
  uint32_t path[] = { 2, 256*100, 256*100,
                      8, 256*1000, 256*800, 0 };

  uint32_t matrix[6] = { 1 << 16, 0, 0, 1 << 16, 0, workspace.core_number * (200 << 8) };

  Draw_Stroke( path, matrix );

  // Should have entered a RISC OS Application by now...
  for (;;) { asm ( "wfi" ); }
}

