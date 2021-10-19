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

// TODO
// Does anybody really use code variables?
// GSTrans on Set or Read of value
// Numbers, code, macros.

// This structure forms the header of the entry in the RMA; the value
// is stored immediately after it, the name follows immediately after
// that.
struct variable {
  uint32_t length:24;
  uint32_t type:8; // enum VarTypes
  variable *next;
};

static char *varname( variable *v )
{
  return (void*) (((char *)(v+1)) + v->length);
}

static char *varval( variable *v )
{
  return (char *)(v+1);
}

static uint32_t gstrans_length( const char *string )
{
  svc_registers gstrans_regs = {};
  gstrans_regs.r[0] = (uint32_t) string;
  gstrans_regs.r[1] = 0;
  gstrans_regs.r[2] = 0;
  do_OS_GSTrans( &gstrans_regs );
  return gstrans_regs.r[2];
}

static void gstrans( const char *string, char *buffer, uint32_t max )
{
  svc_registers gstrans_regs = {};
  gstrans_regs.r[0] = (uint32_t) string;
  gstrans_regs.r[1] = (uint32_t) buffer;
  gstrans_regs.r[2] = max;
  do_OS_GSTrans( &gstrans_regs );
}

static inline char upper( char c )
{
  if (c >= 'A' && c <= 'Z') c += 'A' - 'a';
  return c;
}

// Control- or space-terminated, non-null, case insensitive, but only ASCII
static int varnamecmp( const char *left, const char *right )
{
  int result;

  do {
    result = upper( *right ) - upper( *left );
  } while (result == 0 && *left > ' ' && *right > ' ');

  // The terminators don't have to be the same character for a match
  return (*left <= ' ' && *right <= ' ') ? 0 : result;
}

// Note: Something in there corrupts a register it shouldn't, need to find it!
#define WriteS( string ) asm volatile ( "svc 1\n  .string \""string"\"\n  .balign 4" )

#define WriteNum( n )  \
  uint32_t shift = 32; \
  while (shift > 0) { \
    shift -= 4; \
    static const char hex[] = "0123456789abcdef"; \
    register char c asm( "r0" ) = hex[((n) >> shift) & 0xf]; \
    asm( "svc 0" : : "r" (c) ); \
  };

// Not using OS_Write0, because many strings are not null terminated.
#define Write0( string ) { char *c = (char*) string; register uint32_t r0 asm( "r0" ); for (;*c != '\0' && *c != '\n' && *c != '\r';) { r0 = *c++; asm volatile ( "svc 0" : : "r" (r0) ); }; }

#define NewLine asm ( "svc 3" )

// Control- or space-terminated, non-null, case insensitive, but only ASCII
static bool varnamematch( const char *wildcarded, const char *name )
{
  char w;
  char n;
  bool match;

  do {
    // Wildcards are: * (to match any number of charaters) and # (to match a single character?) FIXME
    // Will match any characters in name, until the character (or terminator) following the *

    if (*wildcarded == '*') {
      // Anything after the wildcard?
      while (*wildcarded == '*') { wildcarded++; }

      if (*wildcarded > ' ') {
        char tofollow = upper( *wildcarded );

        // Skip to the next instance of tofollow in name.
        // If the remainder doesn't match, look for another instance.
        while (*name > ' ') {
          while (*name > ' ' && (tofollow != '#' && upper( *name ) != tofollow)) name++;
          if (varnamematch( wildcarded, name ))
            return true;
        }
      }
      else {
        // Matches to the end of name
        while (*name > ' ') name++;
      }
    }

    w = upper( *wildcarded++ );
    n = upper( *name++ );

    bool wildcard_match = ((w == '#') && (n >= ' ')); // Wildcard #
    match = wildcard_match || (n == w);
  } while (match && *name > ' ' && *wildcarded > ' ');

  return (n == w || (*wildcarded <= ' ' && *name <= ' '));
}

