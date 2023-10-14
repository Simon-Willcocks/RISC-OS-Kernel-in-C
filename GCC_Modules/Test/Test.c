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

const unsigned module_flags = 1;
// Bit 0: 32-bit compatible

// Explicitly no SWIs provided (it's the default, anyway)
#define MODULE_CHUNK "0"

#include "module.h"

NO_start;
NO_init;
NO_finalise;
NO_service_call;
//NO_title;
NO_help;
//NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char __attribute__(( section( ".text" ) )) title[] = "TestModule";

// Don't use C Kernel debug interface, just write to output
#undef WriteS
#undef WriteN
#undef Write0
#undef WriteNum
#undef NewLine

static inline void WriteC( char out )
{
  register uint32_t c asm( "r0" ) = out;
  asm ( "svc %[swi]" : : [swi] "i" (Xbit | OS_WriteC), "r" (c) : "lr" );
}

static inline void Write0( char *out )
{
  register char *c asm( "r0" ) = out;
  asm ( "svc %[swi]" : : [swi] "i" (Xbit | OS_Write0), "r" (c) : "lr" );
}

static inline void NewLine()
{
  asm ( "svc %[swi]" : : [swi] "i" (Xbit | OS_NewLine) : "lr" );
}

void c_test_command( char const *tail, uint32_t count )
{
  Write0( "Number of parameters: " ); WriteC( '0' + count ); NewLine();
  char const *p = tail;
  while (count > 0) {
    char c = *p;
    bool quoted = (c == '"');

    do {
      WriteC( c );
      c = *++p;
    } while ((!quoted && c > ' ') || (quoted && c >= ' ' && c != '"'));

    if (quoted) {
      WriteC( c );
      p++;
      if (c != '"') {
        Write0( "Oops? " ); if (c > ' ') WriteC( c ); NewLine();
      }
    }

    NewLine();
    count--;
    if (count > 0) {
      while (' ' == *p) { p++; }
    }
  }
  char const *q = tail;
  while (q < p) {
    char c = *q++;
    WriteC( c );
  }
  NewLine();
}

void __attribute__(( naked )) test_command()
{
  asm ( "push { "C_CLOBBERED", lr }" );

  register char const *tail asm( "r0" );
  register uint32_t count asm( "r1" );
  c_test_command( tail, count );

  asm ( "pop { "C_CLOBBERED", pc }" );
}

void __attribute__(( naked, section( ".text.init" ) )) in_text_init_section()
{
// If this assembler is at the top level, it gets placed before file_start,
// screwing up the module header.
  asm ( 
   "\nkeywords:"
   "\n  .asciz \"TestCommand\""
   "\n  .align"
   "\n  .word test_command - header"
   "\n  .word 0x00ff0200"
   "\n  .word 0"
   "\n  .word 0"
  
   "\n  .asciz \"TestCommand2\""
   "\n  .align"
   "\n  .word test_command - header"
   "\n  .word 0x00ff0000"
   "\n  .word 0"
   "\n  .word 0"
  
   // End of list
   "\n  .word 0" );
}

