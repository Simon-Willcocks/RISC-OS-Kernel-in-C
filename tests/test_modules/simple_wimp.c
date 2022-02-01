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

/* Common mistakes: */
/* Not specifying "lr" (and "cc") in inline SWI calls, which puts the
   module into an infinite loop. */

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing

#define MODULE_CHUNK "0x8ff00"
#include "module.h"

NO_start;
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

// asm volatile ( "" ); // Stop the optimiser putting in a call to memset
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
  if ((((size_t) hp) & (1 << 2)) != 0 && n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }

  uint32_t wv = hv; wv = wv | (wv << (8 * sizeof( hv )));
  uint32_t *wp = (void*) hp;
  // Next size is double, use if, not while
  if ((((size_t) wp) & (1 << 3)) != 0 && n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }

  uint64_t dv = wv; dv = dv | (dv << (8 * sizeof( wv )));
  uint64_t *dp = (void*) wp;
  // No larger size, use while, not if, and don't check the pointer bit
  while (n >= sizeof( dv )) { *dp++ = dv; n-=sizeof( dv ); }

  wp = (void *) dp; if (n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }
  hp = (void *) wp; if (n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }
  cp = (void *) hp; if (n >= sizeof( cv )) { *cp++ = cv; n-=sizeof( cv ); }

  return s;
}

#define WriteS( string ) asm ( "svc 1\n  .string \""string"\"\n  .balign 4" : : : "lr" )

#define NewLine asm ( "svc 3" : : : "lr" )

#define Write0( string ) do { register uint32_t r0 asm( "r0" ) = (uint32_t) (string); asm ( "push { r0-r12, lr }\nsvc 2\n  pop {r0-r12, lr}" : : "r" (r0) ); } while (false)
#define Write13( string ) do { const char *s = (void*) (string); register uint32_t r0 asm( "r0" ); while (31 < (r0 = *s++)) { asm ( "push { r1-r12, lr }\nsvc 0\n  pop {r1-r12, lr}" : : "r" (r0) ); } } while (false)

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

// This needs a defined struct workspace
C_SWI_HANDLER( c_swi_handler );

static void CloseFile( uint32_t file )
{
  register uint32_t code asm( "r0" ) = 0;
  register uint32_t f asm( "r1" ) = file;
  asm ( "svc 0x2000d" : : "r" (code), "r" (f) : "lr", "cc" );
}

static uint32_t OpenFileForWriting( const char *filename )
{
  register uint32_t code asm( "r0" ) = 0x83; // No File$Path, create for write
  register char const * file asm( "r1" ) = filename;
  register uint32_t open asm( "r0" );
  asm ( "svc 0x2000d" : "=r" (open) : "r" (code), "r" (file) : "lr", "cc" );
  return open;
}

// Pre-Multi-core C Kernel, these parameters are not passed...
void init( uint32_t this_core, uint32_t number_of_cores )
{
  uint32_t file = OpenFileForWriting( "RAM::RamDisc0.$.SimpleWimpOutput" );
  if (file != 0)
    CloseFile( file );
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
