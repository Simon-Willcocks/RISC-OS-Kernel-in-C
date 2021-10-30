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
#define Write0( string ) { register uint32_t r0 asm( "r0" ) = (uint32_t) (string); asm ( "push { r0-r12, lr }\nsvc 2\n  pop {r0-r12, lr}" : : "r" (r0) ); } 

// Return the relocated address of the item in the module: function or constant.
static uint32_t local_ptr( void *p )
{
  register uint32_t result;
  asm ( "adrl %[result], local_ptr" : [result] "=r" (result) );
  return result + (char*) p - (char *) local_ptr;
}

struct workspace {
  uint32_t lock;
  uint32_t *mbox;

  uint32_t *uart;
  uint32_t *gpio;

  void *mailbox_request;
  uint32_t fb_physical_address;
  uint32_t *frame_buffer;
  struct core_workspace {
    struct workspace *shared;
    uint32_t x;
    uint32_t y;
    char display[40][60];
  } core_specific[];
};

static int core( struct core_workspace *cws )
{
  return cws - cws->shared->core_specific;
}

static void *rma_claim( uint32_t bytes )
{
  // XOS_Module 6 Claim
  register void *memory asm( "r2" );
  register uint32_t code asm( "r0" ) = 6;
  register uint32_t size asm( "r3" ) = bytes;
  asm ( "svc 0x2001e" : "=r" (memory) : "r" (size), "r" (code) : "lr" );

  return memory;
}

typedef struct {
  uint32_t va;
  uint32_t pa;
} dma_memory;


static uint32_t lock_for_dma( uint32_t address, uint32_t bytes )
{
  register uint32_t addr asm( "r0" ) = address;
  register uint32_t size asm( "r1" ) = bytes;
  register uint32_t physical asm( "r0" );

  asm ( "svc 0xfc" : "=r" (physical) : "r" (size), "r" (addr) : "lr" );

  return physical;
}

static dma_memory rma_claim_for_dma( uint32_t bytes, uint32_t alignment )
{
  dma_memory result;

  // FIXME: Loop, allocating blocks and attempting to lock the memory for DMA, then release all those that couldn't be locked.
  result.va = (uint32_t) rma_claim( bytes + alignment );
  result.pa = lock_for_dma( result.va, bytes + alignment );

  return result;
}

static struct workspace *new_workspace( uint32_t number_of_cores )
{
  uint32_t required = sizeof( struct workspace ) + number_of_cores * sizeof( struct core_workspace );

  struct workspace *memory = rma_claim( required );

