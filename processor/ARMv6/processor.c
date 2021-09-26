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

#include "kernel.h"

void Initialise_privileged_mode_stack_pointers()
{
  register uint32_t stack asm( "r0" );

  stack = sizeof( workspace.kernel.undef_stack ) + (uint32_t) &workspace.kernel.undef_stack;
  asm ( "msr cpsr, #0xdb\n  mov sp, r0" : : "r" (stack) );

  stack = sizeof( workspace.kernel.abt_stack ) + (uint32_t) &workspace.kernel.abt_stack;
  asm ( "msr cpsr, #0xd7\n  mov sp, r0" : : "r" (stack) );

  stack = sizeof( workspace.kernel.irq_stack ) + (uint32_t) &workspace.kernel.irq_stack;
  asm ( "msr cpsr, #0xd2\n  mov sp, r0" : : "r" (stack) );

  stack = sizeof( workspace.kernel.fiq_stack ) + (uint32_t) &workspace.kernel.fiq_stack;
  asm ( "msr cpsr, #0xd1\n  mov sp, r0" : : "r" (stack) );

  // Finally, end up back in svc32, with the stack pointer unchanged:
  asm ( "msr cpsr, #0xd3" );
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
  asm ( "isb" );        // sync the change to the CCSIDR (from HAL_BCM2835/hdr/BCM2835)

  uint32_t line_size = processor.caches.v7.cache[level].line_size;
  uint32_t ways = processor.caches.v7.cache[level].ways;
  uint32_t sets = processor.caches.v7.cache[level].sets;

  int wayshift; // Number of bits to shift the way index by
  asm ( "clz %[ws], %[assoc]" : [ws] "=r" (wayshift) : [assoc] "r" (ways) );

show_word( 100 + 100 * workspace.core_number, 200 + 50 * level, line_size, White );
show_word( 100 + 100 * workspace.core_number, 210 + 50 * level, ways, White );

  for (int way = 0; way < ways; way++) {
    uint32_t setway = (way << wayshift) | (level << 1);
    for (int set = 0; set < sets; set++) {
      //asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
      asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
    }
  }
show_word( 100 + 100 * workspace.core_number, 220 + 50 * level, sets, White );
show_word( 100 + 100 * workspace.core_number, 230 + 50 * level, wayshift, White );

  asm ( "dsb sy" );
}

static unsigned cache_type( uint32_t clidr, int level )
{
  return (clidr >> (3 * level)) & 7;
}

static void try_everything()
{
  for (int level = 0; level < 7 && cache_type( processor.caches.v7.clidr.raw, level ) != 0; level++) {
    clean_cache_32( level );
  }
}

static void do_nothing()
{
}

static void clear_all()
{
  // asm ( "mcr p15, 0, %[zero], c7, c7, 0" : : [zero] "r" (0) ); // invalidate
  asm ( "mcr p15, 0, %[zero], c7, c14, 0" : : [zero] "r" (0) ); // 
}

static void investigate_cache( processor_fns *fixed )
{
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
  fixed->clean_cache_to_PoU = do_nothing; // try_everything;
  fixed->clean_cache_to_PoC = do_nothing; // try_everything;

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
  fixed->number_of_cores = Cortex_A7_number_of_cores;

  switch (main_id) {
  case 0x410fc070 ... 0x410fc07f: break; // A7
  case 0x410fd030 ... 0x410fd03f: break; // A53
  case 0x410fd080 ... 0x410fd08f: break; // A72
  default: for (;;) { asm( "wfi" ); }
  }

  return Cortex_A7_number_of_cores();
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

