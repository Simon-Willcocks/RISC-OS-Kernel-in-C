/* Copyright 2022 Simon Willcocks
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

/* Dummy module to pretend to be FPEmulator for the time being. */

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing
//   New feature: instead of one private word per core, r12 points to a shared
//   word, initialised by the first core to initialise the module.

#define MODULE_CHUNK "0"
#include "module.h"

NO_start;
NO_init;
NO_finalise;
NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
//NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "FPEmulator";

const char help[] = "FPEmulator\t4.99 (1 Apr 2022) (Fake)";

// This needs a defined struct workspace
C_SWI_HANDLER( c_swi_handler );

struct workspace {
  uint32_t lock;
};

bool __attribute__(( noinline )) c_swi_handler( struct workspace *workspace, SWI_regs *regs )
{
  NewLine; Write0( "FPEmulator SWI " ); WriteNum( regs->number ); NewLine;
  return true;
}