bool do_OS_ReadVarVal( svc_registers *regs )
{
  static error_block buffer_overflow = { 0x1e4, "Buffer overflow" };
  static error_block not_found = { 0x124, "System variable not found" }; // FIXME

  char *wildcarded = (void*) regs->r[0];
  variable *v = workspace.kernel.variables;
  char *buffer = (void*) regs->r[1];
  uint32_t buffer_size = regs->r[2];

  WriteNum( regs->r[0] );

  NewLine;
  Write0( wildcarded );
  WriteS( " " );

  if (regs->r[3] != 0) {
    // Skip over previously matched variable, don't assume we've been passed a real pointer.
    while (v != 0 && (uint32_t) varname( v ) != regs->r[3]) {
      v = v->next;
    }
    if (v != 0)
      v = v->next;
  }

  while (v != 0 && !varnamematch( wildcarded, varname( v ) )) {
    v = v->next;
  }

  regs->r[3] = (uint32_t) varname( v );

  if (v == 0) {
WriteS( " not found" ); NewLine;
    regs->r[2] = 0;
    regs->r[0] = (uint32_t) &not_found;
    return false;
  }

  bool size_request = (0 != (buffer_size & (1 << 31)));

  switch (v->type) {
  case VarType_String:
    WriteS( "String: " );
    if (size_request)
      regs->r[2] = ~v->length;
    else {
      regs->r[2] = v->length;
      memcpy( buffer, varval( v ), v->length );
    }
    break;
  case VarType_Number:
    WriteS( "Number" ); NewLine;
    if (regs->r[4] == 3) return Kernel_Error_UnimplementedSWI( regs );
    if (size_request)
      regs->r[2] = ~4;
    else {
      regs->r[2] = 4;
      *(uint32_t*) buffer = *(uint32_t*) varval( v );
    }
    break;
  case VarType_LiteralString:
    WriteS( "Literal string: " );
    if (size_request)
      regs->r[2] = ~(v->length-1);
    else {
      regs->r[2] = v->length-1;
      memcpy( buffer, varval( v ), v->length-1 );
    }
    break;
  case VarType_Macro:
    {
    WriteS( "Macro: " );
    uint32_t length = gstrans_length( varval( v ) );
    if (size_request)
      regs->r[2] = ~length;
    else {
      regs->r[2] = length;
      gstrans( buffer, varval( v ), length );
    }
    }
    break;
  case VarType_Expanded:
  case VarType_Code:
    return Kernel_Error_UnimplementedSWI( regs );
  }

  if (size_request) {
    // length check
WriteS( "length " ); WriteNum( regs->r[2] ); NewLine;
    regs->r[0] = (uint32_t) &buffer_overflow;
    return false;
  }

NewLine; Write0( regs->r[1] ); NewLine;
asm ( "svc 0xff" );

  return true;
}

bool do_OS_SetVarVal( svc_registers *regs )
{
  if (regs->r[2] < 0) { // Delete
WriteS( "Deleting " ); Write0( regs->r[0] ); NewLine;

    // Alphabetical order

    variable **p = &workspace.kernel.variables;
    int cmp = 0;
    while (cmp <= 0 && (*p) != 0) {
      cmp = varnamecmp( varname( *p ), (const char *) regs->r[0] );
      if (cmp == 0) {
        // Matched, delete it.
        variable *v = *p;
        *p = v->next; // Removed from list
        rma_free( v );
        return true;
      }
      p = &((*p)->next);
    }
    return true; // Should be an error, perhaps?
  }
  else {
// Question: Should the variables be shared among the cores, or not?
// If so, we need to lock them. Also if we go multi-threading
// But beware of GSTrans needing to read strings to expand values being
// inserted or removed. I expect reading the old value of the variable
// being read is quite common, like PATH=$PATH:/newpath in Unix.

/*
svc_registers before;
svc_registers after;

register svc_registers *lr asm( "lr" ) = &before;
asm ( "stm lr, { r0-r12 }" : : "r" (lr) );
*/
WriteS( "Setting " ); Write0( regs->r[0] ); WriteS( " to " ); 

    const char *name = (void*) regs->r[0];
    uint32_t name_length = 0;

    while (name[name_length] >= ' ') name_length++;
    name_length++; // For nul terminator

    const char *value = (void*) regs->r[1];
    uint32_t length = regs->r[2];

    enum VarTypes type = regs->r[4];

    if (length == 0 && (type == VarType_String || type == VarType_LiteralString)) {
WriteS( "*" ); // Indicate unknown length, undocumented feature
      // This is not a documented feature, afaics, but it is used by parts of the OS
      while (value[length] != '\0' && value[length] != 10 && value[length] != 13) {
        register uint32_t r0 asm ( "r0" ) = value[length];
        asm ( "svc 0" : : "r" (r0) : "lr" );
        length++;
      }
    }

if (type == VarType_Number) { WriteS( "a number!" ); }
else { WriteS( "\\\"" ); Write0( regs->r[1] ); WriteS( "\\\"" ); }
NewLine; 

    uint32_t store_length = length;

    if (type == VarType_String) {
      store_length = gstrans_length( value ) + 1;
    }
    else if (type == VarType_Number) {
      store_length = 4;
    }
    else {
      store_length ++; // Store a terminator, so that the value can easily be passed to gstrans or strcpy
    }

    variable *v;
    do {
      v = rma_allocate( sizeof( variable ) + ((length + name_length +3)&~3), regs );
    } while (v == 0);
    v->type = type;
    v->length = length;

    switch (type) {
    case VarType_String:
      {
        gstrans( value, varval( v ), store_length );
        varval( v )[store_length] = '\0';
      }
      break;
    case VarType_Number:
      *(uint32_t*) varval( v ) = regs->r[1];
      break;
    case VarType_LiteralString:
    case VarType_Macro:
      memcpy( varval( v ), value, length );
      varval( v )[length] = '\0';
      break;
    case VarType_Expanded:
    case VarType_Code:
      return Kernel_Error_UnimplementedSWI( regs );
      break;
    }

    memcpy( varname( v ), name, name_length );
    varname( v )[name_length] = '\0';

    variable **p = &workspace.kernel.variables;

    // Alphabetical order
    while ((*p) != 0 && varnamecmp( varname( *p ), varname( v ) ) < 0) {
      p = &((*p)->next);
    }
    v->next = *p;
    *p = v;
  }

  return true;
}

