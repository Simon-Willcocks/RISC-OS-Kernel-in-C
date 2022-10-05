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
#include "trivial_display.h" // show_word - for debugging, TODO: remove

extern uint32_t undef_stack_top;
extern uint32_t abt_stack_top;
extern uint32_t irq_stack_top;
extern uint32_t fiq_stack_top;

void Initialise_privileged_mode_stack_pointers()
{
  asm ( "msr sp_und, %[stack]" : : [stack] "r" (&undef_stack_top) );
  asm ( "msr sp_abt, %[stack]" : : [stack] "r" (&abt_stack_top) );
  asm ( "msr sp_irq, %[stack]" : : [stack] "r" (&irq_stack_top) );
  asm ( "msr sp_fiq, %[stack]" : : [stack] "r" (&fiq_stack_top) );
}

void Initialise_undefined_registers()
{
  uint32_t mode;
  asm ( "mrs %[mode], cpsr" : [mode] "=r" (mode) );
  mode = mode & 0x1f;

  // Set for the current mode.
  asm ( "msr spsr, %[zero]" : : [zero] "r" (0) );

  // Using a banked register access instruction while in the mode is constrained unpredictable.

  if (mode != 0x13)
    asm ( "msr spsr_svc, %[zero]" : : [zero] "r" (0) );

  if (mode != 0x1b)
    asm ( "msr spsr_und, %[zero]" : : [zero] "r" (0) );

  if (mode != 0x17)
    asm ( "msr spsr_abt, %[zero]" : : [zero] "r" (0) );

  if (mode != 0x12)
    asm ( "msr spsr_irq, %[zero]" : : [zero] "r" (0) );

  if (mode != 0x11)
    asm ( "msr spsr_fiq, %[zero]" : : [zero] "r" (0) );
}

void Cortex_A7_set_smp_mode()
{
  uint32_t reg;
  MODIFY_CP15_REG( "c1, c0, 1", (1 << 6), (1 << 6), reg );
}

void Cortex_A53_set_smp_mode()
{
  // Write CPU Extended Control Register (64-bits)
  // ARM Cortex-A53 (probably -A72)
  register uint32_t r0 asm ( "r0" );
  register uint32_t r1 asm ( "r1" );
  asm ( "MRRC p15, 1, r0, r1, c15" : "=r" (r0), "=r" (r1) );
  asm ( "MCRR p15, 1, r0, r1, c15" : : "r" (r0 | (1 << 6)), "r" (r1) );
}

void set_smp_mode()
{
  uint32_t main_id;

  asm ( "MRC p15, 0, %[id], c0, c0, 0" : [id] "=r" (main_id) );

  switch (main_id) {
  case 0x410fc070 ... 0x410fc07f: Cortex_A7_set_smp_mode(); return;
  case 0x410fd030 ... 0x410fd03f: Cortex_A53_set_smp_mode(); return; // A53
  case 0x410fd080 ... 0x410fd08f: Cortex_A53_set_smp_mode(); return; // A72
  default: for (;;) { asm( "wfi" ); }
  }
}

uint32_t Cortex_A7_number_of_cores()
{
  uint32_t result;
  // L2CTLR, ARM DDI 0500G Cortex-A53, generally usable?
  asm ( "MRC p15, 1, %[result], c9, c0, 2" : [result] "=r" (result) );
  return ((result >> 24) & 3) + 1;
}

// This variable is forced into the .text section so that we can get its relative address.
processor_fns __attribute__(( section( ".text" ) )) processor;

static inline processor_fns *unrelocated_pointer()
{
  processor_fns *unrelocated_processor;
  asm ( "adr %[code], processor" : [code] "=r" (unrelocated_processor) );
  return unrelocated_processor;
}

static inline void set_processor( processor_fns *fns )
{
  processor_fns *unrelocated_processor;
  asm ( "adr %[code], processor" : [code] "=r" (unrelocated_processor) );
  uint32_t f = (uint32_t) fns;
  f += ((uint8_t*) unrelocated_processor) - ((uint8_t*) &processor);
  processor_fns *unrelocated_fns = (void*) f;
  *unrelocated_processor = *unrelocated_fns;
}

