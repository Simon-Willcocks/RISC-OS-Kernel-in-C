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
