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

// Bare bones of memory management enabled, multi-processor locks, with trivial
// display.

typedef unsigned uint32_t;

// NOPs replace the vectors, so that the initialisation code will be entered
// whichever event takes place; the processor mode can be examined to see
// what happened (probably with an already-mapped screen).
// This aspect has not been tested, because it turns out that wasn't the
// problem. (SMPEN was.)

asm ( "_start:"
  "\n  .global _start"
  "\n  nop"
  "\n  nop"
  "\n  nop"
  "\n  nop"
  "\n  nop"
  "\n  nop"
  "\n  nop"
  "\n  mov r1, #0x00040000" // Somewhere in unused RAM
  "\n  mrc p15, 0, r0, c0, c0, 5"
  "\n  and r0, r0, #15"
  "\n  add r1, r1, r0, LSL#12"
  "\n  add sp, r1, #4096"
  "\n  b with_stack"
  "\n.align 12" ); // Avoid being overwritten by GPU on startup

static inline int processor_mode()
{
  int result;
  asm ( "mrs %[r], cpsr" : [r] "=r" (result) );
  return result & 0x1f;
}

uint32_t volatile * const gpio = (void*) 0x3f200000;

static void init_gpio()
{
  gpio[2] = (gpio[2] & ~(3 << 6)) | (1 << 6); // Output, pin 22
  for (int i = 0; i < 1000000; i++) { asm ( "" ); }
  gpio[0x28/4] = (1 << 22); // Clr
}

uint32_t __attribute__(( aligned( 0x8000 ) )) L1TT[4][4096] = { 0 };

uint32_t lock = 0;
uint32_t * const mbox = (void*) 0x3f00b000;

// This code conforms to the section 7.2 of PRD03-GENC-007826:
// "Acquiring and Releasing a Lock"
static inline void claim_lock( uint32_t volatile *lock )
{
  register uint32_t failed = 1;

  uint32_t value;

  while (failed) {
    asm volatile ( "ldrex %[value], [%[lock]]"
                   : [value] "=&r" (value)
                   : [lock] "r" (lock) );
    if (value == 0) {
      // The failed and lock registers are not allowed to be the same, so
      // pretend to gcc that the lock may be written as well as read.

      asm volatile ( "strex %[failed], %[value], [%[lock]]"
                     : [failed] "=&r" (failed)
                     , [lock] "+r" (lock)
                     : [value] "r" (1) );
    }
    else {
      asm ( "clrex" );
    }
  }
  asm ( "dmb sy" );
}

static inline void release_lock( uint32_t volatile *lock )
{
  // Ensure that any changes made while holding the lock are visible before the lock is seen to have been released
  asm ( "dmb sy" );
  *lock = 0;
}

static void flush_location_to_poc( void *va )
{
  asm ( "mcr p15, 0, %[va], cr7, cr10, 1" : : [va] "r" (va) );
}

static void flush_location_to_pou( void *va )
{
  asm ( "mcr p15, 0, %[va], cr7, cr11, 1" : : [va] "r" (va) );
}

static void invalidate_cache_at( void *va )
{
//// NONONONO  asm ( "mcr p15, 0, %[va], cr7, cr11, 1" : : [va] "r" (va) );
}

static void clean_and_invalidate_cache()
{
  asm ( "mcr p15, 0, %[zero], cr7, cr14, 0" : : [zero] "r" (0) );
}

typedef union {
  struct {
    uint32_t type2:2;
    uint32_t B:1;
    uint32_t C:1;
    uint32_t XN:1;
    uint32_t Domain:4;
    uint32_t P:1;
    uint32_t AP:2;
    uint32_t TEX:3;
    uint32_t APX:1;
    uint32_t S:1;
    uint32_t nG:1;
    uint32_t zeros:2;
    uint32_t section_base:12;
  };
  uint32_t raw;
} l1tt_section_entry;

static uint32_t physical_address_of( void *p )
{
  return (uint32_t) p;
}

uint32_t * const screen = (void*) 0x04000000; // Virtual address

