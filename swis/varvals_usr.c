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

#include "varvals.h"
#include "swis.h"

struct globals *global = (void *) stack_top;

#define assert( x ) while (!(x)) asm( "bkpt %[line]" : : [line] "i" (__LINE__) )

// Notes about GSTrans and family
// The final GSRead, which returns with C set, returns a copy
// of the terminator (0, 10, 13). GSTrans includes the terminator
// in the buffer, but returns the length of the string before it.

static error_block const no_such_var = { 0x124, "System variable not found" };
static error_block const bad_var_type = { 0x122, "Bad variable type" };
static error_block const no_type_match = { 0x122, "Invalid attempt to delete code variable" };
static error_block const bad_string = { 0xfd, "String not recognised" };

static inline void initialise_heap( void* heap_base, uint32_t heap_size )
{
  register uint32_t cmd asm( "r0" ) = 0;
  register void *base asm( "r1" ) = heap_base;
  register uint32_t size asm( "r3" ) = heap_size;
  register error_block *error asm( "r0" );

  asm volatile ( "svc %[swi]"
    : "=r" (error)
    : [swi] "i" (Xbit | OS_Heap)
    , "r" (cmd)
    , "r" (base)
    , "r" (size)
    : "lr", "cc", "memory" );
}

static inline void *heap_allocate( uint32_t bytes )
{
  register uint32_t cmd asm( "r0" ) = 2; // Get Heap Block
  register uint32_t base asm( "r1" ) = heap;
  register uint32_t size asm( "r3" ) = bytes;
  register void *allocation asm( "r2" );
  register error_block *error asm( "r0" );

  asm volatile ( "svc %[swi]"
    : "=r" (error)
    , "=r" (allocation)
    : [swi] "i" (Xbit | OS_Heap)
    , "r" (cmd)
    , "r" (base)
    , "r" (size)
    : "lr", "cc" );

  return allocation;
}

static inline error_block *heap_free( void *block )
{
  register uint32_t cmd asm( "r0" ) = 3; // Free Heap Block
  register uint32_t base asm( "r1" ) = heap;
  register void *mem asm( "r2" ) = block;
  register error_block *error asm( "r0" );

  asm volatile ( "svc %[swi]"
             "\n  movvc r0, #0"
    : "=r" (error)
    : [swi] "i" (Xbit | OS_Heap)
    , "r" (cmd)
    , "r" (base)
    , "r" (mem)
    : "lr", "cc" );

  return error;
}

// FIXME Move these debug functions to a header file
static inline void debug_string_with_length( char const *s, int length )
{
  register char const *string asm( "r0" ) = s;
  register int len asm( "r1" ) = length;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_DebugString)
      , "r" (string)
      , "r" (len)
      : "lr", "memory" );
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
  register uint32_t number asm( "r0" ) = num;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OSTask_DebugNumber)
      , "r" (number)
      : "lr", "memory" );
}

#define WriteN( s, n ) debug_string_with_length( s, n )
#define Write0( s ) debug_string( s )
#define WriteS( s ) debug_string_with_length( s, sizeof( s ) - 1 )
#define NewLine WriteS( "\n" );
#define Space WriteS( " " );
#define WriteNum( n ) debug_number( n )

#define OP( c ) (0x3f & (c))

// In the client Task's slot, count the length of the name and
// create a pipe over it and fill the pipe with it. 
// Any character <= " " terminates.
// Do not use the stack. May use all registers.
// In:
// r0 -> name
// r1 = 0
// r4 -> result pointer (in RMA, shared between slots)
// r5 = caller (to resume when finished)
// Out: ignored, new pipe stored in result