  memset( memory, 0, required );

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

static inline void set_pixel( uint32_t x, uint32_t y, uint32_t colour, struct workspace *ws )
{
  ws->frame_buffer[x + y * 1920] = colour;
}

static inline void show_character( uint32_t x, uint32_t y, unsigned char c, uint32_t colour, struct workspace *ws )
{
  struct __attribute__(( packed )) {
    uint8_t system_font[128][8];
  } const * const font = (void*) (0xfc040f94-(32*8)); // FIXME

if ((x | y) & (1 << 31)) asm( "bkpt 3" ); // -ve coordinates? No thanks!
  uint32_t dx = 0;
  uint32_t dy = 0;

  for (dy = 0; dy < 8; dy++) {
    for (dx = 0; dx < 8; dx++) {
      if (0 != (font->system_font[c][dy] & (0x80 >> dx)))
        set_pixel( x+dx, y+dy, colour, ws );
      else
        set_pixel( x+dx, y+dy, Black, ws );
    }
  }
}

#define TOP 400

static void show_character_at( int cx, int cy, char ch, int core, uint32_t colour, struct workspace *ws )
{
  int x = cx * 8 + core * (60 * 8) + 4;
  int y = cy * 8 + TOP;
  show_character( x, y, ch, colour, ws );
}

static void show_line( int y, int core, uint32_t colour, struct workspace *ws )
{
  y = y * 8 + TOP;
  int x = core * (60 * 8) + 2;
  set_pixel( x, y, colour, ws );
  set_pixel( x, y+2, colour, ws );
  set_pixel( x, y+4, colour, ws );
  set_pixel( x, y+6, colour, ws );
}

static void new_line( struct core_workspace *workspace )
{
  show_line( workspace->y, core( workspace ), Black, workspace->shared );

  workspace->x = 0;
  workspace->y++;
  if (workspace->y == 40)
    workspace->y = 0;
  for (int x = 0; x < 59; x++) {
    workspace->display[workspace->y][x] = ' ';

    show_character_at( x, workspace->y, ' ', core( workspace ), Black, workspace->shared );
  }
  show_line( workspace->y, core( workspace ), Green, workspace->shared );
}

void C_WrchV_handler( char c, struct core_workspace *workspace )
{
  if (workspace->x == 58 || c == '\n') {
    new_line( workspace );
  }
  if (c == '\r') {
    workspace->x = 0;
  }
  if (c != '\n' && c != '\r') {
    // This part is temporary, until the display update can be triggered by an interrupt FIXME
    // The whole "screen" will be displayed, with a cache flush, and the top line will be (workspace->y + 1) % 40

if (0 != workspace->shared->frame_buffer) {
    if (c < ' ')
      show_character_at( workspace->x, workspace->y, c + '@', core( workspace ), Red, workspace->shared );
    else
      show_character_at( workspace->x, workspace->y, c, core( workspace ), White, workspace->shared );

    asm ( "svc 0xff" : : : "lr", "cc" );
    // End of temporary implementation
}

    workspace->display[workspace->y][workspace->x++] = c;
  }
//for (int i = 0; i < 0x100000; i++) { asm volatile ( "" ); }
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

static void *map_device_page( uint32_t physical_address )
{
  register uint32_t phys asm( "r0" ) = physical_address;
  register uint32_t pages asm( "r1" ) = 1;
  register void *result asm( "r0" );
  asm ( "svc 0xfe" : "=r" (result) : "r" (phys), "r" (pages) : "lr", "cc" );
  return result;
}

#define LED_BLINK_TIME 0x10000000

void led_on( struct workspace *workspace )
{
  workspace->gpio[0x1c/4] = (1 << 22); // Set
}

void led_off( struct workspace *workspace )
{
  workspace->gpio[0x28/4] = (1 << 22); // Clr
}

void led_blink( struct workspace *workspace, int n )
{
  // Count the blinks! Extra short = 0, Long = 5

  if (n == 0) {
    led_on( workspace );
    for (uint64_t i = 0; i < LED_BLINK_TIME / 4; i++) { asm volatile ( "" ); }
    led_off( workspace );
    for (uint64_t i = 0; i < LED_BLINK_TIME; i++) { asm volatile ( "" ); }
  }
  else {
    while (n >= 5) {
      led_on( workspace );
      for (uint64_t i = 0; i < LED_BLINK_TIME * 4; i++) { asm volatile ( "" ); }
      led_off( workspace );
      for (uint64_t i = 0; i < LED_BLINK_TIME; i++) { asm volatile ( "" ); }
      n -= 5;
    }
    while (n > 0) {
      led_on( workspace );
      for (uint64_t i = 0; i < LED_BLINK_TIME; i++) { asm volatile ( "" ); }
      led_off( workspace );
      for (uint64_t i = 0; i < LED_BLINK_TIME; i++) { asm volatile ( "" ); }
      n --;
    }
  }
  for (uint64_t i = 0; i < 4 * LED_BLINK_TIME; i++) { asm volatile ( "" ); }
}

void show_word( int x, int y, uint32_t number, uint32_t colour, struct workspace *ws )
{
  // Note: You cannot use a lookup table without resorting to trickery
  // static const char hex[] = ... loads the absolute address of the array.
  // Use local_ptr( hex ), or calculations...
  for (int nibble = 0; nibble < 8; nibble++) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (c > '9') c += ('a' - '0' - 10);
    show_character( x+64-nibble*8, y, c, colour, ws );
  }
}

static inline uint32_t initialise_frame_buffer( struct workspace *workspace )
{
  uint32_t volatile *mailbox = &workspace->mbox[0x220];

  static const int space_to_claim = 26 * sizeof( uint32_t );

  dma_memory tag_memory = rma_claim_for_dma( space_to_claim, 16 );

  while (0xf & tag_memory.pa) { tag_memory.pa++; tag_memory.va++; }

  uint32_t volatile *dma_tags = (void*) tag_memory.va;

  dma_tags[ 0] = space_to_claim;
  dma_tags[ 1] = 0;
  dma_tags[ 2] = 0x00040001;    // Allocate buffer
  dma_tags[ 3] = 8;
  dma_tags[ 4] = 0;
  dma_tags[ 5] = 2 << 20;       // 2 MB aligned (more for long descriptor translation tables)
  dma_tags[ 6] = 0;
  dma_tags[ 7] = 0x00048003;    // Set physical (display) width/height
  dma_tags[ 8] = 8;
  dma_tags[ 9] = 0;
  dma_tags[10] = 1920;
  dma_tags[11] = 1080;
  dma_tags[12] = 0x00048004;    // Set virtual (buffer) width/height
  dma_tags[13] = 8;
  dma_tags[14] = 0;
  dma_tags[15] = 1920;
  dma_tags[16] = 1080;
  dma_tags[17] = 0x00048005;    // Colour depth
  dma_tags[18] = 4;
  dma_tags[19] = 0;
  dma_tags[20] = 32;
  dma_tags[21] = 0x00048006;    // Pixel order
  dma_tags[22] = 4;
  dma_tags[23] = 0;
  dma_tags[24] = 0;             // 0 = BGR, 1 = RGB
  dma_tags[25] = 0;             // End tag

  asm volatile ( "dsb sy" );
  asm ( "svc 0xff" : : : "lr", "cc" );

  uint32_t request = 8 | tag_memory.pa;

  mailbox[8] = request;

  //workspace->gpio[0x28/4] = (1 << 22); // Clr
  workspace->gpio[0x1c/4] = (1 << 22); // Set
  asm volatile ( "dsb" );

  uint32_t response;

  do {
    while ((mailbox[6] & (1 << 30)) != 0) { } // Empty?

    response = mailbox[0];
  } while (response != request);

  asm ( "svc 0xff" : : : "lr", "cc" );
while (dma_tags[5] == (2 << 20)) {
int offset = 0x28;
for (int c = 0; c < 6; c++) {
  for (int i = 0; i < 0x10000000; i++) { asm volatile( "" ); }
  workspace->gpio[offset/4] = (1 << 22);
  offset = (0x1c + 0x28) - offset;
}
  for (int i = 0; i < 0x10000000; i++) { asm volatile( "" ); }
  for (int i = 0; i < 0x10000000; i++) { asm volatile( "" ); }
}

  workspace->gpio[0x28/4] = (1 << 22); // Clr
  asm volatile ( "dsb" );

  return (dma_tags[5] & ~0xc0000000);
}

void init( uint32_t this_core, uint32_t number_of_cores )
{
  struct workspace **private;
  // Preserve r12, in case we make a function call
  asm volatile ( "mov %[private_word], r12" : [private_word] "=r" (private) );

  bool first_entry = *private == 0;

  struct workspace *workspace;

  if (first_entry) {
    *private = new_workspace( number_of_cores );
  }

  workspace = *private;

  // Map this addresses into all cores
  workspace->mbox = map_device_page( 0x3f00b000 );

  // Temporary?
  //workspace->uart = map_device_page( 0x3f020000 );
  //workspace->uart[0x40] = 'W';

  workspace->gpio = map_device_page( 0x3f200000 );

  uint32_t *gpio = workspace->gpio;
  gpio[2] = (gpio[2] & ~(3 << 6)) | (1 << 6); // Output, pin 22
  asm volatile ( "dsb" );
  //gpio[0x28/4] = (1 << 22); // Clr
  gpio[0x1c/4] = (1 << 22); // Set
  asm volatile ( "dsb" );

  if (first_entry) {
    workspace->fb_physical_address = initialise_frame_buffer( workspace );

    register uint32_t r0 asm ( "r0" ) = 30;
    register uint32_t r1 asm ( "r1" ) = workspace->fb_physical_address;
    register uint32_t r2 asm ( "r2" ) = 8 << 20;  // Allows access to slightly more RAM than needed, FIXME (1920x1080x4 = 0x7e9000)
    // TODO Add a few more virtual lines, so that we're allocated the full 8MiB.
    register void *base asm ( "r1" );

    asm ( "svc %[os_dynamicarea]" : "=r" (base) : [os_dynamicarea] "i" (0x66), "r" (r0), "r" (r1), "r" (r2) : "lr", "cc" );

    workspace->frame_buffer = base;
  }

  if (first_entry) {
    for (int y = 0; y < 1080; y++) {
      for (int x = 0; x < 1920; x++) {
        set_pixel( x, y, 0xff000033, workspace );
      }
    }
  }

  workspace->core_specific[this_core].shared = workspace;
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
}

