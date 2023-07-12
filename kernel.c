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

/*
Sections of RISC OS:

  Processor (cores)
  Memory
  Kernel devices (MMU, Interrupt controller)
  Devices (Timer, Display, Busses...)
  Boot sequence


Drop support for: 26-bit modes
*/

#include "inkernel.h"

// Kernel_default_undef, Kernel_default_reset temporarily in memory_manager.c - BSOD

// Kernel_default_irq in task_slot.c
// Kernel_default_svc in swis.c
// Kernel_default_prefetch in memory_manager.c
// Kernel_default_prefetch in mmu.c

void __attribute__(( noreturn, noinline )) Kernel_start()
{
  // Fail early, fail hard
  {
    // Problems occur when the shared and core workspaces overlap
    // This can be fixed in the linker script by moving their allocated space further apart
    // This check should ring alarm bells (in qemu).
    char *ws = (void*) &workspace;
    char *sh = (void*) &shared;
    if ((ws > sh) && (ws - sh) < sizeof( shared )) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
    if ((ws < sh) && (sh - ws) < sizeof( workspace )) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  }

  // This is just an initial block until RAM has been reported to memory manager
  // The core that was gifted the lock before the MMU was initialised will not block.
  if (claim_lock( &shared.kernel.boot_lock )) {
    // Final use of the pre-mmu sequence's ram_blocks array, now read-only

    for (int i = 0; boot_data.ram_blocks[i].size != 0; i++) {
      Kernel_add_free_RAM( boot_data.ram_blocks[i].base >> 12, boot_data.ram_blocks[i].size >> 12 );
    }

    extern uint32_t svc_stack_top;
    // FIXME: probably only need a few pages
    uint32_t legacy_svc_stack = Kernel_allocate_pages( (1 << 20), (1 << 20) );
    uint32_t va = ((uint32_t) &svc_stack_top);
    va = va & 0xfff00000;

    MMU_map_shared_at( (void*) va, legacy_svc_stack, (1 << 20) );
  }

  // Allow the others to continue, now the free RAM has been registered.
  release_lock( &shared.kernel.boot_lock );

  int32_t vector_offset = ((uint32_t*) &workspace.vectors.reset_vec - &workspace.vectors.reset - 2) * 4;

  workspace.vectors.reset         = 0xe59ff000 + vector_offset; // ldr pc, ..._vec
  workspace.vectors.undef         = 0xe59ff000 + vector_offset;
  workspace.vectors.svc           = 0xe59ff000 + vector_offset;
  workspace.vectors.prefetch      = 0xe59ff000 + vector_offset;
  workspace.vectors.data          = 0xe59ff000 + vector_offset;
  workspace.vectors.unused_vector = 0xeafffffe; // for (;;) {}
  workspace.vectors.irq           = 0xe59ff000 + vector_offset;
  workspace.vectors.fiq[0]        = 0xeafffffe; // for (;;) {}

  workspace.vectors.reset_vec     = Kernel_default_reset;
  workspace.vectors.undef_vec     = Kernel_default_undef;
  workspace.vectors.svc_vec       = Kernel_default_svc;
  workspace.vectors.prefetch_vec  = Kernel_default_prefetch;
  workspace.vectors.data_vec      = Kernel_default_data_abort;
  workspace.vectors.unused        = 0; // Needed to keep the distance the same
  workspace.vectors.irq_vec       = Kernel_default_irq;

  Initialise_undefined_registers();

  // NEW: One legacy SVC stack, protected (how?)
  //MMU_map_shared_at( 0xffd00000, Kernel_allocate_pages( (1 << 20), (1 << 20) ), (1 << 20) );
  Initialise_privileged_mode_stack_pointers();

  // We're going to stick with the tiny boot SVC stack until the first
  // TaskSlot is initialised. (Assuming you're not doing something else
  // from this point on in Boot.)

  Boot();

  for (;;) { asm( "wfi" ); }
}

