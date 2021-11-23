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

bool do_module_swi( svc_registers *regs, uint32_t svc );

void __attribute__(( naked, noreturn )) Kernel_default_reset();
void __attribute__(( naked, noreturn )) Kernel_default_undef();
void __attribute__(( naked, noreturn )) Kernel_default_prefetch();
void __attribute__(( naked, noreturn )) Kernel_default_data_abort();
void __attribute__(( naked, noreturn )) Kernel_default_irq();
void __attribute__(( naked, noreturn )) Kernel_default_svc();

static inline error_block *OSCLI( const char *command )
{
  register const char *c asm( "r0" ) = command;
  register error_block *result asm( "r0" );
  asm volatile ( "svc 0x20005\n  movvc r0, #0" : [error] "=r" (result) : "r" (c) : "lr", "cc" );
  return result;
}


// TEMPORARY!

#define assert( x ) while (!(x)) { asm ( "bkpt 5" ); }

#define WriteS( string ) asm volatile ( "svc 1\n  .string \""string"\"\n  .balign 4" : : : "cc", "lr" )

extern const char hex[16];

// Warning, using the same variable name for n as inside the braces quietly fails
// Hence: write_num_number_to_write
#define WriteNum( n ) { \
  uint32_t write_num_number_to_write = n; \
  uint32_t shift = 32; \
  while (shift > 0) { \
    shift -= 4; \
    register char c asm( "r0" ) = hex[(write_num_number_to_write >> shift) & 0xf]; \
    asm volatile ( "svc 0" : : "r" (c) : "cc", "lr" ); \
  }; }

// Not using OS_Write0, because many strings are not null terminated.
#define Write0( string ) { char *c = (char*) string; while (*c != '\0' && *c != '\n' && *c != '\r') { register uint32_t r0 asm( "r0" ) = *c++; asm volatile ( "svc 0" : : "r" (r0) : "cc", "lr" ); }; }

#define WriteN( string, len ) { register uint32_t r0 asm( "r0" ) = (uint32_t) string; register uint32_t r1 asm( "r1" ) = len; asm volatile ( "svc 0x46" : : "r" (r0), "r" (r1) : "cc", "lr" ); }

#define NewLine asm ( "svc 3" : : : "cc", "lr" )