static void clean_cache_32( int level ) // 0 to 6
{
  asm ( "dsb sy" );
  // Select cache level
  asm ( "mcr p15, 2, %[level], c0, c0, 0" : : [level] "r" (level << 1) ); // CSSELR Cache Size Selection Register.
  asm ( "dsb sy" );
  asm ( "isb" );        // sync the change to the CCSIDR (from HAL_BCM2835/hdr/BCM2835)

  uint32_t line_size = processor.caches.v7.cache[level].line_size;
  uint32_t ways = processor.caches.v7.cache[level].ways;
  uint32_t sets = processor.caches.v7.cache[level].sets;

  int wayshift; // Number of bits to shift the way index by
  asm ( "clz %[ws], %[assoc]" : [ws] "=r" (wayshift) : [assoc] "r" (ways) );

show_word( 100 + 100 * workspace.core_number, 800 + 50 * level, line_size, White );
show_word( 100 + 100 * workspace.core_number, 810 + 50 * level, ways, White );

  for (int way = 0; way < ways; way++) {
    uint32_t setway = (way << wayshift) | (level << 1);
    for (int set = 0; set < sets; set++) {
      //asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
      asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
    }
  }
show_word( 100 + 100 * workspace.core_number, 820 + 50 * level, sets, White );
show_word( 100 + 100 * workspace.core_number, 830 + 50 * level, wayshift, White );

  asm ( "dsb sy" );
}

static unsigned cache_type( uint32_t clidr, int level )
{
  return (clidr >> (3 * level)) & 7;
}

static void try_everything()
{
  bool reclaimed = claim_lock( &shared.mmu.lock );
  assert( !reclaimed );

  for (int level = 0; level < 7 && cache_type( processor.caches.v7.clidr.raw, level ) != 0; level++) {
    clean_cache_32( level );
  }
  if (!reclaimed)
    release_lock( &shared.mmu.lock );
}

static void do_nothing()
{
}

static inline void clear_all()
{
  // asm ( "mcr p15, 0, %[zero], c7, c7, 0" : : [zero] "r" (0) ); // invalidate
  asm ( "mcr p15, 0, %[zero], c7, c14, 0" : : [zero] "r" (0) ); // 
}

// This one works, but probably does too much.
static void set_way_no_CCSIDR2()
{
  asm ( "dsb sy" );
  // Select cache level
  for (int level = 1; level <= 2; level++) {
    uint32_t size;
    asm ( "mcr p15, 2, %[level], c0, c0, 0" : : [level] "r" ((level-1) << 1) ); // CSSELR Selection Register.
    asm ( "mrc p15, 1, %[size], c0, c0, 0" : [size] "=r" (size) ); // CSSIDR
    uint32_t line_size = ((size & 7)+4);
    uint32_t ways = 1 + ((size & 0xff8) >> 3);
    uint32_t sets = 1 + ((size & 0x7fff000) >> 13);
    int wayshift; // Number of bits to shift the way index by
    asm ( "clz %[ws], %[assoc]" : [ws] "=r" (wayshift) : [assoc] "r" (ways - 1) );

    for (int way = 0; way < ways; way++) {
      uint32_t setway = (way << wayshift) | ((level - 1) << 1);
      for (int set = 0; set < sets; set++) {
        asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
      }
    }
  }

  asm ( "dsb sy" );
}

static void investigate_cache( processor_fns *fixed )
{
  uint32_t id_mmfr4;
  asm ( "mrc p15, 0, %[reg], c0, c2, 6" : [reg] "=r" (id_mmfr4) );

  if (0 == ((id_mmfr4 >> 24) & 15)) {
    fixed->clean_cache_to_PoU = set_way_no_CCSIDR2;
    fixed->clean_cache_to_PoC = set_way_no_CCSIDR2;
    return;
  }
/* Does not work.
  fixed->clean_cache_to_PoU = clear_all;
  fixed->clean_cache_to_PoC = clear_all;
  return;
*/ 

  uint32_t clidr;
/* Does not work (Pi3)
  asm ( "mcr p15, 1, %[clidr], c0, c0, 1" : [clidr] "=r" (clidr) ); // Cache Level ID Register
*/
  // Two levels of cache that can be cleared by set/way
  clidr = (2 << 24) | (3 << 3) | (3 << 0);

  fixed->caches.v7.clidr.raw = clidr;

  if (fixed->caches.v7.clidr.LoC == 0) {
    fixed->clean_cache_to_PoU = do_nothing;
    fixed->clean_cache_to_PoC = do_nothing;
    return;
  }

  // Only one implementation, at present.
  fixed->clean_cache_to_PoU = try_everything;
  fixed->clean_cache_to_PoC = try_everything;
    fixed->clean_cache_to_PoU = do_nothing;
    fixed->clean_cache_to_PoC = do_nothing;

  // Information for the routines
  for (int level = 0; level < 7 && 0 != cache_type( clidr, level ); level++) {
    asm ( "mcr p15, 2, %[level], c0, c0, 0" : : [level] "r" (level << 1) ); // Cache Size Selection Register.
    asm ( "dsb" );
    uint32_t ccsidr;
    asm ( "mrc p15, 1, %[size], c0, c0, 0" : [size] "=r" (ccsidr) );
    fixed->caches.v7.cache[level].ways = ((ccsidr >> 3) & 0x3ff);
    fixed->caches.v7.cache[level].sets = (ccsidr >> 13) & 0x7fff;
    fixed->caches.v7.cache[level].line_size = (ccsidr & 7) + 4;
  }
}

