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

/* WIMP Module, as an experiment. */

/* Based off A Beginner's Guide to Wimp Programming, at least to start with. */

/* Build commands:
  arm-linux-gnueabi-gcc-8 simple_wimp.c -Wall -o SimpleWimp.elf -fpic -I ../.. 
    -nostartfiles -nostdlib -fno-zero-initialized-in-bss -static -O4
    -g -march=armv8-a+nofp -T ../../module.script -I . -DNO_MODULE_SIZE &&
  arm-linux-gnueabi-objdump -x --disassemble-all SimpleWimp.elf > SimpleWimp.dump &&
  arm-linux-gnueabi-objcopy -R .ignoring -O binary SimpleWimp.elf SimpleWimp
*/

/* Common mistakes: */
/* Not specifying "lr" (and "cc") in inline SWI calls, which puts the
   module into an infinite loop. */
/* Not specifying registers clobbered by a SWI, either in clobbered list,
   if they're not also input to the SWI, or as output registers. */

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing

#define MODULE_CHUNK "0x8ff00"
#include "module.h"

//NO_start;
//NO_init;
NO_finalise;
NO_service_call;
//NO_title;
NO_help;
NO_keywords;
//NO_swi_handler;
//NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "WIMPModule";

/************** A lot of this should go into module.h, I think */

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

// Check that this doesn't get optimised to a call to memset!
void *memset(void *s, int c, size_t n)
{
  // In this pattern, if there is a larger size, and it is double the current one, use "if", otherwise use "while"
  char cv = c & 0xff;
  char *cp = s;
  // Next size is double, use if, not while
  if ((((size_t) cp) & (1 << 0)) != 0 && n >= sizeof( cv )) { *cp++ = cv; n-=sizeof( cv ); }

  uint16_t hv = cv; hv = hv | (hv << (8 * sizeof( cv )));
  uint16_t *hp = (void*) cp;
  // Next size is double, use if, not while
  if ((((size_t) hp) & (1 << 1)) != 0 && n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }

  uint32_t wv = hv; wv = wv | (wv << (8 * sizeof( hv )));
  uint32_t *wp = (void*) hp;
  // Next size is double, use if, not while
  if ((((size_t) wp) & (1 << 2)) != 0 && n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }

  uint64_t dv = wv; dv = dv | (dv << (8 * sizeof( wv )));
  uint64_t *dp = (void*) wp;
  // No larger size, use while, not if, and don't check the pointer bit
  while (n >= sizeof( dv )) { *dp++ = dv; n-=sizeof( dv ); }

  wp = (void *) dp; if (n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }
  hp = (void *) wp; if (n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }
  cp = (void *) hp; if (n >= sizeof( cv )) { *cp++ = cv; n-=sizeof( cv ); }

  return s;
}

#ifdef DEBUG_OUTPUT
#define WriteS( string ) asm ( "svc 1\n  .string \""string"\"\n  .balign 4" : : : "lr" )

#define NewLine asm ( "svc 3" : : : "lr" )

#define Write0( string ) do { register uint32_t r0 asm( "r0" ) = (uint32_t) (string); asm ( "push { r0-r12, lr }\nsvc 2\n  pop {r0-r12, lr}" : : "r" (r0) ); } while (false)
#define Write13( string ) do { const char *s = (void*) (string); register uint32_t r0 asm( "r0" ); while (31 < (r0 = *s++)) { asm ( "push { r1-r12, lr }\nsvc 0\n  pop {r1-r12, lr}" : : "r" (r0) ); } } while (false)
#else
#define WriteS( string )
#define NewLine
#define Write0( string )
#define Write13( string )
#endif

static void WriteNum( uint32_t number )
{
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (c > '9') c += ('a' - '0' - 10);
    register uint32_t r0 asm( "r0" ) = c;
    asm( "svc 0": : "r" (r0) : "lr", "cc" );
  }
}