static void __attribute__(( naked )) make_terminated_string_pipe()
{
  asm volatile (
        "0:"
    "\n  ldrb r3, [r0, r1]"
    "\n  cmp  r3, #32"
    "\n  addgt r1, r1, #1"
    "\n  bgt 0b"
    "\n  mov r2, r1"    // max block size = max data
    "\n  mov r3, r0"    // name
    "\n  svc %[create]" // r0 out = handle
    "\n  svcvc %[wait]"
    "\n  svcvc %[filled]"
    "\n  svcvc %[forget]"
    "\n  strvc r0, [r4]"
    "\n  mov r1, r5"
    "\n  svc %[set_receiver]"       // The caller may read from the pipe
    "\n  mov r0, r5"
    "\n  svc %[finished]"
    :
    : [finished] "i" (OSTask_RelinquishControl)
    , [create] "i" (Xbit | OSTask_PipeCreate)
    , [wait] "i" (Xbit | OSTask_PipeWaitForSpace)
    , [filled] "i" (Xbit | OSTask_PipeSpaceFilled)
    , [forget] "i" (Xbit | OSTask_PipeNoMoreData)
    , [set_receiver] "i" (Xbit | OSTask_PipeSetReceiver)
    : "lr", "cc"
    );
}

// In the client Task's slot, create a pipe over the data
// and fill the pipe with it.
// Do not use the stack. May use all registers.
// In:
// r1 = size
// r2 = size
// r3 -> data (possibly in app space)
// r4 -> result pointer (in RMA, shared between slots)
// r5 = caller (to resume when finished)
// Out: ignored, new pipe stored in result

static void __attribute__(( naked )) make_data_pipe()
{
  asm volatile (
    "\n  svc %[create]" // r0 out = handle
    "\n  svcvc %[wait]"
    "\n  svcvc %[filled]"
    "\n  svcvc %[forget]"
    "\n  movvc r1, r5"
    "\n  svcvc %[set_receiver]"     // The caller may read from the pipe
    "\n  strvc r0, [r4]"
    "\n  mov r0, r5"
    "\n  svc %[finished]"
    :
    : [finished] "i" (OSTask_RelinquishControl)
    , [create] "i" (Xbit | OSTask_PipeCreate)
    , [wait] "i" (Xbit | OSTask_PipeWaitForSpace)
    , [filled] "i" (Xbit | OSTask_PipeSpaceFilled)
    , [set_receiver] "i" (Xbit | OSTask_PipeSetReceiver)
    , [forget] "i" (Xbit | OSTask_PipeNoMoreData)
    : "lr", "cc"
    );
}

// In the client Task's slot, create a pipe over the buffer
// and allow the caller to fill it.
// Do not use the stack. May use all registers.
// In:
// r1 = size
// r2 = size
// r3 -> buffer (possibly in app space)
// r4 -> result pointer (in RMA, shared between slots)
// r5 = caller (to resume when finished)
// Out: ignored, new pipe stored in result

static void __attribute__(( naked )) make_buffer_pipe()
{
  asm volatile (
    "\n  svc %[create]"         // r0 out = handle
    "\n  strvc r0, [r4]"
    "\n  mov r1, r5"
    "\n  svc %[set_sender]"      // The caller may write to the pipe
    "\n  mov r0, r5"
    "\n  svc %[finished]"
    :
    : [finished] "i" (OSTask_RelinquishControl)
    , [set_sender] "i" (Xbit | OSTask_PipeSetSender)
    , [create] "i" (Xbit | OSTask_PipeCreate)
    : "lr", "cc"
    );
}

// In the client Task's slot, read to completion and close
// a buffer pipe.
// Do not use the stack. May use all registers.
// In:
// r0 = pipe
// r5 = caller (to resume when finished)
// Out: nothing. The data is safely where the client
// wanted it.

// Thought: could be modified to return directly after
// SWI? If so, could be used to "fire and forget", wait
// for the data, continue with task, without bothering
// the provider? Only have LR to play with, though.

static void __attribute__(( naked )) close_buffer_pipe_routine()
{
  asm volatile (
    "\n  svc %[wait]"           // Returns when data safe
    "\n  svc %[forget]"         // Release the pipe resources
    "\n  mov r0, r5"
    "\n  svc %[finished]"
    :
    : [finished] "i" (OSTask_RelinquishControl)
    , [wait] "i" (Xbit | OSTask_PipeWaitForData)
    , [forget] "i" (Xbit | OSTask_PipeNotListening)
    : "lr", "cc"
    );
}