static void blue_peter( uint32_t *screen )
{
  for (uint32_t y = 10; y < 1060; y++) {
    uint32_t *p = &screen[y * 1920 + 10];
    for (int x = 0; x < 1900; x++) *p++ = 0xff0000ff;
  }
  for (uint32_t y = 1060 / 4; y < 1060 * 3 / 4; y++) {
    uint32_t *p = &screen[y * 1920 + 1900 / 4];
    for (int x = 0; x < 1900 / 2; x++) *p++ = 0xffffffff;
  }
}

static uint32_t __attribute__(( aligned( 16 ) )) volatile tags[26] =
  { sizeof( tags ), 0,
    // Tags: Tag, buffer size, request code, buffer
    0x00040001, // Allocate buffer
    8, 0, 2 << 20, 0, // Size, Code, In: Alignment, Out: Base, Size
    0x00048003, // Set physical (display) width/height
    8, 0, 1920, 1080,
    0x00048004, // Set virtual (buffer) width/height
    8, 0, 1920, 1080,
    0x00048005, // Set depth
    4, 0, 32,
    0x00048006, // Set pixel order
    4, 0, 0,    // 0 = BGR, 1 = RGB
    0 }; // End tag

static uint32_t frame_buffer_address()
{
  for (int i = 0; i < 1000000; i++) { asm ( "" ); }
  gpio[0x28/4] = (1 << 22); // Clr

  struct __attribute__(( packed )) BCM_mailbox {
    uint32_t value; // Request or Response, depending if from or to ARM,
               // (Pointer & 0xfffffff0) | Channel 0-15
    uint32_t res1;
    uint32_t res2;
    uint32_t res3;
    uint32_t peek;  // Presumably doesn't remove the value from the FIFO
    uint32_t sender;// ??
    uint32_t status;// bit 31: Tx full, 30: Rx empty
    uint32_t config;
  } volatile *mailbox = (void*) 0x3f00b880; // 0x3f000000 + 0xb880
  // ARM may read mailbox 0, write mailbox 1.

  uint32_t request = 8 | physical_address_of( (void*) &tags );
  mailbox[1].value = request;

  uint32_t const toggle = (1 << 26);
  uint32_t count = 0;
    
  do {
    while ((mailbox[0].status & (1 << 30)) != 0) {
      if (((++count) & (toggle - 1)) == 0) {
        if ((count & toggle) != 0) {
          gpio[0x1c/4] = (1 << 22); // Set
        }
        else {
          gpio[0x28/4] = (1 << 22); // Clr
        }
      }
    }
  } while (mailbox[0].value != request);

  blue_peter( (void*) (tags[5] & ~0xc0000000) );

  return (tags[5] & ~0xc0000000);
}

static void fill_rect( uint32_t left, uint32_t top, uint32_t w, uint32_t h, uint32_t c )
{
  for (uint32_t y = top; y < top + h; y++) {
    uint32_t *p = &screen[y * 1920 + left];
    for (int x = 0; x < w; x++) { *p++ = c; flush_location_to_poc( p-1 ); }
  }
}

static void fight_for( uint32_t *lock, int x, uint32_t y, int c )
{
  static uint32_t colour = 0xffffffff;
  static uint32_t people_with_lock[32] = { 0 };
  static uint32_t onelock = 0;

  claim_lock( &onelock );

  people_with_lock[c] = 1;
  //clean_and_invalidate_cache();
  for (int i = 0; i < 32; i++) {
    if (i != c && people_with_lock[i]) {
      fill_rect( 200, 200*c + 500, 1000, 50, 0xffffffff << (6 * c) );
    }
  }

  flush_location_to_pou( &people_with_lock[c] );
  for (int i = 0; i < 32; i++) {
    invalidate_cache_at( &colour );
    if (i != c && people_with_lock[i]) {
      fill_rect( 200, 200*c + 500, 1000, 50, 0xffffffff << (6 * c) );
    }
  }

  colour = colour << 1;

  for (int i = 0; i < 18; i++) {
    //flush_location_to_pou( &colour );

    fill_rect( x, y, 50, 50, colour << i );

    for (int i = 0; i < 1 << 20; i++) {
      for (int i = 0; i < 32; i++) {
        if (i != c && people_with_lock[i]) {
          fill_rect( 200, 200*c + 500, 1000, 50, 0xffffffff << (6 * c) );
        }
      }
    }
  }

  people_with_lock[c] = 0;
  release_lock( &onelock );
  for (int i = 0; i < 100; i++) { asm ( "" ); }
}