static void WriteSmallNum( uint32_t number, int min )
{
  bool started = false;
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (!started && c == '0' && nibble >= min) continue;
    started = true;
    if (c > '9') c += ('a' - '0' - 10);
    register uint32_t r0 asm( "r0" ) = c;
    asm( "svc 0": : "r" (r0) : "lr", "cc" );
  }
}

// Return the relocated address of the item in the module: function or constant.
static void * local_ptr( const void *p )
{
  register uint32_t result;
  asm ( "adrl %[result], local_ptr" : [result] "=r" (result) );
  return (void*) (result + (char*) p - (char *) local_ptr);
}
/************ End */

static void *rma_claim( uint32_t bytes )
{
  // XOS_Module 6 Claim
  register uint32_t code asm( "r0" ) = 6;
  register uint32_t size asm( "r3" ) = bytes;
  register void *memory asm( "r2" );
  asm ( "svc 0x2001e" : "=r" (memory) : "r" (size), "r" (code) : "lr", "cc" );

  return memory;
}

static uint32_t OpenFileForWriting( const char *filename )
{
  register uint32_t code asm( "r0" ) = 0x83; // No File$Path, create for write
  register char const * file asm( "r1" ) = filename;
  register uint32_t open asm( "r0" );
  asm ( "svc 0x2000d" : "=r" (open) : "r" (code), "r" (file) : "lr", "cc" );
  return open;
}

static void CloseFile( uint32_t file )
{
  register uint32_t code asm( "r0" ) = 0;
  register uint32_t f asm( "r1" ) = file;
  register uint32_t error = 0;
  asm ( "svc 0x2000d\n  movvs %[error], r0" : [error] "=r" (error) : "r" (code), "r" (f) : "lr", "cc" );
  if (error != 0) {
    OpenFileForWriting( "Error" );
  }
}

static inline error_block *OSCLI( const char *command )
{
  register const char *c asm( "r0" ) = command;
  register error_block *result asm( "r0" );
  asm volatile ( "svc 0x20005\n  movvc %[error], #0" : [error] "=r" (result) : "r" (c) : "lr", "cc" );
  return result;
}

enum {
  VV_GSTrans_on_write,  // String       r2 ignored (input is scanned to find length)
  VV_NumberFromMemory,  // Number       r2 must be 4?
  VV_GSTrans_on_read,   // Macro        r2 probably not ignored
  VV_Evaluate_on_read,  // Expanded     ditto
  VV_No_GSTrans,        // Literal      r2 needed
  VV_Code = 16 };       // Code         r2 probably not ignored

static inline void SetVarVal_number( char const *var, uint32_t num )
{
  register const char *v asm( "r0" ) = var;
  register uint32_t *val asm( "r1" ) = &num;
  register uint32_t len asm( "r2" ) = 4;
  register uint32_t context asm( "r3" ) = 0;
  register uint32_t type asm( "r4" ) = VV_NumberFromMemory;

  asm volatile ( "svc 0x20024" 
      : "=r" (type), "=r" (context)
      : "r" (v), "r" (val), "r" (len), "r" (context), "r" (type), "m" (num)
      : "lr", "cc" );
}

static inline void SetVarVal_string( char const *var, char const *str, uint32_t length )
{
  register const char *v asm( "r0" ) = var;
  register char const *val asm( "r1" ) = str;
  register uint32_t len asm( "r2" ) = length;
  register uint32_t context asm( "r3" ) = 0;
  register uint32_t type asm( "r4" ) = VV_No_GSTrans;

  register uint32_t *error;

  asm volatile ( "svc 0x20024\n  movvs %[error], r0\n  movvc %[error], #0"
      : [error] "=r" (error), "=r" (type), "=r" (context)
      : "r" (v), "r" (val), "r" (len), "r" (context), "r" (type)
      : "lr", "cc" );
  if (error != 0) {
    SetVarVal_number( "Wimper$Error", *error );
    // SetVarVal_string( "Wimper$Error$", (char*) (error+1), 32 );
  }
}