// In:
// r0 -> name
// r1 = 0
// r2 = 0 or ' ', if space may be the terminator
// r4 -> result pointer (in RMA, shared between slots)
// r5 = caller (to resume when finished)
// Out: ignored, new pipe stored in result, pipe data includes the terminating character
static void __attribute__(( naked )) make_gs_string_pipe()
{
  asm volatile (
        "0:"
    "\n  ldrb r3, [r0, r1]"
    "\n  add r1, r1, #1"        // Whether it's the terminator, or not
    "\n  cmp   r3, #'\r'"
    "\n  cmpne r3, #'\n'"
    "\n  cmpne r3, #0"
    "\n  cmpne r3, r2"
    "\n  bne 0b"
    "\n  mov r2, r1"    // max block size = max data
    "\n  mov r3, r0"    // name
    "\n  svc %[create]" // r0 out = handle
    "\n  svcvc %[wait]"
    "\n  svcvc %[filled]"
    "\n  svcvc %[forget]"
    "\n  strvc r0, [r4]"
    "\n  mov r1, r5"
    "\n  svc %[set_receiver]"       // The caller may read from the pipe
    "\n  mov r0, r5"
    "\n  svc %[finished]"
    :
    : [finished] "i" (OSTask_RelinquishControl)
    , [create] "i" (Xbit | OSTask_PipeCreate)
    , [wait] "i" (Xbit | OSTask_PipeWaitForSpace)
    , [filled] "i" (Xbit | OSTask_PipeSpaceFilled)
    , [forget] "i" (Xbit | OSTask_PipeNoMoreData)
    , [set_receiver] "i" (Xbit | OSTask_PipeSetReceiver)
    : "lr", "cc"
    );
}

struct variable {
  variable *next;
  uint32_t name_length:8;
  uint32_t value_length:19;
  uint32_t type:5;
  void *value;
  char name[];
};

static inline char uppercase( char c )
{
  // Temporary, catch wildcard use until it's implemented
  assert( c != '*' && c != '#' );

  switch (c) {
  case 'a' ... 'z': return (c - 'a') + 'A';
  default: return c;
  }
}

static inline int char_cmp( char l, char r )
{
  return uppercase( l ) - uppercase( r );
}

static inline int name_cmp( variable *var, char const *name, uint16_t len )
{
  char const *end; // Points to character after last comparable
  if (len > var->name_length)
    end = &var->name[var->name_length];
  else
    end = &var->name[len];

  char const *l = var->name;
  char const *r = name;
  while (l < end) {
    int cmp = char_cmp( *l, *r );
    if (cmp != 0) return cmp;
    l++; r++;
  }
  return var->name_length - len;
}

// Notes TODO:
// Wildcards
//   ReadVarVal points R3 at the name of the matched variable,
//              this has to be in read-only memory accessible
//              to the slot. (PRM1-317)
//              This only seems to be necessary if the name is
//              wildcarded. The memory should be owned by the
//              slot and discarded on exit (or on the last read).

static inline variable *get_variable_for_writing( char const *name, uint16_t len )
{
  variable **p = &global->head;
  int cmp = -1;
  while (*p != 0 && (cmp = name_cmp( *p, name, len )) < 0) {
    p = &(*p)->next;
  }
  // p points to the pointer that should point to the variable.
  if (cmp == 0) {
    return *p; // Already exists
  }

  variable *var = heap_allocate( sizeof( variable ) + len );
  var->value = 0;
  var->value_length = 0;
  var->type = VarType_None;
  var->name_length = len;
  for (int i = 0; i < len; i++) var->name[i] = name[i];

  return var;
}

static inline error_block const *DeleteVariable( char const *name, uint16_t len, uint32_t type )
{
  variable **p = &global->head;
  int cmp = -1;
  while (*p != 0 && (cmp = name_cmp( *p, name, len )) < 0) {
    p = &(*p)->next;
  }
  // p points to the pointer that should point to the variable.
  variable *to_be_deleted = *p;
  if (cmp == 0) {
    if (to_be_deleted->type == 16 && type != 16) {
      return &no_type_match;
    }
    assert( to_be_deleted != 0 );
    *p = to_be_deleted->next;
    to_be_deleted->name_length = 0; // In case it was context for someone
    if (to_be_deleted->value != 0) heap_free( to_be_deleted->value );
    heap_free( to_be_deleted );
  }
  else {
    return &no_such_var;
  }

  return 0;
}

