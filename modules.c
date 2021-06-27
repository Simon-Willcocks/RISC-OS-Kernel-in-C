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

typedef struct {
  uint32_t offset_to_start;
  uint32_t offset_to_initialisation;
  uint32_t offset_to_finalisation;
  uint32_t offset_to_service_call_handler;
  uint32_t offset_to_title_string;
  uint32_t offset_to_help_string;
  uint32_t offset_to_help_and_command_keyword_table;
  uint32_t swi_chunk;
  uint32_t offset_to_swi_handler;
  uint32_t offset_to_swi_decoding_table;
  uint32_t offset_to_swi_decoding_code;
} module_header;

struct module {
  module_header *header;
  uint32_t private_word;
  module *next;  // Simple singly-linked list
};

static uint32_t start_code( module_header *header )
{
  return header->offset_to_start + (uint32_t) header;
}

static uint32_t initialisation_code( module_header *header )
{
  return header->offset_to_initialisation + (uint32_t) header;
}

static uint32_t finalisation_code( module_header *header )
{
  return header->offset_to_finalisation + (uint32_t) header;
}

static uint32_t service_call_handler_code( module_header *header )
{
  return header->offset_to_service_call_handler + (uint32_t) header;
}

static uint32_t swi_handler_code( module_header *header )
{
  return header->offset_to_swi_handler + (uint32_t) header;
}

static uint32_t swi_decoding_table_code( module_header *header )
{
  return header->offset_to_swi_decoding_table + (uint32_t) header;
}

static uint32_t swi_decoding_code( module_header *header )
{
  return header->offset_to_swi_decoding_code + (uint32_t) header;
}

static const char *title_string( module_header *header )
{
  return (const char *) header->offset_to_title_string + (uint32_t) header;
}

static const char *help_string( module_header *header )
{
  return (const char *) header->offset_to_help_string + (uint32_t) header;
}

static bool run_module_code( svc_registers *regs, uint32_t code, uint32_t r11, uint32_t r12 )
{
  register uint32_t legacy_code asm( "r10" ) = code;
  register uint32_t swi_number asm( "r11" ) = r11;
  register uint32_t private_word_ptr asm( "r12" ) = r12;
  register uint32_t failed;
  asm ( "  push { %[regs] }"
      "\n  ldm %[regs], { r0-r9 }"
      "\n  blx r10"
      "\n  pop { r10 }"
      "\n  stm r10, { r0-r9 }"
      "\n  movvs %[failed], #1"
      "\n  movvc %[failed], #0"
      : [failed] "=r" (failed)
      : [regs] "r" (regs)
      , "r" (legacy_code)
      , "r" (swi_number)
      , "r" (private_word_ptr)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9" );
  return failed;
}

bool do_module_swi( svc_registers *regs, uint32_t svc )
{
  uint32_t chunk = svc & ~Xbit & ~0x3f;
  module *m = workspace.kernel.module_list;
  while (m != 0 && m->header->swi_chunk != chunk) {
    m = m->next;
  }
  if (m == 0) {
    regs->r[0] = Kernel_Error_UnknownSWI;
    return false;
  }
  return run_module_code( regs, swi_handler_code( m->header ), svc & 0x3f, (uint32_t) &m->private_word );
}

bool do_OS_ServiceCall( svc_registers *regs )
{
  bool no_error = true;
  module *m = workspace.kernel.module_list;

  while (m != 0 && regs->r[1] != 0 && no_error) {
    no_error = run_module_code( regs, service_call_handler_code( m->header ), 0, (uint32_t) &m->private_word );
    m = m->next;
  }

  return no_error;
}



