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


static void fill_rect( uint32_t left, uint32_t top, uint32_t w, uint32_t h, uint32_t c )
{
  extern uint32_t frame_buffer;
  uint32_t *screen = &frame_buffer;

  for (uint32_t y = top; y < top + h; y++) {
    uint32_t *p = &screen[y * 1920 + left];
    for (int x = 0; x < w; x++) { *p++ = c; }
  }
}

// Kernel_default_undef, Kernel_default_irq, Kernel_default_reset temporarily in memory_manager.c - BSOD

// Kernel_default_svc in swis.c
// Kernel_default_prefetch, Kernel_default_data_abort in memory_manager.c

void __attribute__(( noreturn, noinline )) Kernel_start()
{
  if (workspace.core_number == 0) {
    // Final use of the pre-mmu sequence's ram_blocks array, now read-only

    for (int i = 0; boot_data.ram_blocks[i].size != 0; i++) {
      Kernel_add_free_RAM( boot_data.ram_blocks[i].base >> 12, boot_data.ram_blocks[i].size >> 12 );
    }
    if (boot_data.less_aligned.size != 0) {
      Kernel_add_free_RAM( boot_data.less_aligned.base >> 12, boot_data.less_aligned.size >> 12 );
    }
  }
  //else { for (;;) { asm ( "wfi" ); } } // Uncomment when debugging to reduce distractions

  int32_t vector_offset = ((uint32_t*) &workspace.vectors.reset_vec - &workspace.vectors.reset - 2) * 4;

  workspace.vectors.reset         = 0xe59ff000 + vector_offset; // ldr pc, ..._vec
  workspace.vectors.undef         = 0xe59ff000 + vector_offset;
  workspace.vectors.svc           = 0xe59ff000 + vector_offset;
  workspace.vectors.prefetch      = 0xe59ff000 + vector_offset;
  workspace.vectors.data          = 0xe59ff000 + vector_offset;
  workspace.vectors.irq           = 0xe59ff000 + vector_offset;
  workspace.vectors.fiq[0]        = 0xeafffffe; // for (;;) {}

  workspace.vectors.reset_vec     = Kernel_default_reset;
  workspace.vectors.undef_vec     = Kernel_default_undef;
  workspace.vectors.svc_vec       = Kernel_default_svc;
  workspace.vectors.prefetch_vec  = Kernel_default_prefetch;
  workspace.vectors.data_vec      = Kernel_default_data_abort;
  workspace.vectors.irq_vec       = Kernel_default_irq;

  Initialise_privileged_mode_stack_pointers();

  Initialise_system_DAs();

  Boot();

  for (;;) { asm( "wfi" ); }
}