static inline error_block const *SetVarVal( variable *var, char const *start, uint32_t len, uint32_t type )
{
  error_block const *result = 0;

  if (var->value != 0) {
    heap_free( var->value );
    var->value = 0;
    var->value_length = 0;
  }

  switch (type) {
  case VarType_Number:
  case VarType_Macro:
  case VarType_Expanded:
    asm ( "bkpt 1" );
  case VarType_String: // FIXME scan the string into a memory block first!!
  case VarType_LiteralString:
  case VarType_Code:
    // Simplest implementation: a direct copy
    var->value_length = len;
    // Workaround for possible OS_Heap 2 bug - claims heap full allocating zero bytes?
    if (len > 0) {
      var->value = heap_allocate( len );
      memcpy( var->value, start, len );
    }
    else
      var->value = 0;
    var->type = type;
    break;
  default:
    {
    asm ( "bkpt 2" );
      return &bad_var_type;
    }
  }

  return result;
}

static inline variable const *find_existing_variable( char const *name, uint16_t len )
{
  variable *p = global->head;
  int cmp;
  while (p != 0 && (cmp = name_cmp( p, name, len )) < 0) {
    p = p->next;
  }
  // p points to the pointer that should point to the variable.
  if (cmp == 0) {
    return p; // Already exists
  }
  else {
    return 0;
  }
}

static int32_t scan_string( uint32_t flags );

error_block const *ReadVarVal( svc_registers *regs, variable const *var, uint32_t buffer_pipe )
{
  error_block const *error = 0;

  global->buffer = PipeOp_WaitForSpace( buffer_pipe, 0 );
  assert( global->buffer.available == regs->r[2] );

  uint32_t length_filled = 0;

  switch (var->type) {
  case VarType_String:
    {
      // Simplest case, no GSTrans on reading
      uint32_t len = var->value_length;
      if (len > global->buffer.available) len = global->buffer.available;
      char const *s = var->value;
      char *d = global->buffer.location;
      for (int i = 0; i < len; i++) { *d++ = *s++; }
      length_filled = len;
    }
    break;
  case VarType_Number:
    {
      char const *s = (void*) &var->value;
      char *d = global->buffer.location;
      for (int i = 0; i < 4; i++) { *d++ = *s++; }
      
      length_filled = 4;
    }
    break;
  case VarType_Macro:
    {
      global->string.location = var->value;
      global->string.available = var->value_length;
      int32_t out = scan_string( 0 );
      if (out < 0) error = &bad_string;
      else {
        length_filled = out;
      }
    }
    break;
  case VarType_Code:
    asm ( "bkpt 1" );
    break;
  default:
    {
      assert( false );
      error = &bad_var_type;
    }
  }

  regs->r[2] = length_filled;
  PipeOp_SpaceFilled( buffer_pipe, length_filled );
  PipeOp_NoMoreData( buffer_pipe );

  // TODO close client end of pipe...
  return error;
}

static error_block const *ReadVarLen( svc_registers *regs, variable const *var )
{
  assert( regs->r[4] != 3 ); // What then?
  regs->r[2] = ~var->value_length;
  return 0;
}

// Do the GSTrans bit, returning the remaining space in the buffer (-1 on error)

static int digit_in_base( char d, int base )
{
  if (base == 0) { // No base yet set, base is always base 10
    base = 10;
  }

  switch (d) {
  case '0' ... '9': return (d < ('0' + base) ? (d - '0') : -1);
  case 'A' ... 'Z': return (d < ('A' + base - 10) ? (d - 'A' + 10) : -1);
  case 'a' ... 'z': return (d < ('a' + base - 10) ? (d - 'a' + 10) : -1);
  }

  return -1;
}

static int32_t expand_variable( variable const *var, char *out, uint32_t remaining )
{
#ifdef DEBUG__SHOW_NEW_GSTRANS
  WriteS( "Expanding variable " ); WriteN( var->name, var->name_length ); NewLine;
#endif

  return var->value_length; // FIXME
}

