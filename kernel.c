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

#include "kernel.h"

void Kernel_default_reset() {}
void Kernel_default_undef() {}
void Kernel_default_svc() {}
void Kernel_default_prefetch() {}
void Kernel_default_data_abort() {}
void Kernel_default_irq() {}


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

  int32_t vector_offset = ((uint32_t*) &workspace.vectors.reset_vec - &workspace.vectors.reset) * 4;

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

  // Initialise privileged mode stack pointers
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

  // Running in virtual memory with a stack and workspace for each core.
  for (;;) { asm ( "wfi" ); }
}

