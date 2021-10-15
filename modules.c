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

  clear_VF();

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
show_word( 200 * workspace.core_number, 500, regs->r[2], White );
clean_cache_to_PoC();

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
show_word( 200 * workspace.core_number, 520, (uint32_t) name, White );
clean_cache_to_PoC();

  uint32_t *rom_modules = &_binary_AllMods_start;
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

  while (0 != *rom_module) {
    module_header *header = (void*) (rom_module+1);
    register const char *title = title_string( header );
    if (0 == strcmp( title, name )) {
      register uint32_t code asm( "r0" ) = 10;
      register module_header *module asm( "r1" ) = header;

      asm ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "lr" );
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
  register uint32_t *matrix asm( "r2" ) = transformation_matrix;
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

static inline uint32_t Font_FindFont( const char *name, uint32_t xpoints, uint32_t ypoints, uint32_t xdpi, uint32_t ydpi )
{
  register uint32_t result asm( "r0" );
  register uint32_t rname asm( "r1" ) = name;
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
  // Always does a simple return to caller, never intercepting because there's no lower call.
  asm ( "push { r0-r11, lr }" );
  switch (r0) {
  case 0xa1:
    {
    switch (r1) {
    case  24: asm ( "mov r0, %[v]\nstr r0, [sp, #8]" : : [v] "i" (1) ); break; // UK Territory
    case 134: asm ( "mov r0, %[v]\nstr r0, [sp, #8]" : : [v] "i" (128) ); break; // Font Cache pages: 512k
    case 200 ... 205: asm ( "mov r0, %[v]\nstr r0, [sp, #8]" : : [v] "i" (32) ); break; // FontMax 1-5
    default: asm ( "wfi" );
    }
    }
    break;
  default: asm ( "wfi" );
  }
  asm ( "pop { r0-r11, pc }" );
}

static vector default_ByteV = { .next = 0, .code = (uint32_t) default_os_byte, .private_word = 0 };
static vector default_WrchV = { .next = 0, .code = (uint32_t) default_os_writec, .private_word = 0 };

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
      *p = 0xff00ff80; // c;
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

void Boot()
{
  workspace.kernel.vectors[6] = &default_ByteV;
  workspace.kernel.vectors[3] = &default_WrchV;

  SetInitialVduVars();

  // This is obviously becoming the boot sequence, to be refactored when something's happening...

  set_var( "Run$Path", "" );
  set_var( "File$Path", "" );

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
  workspace.vdu.vduvars[154 - 128] = 0xffffffff; // BG (fill) white

  workspace.vdu.vduvars[166 - 128] = (uint32_t) fast_horizontal_line_draw;

  init_module( "DrawMod" );
/*
  init_module( "SharedCLibrary" );
  init_module( "FileSwitch" ); // Uses MessageTrans, but survives it not being there at startup
  init_module( "TerritoryManager" ); // Uses MessageTrans to open file
  init_module( "ResourceFS" ); // Uses TerritoryManager

/*
  This requires more functionality in the system variables than currently implemented. SetMacro, etc.
  init_module( "FontManager" );
  init_module( "ROMFonts" );

  init_module( "ColourTrans" );

  init_module( "Messages" );
  // init_module( "MessageTrans" ); // Needs memory at the address returned by OSRSI6_DomainId 
  init_module( "UK" );
*/
  // init_module( "DrawFile" ); Seems to stall

//  init_module( "UtilityMod" );
/*
  init_module( "WindowManager" );
  init_module( "BufferManager" );
  init_module( "DeviceFS" );
  init_module( "RTSupport" );
  init_module( "USBDriver" );
  init_module( "FileCore" );
*/

  TaskSlot *slot = MMU_new_slot();
  physical_memory_block block = { .virtual_base = 0x8000, .physical_base = Kernel_allocate_pages( 4096, 4096 ), .size = 4096 };
  TaskSlot_add( slot, block );
  MMU_switch_to( slot );

  // This appears to be necessary. Perhaps it should be in MMU_switch_to.
  clean_cache_to_PoC();

  register param asm ( "r0" ) = workspace.core_number;
  asm ( "isb"
    "\n\tmsr cpsr, #0x17 // Abort mode: eret is unpredictable in System mode"
    "\n\tdsb"
    "\n\tisb"
    "\n\tmsr spsr, %[usermode]"
    "\n\tmov lr, %[usr]"
    "\n\tmsr sp_usr, %[stacktop]"

    "\n\tisb"
    "\n\teret" : : [stacktop] "r" (0x9000), [usr] "r" (user_mode_code), "r" (param), [usermode] "r" (0x10) );

  __builtin_unreachable();
}

static inline void show_character( uint32_t x, uint32_t y, unsigned char c, uint32_t colour )
{
  extern uint8_t system_font[128][8];
  uint32_t dx = 0;
  uint32_t dy = 0;
  c = (c - ' ') & 0x7f;

  for (dy = 0; dy < 8; dy++) {
    for (dx = 0; dx < 8; dx++) {
      if (0 != (system_font[c+1][dy] & (0x80 >> dx)))
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
  clean_cache_to_PoC();
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

//uint32_t font = 0;;
//if (core_number == 2) font = Font_FindFont( "Trinity.Medium", 24*16, 24*16, 0, 0 );

extern uint32_t frame_buffer;
claim_lock( &frame_buffer ); // Just for fun, uses the top left pixel! It looks better with the lock than without, but locking the whole screen (with a real shared lock variable) might slow things down too much.
  for (;;) {
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

    asm ( "svc %[swi]" : "=&m" (matrix) : [swi] "i" (OS_FlushCache) );
release_lock( &frame_buffer );
    for (int i = 0; i < 0x800000; i++) { asm ( "" ); }

/*
if (core_number == 3) {
show_string( core_number * 200, 400, "Hello?", White );
//Font_Paint( font, "First text", 0b100010000, core_number * 200, 400, 0 );
}
*/

claim_lock( &frame_buffer );

    SetColour( 0, 0x000000 );
//    Draw_Fill( path1, matrix );
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