static int32_t scan_string( uint32_t flags )
{
#ifdef DEBUG__SHOW_NEW_GSTRANS
  WriteS( "Scanning \"" ); WriteN( global->string.location, global->string.available ); WriteS( "\"" ); NewLine;
#endif
  bool const copy_quotes = 0 != (flags & 2);
  bool const ignore_control_codes = 0 != (flags & 1);

  char const *in = global->string.location;
  char const *end = in + global->string.available;

  char *out = global->buffer.location;
  uint32_t remaining = global->buffer.available;

  bool set_top_bit = false;

  while (' ' == *in && in < end) { in++; }

  bool quoted_string = false;

  // Don't copy OUTER quotes unless bit 31 is set.
      // If not copying quotes, and the first non-space in the input was a
      // quote, a second teminates the process. Otherwise, quotes stay in
      // the output.

      // e.g. '   "abc"def'             -> 'abc'
      //      'abc "def" ghi'           -> 'abc "def" ghi'
      //      '   "abc "def" ghi"'      -> 'abc ' (with trailing space

  if (in < end && '"' == *in && !copy_quotes) {
    quoted_string = true;
    flags = flags & ~(1 << 29);
    in++;
  }

  while (remaining > 0
      && in < end
      && (copy_quotes || !quoted_string || *in != '"')) {
    char c = *in++;

    if (c == '|' && !ignore_control_codes) {
      char next = *in++;

      if (in >= end) {
        return -1;
      }

      switch (next) {
      case '@': c = '\0'; break;
      case 'A' ... 'Z': c = next - 'A' + 1; break;
      case 'a' ... 'z': c = next - 'a' + 1; break;
      case '[':
      case '{': c = 27; break;
      case '\\': c = 28; break;
      case ']':
      case '}': c = 29; break;
      case '^':
      case '~': c = 30; break;
      case '_':
      case '\'': c = 31; break; // Is this correct? "grave accent"
      case '"': c = '"'; break;
      case '|': c = '|'; break;
      case '<': c = '<'; break;
      case '?': c = '\x7f'; break;
      case '!': set_top_bit = true; continue; // No single character to append
      default:
        {
          return -1;
        }
      }
    }
    else if (c == '<') {
      if (set_top_bit) {
        return -1; // TODO I don't know if this is correct, |!<16_33> could be 163?
      }

      bool is_number = true; // As far as we know so far
      int i = 0;
      bool base = 0; // 0 is default, base 10, unless there's an underscore
      uint32_t number = 0;
      if (in[i] == '&') {
        base = 16;
        i++;
      }

      while (&in[i] < end && in[i] != '>' && in[i] > ' ') {
        if (in[i] == '_' && base == 0 && is_number && number > 1 && number <= 36) {
          base = number;
          number = 0;
        }
        else {
          int d = digit_in_base( in[i], base );
          is_number = is_number && (d >= 0);
          if (is_number) {
            // FIXME check for overflow?
            number = number * (base == 0 ? 10 : base) + d;
          }
        }
        i++;
      }

      if (in[i] == '>') { // Valid syntax
        if (is_number) {
          c = number & 0xff;
        }
        else {
          variable const *var = find_existing_variable( in, i );
          if (var != 0) {
            uint32_t written = expand_variable( var, out, remaining );
            out += written;
            remaining -= written;
          }
        }
        in += i + 1;
        continue;
      }
      else {
        // Just copy the characters
        if (i > remaining) i = remaining;
        memcpy( out, in, i );
        out += i;
        in += i;
        remaining -= i;
        continue;
      }
    }

    if (set_top_bit) {
      set_top_bit = false;
      c = c | 0x80;
    }

    *out++ = c;
    remaining--;
  }

  if ((remaining == 0 || in == end)
      && (!copy_quotes && quoted_string)) {
    return -1;
  }

  if (set_top_bit) {
    return -1;
  }

  // Copy terminator
  if (remaining > 0)
    *out = *in;

#ifdef DEBUG__SHOW_NEW_GSTRANS
  WriteS( "Output: \"" ); WriteN( global->buffer.location, out - (char*) global->buffer.location ); WriteS( "\"" ); NewLine;
#endif
  return global->buffer.available - remaining;
}

//void CLI( uint32_t task_handle )

void EvaluateExpression( uint32_t task_handle )
{
}