void with_stack( uint32_t core_number )
{
  switch (processor_mode()) {
  case 0x17: // Abort
    fill_rect( 800, 800, 200, 200, 0xffff0000 );
    for (;;) {}
    break;
  case 0x1b: // Undef
    fill_rect( 800, 800, 200, 200, 0xff00ff00 );
    for (;;) {}
    break;
  }

  // This is very important! Without enabling SMP on all processors, claim_lock
  // will not work! (Many days gave their lives to bring us this information.)
  register uint32_t r0 asm( "r0" ) = (1 << 6); // SMPEN
  register uint32_t r1 asm( "r1" ) = 0;
  asm ( "MCRR p15, 1, r0, r1, c15" : : "r" (r0), "r" (r1) ); // Write CPU Extended Control Register (64-bits)

  l1tt_section_entry code_entry = {
    .type2 = 2,
    // .B = 0, .C = 0, .XN = 0,
    .TEX = 5, .C = 0, .B = 1, .XN = 0, // Write-Back cached, Write Allocate, Buffered, both L1 (TEX[1:0]) L2 (CB)
    .Domain = 0, .P = 0,
    .AP = 3, .APX = 0, .S = 1
    };

  // The code is in the first megabyte.

  L1TT[core_number][0] = code_entry.raw;

  static uint32_t volatile screen_physical_address = 0;

  if (core_number == 0) {
    init_gpio();
    screen_physical_address = frame_buffer_address();
  }

  while (screen_physical_address == 0) {}

  l1tt_section_entry screen_entry = {
    .type2 = 2,
    .B = 1, .C = 1, .XN = 0,
    .Domain = 0, .P = 0,
    .AP = 3, .APX = 0, .S = 1
    };

  for (uint32_t i = 0; i < 8; i++) {
    screen_entry.section_base = (screen_physical_address >> 20) + i;
    L1TT[core_number][i+64] = screen_entry.raw;
  }

#define MODIFY_CP15_REG( reg, bits, new_values, s ) \
    { if (new_values == 0) { asm volatile ( "mrc p15, 0, %[v], "reg"\n  bic %[v], %[b]\n  mcr p15, 0, %[v], "reg"" : [v] "=&r" (s) : [b] "Ir" (new_values) ); } \
    else { asm volatile ( "mrc p15, 0, %[v], "reg"\n  bic %[v], %[b]\n  eor %[v], %[v], %[n]\n  mcr p15, 0, %[v], "reg"" : [v] "=&r" (s) : [b] "ir" (new_values), [n] "Ir" (new_values) ); } }

  uint32_t tmp;
  MODIFY_CP15_REG( "c1, c0, 1", (1 << 5), (1 << 5), tmp );

  asm ( "mcr p15, 0, %[ttbr0], c2, c0, 0" : : [ttbr0] "r" (L1TT[core_number]) );
  asm ( "mcr p15, 0, %[dacr], c3, c0, 0" : : [dacr] "r" (1) ); // Only using Domain 0, at the moment, allow access.

  uint32_t tcr;

  asm ( "  mrc p15, 0, %[tcr], c1, c0, 0" : [tcr] "=r" (tcr) );

  tcr |=  (1 << 23); // XP, bit 23, 1 = subpage AP bits disabled.
  tcr &= ~(1 << 29); // Access Bit not used
  tcr |=  (1 << 13); // High vectors; there were problems with setting this bit independently, so do it here
  tcr |=  (1 << 12); // Instruction cache
  tcr |=  (1 <<  2); // Data cache
  tcr |=  (1 <<  0); // MMU Enable

  // Don't have to do anything clever, nothing's moved.
  asm ( "  dsb sy"
      "\n  mcr p15, 0, %[tcr], c1, c0, 0" 
      :
      : [tcr] "r" (tcr) );

  for (int i = 0; i < 40; i++) {
    fight_for( (uint32_t*) ((i & 3) << 20), 100 + 200 * core_number + i, (i & 3) * 100, core_number );
  }

  // Tentative results: BC 00 no, 10 no, 11 displays 4 rectangles, but all white. Further investigation: nothing blocks on the so-called "lock"!

  for (;;) {}
  __builtin_unreachable();
}
