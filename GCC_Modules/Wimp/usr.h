/* Copyright 2023 Simon Willcocks
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

// Files including this file contain usr32 mode module code

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

#define C_CLOBBERED "r0-r3,r12"

#define assert( c ) while (!(c)) { asm( "bkpt 65535" ); }

#include "kernel_swis.h"
#include "taskop.h"
#include "workspace.h"

static inline void debug_string_with_length( char const *s, int length )
{
  register int code asm( "r0" ) = TaskOp_DebugString;
  register char const *string asm( "r1" ) = s;
  register int len asm( "r2" ) = length;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (code)
      , "r" (len)
      , "r" (string)
      : "memory" );
}

static inline void debug_string( char const *s )
{
  debug_string_with_length( s, strlen( s ) );
}

static inline void debug_number( uint32_t num )
{
  register int code asm( "r0" ) = TaskOp_DebugNumber;
  register uint32_t number asm( "r1" ) = num;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (code)
      , "r" (number)
      : "memory" );
}

#define WriteN( s, n ) debug_string_with_length( s, n )
#define Write0( s ) debug_string( s )
#define WriteS( s ) debug_string_with_length( s, sizeof( s ) - 1 )
#define NewLine debug_string_with_length( "\n\r", 2 )
#define Space debug_string_with_length( " ", 1 )
#define WriteNum( n ) debug_number( (uint32_t) (n) )

#define number_of( arr ) (sizeof( arr ) / sizeof( arr[0] ))

typedef struct {
  uint32_t code;
  char desc[];
} error_block;