void GSInit( uint32_t task_handle )
{
}

void SubstituteArgs( uint32_t task_handle )
{
}

void SubstituteArgs32( uint32_t task_handle )
{
}

static inline uint32_t get_varname_pipe( uint32_t client, uint32_t caller, uint32_t *result, uint32_t name )
{
  svc_registers temp;
  temp.r[0] = name;
  temp.r[1] = 0;
  temp.r[4] = (uint32_t) result;
  temp.r[5] = caller;
  temp.lr = (uint32_t) make_terminated_string_pipe;
  temp.spsr = 0x10;
  error_block *error = Task_RunThisForMe( client, &temp );
  assert( error == 0 );
  return *result;
}

static inline uint32_t get_data_pipe( uint32_t client, uint32_t caller, uint32_t *result, uint32_t value, uint32_t len )
{
  svc_registers temp;
  temp.r[1] = len;
  temp.r[2] = len;
  temp.r[3] = value;
  temp.r[4] = (uint32_t) result;
  temp.r[5] = caller;
  temp.lr = (uint32_t) make_data_pipe;
  temp.spsr = 0x10;
  error_block *error = Task_RunThisForMe( client, &temp );
  assert( error == 0 );
  return *result;
}

static inline uint32_t get_buffer_pipe( uint32_t client, uint32_t caller, uint32_t *result, uint32_t value, uint32_t len )
{
  svc_registers temp;
  temp.r[1] = len;
  temp.r[2] = len;
  temp.r[3] = value;
  temp.r[4] = (uint32_t) result;
  temp.r[5] = caller;
  temp.lr = (uint32_t) make_buffer_pipe;
  temp.spsr = 0x10;
  error_block *error = Task_RunThisForMe( client, &temp );
  assert( error == 0 );
  return *result;
}

static inline void close_buffer_pipe( uint32_t client, uint32_t caller, uint32_t pipe )
{
  svc_registers temp;
  temp.r[0] = pipe;
  temp.r[1] = 0;        // I know I've already sent the last of the data.
  temp.r[5] = caller;
  temp.lr = (uint32_t) close_buffer_pipe_routine;
  temp.spsr = 0x10;
  error_block *error = Task_RunThisForMe( client, &temp );
  assert( error == 0 );
}

static inline uint32_t get_gs_string_pipe( uint32_t client, uint32_t caller, uint32_t *result, uint32_t string, bool space_terminated )
{
  svc_registers temp;
  temp.r[0] = string;
  temp.r[1] = 0;
  temp.r[2] = space_terminated ? 0 : ' ';
  temp.r[4] = (uint32_t) result;
  temp.r[5] = caller;
  temp.lr = (uint32_t) make_gs_string_pipe;
  temp.spsr = 0x10;
  error_block *error = Task_RunThisForMe( client, &temp );
  assert( error == 0 );
  return *result;
}

uint32_t read_top()
{
  register uint32_t memory_top asm( "r0" ) = 0;
  asm volatile ( "svc %[swi]" : "=r" (memory_top) : [swi] "i" (OSTask_AppMemoryTop), "r" (memory_top) );
  return memory_top;
}

