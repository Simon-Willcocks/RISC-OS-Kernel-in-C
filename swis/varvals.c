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

extern void show_string( uint32_t x, uint32_t y, const char *string, uint32_t colour );

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

static uint32_t gstrans_length( const char *string )
{
  svc_registers gstrans_regs = {};
  gstrans_regs.r[0] = string;
  gstrans_regs.r[1] = 0;
  gstrans_regs.r[2] = 0;
  do_OS_GSTrans( &gstrans_regs );
  return gstrans_regs.r[2];
}

static void gstrans( const char *string, char *buffer, uint32_t max )
{
  svc_registers gstrans_regs = {};
  gstrans_regs.r[0] = string;
  gstrans_regs.r[1] = buffer;
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

// Control- or space-terminated, non-null, case insensitive, but only ASCII
static bool varnamematch( const char *wildcarded, const char *name )
{
show_string( 800, 100, wildcarded, White );
show_string( 800, 110, name, White );

  char w;
  char n;

  do {
    bool wildcard = (*wildcarded == '*');

    // Are there more wildcards than '*'?
    // Will match any characters in name, until the character (or terminator) following the *
    while (*wildcarded == '*' && *wildcarded > ' ') wildcarded++;

    if (wildcard) {
      char tofollow = upper( *wildcarded );

      // Skip to the next instance of l in name.
      // If the remainder doesn't match, look for another instance.
      while (*name > ' ') {
        while (*name > ' ' && upper( *name ) != tofollow) name++;
        if (varnamematch( wildcarded, name ))
          return true;
      }
    }

    w = upper( *wildcarded++ );
    n = upper( *name++ );
  } while (n == w && *name > ' ' && *wildcarded > ' ');

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

show_string( 100, 100, wildcarded, White );
int y = 100;

  if (regs->r[3] != 0) {
    // Skip over previously matched variable, don't assume we've been passed a real pointer.
    while (v != 0 && (uint32_t) v != regs->r[3]) {
show_string( 500, y, varname( v ), White ); y += 10;
      v = v->next;
    }
    if (v != 0)
      v = v->next;
  }

  while (v != 0 && !varnamematch( wildcarded, varname( v ) )) {
show_string( 500, y, varname( v ), White ); y += 10;
    v = v->next;
  }

  regs->r[3] = (uint32_t) v;

  if (v == 0) {
show_string( 100, 100, (char*) regs->r[0], Red );
    regs->r[2] = 0;
    regs->r[0] = (uint32_t) &not_found;
    return false;
  }

  bool size_request = (0 != (buffer_size & (1 << 31)));

show_word( 0, 100, v->type, White );
  switch (v->type) {
  case VarType_String:
    if (size_request)
      regs->r[2] = ~v->length;
    else {
      regs->r[2] = v->length;
      memcpy( buffer, (char*) (v+1), v->length );
    }
    break;
  case VarType_Number:
    if (regs->r[4] == 3) return Kernel_Error_UnimplementedSWI( regs );
    if (size_request)
      regs->r[2] = ~4;
    else {
      regs->r[2] = 4;
      *(uint32_t*) buffer = *(uint32_t*) (v+1);
    }
    break;
  case VarType_LiteralString:
    if (size_request)
      regs->r[2] = ~(v->length-1);
    else {
      regs->r[2] = v->length-1;
      memcpy( buffer, (char*) (v+1), v->length-1 );
    }
    break;
  case VarType_Macro:
    {
    uint32_t length = gstrans_length( (char*) (v+1) );
    if (size_request)
      regs->r[2] = ~length;
    else {
      regs->r[2] = length;
      gstrans( buffer, (char*) (v+1), length );
    }
    }
    break;
  case VarType_Expanded:
  case VarType_Code:
    return Kernel_Error_UnimplementedSWI( regs );
  }

  if (size_request) {
    // length check
show_word( 100, 90, regs->r[2], Blue );
    regs->r[0] = (uint32_t) &buffer_overflow;
    return false;
  }

show_string( 100, 100, (char*) regs->r[1], Green );
  return true;
}

bool do_OS_SetVarVal( svc_registers *regs )
{
show_string( 100, 120, (char*) regs->r[0], White );
show_string( 100, 130, (char*) regs->r[1], White );
  if (regs->r[2] < 0) { // Delete
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

    const char *name = (void*) regs->r[0];
    uint32_t name_length = 0;
    while (name[name_length] >= ' ') name_length++;
    name_length++; // For nul terminator

    const char *value = (void*) regs->r[1];
    uint32_t length = regs->r[2];

    enum VarTypes type = regs->r[4];

    if (length == 0 && (type == VarType_String || type == VarType_LiteralString)) {
      // This is not a documented feature, afaics, but it is used by parts of the OS
      while (value[length] != '\0' && value[length] != 10 && value[length] != 13) length++;
    }

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
        gstrans( value, (char*) (v+1), store_length );
        ((char*) (v+1))[store_length] = '\0';
      }
      break;
    case VarType_Number:
      *(uint32_t*) (v+1) = regs->r[1];
      break;
    case VarType_LiteralString:
    case VarType_Macro:
      memcpy( (char*) (v+1), value, length );
      ((char*) (v+1))[length] = '\0';
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

