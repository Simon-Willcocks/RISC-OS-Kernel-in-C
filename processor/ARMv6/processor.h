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
    { if (new_values == 0) { asm volatile ( "mrc p15, 0, %[v], "reg"\n  bic %[v], %[v], %[b]\n  mcr p15, 0, %[v], "reg"" : [v] "=&r" (s) : [b] "Ir" (new_values) ); } \
      else if (new_values == bits) { asm volatile ( "mrc p15, 0, %[v], "reg"\n  orr %[v], %[v], %[b]\n  mcr p15, 0, %[v], "reg"" : [v] "=&r" (s) : [b] "ir" (new_values) ); } \
      else { asm volatile ( "mrc p15, 0, %[v], "reg"\n  bic %[v], %[b]\n  eor %[v], %[v], %[n]\n  mcr p15, 0, %[v], "reg"" : [v] "=&r" (s) : [b] "ir" (bits), [n] "Ir" (new_values) ); } }

typedef struct {
  uint32_t (*number_of_cores)();
} processor_fns;

extern processor_fns processor;

// Fills in processor_fns structure, which will become read only by the time the MMU is enabled
// and the function pointers will be valid. Returns the number of cores.
uint32_t pre_mmu_identify_processor();

#define PROCESSOR_PROC( name ) static inline void name() { processor.name(); }
#define PROCESSOR_FN( name ) static inline uint32_t name() { return processor.name(); }

PROCESSOR_FN( number_of_cores )
void set_smp_mode();

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

static inline uint32_t fault_address()
{
  uint32_t result;
  asm ( "mrc p15, 0, %[dfar], c6, c6, 0" : [dfar] "=r" (result ) );
  return result;
}

static inline uint32_t fault_type()
{
  uint32_t result;
  asm ( "mrc p15, 0, %[dfsr], c5, c0, 0" : [dfsr] "=r" (result ) );
  return result;
}

void Initialise_privileged_mode_stack_pointers();

// There's no possibility of a RISC OS thread of execution to hand over to
// another running on the same core, so there's no call for a particularly
// flexible lock system.

// This code conforms to the section 7.2 of PRD03-GENC-007826: "Acquiring
// and Releasing a Lock"
// It requires every core involved to have had its SMP bit set (look in the
// TRM for the processor).
// It requires that the memory containing the lock is normal memory and cached.
// If a core avoids using AMP, it can still communicate with other cores using
// uncached memory, mailboxes and careful cleaning and/or invalidation of caches.
static inline void claim_lock( uint32_t volatile *lock )
{
  uint32_t failed = 1;
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

static inline void flush_location( void *va )
{
  // DCCMVAC
  asm ( "mcr p15, 0, %[va], cr7, cr10, 1" : : [va] "r" (va) );
}

// level = 1 -> Level 1 DCache
// level = 2 -> Level 2 DCache
void clean_cache( uint32_t level );