void c_environment_vars_task( uint32_t handle, uint32_t queue )
{
  initialise_heap( heap, read_top() - heap );
  global->head = 0;
  uint32_t *result = rma_allocate( sizeof( uint32_t ) );

  for (;;) {
    queued_task task = Task_QueueWait( queue );

    uint32_t client = task.task_handle;

    svc_registers regs;
    Task_GetRegisters( client, &regs );
    uint32_t name_pipe = 0;
    PipeSpace name;
    error_block const *error = 0;

    switch (task.swi) {
    case OP( OS_ReadVarVal ):
    case OP( OS_SetVarVal ):
      name_pipe = get_varname_pipe( client, handle, result, regs.r[0] );
      name = PipeOp_WaitForData( name_pipe, 0 );
      assert( name.available > 0 );
      WriteN( name.location, name.available ); NewLine;
      break;
    }

    switch (task.swi) {
    case OP( OS_ReadVarVal ):
      {
        variable const *var = find_existing_variable( name.location, name.available );
        if (var == 0) {
          error = &no_such_var;
          regs.r[2] = 0;
        }
        else if (0 == ((1 << 31) & regs.r[2])) {
          error = ReadVarLen( &regs, var );
        }
        else {
          uint32_t buffer_pipe = get_buffer_pipe( client, handle, result, regs.r[1], regs.r[2] );
          if (regs.r[4] == 3) {
            global->buffer = PipeOp_WaitForSpace( buffer_pipe, 0 );
            int32_t len = expand_variable( var, global->buffer.location, global->buffer.available );
            if (len < 0) {
              regs.r[2] = 0;
              error = &bad_string;
            }
            regs.r[2] = len;
          }
          else {
            error = ReadVarVal( &regs, var, buffer_pipe );
          }
        }
      }
      break;
    case OP( OS_SetVarVal ):
      {
        int32_t length = (int32_t) regs.r[2];
        if (length < 0) {
          error = DeleteVariable( name.location, name.available, regs.r[4] );
        }
        else {
          variable *var = get_variable_for_writing( name.location, name.available );
          uint32_t data_pipe = get_data_pipe( client, handle, result, regs.r[1], regs.r[2] );
          PipeSpace data = PipeOp_WaitForData( data_pipe, 0 );
          assert( data.available == regs.r[2] );
          error = SetVarVal( var, data.location, data.available, regs.r[4] );
        }
      }
      break;
//    case OP( OS_CLI ): CLI( client ); break;
    case OP( OS_EvaluateExpression ): EvaluateExpression( client ); asm ( "bkpt 8" ); break;

    case OP( OS_GSInit ):
    case OP( OS_GSTrans ):
      {
        uint32_t string_pipe = get_gs_string_pipe( client, handle, result, regs.r[0], 0 != (regs.r[2] & (1 << 29)) );
        global->string = PipeOp_WaitForData( string_pipe, 0 );
        regs.r[0] += global->string.available + 1; // point to character after terminator

        uint32_t buffer_pipe = 0;
        bool gs_trans = OP( OS_GSTrans ) == task.swi;
        if (gs_trans) {
          buffer_pipe = get_buffer_pipe( client, handle, result, regs.r[1], regs.r[2] & ~0xe0000000 );
        }
        else {
          buffer_pipe = PipeOp_CreateForTransfer( 4096 );
          PipeOp_SetReceiver( buffer_pipe, client );
        }

        global->buffer = PipeOp_WaitForSpace( buffer_pipe, 0 );

        // Three possible results:
        // 1. It all works, everything fits.
        // 2. The string is invalid
        // 3. The translated output would overflow the buffer
        int32_t out = scan_string( regs.r[2] >> 30 );

        // PRM 1-468 says r1 can be set to zero, but I can't see when that would happen.

        if (out < 0) {
          if (!gs_trans) {
            // Error gets reported by GSRead, not GSInit
            regs.r[0] = (uint32_t) &bad_string;
            regs.r[2] = 0xffffffff;
          }
          else {
            error = &bad_string;
          }
        }
        else {
          PipeOp_SpaceFilled( buffer_pipe, out );
          PipeOp_NoMoreData( buffer_pipe );

          if (gs_trans) {
            if (out > global->buffer.available) {
              // Overflowed
              regs.spsr |= CF;
            }
            else {
              // All good
              regs.r[2] = out;
              regs.spsr &= ~CF;
            }

            close_buffer_pipe( client, handle, buffer_pipe );
          }
          else {
            // Otherwise, the pipe will be closed (and deleted)
            // on the final read from GSRead
            // Note: this implementation doesn't correspond to the
            // documentation, which points r1 at the first non-space
            // character. I don't think that matters.

            regs.r[0] = buffer_pipe;
            regs.r[2] = 0; // Index
          }
        }
      }
      break;
    case OP( OS_SubstituteArgs ): SubstituteArgs( client ); break;
    case OP( OS_SubstituteArgs32 ): SubstituteArgs32( client ); break;
    }

    if (0 != error) {
      regs.spsr |= VF;
      regs.r[0] = (uint32_t) error;
    }
    else {
      regs.spsr &= ~VF;
    }

    Task_ReleaseTask( client, &regs );
  }

  __builtin_unreachable();
}

