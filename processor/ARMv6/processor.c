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

processor_fns __attribute__(( section( ".text" ) )) processor;

processor_fns __attribute__(( section( ".text" ) )) cortex_a7_fns = {
  .number_of_cores = Cortex_A7_number_of_cores
};

processor_fns __attribute__(( section( ".text" ) )) cortex_a53_fns = {
  .number_of_cores = Cortex_A7_number_of_cores
};

static inline void set_processor( processor_fns *fns )
{
  processor_fns *unrelocated_processor;
  asm ( "adr %[code], processor" : [code] "=r" (unrelocated_processor) );
  uint32_t f = (uint32_t) fns;
  f += ((uint8_t*) unrelocated_processor) - ((uint8_t*) &processor);
  processor_fns *unrelocated_fns = (void*) f;
  *unrelocated_processor = *unrelocated_fns;
}

uint32_t pre_mmu_identify_processor()
{
  // This should be the only place where the processor type gets looked at.
  // OK, here and in set_smp_mode; the only two before the MMU is enabled.
  // The pointers will be fixed in read-only memory when the MMU is enabled.

  uint32_t main_id;

  asm ( "MRC p15, 0, %[id], c0, c0, 0" : [id] "=r" (main_id) );

  switch (main_id) {
  case 0x410fc070 ... 0x410fc07f: set_processor( &cortex_a7_fns ); return Cortex_A7_number_of_cores(); // A7
  case 0x410fd030 ... 0x410fd03f: set_processor( &cortex_a53_fns ); return Cortex_A7_number_of_cores(); // A53
  case 0x410fd080 ... 0x410fd08f: set_processor( &cortex_a53_fns ); return Cortex_A7_number_of_cores(); // A72
  default: for (;;) { asm( "wfi" ); }
  }

  return 1;
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

void clean_cache( uint32_t level )
/*
{
  // Taken direct from ARM DAI 0527A 4.3.1 Cleaning and invalidating the caches
  asm (
  "\nMOV R0, %[level]"
  "\nMCR P15, 2, R0, C0, C0, 0"
  "\nMRC P15, 1, R4, C0, C0, 0"
  "\nAND R1, R4, #0x7 "
  "\nADD R1, R1, #0x4 "
  "\nLDR R3, =0x7FFF "
  "\nAND R2, R3, R4, LSR #13 "
  "\nLDR R3, =0x3FF "
  "\nAND R3, R3, R4, LSR #3"
  "\nCLZ R4, R3"
  "\nMOV R5, #0"
  "\n"
  "\nway_loop:"
  "\nMOV R6, #0"
  "\n"
  "\nset_loop:"
  "\n"
  "\nORR R7, R0, R5, LSL R4"
  "\nORR R7, R7, R6, LSL R1"
  "\nMCR P15, 0, R7, C7, C6, 2"
  "\nADD R6, R6, #1"
  "\nCMP R6, R2"
  "\nBLE set_loop"
  "\nADD R5, R5, #1"
  "\nCMP R5, R3"
  "\nBLE way_loop"
  : : [level] "ir" ((level - 1) << 1)
  : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7" );
}
{
  // Assuming FEAT_CCIDX
  // This is a function that should be filled in by the pre_mmu_identify_processor routine
  level = (level - 1) << 1;
  asm ( "dsb sy" );
  asm ( "mcr p15, 2, %[level], c0, c0, 0" : : [level] "r" (level) ); // CSSELR Cache Size Selection Register.
  uint32_t ccsidr;
  uint32_t ccsidr2;
  asm ( "mrc p15, 1, %[size], c0, c0, 0" : [size] "=r" (ccsidr) );
  asm ( "mrc p15, 1, %[size], c0, c0, 2" : [size] "=r" (ccsidr2) );

  uint32_t line_size = (ccsidr & 7) + 4;
  uint32_t ways = (ccsidr >> 3) & ((1 << 20)-1);
  uint32_t sets = (ccsidr2 >> 0) & ((1 << 23)-1);

  int wayshift; // Number of bits to shift the way index by
  asm ( "clz %[ws], %[assoc]" : [ws] "=r" (wayshift) : [assoc] "r" (ways) );

show_word( 100 + 100 * workspace.core_number, 150, line_size, White );
show_word( 100 + 100 * workspace.core_number, 160, ways, White );
show_word( 100 + 100 * workspace.core_number, 170, sets, White );
show_word( 100 + 100 * workspace.core_number, 180, wayshift, White );

  for (int way = 0; way < ways; way++) {
    uint32_t setway = (way << wayshift) | level;
    for (int set = 0; set < sets; set++) {
      asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
    }
  }
  asm ( "dsb sy" );
}

*/
{
  // Assuming NOT FEAT_CCIDX
  // This is a function that should be filled in by the pre_mmu_identify_processor routine
  level = (level - 1) << 1;
  asm ( "dsb sy" );
  asm ( "mcr p15, 2, %[level], c0, c0, 0" : : [level] "r" (level) ); // CSSELR Cache Size Selection Register.
  uint32_t ccsidr;
  asm ( "mrc p15, 1, %[size], c0, c0, 0" : [size] "=r" (ccsidr) );

  uint32_t line_size = (ccsidr & 7) + 4;
  uint32_t ways = ((ccsidr >> 3) & 0x3ff);
  uint32_t sets = (ccsidr >> 13) & 0x7fff;

  int wayshift; // Number of bits to shift the way index by
  asm ( "clz %[ws], %[assoc]" : [ws] "=r" (wayshift) : [assoc] "r" (ways) );

show_word( 100 + 100 * workspace.core_number, 150, line_size, White );
show_word( 100 + 100 * workspace.core_number, 160, ways, White );
show_word( 100 + 100 * workspace.core_number, 170, sets, White );
show_word( 100 + 100 * workspace.core_number, 180, wayshift, White );

  for (int way = 0; way < ways; way++) {
    uint32_t setway = (way << wayshift) | level;
    for (int set = 0; set < sets; set++) {
      asm ( "mcr p15, 0, %[sw], c7, c14, 2" : : [sw] "r" (setway | (set << line_size)) ); // DCCISW
    }
  }
  asm ( "dsb sy" );
}
