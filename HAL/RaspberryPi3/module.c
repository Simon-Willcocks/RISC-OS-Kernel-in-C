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
//NO_title;
NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "HAL";

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
static void * local_ptr( const void *p )
{
  register uint32_t result;
  asm ( "adrl %[result], local_ptr" : [result] "=r" (result) );
  return (void*) (result + (char*) p - (char *) local_ptr);
}

struct workspace {
  uint32_t lock;
  uint32_t *mbox;

  uint32_t *gpio;
  uint32_t *uart;

  void *mailbox_request;
  uint32_t fb_physical_address;
  uint32_t *frame_buffer;
  struct core_workspace {
    struct workspace *shared;
    uint8_t queued;
    uint8_t queue[15];
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
  Blue    = 0xffff0000,
  Green   = 0xff00ff00,
  Red     = 0xff0000ff,
  Yellow  = 0xff00ffff,
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

static void add_to_display( char c, struct core_workspace *workspace )
{
  if (core( workspace ) == 0) {
    // Duplicate core 0 output on uart (no checks for overflows)
    if (c < ' ' && c != '\r' && c != '\n') {
      workspace->shared->uart[0] = '|';
      workspace->shared->uart[0] = c + '@';
    }
    else
      workspace->shared->uart[0] = c;
  }

  if (workspace->x == 58 || c == '\n') {
    new_line( workspace );
  }
  if (c == '\r') {
    workspace->x = 0;
  }

  if (c != '\n' && c != '\r') workspace->display[workspace->y][workspace->x++] = c;
}

static void add_string( const char *s, struct core_workspace *workspace )
{
  s = local_ptr( s );
  while (*s != 0) {
    add_to_display( *s++, workspace );
  }
}

static void add_num( uint32_t number, struct core_workspace *workspace )
{
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (c > '9') c += ('a' - '0' - 10);
    add_to_display( c, workspace );
  }
}

int32_t int16_at( uint8_t *p )
{
  int32_t result = p[1];
  result = (result << 8) | p[0];
  return result;
}

const char *const specials[] = {
"##ignored\n\r",
"##Next char to printer only (ignore both)\n\r",
"##Enable printer (ignored)\n\r",
"##Disable printer (ignored)\n\r",
"##Split cursors\n\r",
"##Join cursors (print text at graphics cursor)\n\r",
"##Enable VDU drivers (see also VDU 21 and OS_Byte 117)\n\r",
"##Bell\n\r",
"##Backspace (does not delete last character)\n\r",
"##Horizontal tab\n\r",
"##Line feed\n\r",
"##Vertical tab (back one line)\n\r",
"##Form feed/clear screen\n\r",
"##Carriage return\n\r",
"##Paged mode on\n\r",
"##Paged mode off\n\r",
"##Clear graphics window\n\r",
"##Set text colour (6 bit RGB, obsolete?)\n\r",
"##GCOL action, colour\n\r",
"##Set palette, logical, mode, r, g, b (obsolete?)\n\r",
"##Restore default colours\n\r",
"##Disable screen display (see VDU 6)\n\r",
"##Change display mode\n\r",
"##Miscellaneous commands\n\r",
"##Define graphics window, x1; y1; x2; y2\n\r",
"##Plot, type, x; y;\n\r",
"##Restore default windows\n\r",
"##No operation\n\r",
"##Define text window, x1, y1, x2, y2 obsolete with 4K monitors, no?\n\r",
"##Set graphics origin, x; y;\n\r",
"##Home text (or graphics) cursor for text\n\r",
"##Position text cursor, x, y\n\r" };

void __attribute__(( noinline )) C_WrchV_handler( char c, struct core_workspace *workspace )
{
  static const uint8_t bytes[32] = { 1, 2, 1, 1,  1, 1, 1, 1,
                                     1, 1, 1, 1,  1, 1, 1, 1,
                                     1, 2, 3, 6,  1, 1, 2, 21,
                                     9, 6, 1, 1,  5, 5, 1, 3 };
  const uint8_t *parameter_bytes = local_ptr( bytes );

  if (workspace->queued != 0) {
    workspace->queue[workspace->queued] = c;
    workspace->queued++;
  }
  else if (c < ' ') {
    // VDU codes?
    workspace->queue[0] = c;
    workspace->queued = 1;
  }

  if (workspace->queued != 0) {
    if (workspace->queued == parameter_bytes[workspace->queue[0]]) {
      // Got all the bytes we need to perform the action
      workspace->queued = 0;

      switch (workspace->queue[0]) {
      case 10: add_to_display( c, workspace ); break;       // Line feed
      case 13: add_to_display( c, workspace ); break;       // Carriage return
      case 25: // Plot
        {
          uint8_t type = workspace->queue[1];
          int32_t x = int16_at( &workspace->queue[2] );
          int32_t y = int16_at( &workspace->queue[4] );
          register uint32_t rt asm( "r0" ) = type;
          register uint32_t rx asm( "r1" ) = x;
          register uint32_t ry asm( "r2" ) = y;
          asm ( "svc %[swi]" : : [swi] "i" (0x20045), "r" (rt), "r" (rx), "r" (ry) : "lr", "cc" );
add_to_display( 'P', workspace );
show_character_at( workspace->x, workspace->y, 'p', core( workspace ), Blue, workspace->shared );
        }
        break;
      default:
        {
          const char *const *s = local_ptr( specials );
          add_string( s[workspace->queue[0]], workspace );
          asm ( "bkpt 1" ); break;
        }
      }
    }
  }
  else {
    add_to_display( c, workspace );

    // This part is temporary, until the display update can be triggered by an interrupt FIXME
    // The whole "screen" will be displayed, with a cache flush, and the top line will be (workspace->y + 1) % 40
    if (0 != workspace->shared->frame_buffer) {
      if (c < ' ')
        show_character_at( workspace->x, workspace->y, c + '@', core( workspace ), Red, workspace->shared );
      else
        show_character_at( workspace->x, workspace->y, c, core( workspace ), White, workspace->shared );

      asm ( "svc 0xff" : : : "lr", "cc" );
    }
    // End of temporary implementation
  }

  clear_VF();
}

static void __attribute__(( naked )) WrchV_handler( char c )
{
  // OS_WriteC must preserve all registers, C will ensure the callee saved registers are preserved.
  asm ( "push { r0, r1, r2, r3, r12 }" );
  register struct core_workspace *workspace asm( "r12" );
  C_WrchV_handler( c, workspace );
  // Intercepting call (pops pc from the stack)
  asm ( "bvc 0f\n  bkpt #2\n0:" );
  asm ( "pop { r0, r1, r2, r3, r12, pc }" );
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
  asm volatile ( "dsb" ); // Probably overkill on the dsbs, but we're alternating between two devices
  workspace->gpio[0x1c/4] = (1 << 22); // Set
  asm volatile ( "dsb" );
}

void led_off( struct workspace *workspace )
{
  asm volatile ( "dsb" );
  workspace->gpio[0x28/4] = (1 << 22); // Clr
  asm volatile ( "dsb" );
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

// TODO Take that code out of DynamicArea, and have another SWI for defining the screen, including the width and height...
static uint32_t *map_screen_into_memory( uint32_t address )
{
  register uint32_t r0 asm ( "r0" ) = 30;
  register uint32_t r1 asm ( "r1" ) = address;
  register uint32_t r2 asm ( "r2" ) = 8 << 20;  // Allows access to slightly more RAM than needed, FIXME (1920x1080x4 = 0x7e9000)
  // TODO Add a few more virtual lines, so that we're allocated the full 8MiB.
  register uint32_t *base asm ( "r1" );

  asm ( "svc %[os_dynamicarea]" : "=r" (base) : [os_dynamicarea] "i" (0x66), "r" (r0), "r" (r1), "r" (r2) : "lr", "cc" );

  return base;
}

static inline void stop_and_blink( struct workspace *workspace )
{
  bool on = true;
  for (;;) {
    for (int i = 0; i < 0x10000000; i++) { asm volatile( "" ); }

    if (on) led_off( workspace );
    else led_on( workspace );
    on = !on;
  }
}

static inline uint32_t initialise_frame_buffer( struct workspace *workspace )
{
  uint32_t volatile *mailbox = &workspace->mbox[0x220];

  const int width = 1920;
  const int height = 1080;

  static const int space_to_claim = 26 * sizeof( uint32_t );
  static const uint32_t alignment = 2 << 20; // 2 MB aligned (more for long descriptor translation tables than short ones)

  dma_memory tag_memory = rma_claim_for_dma( space_to_claim, 16 );

  while (0xf & tag_memory.pa) { tag_memory.pa++; tag_memory.va++; }

  uint32_t volatile *dma_tags = (void*) tag_memory.va;

  // Note: my initial sequence of tags, 0x00040001, 0x00048003, 0x00048004, 0x00048005, 0x00048006,
  // didn't get a valid size value from QEMU.

  int index = 0;
  dma_tags[index++] = space_to_claim;
  dma_tags[index++] = 0;
  dma_tags[index++] = 0x00048005;    // Colour depth
  dma_tags[index++] = 4;
  dma_tags[index++] = 0;
  dma_tags[index++] = 32;
  dma_tags[index++] = 0x00048006;    // Pixel order
  dma_tags[index++] = 4;
  dma_tags[index++] = 0;
  dma_tags[index++] = 1;             // 0 = BGR, 1 = RGB
  dma_tags[index++] = 0x00048003;    // Set physical (display) width/height
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  dma_tags[index++] = width;
  dma_tags[index++] = height;
  dma_tags[index++] = 0x00048004;    // Set virtual (buffer) width/height
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  dma_tags[index++] = width;
  dma_tags[index++] = height + 13;    // Some hidden lines so that we are allocated whole MiB. FIXME for non-1080p
  // Despite a line of 1920 pixels being about 8k, the allocated amount varies enormously
  // 1088 results in 0x7f8000 (32KiB less than 8 MiB)
  // 1089 results in 0x816000 (88KiB more than 8 MiB)
  // 1093 is, by definition more than 8MB, so qemu, returning a closer size than the real hardware, will still work
  // It's safer to map in less than is allocated than more, since the ARM could corrupt GPU memory in the latter case
  // Mapping 0x800000 of the 0x816000 simply means 88KiB of memory won't be accessable by anyone.
  // Maybe we can use some of it for mouse pointers or something, as long as the GPU isn't used to clear the screen?
  dma_tags[index++] = 0x00040001;    // Allocate buffer
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  int buffer_tag = index;
  dma_tags[index++] = alignment;
  dma_tags[index++] = 0;
  dma_tags[index++] = 0;             // End tag

  asm volatile ( "dsb sy" );
  asm ( "svc 0xff" : : : "lr", "cc" );

  uint32_t request = 8 | tag_memory.pa;

  while (dma_tags[buffer_tag] == alignment) {
    mailbox[8] = request;
    asm volatile ( "dsb" );

    //workspace->gpio[0x28/4] = (1 << 22); // Clr
    workspace->gpio[0x1c/4] = (1 << 22); // Set
    asm volatile ( "dsb" );

    uint32_t response;

    do {
      uint32_t countdown = 0x10000;
      while ((mailbox[6] & (1 << 30)) != 0 && --countdown > 0) { asm volatile ( "dsb" ); } // Empty?
      if (countdown == 0) break;

      response = mailbox[0];
      if (response != request) stop_and_blink( workspace );
    } while (response != request);

    asm ( "svc 0xff" : : : "lr", "cc" );
  }

  workspace->gpio[0x28/4] = (1 << 22); // Clr
  asm volatile ( "dsb" );

  return (dma_tags[buffer_tag] & ~0xc0000000);
}

static void WriteNum( uint32_t number )
{
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (c > '9') c += ('a' - '0' - 10);
    register uint32_t r0 asm( "r0" ) = c;
    asm( "svc 0": : "r" (r0) : "lr", "cc" );
  }
}

void init( uint32_t this_core, uint32_t number_of_cores )
{
  struct workspace **private;
  // Preserve r12, in case we make a function call
  asm volatile ( "mov %[private_word], r12" : [private_word] "=r" (private) );

  bool first_entry = (*private == 0);

  struct workspace *workspace;

  if (first_entry) {
    *private = new_workspace( number_of_cores );
  }

  workspace = *private;

  // Map this addresses into all cores
  workspace->mbox = map_device_page( 0x3f00b000 );

  workspace->gpio = map_device_page( 0x3f200000 );
  workspace->uart = map_device_page( 0x3f201000 );

  if (first_entry) {
    uint32_t *gpio = workspace->gpio;
    gpio[2] = (gpio[2] & ~(3 << 6)) | (1 << 6); // Output, pin 22
    asm volatile ( "dsb" );

    workspace->fb_physical_address = initialise_frame_buffer( workspace );
  }

  workspace->frame_buffer = map_screen_into_memory( workspace->fb_physical_address );

  workspace->core_specific[this_core].shared = workspace;
  workspace->core_specific[this_core].queued = 0; // VDU code queue size, including character that started it filling
  workspace->core_specific[this_core].x = 0;
  workspace->core_specific[this_core].y = 0;
  for (int y = 0; y < 40; y++) {
    for (int x = 0; x < 60; x++) {
      workspace->core_specific[this_core].display[y][x] = ' ';
    }
  }

  {
    void *handler = local_ptr( WrchV_handler );
    register uint32_t vector asm( "r0" ) = 3;
    register void *routine asm( "r1" ) = handler;
    register struct core_workspace *handler_workspace asm( "r2" ) = &workspace->core_specific[this_core];
    asm ( "svc 0x2001f" : : "r" (vector), "r" (routine), "r" (handler_workspace) : "lr" );
  }

  WriteS( "HAL obtained WrchV\\n\\r" );

  if (first_entry) { WriteS( "HAL initialised frame buffer\\n\\r" ); }
}

