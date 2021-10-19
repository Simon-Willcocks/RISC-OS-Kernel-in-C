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

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing
//   New feature: instead of one private word per core, r12 points to a shared 
//   word, initialised by the first core to initialise the module.

// Explicitly no SWIs provided (it's the default, anyway)
#define MODULE_CHUNK "0"

#include "module.h"

NO_start;
//NO_init;
NO_finalise;
NO_service_call;
NO_title;
NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

// asm volatile ( "" ); // Stop the optimiser putting in a call to memset
// Check that this doesn't get optimised to a call to memset!
void *memset(void *s, int c, size_t n)
{
  char cv = c & 0xff;
  char *cp = s;
  while ((((uint32_t) cp) & sizeof( cv )) != 0 && n >= sizeof( cv )) { *cp++ = cv; n-= sizeof( cv ); }

  uint16_t hv = cv; hv = hv | (hv << 8 * sizeof( cv ));
  uint16_t *hp = (void*) cp;
  while ((((uint32_t) hp) & sizeof( hv )) != 0 && n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }

  uint32_t wv = hv; wv = wv | (wv << 8 * sizeof( hv ));
  uint32_t *wp = (void*) cp;
  while ((((uint32_t) wp) & sizeof( wv )) != 0 && n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }

  uint64_t dv = wv; dv = dv | (dv << 8 * sizeof( wv ));
  uint64_t *dp = (void*) cp;
  while ((((uint32_t) dp) & sizeof( dv )) != 0 && n >= sizeof( dv )) { *dp++ = dv; n-=sizeof( dv ); }

  if (n >= sizeof( wv )) { wp = (void *) dp; *wp++ = wv; n-=sizeof( wv ); }
  if (n >= sizeof( hv )) { hp = (void *) wp; *hp++ = hv; n-=sizeof( hv ); }
  if (n >= sizeof( cv )) { cp = (void *) hp; *cp++ = cv; n-=sizeof( cv ); }

  return s;
}

#define WriteS( string ) asm ( "svc 1\n  .string \""string"\"\n  .balign 4" : : : "lr" )
#define Write0( string ) { register uint32_t r0 asm( "r0" ) = (uint32_t) (string); asm ( "push { r0-r12, lr }\nsvc 2\n  pop {r0-r12, lr}" : : "r" (r0) ); } 

static uint32_t local_ptr( void *p )
{
  register uint32_t result;
  asm ( "adrl %[result], local_ptr" : [result] "=r" (result) );
  return result + (char*) p - (char *) local_ptr;
}

struct workspace {
  uint32_t lock;
  struct core_workspace {
    int core;
    uint32_t x;
    uint32_t y;
    char display[40][60];
  } core_specific[];
};

static struct workspace *new_workspace( uint32_t number_of_cores )
{
  uint32_t required = sizeof( struct workspace ) + number_of_cores * sizeof( struct core_workspace );

  // XOS_Module 6 Claim
  register struct workspace *memory asm( "r2" );
  register uint32_t code asm( "r0" ) = 6;
  register uint32_t size asm( "r3" ) = required;
  asm ( "svc 0x2001e" : "=r" (memory) : "r" (size), "r" (code) : "lr" );

  memory->lock = 0;

  return memory;
}

enum fb_colours {
  Black   = 0xff000000,
  Grey    = 0xff888888,
  Blue    = 0xff0000ff,
  Green   = 0xff00ff00,
  Red     = 0xffff0000,
  Yellow  = 0xffffff00,
  Magenta = 0xffff00ff,
  White   = 0xffffffff };

static inline void set_pixel( uint32_t x, uint32_t y, uint32_t colour )
{
  uint32_t *frame_buffer = (void*) 0xef000000; // FIXME location of DA 6, which we'll be setting anyway.
  frame_buffer[x + y * 1920] = colour;
}

static inline void show_character( uint32_t x, uint32_t y, unsigned char c, uint32_t colour )
{
  struct __attribute__(( packed )) {
    uint8_t system_font[128][8];
  } const * const font = (void*) (0xfc040f94-(32*8)); // FIXME

if ((x | y) & (1 << 31)) asm( "bkpt 3" );
  uint32_t dx = 0;
  uint32_t dy = 0;

  for (dy = 0; dy < 8; dy++) {
    for (dx = 0; dx < 8; dx++) {
      if (0 != (font->system_font[c][dy] & (0x80 >> dx)))
        set_pixel( x+dx, y+dy, colour );
      else
        set_pixel( x+dx, y+dy, Black );
    }
  }
}

static void new_line( struct core_workspace *workspace )
{
  workspace->x = 0;
  workspace->y++;
  if (workspace->y == 40)
    workspace->y = 0;
  for (int x = 0; x < 60; x++) {
    workspace->display[workspace->y][x] = ' ';
  }
}

void C_WrchV_handler( char c, struct core_workspace *workspace )
{
  if (workspace->x == 39 || c == '\n') {
    new_line( workspace );
  }
  if (c == '\r') {
    workspace->x = 0;
  }
  if (c != '\n' && c != '\r') {
    // This part is temporary, until the display update can be triggered by an interrupt FIXME
    // The whole "screen" will be displayed, with a cache flush, and the top line will be (workspace->y + 1) % 40
    int x = (workspace->x) * 8 + workspace->core * (60 * 8);
    int y = workspace->y * 8 + 200;

    if (c < ' ')
      show_character( x, y, c + '@', Red );
    else
      show_character( x, y, c, White );
    // End of temporary implementation

    workspace->display[workspace->y][workspace->x++] = c;
  }

  clear_VF();
}

static void WrchV_handler( char c )
{
  // OS_WriteC must preserve all registers, C will ensure the callee saved registers are preserved.
  asm ( "push { r0, r1, r2, r3, r12 }" );
  register struct core_workspace *workspace asm( "r12" );
  C_WrchV_handler( c, workspace );
  asm ( "pop { r0, r1, r2, r3, r12 }" );
}

void init( uint32_t this_core, uint32_t number_of_cores )
{
  struct workspace **private;
  // Preserve r12, in case we make a function call
  asm volatile ( "mov %[private_word], r12" : [private_word] "=r" (private) );

  struct workspace *workspace;

  if (*private == 0) {
    *private = new_workspace( number_of_cores );
  }

  workspace = *private;

  workspace->core_specific[this_core].core = this_core;
  workspace->core_specific[this_core].x = 0;
  workspace->core_specific[this_core].y = 0;
  for (int y = 0; y < 40; y++) {
    for (int x = 0; x < 60; x++) {
      workspace->core_specific[this_core].display[y][x] = ' ';
    }
  }

  register uint32_t vector asm( "r0" ) = 3;
  register uint32_t routine asm( "r1" ) = local_ptr( WrchV_handler );
  register struct core_workspace *handler_workspace asm( "r2" ) = &workspace->core_specific[this_core];
  asm ( "svc 0x2001f" : : "r" (vector), "r" (routine), "r" (handler_workspace) : "lr" );

  WriteS( "HAL obtained WrchV\\n" );

  Write0( local_ptr( "Hello" ) );
}