// This needs a defined struct workspace
C_SWI_HANDLER( c_swi_handler );

struct core_workspace {
};

typedef union poll_block poll_block;

union poll_block {
  uint8_t bytes[256];
  struct {
    uint32_t size;
    uint32_t sender;
    uint32_t my_ref;
    uint32_t your_ref;
    uint32_t action;
    uint32_t data;
  } message;
};

struct workspace {
  uint32_t lock;
  uint32_t file_handle;
  uint32_t stack[1024]; // How much is needed? Must be followed by poll (see start).
  poll_block poll;
  struct core_workspace cores[];
};

static uint32_t OS_ReadMonotonicTime()
{
  register uint32_t time asm( "r0" );
  asm volatile ( "svc 0x20042" : "=r" (time) : : "lr", "cc" );
  return time;
}

static uint32_t Wimp_Initialise( char const *name, uint32_t const *messages )
{
  register uint32_t known_version asm( "r0" ) = 400;
  register uint32_t task asm( "r1" ) = 0x4b534154;
  register char const *const Rname asm( "r2" ) = name;
  register uint32_t *Rmessages asm( "r3" ) = messages;

  register uint32_t version asm( "r0" );
  register uint32_t handle asm( "r1" );
  asm volatile ( "svc 0x600c0" 
      : "=r" (version), "=r" (handle) 
      : "r" (known_version), "r" (task), "r" (Rname) 
      : "lr", "cc" );

  return handle;
}

static uint32_t Wimp_PollIdle( uint32_t mask, poll_block *poll, uint32_t *poll_word, uint32_t time )
{
  register uint32_t Rmask asm( "r0" ) = mask;
  register poll_block *Rblock asm( "r1" ) = poll;
  register uint32_t *Rtime asm( "r2" ) = time;
  register uint32_t *Rpoll_word asm( "r3" ) = poll_word;
  register uint32_t code asm( "r0" );
  asm volatile ( "svc 0x600c7" : "=r" (code) : "r" (Rmask), "r" (Rblock), "r" (Rtime), "r" (Rpoll_word) : "lr", "cc" );
  return code;
}

static uint32_t Wimp_Poll( uint32_t mask, poll_block *poll, uint32_t *poll_word )
{
  register uint32_t Rmask asm( "r0" ) = mask;
  register poll_block *Rblock asm( "r1" ) = poll;
  register uint32_t *Rpoll_word asm( "r3" ) = poll_word;
  register uint32_t code asm( "r0" );
  asm volatile ( "svc 0x600c7" : "=r" (code) : "r" (Rmask), "r" (Rblock), "r" (Rpoll_word) : "lr", "cc" );
  return code;
}

static void Wimp_CloseDown( uint32_t handle )
{
  register uint32_t Rhandle asm( "r0" ) = handle;
  register uint32_t Rmagic asm( "r1" ) = 0x4b534154;
  // Li
  asm volatile ( "svc 0x600dd" 
      : "=r" (Rhandle) // Listed because clobbered
      : "r" (Rhandle), "r" (Rmagic) 
      : "lr", "cc" );
}

static void Log( char const *string )
{
  char buf[80];
  char *d = buf;
  char c;
  char const *echo = "echo ";
  do {
    c = *echo++;
    *d++ = c;
  } while (c != '\0');
  d--;
  do {
    c = *string++;
    if (c >= ' ') {
      *d++ = c;
    }
  } while (c >= ' ');
  char const *redirect = " { >> WimperLog }";
  do {
    c = *redirect++;
    *d++ = c;
  } while (c != '\0');

  OSCLI( buf );
/*
  uint32_t file = OpenFileForWriting( string );
  if (file != 0)
    CloseFile( file );
*/
}

static void LogNumber( uint32_t num )
{
  char buf[10];
  char *c = &buf[9];
  *--c = '\0';
  if (num == 0) *--c = '0';
  while (num > 0) {
    *--c = '0' + (num % 10);
    num = num / 10;
  }
  Log( c );
}

