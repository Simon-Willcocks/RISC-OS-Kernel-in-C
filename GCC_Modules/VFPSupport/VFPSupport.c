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

/* Dummy module to pretend to be VFPSupport for the time being. */

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing
//   New feature: instead of one private word per core, r12 points to a shared
//   word, initialised by the first core to initialise the module.

#define MODULE_CHUNK "0x58EC0"
#include "module.h"

NO_start;
NO_init;
NO_finalise;
NO_service_call;
//NO_title;
NO_help;
NO_keywords;
//NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "VFPSupport";

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

#ifdef DEBUG_OUTPUT

#define WriteS( string ) asm ( "svc 1\n  .string \""string"\"\n  .balign 4" : : : "lr" )

#define NewLine asm ( "svc 3" : : : "lr" )

#define Write0( string ) do { register uint32_t r0 asm( "r0" ) = (uint32_t) (string); asm ( "push { r0-r12, lr }\nsvc 2\n  pop {r0-r12, lr}" : : "r" (r0) ); } while (false)
#define Write13( string ) do { const char *s = (void*) (string); register uint32_t r0 asm( "r0" ); while (31 < (r0 = *s++)) { asm ( "push { r1-r12, lr }\nsvc 0\n  pop {r1-r12, lr}" : : "r" (r0) ); } } while (false)
#else
#define WriteS( string )
#define NewLine
#define Write0( string )
#define Write13( string )
#endif

static void WriteNum( uint32_t number )
{
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (c > '9') c += ('a' - '0' - 10);
    register uint32_t r0 asm( "r0" ) = c;
    asm( "svc 0": : "r" (r0) : "lr", "cc" );
  }
}

// This needs a defined struct workspace
C_SWI_HANDLER( c_swi_handler );

struct workspace {
  uint32_t lock;
};

bool __attribute__(( noinline )) c_swi_handler( struct workspace *workspace, SWI_regs *regs )
{
  NewLine; Write0( "VFPSupport SWI " ); WriteNum( regs->number ); NewLine;
  return true;
}
