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

// Some versions of this break routines with variables called regs that aren't svc_registers *
extern svc_registers regs[];
extern void __attribute__(( noreturn )) assertion_failed( uint32_t *abt, svc_registers *regs, const char *assertion );

#define assert( x ) if (!(x)) asm ( "bkpt %[line]" : : [line] "i" (__LINE__), "r" (#x) );
//{ asm volatile ( "push {r0-r12,r14,r15}" ); register svc_registers *r asm( "r1" ) = &regs[0]; register const char *ass asm( "r2" ) = #x; asm volatile ( "mov r0, sp\n  bl assertion_failed" : : "r" (r), "r" (ass) ); }
//#define assert( x ) while (!(x)) { asm ( "wfi"  : : "r" (#x) ); }

extern const char hex[16];

void SVCWriteNum( uint32_t n );
void SVCWriteN( char const *s, int len );
void SVCWrite0( char const *s );

#ifndef NO_DEBUG_OUTPUT

#define WriteNum( n ) SVCWriteNum( (uint32_t) (n) )
#define WriteN( s, n ) SVCWriteN( s, n )

#define WriteS( string ) WriteN( string, sizeof( string ) - 1 )

#define Write0( string ) SVCWrite0( (char const *) string )

#define NewLine WriteS( "\n" )
#define Space WriteS( " " )

#else
#define WriteNum( n )
#define WriteN( s, n )
#define WriteS( string )
#define Write0( string )
#define NewLine 
#define Space 
#endif