uint32_t pre_mmu_identify_processor()
{
  // This should be the only place where the processor type gets looked at.
  // OK, here and in set_smp_mode; the only two before the MMU is enabled.
  // The pointers will be fixed in read-only memory when the MMU is enabled.

  uint32_t main_id;
  processor_fns *fixed = unrelocated_pointer();

  asm ( "mrc p15, 0, %[id], c0, c0, 0" : [id] "=r" (main_id) );

  investigate_cache( fixed );
  fixed->number_of_cores = Cortex_A7_number_of_cores();

  switch (main_id) {
  case 0x410fc070 ... 0x410fc07f: break; // A7
  case 0x410fd030 ... 0x410fd03f: break; // A53
  case 0x410fd080 ... 0x410fd08f: break; // A72
  default: for (;;) { asm( "wfi" ); }
  }

  return fixed->number_of_cores;
}

void *memset(void *s, int c, uint32_t n)
{
  uint8_t *p = s;
  for (int i = 0; i < n; i++) { p[i] = c; }
  return s;
}

void *memcpy(void *d, const void *s, uint32_t n)
{
  uint8_t *dest = d;
  const uint8_t *src = s;
  for (int i = 0; i < n; i++) { dest[i] = src[i]; }
  return d;
}



// Change the word at `word' to the value `to' if it contained `from'.
// Returns the original content of word (= from if changed successfully)
uint32_t change_word_if_equal( uint32_t volatile *word, uint32_t from, uint32_t to )
{
  uint32_t failed;
  uint32_t value;

  do {
    asm volatile ( "ldrex %[value], [%[word]]"
                   : [value] "=&r" (value)
                   : [word] "r" (word) );

    if (value == from) {
      // The failed and word registers are not allowed to be the same, so
      // pretend to gcc that the word may be written as well as read.

      asm volatile ( "strex %[failed], %[value], [%[word]]"
                     : [failed] "=&r" (failed)
                     , [word] "+r" (word)
                     : [value] "r" (to) );
    }
    else {
      asm ( "clrex" );
      break;
    }
  } while (failed);
  asm ( "dmb sy" );

  return value;
}

// Temporarily in C file for tracing in QEMU

bool claim_lock( uint32_t volatile *lock )
{
  // TODO: return 0 != change_word_if_equal( lock, 0, workspace.core_number + 1 );
  uint32_t failed;
  uint32_t value;
  uint32_t core = workspace.core_number+1;

  do {
    asm volatile ( "ldrex %[value], [%[lock]]"
                   : [value] "=&r" (value)
                   : [lock] "r" (lock) );

    if (value == core) return true;

    if (value == 0) {
      // The failed and lock registers are not allowed to be the same, so
      // pretend to gcc that the lock may be written as well as read.

      asm volatile ( "strex %[failed], %[value], [%[lock]]"
                     : [failed] "=&r" (failed)
                     , [lock] "+r" (lock)
                     : [value] "r" (core) );
    }
    else {
      asm ( "clrex" );
      failed = true;
    }
  } while (failed);
  asm ( "dmb sy" );

  return false;
}

void release_lock( uint32_t volatile *lock )
{
  // Ensure that any changes made while holding the lock are visible before the lock is seen to have been released
  asm ( "dmb sy" );
  *lock = 0;
}
