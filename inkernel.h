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

extern void __attribute__(( noreturn )) assertion_failed();

#define assert( x ) if (!(x)) { Write0( "FAILED: " ); Write0( __func__ ); Write0( " "#x ); assertion_failed(); }

extern const char hex[16];

void SVCWriteNum( uint32_t n );
void SVCWriteN( char const *s, int len );

#ifndef NO_DEBUG_OUTPUT

#define WriteNum( n ) SVCWriteNum( (uint32_t) (n) )
#define WriteN( s, n ) SVCWriteN( s, n )

#define WriteS( string ) WriteN( string, sizeof( string ) - 1 )

#define Write0( string ) do { char *s = (char*) string; if (s == 0) s = "<NULL>"; int len = 0; for (len = 0; (s[len] != '\0' && s[len] != '\n' && s[len] != '\r'); len++) {}; WriteN( s, len ); } while (false)

#define NewLine WriteS( "\n" )
#define Space WriteS( " " )

#else
#define WriteS( string )
#define Write0( string )
#define NewLine 
#define Space 
#endif
