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

#if 1


// Not used at the moment, need to re-implement GS SWIs first, the legacy code uses the legacy variables code.
static bool run_risos_code_implementing_swi( svc_registers *regs, uint32_t start )
{
  clear_VF();

  register uint32_t non_kernel_code asm( "r10" ) = start;

  asm (
      "\n  push { %[regs] }"
      "\n  ldm %[regs], { r0-r9 }"
      "\n  adr %[regs], return"
      "\n  push { %[regs] } // return address"
      "\n  mov lr, #0 // Clear all flags - this may be wrong"
      "\n  bx r10"
      "\nreturn:"
      "\n  pop { %[regs] }"
      "\n  stm %[regs], { r0-r9 }"
      "\n  ldr r1, [%[regs], %[spsr]]"
      "\n  bic r1, #0xf0000000"
      "\n  and r2, r14, #0xf0000000"
      "\n  orr r1, r1, r2"
      "\n  str r1, [%[regs], %[spsr]]"
      :
      : [regs] "r" (regs)
      , "r" (non_kernel_code)
      , [spsr] "i" (4 * (&regs->spsr - &regs->r[0]))
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9" );

  return (regs->spsr & VF) == 0;
}


bool do_OS_ReadVarVal( svc_registers *regs )
{
  // WriteS( "Reading " ); Write0( regs->r[0] ); NewLine; /// WriteS( ": \\\"" ); Write0( regs->r[1] ); WriteS( "\\\"\\n\\r" );
  bool result = run_risos_code_implementing_swi( regs, 0xfc020ab4 );
  return result;
}

bool do_OS_SetVarVal( svc_registers *regs )
{
  // WriteS( "Setting " ); Write0( regs->r[0] ); WriteS( " to \\\"" ); Write0( regs->r[1] ); WriteS( "\\\"\\n\\r" );
  bool result = run_risos_code_implementing_swi( regs, 0xfc020c58 );
  return result;
}










#else
// TODO
// Does anybody really use code variables? Yes!
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
  uint32_t store = (uint32_t) string;

  asm goto (
    "\n  ldr r0, %[store]"
    "\n  mov r3, #0"
    "\n  svc 0x25"
    "\n  bvs %l[failed]"
    "\n0:"
    "\n  svc 0x26"
    "\n  bvs %l[failed]"
    "\n  addcc r3, r3, #1"
    "\n  bcc 0b"
    "\n  str r3, %[store]"
    :
    : [store] "m" (store)
    : "r0", "r1", "r2", "r3", "memory", "cc", "lr"
    : failed );

  return store;

failed:
  return 0;
}

static uint32_t gstrans( const char *string, char *buffer, uint32_t max )
{
  register const char *r0 asm( "r0" ) = string;
  register char *r1 asm( "r1" ) = buffer;
  register uint32_t r2 asm( "r2" ) = max;
  asm ( "svc 0x27" : "=r" (r2) : "r" (r0), "r" (r1), "r" (r2) : "lr", "memory" );
  return r2;
}

static inline char upper( char c )
{
  if (c >= 'A' && c <= 'Z') c += 'A' - 'a';
  return c;
}

// Control- or space-terminated, non-null, case insensitive, but only ASCII
static int varnamecmp( const char *left, const char *right )
{
  int result = 0;

  while (result == 0 && *left > ' ' && *right > ' ') {
    result = upper( *left++ ) - upper( *right++ );
  }

  // The terminators don't have to be the same character for a match
  return (*left <= ' ' && *right <= ' ') ? 0 : result;
}

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

  if (v == 0) {
WriteS( "not found" ); NewLine;
    regs->r[0] = (uint32_t) &not_found;
    regs->r[2] = 0; // Length
    regs->r[3] = 0; // Name

    return false;
  }

  regs->r[3] = (uint32_t) varname( v );

  bool size_request = (0 != (buffer_size & (1 << 31)));

  switch (v->type) {
  case VarType_String:
    WriteS( "String: " );
    regs->r[2] = v->length - 1; // Stored with a nul terminator
    if (!size_request) {
      memcpy( buffer, varval( v ), v->length-1 );
    }
    break;
  case VarType_Number:
    WriteS( "Number" ); NewLine;
    if (regs->r[4] == 3) return Kernel_Error_UnimplementedSWI( regs );
    if (!size_request) {
      regs->r[2] = 4;
      *(uint32_t*) buffer = *(uint32_t*) varval( v );
    }
    break;
  case VarType_LiteralString:
    WriteS( "Literal string: " );
    regs->r[2] = v->length-1;
    if (!size_request) {
      memcpy( buffer, varval( v ), v->length-1 );
    }
    break;
  case VarType_Macro:
    {
    WriteS( "Macro: " );
    uint32_t length = gstrans_length( varval( v ) );
    regs->r[2] = length;
    if (!size_request) {
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
    uint32_t l = regs->r[2];
WriteS( "length " ); WriteNum( l ); NewLine;
    regs->r[0] = (uint32_t) &buffer_overflow;
    regs->r[2] = ~regs->r[2];
    return false;
  }

NewLine; Write0( regs->r[1] ); NewLine;
asm ( "svc 0xff" : : : "cc", "lr" );

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
// Answer: No.
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
        length++;
      }
    }

if (type == VarType_Number) { WriteNum( *(uint32_t*) regs->r[1] ); }
else { WriteS( "\\\"" ); Write0( regs->r[1] ); WriteS( "\\\"" ); WriteNum( length ); }
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
      v = rma_allocate( sizeof( variable ) + ((store_length + name_length +3)&~3), regs );
if (v == 0) {
  WriteS( "No space in RMA" ); for (;;) { asm ( "svc 0xff\n  wfi" : : : "cc", "lr" ); }
}
    } while (v == 0);
    v->type = type;

NewLine; WriteS( "Store length: " ); WriteNum( store_length ); NewLine;
    v->length = store_length;

    switch (type) {
    case VarType_String:
      {
        uint32_t wrote = gstrans( value, varval( v ), store_length );
        if (wrote != store_length-1) asm ( "bkpt 3" );
        varval( v )[store_length] = '\0';
WriteS( "Wrote: " ); Write0( varval( v ) ); NewLine; WriteN( varval( v ), v->length - 1 );
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
    int cmp;
    while ((*p) != 0 && (cmp = varnamecmp( varname( *p ), varname( v ) )) < 0) {
NewLine; Write0( varname( *p ) ); WriteS( " " ); WriteNum( (*p)->length-1 ); WriteS( " " ); WriteN( varval( *p ), (*p)->length - 1 );
      p = &((*p)->next);
    }
    if (0 == cmp) {
      v->next = (*p)->next;
      rma_free( *p );
    }
    else {
      v->next = *p;
    }
    *p = v;
  }

  return true;
}

#endif