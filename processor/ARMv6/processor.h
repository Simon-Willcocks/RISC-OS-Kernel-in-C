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

// Take the existing value from a system register, clear the bits that
// are set in bits, and toggle the bits that are in new_values (which
// sets any bits that are set in both bits and new_values).
#define MODIFY_CP15_REG( reg, bits, new_values, s ) \
    { if (new_values == 0) { asm volatile ( "mrc p15, 0, %[v], "reg"\n  bic %[v], %[b]\n  mcr p15, 0, %[v], "reg"" : [v] "=&r" (s) : [b] "Ir" (new_values) ); } \
      else { asm volatile ( "mrc p15, 0, %[v], "reg"\n  bic %[v], %[b]\n  eor %[v], %[v], %[n]\n  mcr p15, 0, %[v], "reg"" : [v] "=&r" (s) : [b] "ir" (new_values), [n] "Ir" (new_values) ); } }

static inline void set_high_vectors()
{
  register uint32_t v;
  MODIFY_CP15_REG( "c1, c0, 0", (1 << 13), (1 << 13), v );
}

static inline uint32_t get_swi_number( uint32_t instruction_following_swi )
{
  uint32_t result;
  asm ( "ldr %[r], [%[next], #-4]" : [r] "=r" (result) : [next] "r" (instruction_following_swi) );
  return result & 0x00ffffff;
}

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

void Initialise_privileged_mode_stack_pointers();

static inline void claim_lock( uint32_t *lock, uint32_t marker /* unique, non-0. 0 means unlocked */ )
{
  uint32_t failed = 1;
  uint32_t value;

  // The failed and lock registers are not allowed to be the same, so
  // pretend the lock may be written as well as read.

  while (failed) {
    asm volatile ( "ldrex %[value], [%[lock]]"
                   : [value] "=&r" (value)
                   , [lock] "+r" (lock) );
    if (value == 0) {
      asm volatile ( "strex %[failed], %[value], [%[lock]]"
                     : [failed] "=&r" (failed)
                     , [lock] "+r" (lock)
                     : [value] "r" (marker) );
    }
    else {
      asm volatile ( "clrex" );
    }
  }
}

static inline void release_lock( uint32_t *lock )
{
  *lock = 0;
  // Probably need a DSB here, at least. Or LDREX to check we're the owner.
}