static uint32_t __attribute__(( noinline )) c_start( struct workspace *workspace )
{
  static uint32_t const messages[] = { 0 }; // All messages

  Log( "Wimp_Initialising" );
  uint32_t handle = Wimp_Initialise( "Wimper", &messages );
  Log( "Wimp_Initialised" );
  bool exit = false;;

  while (!exit) {
    Log( "Wimp_Polling" );
    uint32_t code = Wimp_PollIdle( 0, &workspace->poll, OS_ReadMonotonicTime() + 100, 0 );
    Log( "Wimp_Polled" );
    LogNumber( code );
    switch (code) {
    case 17: // Message
    case 18: // Message, recorded
    case 19: // Message acknowledge
      Log( "Message" );
      LogNumber( workspace->poll.message.action );
      exit = 0 == workspace->poll.message.action;
      break;
    }
  }

  Log( "Wimp_CloseDown" );
  Wimp_CloseDown( handle );
  Log( "OS_Exit" );

  return 0;
}

// Entered with no stack!
void __attribute__(( naked )) start()
{
  struct workspace *workspace;

  asm volatile ( "ldr %[private_word], [r12]" : [private_word] "=r" (workspace) );
  asm volatile ( "mov sp, %[stacktop]" : : [stacktop] "r" (&workspace->poll) );

  // FIXME: Do I want to return an error block, or should that be an exceptional exit...?
  uint32_t result = c_start( workspace );

  register uint32_t error asm( "r0" ) = 0;
  register uint32_t abex asm( "r1" ) = 0x58454241;
  register uint32_t retcode asm( "r2" ) = result;
  // OS_Exit
  asm ( "svc 0x20011" : : "r" (error), "r" (abex), "r" (retcode) );
}

static struct workspace *new_workspace( uint32_t number_of_cores )
{
  uint32_t required = sizeof( struct workspace ) + number_of_cores * sizeof( struct core_workspace );

  struct workspace *memory = rma_claim( required );
  SetVarVal_number( "Wimper$Mem", (uint32_t) memory );

  memset( memory, 0, required );

  return memory;
}

// Pre-Multi-core C Kernel, these parameters are not passed...
// Don't assume they're valid until the OS version has been checked.
void init( uint32_t this_core, uint32_t number_of_cores )
{
  struct workspace **private;
  // Preserve r12, in case we make a function call
  asm volatile ( "mov %[private_word], r12" : [private_word] "=r" (private) );

  bool first_entry = (*private == 0);

  struct workspace *workspace;

  if (first_entry) {
OSCLI( "echo Hello { > WimperLog }" );
Log( "First" );
    *private = new_workspace( (module_flags & 2) ? number_of_cores : 1 );
  }

  workspace = *private;

  // workspace->file_handle = OpenFileForWriting( "RAM::RamDisc0.$.SimpleWimpOutput" );
}


static bool DoSomething( struct workspace *workspace, SWI_regs *regs )
{
  regs->r[0] = 0x55554444;
  regs->r[1] = 0x55444455;
  regs->r[2] = 0x44445555;
  return true;
}

static bool CreateFile( struct workspace *workspace, SWI_regs *regs )
{
/*
  uint32_t file = OpenFileForWriting( "RAM::RamDisc0.$.SimpleWimpOutput" );
  if (file != 0)
    CloseFile( file );
  regs->r[0] = file;
*/
  regs->r[0] = 0x24242424;
  return true;
}

bool __attribute__(( noinline )) c_swi_handler( struct workspace *workspace, SWI_regs *regs )
{
  switch (regs->number) {
  case 0x00: return DoSomething( workspace, regs );
  case 0x01: return CreateFile( workspace, regs );
  }
  static const error_block error = { 0x1e6, "Bad Wimper SWI" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

char const swi_names[] = { "Wimper" 
          "\0DoSomething" 
          "\0CreateFile" 
          "\0" };
