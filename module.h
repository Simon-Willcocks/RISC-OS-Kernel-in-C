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

/* Together with module.script, this routine generates a RISC OS module
 * header.
 *
 * arm-linux-gnueabi-gcc-8 my_module.c -o my_module.elf -nostartfiles -nostdlib -fpic \
   -fno-zero-initialized-in-bss -static -g -march=armv8-a+nofp -T module.script &&
 * arm-linux-gnueabi-objcopy  -R .ignoring -O binary my_module.elf my_module.bin
 *
 * Usage:
 * #define MODULE_CHUNK <chunk number> (not needed if no SWI chunk)
 * #include "module.h"
 * static const uint32_t module_flags = <flags value>; (required)
 *
 * You can then provide implementations for the following, as required.
 *
 * start (function)
 * init (function)
 * finalise (function)
 * service_call (function)
 * title (const char [])
 * help (const char [])
 * keywords (const char [])
 * swi_handler (function)
 * swi_names (const char [])
 * swi_decoder (function)
 * messages_file (const char [])
 *
 * By default the module is preceeded by a word containing the length of the
 * module, plus the size of the word, for easy concatenation of modules (which
 * can be terminated by a word containing zero. If this is not wanted, define
 * NO_MODULE_SIZE when compiling.
 *
 */
void __attribute__(( naked, section( ".text.init" ) )) file_start()
{
#ifndef MODULE_CHUNK
#define MODULE_CHUNK "0"
#endif
  asm volatile ( 
#ifndef NO_MODULE_SIZE
  "\n  .word module_end-header+4"
#endif
  "\nheader:"
  "\n  .word start-header"
  "\n  .word init-header"
  "\n  .word finalise-header"
  "\n  .word service_call-header"
  "\n  .word title-header"
  "\n  .word help-header"
  "\n  .word keywords-header"
  "\n  .word "MODULE_CHUNK
  "\n  .word swi_handler-header"
  "\n  .word swi_names-header"
  "\n  .word swi_decoder-header"
  "\n  .word messages_file-header"
  "\n  .word module_flags-header" );
}

#define NO_start asm( "start = header" )
#define NO_init asm( "init = header" )
#define NO_finalise asm( "finalise = header" )
#define NO_service_call asm( "service_call = header" )
#define NO_title asm( "title = header" )
#define NO_help asm( "help = header" )
#define NO_keywords asm( "keywords = header" )
#define NO_swi_handler asm( "swi_handler = header" )
#define NO_swi_names asm( "swi_names = header" )
#define NO_swi_decoder asm( "swi_decoder = header" )
#define NO_messages_file asm( "messages_file = header" )

typedef unsigned long long uint64_t;
typedef unsigned        uint32_t;
typedef int             int32_t;
typedef unsigned short  uint16_t;
typedef short           int16_t;
typedef signed char     int8_t;
typedef unsigned char   uint8_t;
typedef unsigned        size_t;
typedef unsigned        bool;
#define true  (0 == 0)
#define false (0 != 0)

#define assert( c ) while (!(c)) { asm( "bkpt 65535" ); }

#define number_of( arr ) (sizeof( arr ) / sizeof( arr[0] ))

typedef struct {
  uint32_t code;
  char desc[];
} error_block;

#define C_SWI_HANDLER( cfn ) \
typedef struct { \
  uint32_t r[10]; \
  uint32_t number; \
  struct workspace **private_word; \
} SWI_regs; \
 \
bool __attribute__(( noinline )) cfn( struct workspace *workspace, SWI_regs *regs ); \
 \
void __attribute__(( naked )) swi_handler() \
{ \
  SWI_regs *regs; \
  register struct workspace **private_word asm( "r12" ); \
  asm volatile ( "push {r0-r9, r11, r12, r14}\n  mov %[regs], sp" : [regs] "=r" (regs), "=r" (private_word) ); \
  if (!cfn( *private_word, regs )) { \
    /* Error: Set V flag */ \
    asm ( "msr cpsr_f, #(1 << 28)" ); \
  } \
  asm volatile ( "pop {r0-r9, r11, r12, pc}\n  mov r0, sp" ); \
}

// memset not static, this include file should only be included once in a module;
// The optimiser occasionally uses this routine.
void *memset(void *s, int c, size_t n)
{
  // In this pattern, if there is a larger size, and it is double the current one, use "if", otherwise use "while"
  char cv = c & 0xff;
  char *cp = s;
  // Next size is double, use if, not while
  if ((((size_t) cp) & (1 << 0)) != 0 && n >= sizeof( cv )) { *cp++ = cv; n-=sizeof( cv ); }

  uint16_t hv = cv; hv = hv | (hv << (8 * sizeof( cv )));
  uint16_t *hp = (void*) cp;
  // Next size is double, use if, not while
  if ((((size_t) hp) & (1 << 1)) != 0 && n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }

  uint32_t wv = hv; wv = wv | (wv << (8 * sizeof( hv )));
  uint32_t *wp = (void*) hp;
  // Next size is double, use if, not while
  if ((((size_t) wp) & (1 << 2)) != 0 && n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }

  uint64_t dv = wv; dv = dv | (dv << (8 * sizeof( wv )));
  uint64_t *dp = (void*) wp;
  // No larger size, use while, not if, and don't check the pointer bit
  while (n >= sizeof( dv )) { *dp++ = dv; n-=sizeof( dv ); }

  wp = (void *) dp; if (n >= sizeof( wv )) { *wp++ = wv; n-=sizeof( wv ); }
  hp = (void *) wp; if (n >= sizeof( hv )) { *hp++ = hv; n-=sizeof( hv ); }
  cp = (void *) hp; if (n >= sizeof( cv )) { *cp++ = cv; n-=sizeof( cv ); }

  return s;
}

#define OS_ThreadOp 0xf9
enum { Start, Exit, WaitUntilWoken, Sleep, Resume, GetHandle, LockClaim, LockRelease,
       WaitForInterrupt = 32, InterruptIsOff, NumberOfInterruptSources };
#define OS_IntOff 0x14

static inline void memory_write_barrier()
{
  asm ( "dsb sy" );
}

static inline void memory_read_barrier()
{
  asm ( "dsb sy" );
}

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

static inline void set_VF()
{
  asm ( "msr cpsr_f, #(1 << 28)" );
}

static inline void debug_string_with_length( char const *s, int length )
{
  register int code asm( "r0" ) = 48;
  register char const *string asm( "r1" ) = s;
  register int len asm( "r2" ) = length;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (code)
      , "r" (len)
      , "r" (string)
      : "lr" );
}

static inline void debug_string( char const *s )
{
  int len = 0;
  char const *p = s;
  while (*p != '\0') { p++; len++; }
  debug_string_with_length( s, len );
}

static inline void debug_number( uint32_t num )
{
  register int code asm( "r0" ) = 49;
  register uint32_t number asm( "r1" ) = num;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (code)
      , "r" (number)
      : "lr" );
}

#define WriteN( s, n ) debug_string_with_length( s, n )
#define Write0( s ) debug_string( s )
#define WriteS( s ) debug_string_with_length( s, sizeof( s ) - 1 )
#define NewLine WriteS( "\n" );
#define Space WriteS( " " );
#define WriteNum( n ) debug_number( n )


