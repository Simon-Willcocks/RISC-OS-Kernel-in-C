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

/* Portable replacement. */

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing
//   New feature: instead of one private word per core, r12 points to a shared
//   word, initialised by the first core to initialise the module.

#define MODULE_CHUNK "0x42fc0"
#include "module.h"

NO_start;
NO_init;
NO_finalise;
NO_service_call;
//NO_title;
NO_help;
NO_keywords;
//NO_swi_handler;
//NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "Portable";

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

struct workspace { };

// This needs a defined struct workspace
C_SWI_HANDLER( c_swi_handler );

static void print( char const *const string )
{
  register char const *const r0 asm( "r0" ) = string;
  asm ( "svc 2" : : "r" (r0) : "lr", "cc" );
}

char const swi_names[] = { "Speed"
          "\0Control"
          "\0ReadBMUVariable"
          "\0WriteBMUVariable"
          "\0CommandBMU"
          "\0ReadFeatures"
          "\0Idle"
          "\0Stop"
          "\0Status"
          "\0Contrast"
          "\0Refresh"
          "\0Halt"
          "\0SleepTime"
          "\0SMBusOp"
          "\0Speed2"
          "\0WakeTime"
          "\0EnumerateBMU"
          "\0ReadBMUVariables"
          "\0" };

bool __attribute__(( noinline )) c_swi_handler( struct workspace *workspace, SWI_regs *regs )
{
  print( "Handling Portable SWI " );

  switch (regs->number) {
  case 5: // Portable_ReadFeatures
    {
      regs->r[1] = 0; // None!
      return true;
    }
  default:
    {
      char const *name = swi_names;
      int n = regs->number;
      while (n > 0 && *name != '\0') {
        n--;
        while (*name != '\0') name++;
        name++;
      }
      print( name == 0 ? "Unknown" : name );
    }
  }
  static const error_block error = { 0x1ff, "Portable features not supported" };
  regs->r[0] = (uint32_t) &error;
  return false;
}
