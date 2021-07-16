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
#include "swis.h"

typedef struct {
  uint32_t module_start;
  uint32_t swi_handler;
  uint32_t private;
} swi_handler;

typedef struct {
  uint32_t code;
  char desc[];
} error_block;

bool do_module_swi( svc_registers *regs, uint32_t svc );

void __attribute__(( naked, noreturn )) Kernel_default_reset();
void __attribute__(( naked, noreturn )) Kernel_default_undef();
void __attribute__(( naked, noreturn )) Kernel_default_prefetch();
void __attribute__(( naked, noreturn )) Kernel_default_data_abort();
void __attribute__(( naked, noreturn )) Kernel_default_irq();
void __attribute__(( naked, noreturn )) Kernel_default_svc();

