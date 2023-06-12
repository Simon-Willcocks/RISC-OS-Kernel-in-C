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

#include "inkernel.h"

typedef struct expression_result expression_result;
typedef struct expression_state expression_state;

struct expression_result {
  char *string; // 0 => result is a number, stored in number
  uint32_t number;
};

typedef struct expression_workspace {
  char *memory;
  uint32_t length;
} expression_workspace;

struct expression_state {
  char const *expr;
  uint32_t len;
  expression_workspace ws;
};

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

static error_block *find_end_of_string( expression_state *state )
{
  // Passed in state starts at initial "
  char const *p = state->expr + 1;
  uint32_t len = state->len - 1;
  uint32_t count = 2; // The expected quotes
  while (len > 0 && *p != '"') {
    if (len > 1 && *p == '|') {
      p++;
      len--;
      count++;
    }
    p++;
    len--;
    count++;
  }
  state->len = count;
  if (*p != '"') {
    static error_block error = { 666, "Missing closing \"" };
    return &error;
  }

#ifdef DEBUG__SHOW_NEW_GSTRANS
  WriteS( "String: " ); WriteN( state->expr, count ); NewLine;
#endif

  return 0;
}

static error_block *find_closing_paren( expression_state *state )
{
  // Passed in state starts at initial '('
  char const *p = state->expr;
  int length = state->len;
  int depth = 0;
  while (length > 0 && (depth != 1 || *p != ')')) { // Exit loop at final close
    if (*p == '(') depth++;
    if (*p == ')') depth--;
    if (*p == '"') {
      expression_state string = { p, length }; // Won't use workspace
      error_block *error = find_end_of_string( &string );
      if (error != 0) {
        return error;
      }
      p += string.len;
      length -= string.len;
    }
    else {
      p++;
      length--;
    }
  }

  if (*p != ')') {
    static error_block error = { 666, "Missing closing )" };
    return &error;
  }

  p++;
  state->len = p - state->expr;

  return 0;
}

static error_block *find_closing_angle( expression_state *state )
{
  char const *p = state->expr;
  uint32_t length = state->len;
  while (length > 0 && *p != '>' && *p != ' ') {
    p++; length--;
  }
  if (*p != '>') {
    static error_block error = { 666, "Missing closing >" };
    return &error;
  }
  state->len = p - state->expr + 1;
  return 0;
}

static inline void skip_spaces( expression_state *state )
{
  while (state->len > 0 && (*state->expr == ' ' || *state->expr == '\t')) {
    state->expr++; state->len--;
  }
}

static error_block *to_integer( expression_result *result )
{
  char const *in = result->string;

  if (in == 0) return 0; // Already a number

  uint32_t base = 0;
  uint32_t number = 0;
  uint32_t length = result->number;
  if (in[0] == '&') {
    base = 16; in++; length--;
  }
  for (int i = 0; i < length; i++)
  {
    if (in[i] == '_' && base == 0 && number > 1 && number <= 36) {
      base = number;
      number = 0;
      if (i == length-1) goto error; // Can't be the last character, where's the number?
    }
    else {
      int d = digit_in_base( in[i], base );
      if (d < 0) goto error;

      number = number * (base == 0 ? 10 : base) + d;
      // FIXME: check for overflow
    }
  }
  result->string = 0;
  result->number = number;

#ifdef DEBUG__SHOW_NEW_EVAL
  WriteS( "String \"" ); WriteN( in, length ); WriteS( "\" -> " ); WriteNum( result->number ); NewLine;
#endif

  return 0;

error:
  {
  static error_block error = { 666, "String is not convertable to integer" };
  return &error;
  }
}

// Treating the expression as
// expression ::= [ <unary_operator> ] <element> { <binary_operator> [ <unary_operator> ] <element> }
// element ::= ( "(" <expression> ")" | ( ( <string> | <number> | "<" <varname> ">" | TRUE | FALSE ) )
// Evaluating from left to right.
// The result gets put into the given structure, with the strings stored in the workspace.
// Once the caller has finished using the results from any subexpression, it can safely revert the workspace to
// where it was before the subexpression was evaluated.

static error_block *EvaluateExpr( expression_result *result,
                                  expression_state *state );


typedef error_block *(*unop)( expression_result *out, expression_result *arg );

static error_block *unary_plus( expression_result *out, expression_result *arg )
{
  out->string = 0;

  if (arg->string != 0) {
    error_block *error = to_integer( arg );
    if (error != 0) return error;
  }

  out->number = arg->number;

  return 0;
}

static error_block *unary_minus( expression_result *out, expression_result *arg )
{
  out->string = 0;

  if (arg->string != 0) {
    error_block *error = to_integer( arg );
    if (error != 0) return error;
  }

  out->number = -arg->number;

  return 0;
}

static error_block *unary_LEN( expression_result *out, expression_result *arg )
{
  out->string = 0;
  if (arg->string != 0) {
    out->number = arg->number;
  }
  else {
    // Don't convert the number to a string, just work out how many digits it would be.
    out->number = 1;
    int32_t argn = (int32_t) arg->number;
    if (argn < 0) {
      argn = -argn;
      out->number++;
    }
    do {
      argn = argn / 10;
      if (argn != 0) out->number++;
    } while (argn != 0);
  }
  return 0;
}

// Special case, this is the only one that needs workspace, sufficient 
// characters will have been allocated in the workspace at out->string
// to take a signed 32-bit number.
// (11 characters: &80000000 -> -2147483648.)
// This routine will never be passed a string argument
static error_block *unary_STR( expression_result *out, expression_result *arg )
{
  int32_t number = arg->number;
  char *outc = out->string;
  int minuses = 0;
  int digits;

  if (number < 0) {
    *outc++ = '-';
    number = -number;
    minuses = 1;
  }

       if (number >= 1000000000) digits = 10;
  else if (number >= 100000000) digits = 9;
  else if (number >= 10000000) digits = 8;
  else if (number >= 1000000) digits = 7;
  else if (number >= 100000) digits = 6;
  else if (number >= 10000) digits = 5;
  else if (number >= 1000) digits = 4;
  else if (number >= 100) digits = 3;
  else if (number >= 10) digits = 2;
  else digits = 1;

  out->number = digits + minuses;

  while (digits > 0) {
    outc[digits--] = '0' + (number % 10);
  }

  // No terminator
  return 0;
}

static inline bool terminator( char c, uint32_t flags )
{
  return c == '\0' || c == '\r' || c == '\n' || ((0 != (flags & (1 << 29))) && c == ' ');
}

static error_block *read_unary_operator( expression_state *state, unop *operator )
{
  static const struct {
    char const *op;
    unop func;
  } ops[] = {
    { "+", unary_plus },
    { "-", unary_minus },
    { "STR", unary_STR },
    { "LEN", unary_LEN },
    { "VAL", unary_plus } // Same thing
  };

  *operator = 0;

  for (int i = 0; i < number_of( ops ); i++) {
    char const *match = state->expr;
    char const *p = ops[i].op;
    int length = state->len;
    while (*p != '\0' && length > 0) {
      if (*p != *match) break;
      p++; match++; length --;
    }
    if (*p == '\0') {
      *operator = ops[i].func;
      state->expr += p - ops[i].op;
      state->len -= p - ops[i].op;
    }
  }

  return 0;
}

static error_block *EvalElement( expression_result *result,
                                 expression_state *state )
{
  skip_spaces( state );
  if (state->len == 0) {
    expression_result nothing = { 0 }; // number zero (would zero length string be better?)
    *result = nothing;
    return 0;
  }

  // Unary operators:
  //   Numeric argument: +, -, STR, 
  //   String argument:  LEN, VAL

  unop unaryop;
  error_block *error = read_unary_operator( state, &unaryop );
  if (error != 0) return error;

  if (unaryop != 0) {
    skip_spaces( state );
    if (state->len == 0) {
      static error_block error = { 356, "Missing operand" };
      return &error;
    }

    // Make a temporary copy, the parent will not be affected unless
    // the operation is STR.
    
    expression_workspace restore = state->ws;
    expression_result arg;
    error = EvalElement( &arg, state );
    if (error != 0) return error;

    if (unaryop == unary_STR) {
      // This one needs to affect the workspace
      if (arg.string != 0) {
        // Already a string, basically a NOP
        *result = arg;
        unaryop = 0;
      }
      else { // Translating a signed integer to a string
        // Partially fill in the result
        result->string = state->ws.memory;

        state->ws.memory += 12; // Largest size ever needed for a 32-bit integer
        state->ws.length -= 12;
      }
    }

    if (unaryop != 0) {
      error = unaryop( result, &arg );

      // Discard the latest element, it's replaced by the result of the unary operator
      state->ws = restore;
    }

    return error;
  }

  switch (*state->expr) {
  case '"': // Quoted string, to be GSTrans'd
    {
      expression_state string = { state->expr, state->len }; // Won't use workspace
      error_block *error = find_end_of_string( &string );
      if (error != 0) return error;

      state->expr += string.len;
      state->len -= string.len;

WriteS( "GSTrans'd string: " ); WriteN( string.expr, string.len );

      {
        string.len -= 2; // Quotes
        string.expr += 1; // Skip leading quote

        // Copy the content of the string, so it can be terminated
        char copy[string.len + 1];
        for (int i = 0; i < string.len; i++) {
          copy[i] = string.expr[i];
        }
        copy[string.len] = '\0';

        svc_registers regs = { { (uint32_t) copy, (uint32_t) state->ws.memory, state->ws.length, 0, 0 } };
        if (!do_OS_GSTrans( &regs )) {
          return (error_block *) regs.r[0];
        }

        result->string = state->ws.memory;
        result->number = regs.r[2];
        result->string[result->number] = '\0';
WriteS( " -> " ); WriteN( result->string, result->number ); NewLine;
        state->ws.length -= (result->number + 1);
        state->ws.memory += result->number + 1;
      }
    }
    break;
  case '<': // Variable expansion (may be string or number)
    {
      expression_state element = { state->expr, state->len, { state->ws.memory, state->ws.length } };
      error_block *error = find_closing_angle( &element );
      if (error != 0) return error;
      state->expr += element.len;
      state->len -= element.len;

      char const *varname = element.expr + 1;
      int namelen = element.len - 2; // Without <>
#ifdef DEBUG__SHOW_NEW_EVAL
      WriteS( "Expanding variable: " ); WriteN( varname, namelen ); NewLine;
#endif
      {
        char copy[namelen];
        for (int i = 0; i < namelen; i++) {
          copy[i] = varname[i];
        }
        copy[namelen] = '\0';

        svc_registers regs = { { (uint32_t) copy, (uint32_t) state->ws.memory, state->ws.length, 0, 0 } };
        if (!do_OS_ReadVarVal( &regs )) {
          return (error_block *) regs.r[0];
        }
        switch (regs.r[4]) { // Type
        case 0: // String
          {
            result->string = state->ws.memory;
            result->number = regs.r[2];
            result->string[result->number] = '\0';
            state->ws.length -= (result->number + 1);
            state->ws.memory += result->number + 1;
          }
          break;
        case 1: // Number (32-bit binary)
          {
            result->string = 0;
            result->number = *(uint32_t*) regs.r[2];
          }
          break;
        case 2: // Macro
          {
          }
          break;
        }
      }
      break;
    }
  case '(': // Subexpression
    {
      expression_state element = *state;

      error_block *error = find_closing_paren( &element );
      if (error != 0) return error;

      state->len -= element.len;
      state->expr = element.expr + element.len;

      // Remove the parentheses
      element.expr += 1;
      element.len -= 2;
      EvaluateExpr( result, &element );
    }
    break;
  case '&':
  case '0' ... '9':
    {
      uint32_t base = 0;
      if (*state->expr == '&') { base = 16; state->expr++; state->len--; }
      uint32_t number = 0;
      bool number_present = false;
      while (state->len > 0) {
        char c = *state->expr;
        if (c == '_' && base == 0 && number > 1 && number <= 36) {
          base = number;
          number = 0;
          number_present = false;
        }
        else {
          int d = digit_in_base( c, base );
          if (d < 0) break;

          number = number * (base == 0 ? 10 : base) + d;
          number_present = true;
        }

        state->expr++; state->len--;
      }
      if (!number_present) {
        static error_block error = { 363, "(Number)" };
        return &error;
      }
      result->string = 0; // Label as number
      result->number = number;
    }
    break;
  case 'T':
    {
      if (state->len == 4
       && state->expr[1] == 'R'
       && state->expr[2] == 'U'
       && state->expr[3] == 'E') {
        state->expr += 4; state->len -= 4;
        result->string = 0;
        result->number = -1;
      }
    }
    break;
  case 'F':
    {
      if (state->len == 5
       && state->expr[1] == 'A'
       && state->expr[2] == 'L'
       && state->expr[3] == 'S'
       && state->expr[4] == 'E') {
        state->expr += 5; state->len -= 5;
        result->string = 0;
        result->number = 0;
      }
    }
    break;
  default:
    {
      static error_block error = { 360, "Unknown operand" };
      return &error;
    }
  }

#ifdef DEBUG__SHOW_NEW_EVAL
  if (result->string != 0) {
    WriteS( "String element: " ); WriteNum( result->number ); Space;
    WriteN( result->string, result->number ); NewLine;
  }
  else {
    WriteS( "Number element: " ); WriteNum( result->number ); NewLine;
  }
#endif

  return 0;
}

enum { expr_FALSE = 0, expr_TRUE = -1 };

typedef error_block *(*binop)( expression_result *out, expression_result *left, expression_result *right );

#define INEQUALITY( name, OP ) \
error_block *binop_##name( expression_result *out, expression_result *left, expression_result *right ) \
{ \
  out->string = 0; \
  if (left->string == 0 || right->string == 0) { \
    /* Compare numbers */ \
    error_block *error = to_integer( right ); \
    if (error != 0) return error; \
 \
    error = to_integer( left ); \
    if (error != 0) return error; \
 \
    out->number = (left->number OP right->number) ? expr_TRUE : expr_FALSE; \
  } \
  else { \
    bool result = (left->number != right->number) && (left->number OP right->number); \
    uint32_t shorter = left->number > right->number ? right->number : left->number; \
    int i = 0; \
    while (left->string[i] == right->string[i] && i < shorter) i++; \
    if (i < shorter) result = (left->string[i] OP right->string[i]); \
    out->number = result ? expr_TRUE : expr_FALSE; \
  } \
 \
  return 0; \
}

INEQUALITY( less_than, < );
INEQUALITY( greater_than, > );

error_block *binop_equals( expression_result *out, expression_result *left, expression_result *right )
{
#ifdef DEBUG__SHOW_NEW_EVAL
Write0( __func__ ); NewLine;
#endif

  bool result;

  if (left->string == 0 || right->string == 0) {
    /* Compare numbers */
    error_block *error = to_integer( left );
    if (error != 0) return error;

    error = to_integer( right );
    if (error != 0) return error;
#ifdef DEBUG__SHOW_NEW_EVAL
  WriteS( "Left: " ); WriteNum( left->number ); NewLine;
  WriteS( "Right: " ); WriteNum( right->number ); NewLine;
#endif

    result = (left->number == right->number);
  }
  else {
    // Strings
#ifdef DEBUG__SHOW_NEW_EVAL
  WriteS( "Left: " ); WriteNum( left->number ); Space; WriteN( left->string, left->number ); NewLine;
  WriteS( "Right: " ); WriteNum( right->number ); Space; WriteN( right->string, right->number ); NewLine;
#endif

    result = (left->number == right->number);
    if (result) { // So far...
      for (int i = 0; i < left->number && result; i++) {
        result = (left->string[i] == right->string[i]);
      }
    }
  }

  out->string = 0;
  out->number = result ? expr_TRUE : expr_FALSE;

  return 0;
}

#define INVERSE( name, not ) \
error_block *binop_##name( expression_result *out, expression_result *left, expression_result *right ) \
{ \
  error_block *error = binop_##not( out, left, right ); \
  if (error) return error; \
  out->number = ~out->number; \
  return 0; \
}

INVERSE( not_equal, equals );
INVERSE( less_equal, greater_than );
INVERSE( greater_equal, less_than );

static error_block *read_binary_operator( expression_state *state, binop *operator )
{
  switch (*state->expr) {
  case '=':
    {
      state->expr++;
      state->len--;
      *operator = binop_equals;
      return 0;
    }
    break;
  case '<':
    {
      state->expr++;
      state->len--;
      *operator = binop_less_than;

      if (state->len > 0) {
        char c2 = *state->expr;
        if (c2 == '>') {
          *operator = binop_not_equal;
          state->expr++;
          state->len--;
        }
        else if (c2 == '=') {
          *operator = binop_less_equal;
          state->expr++;
          state->len--;
        }
      }
      return 0;
    }
    break;
  case '>':
    {
      state->expr++;
      state->len--;
      *operator = binop_greater_than;

      if (state->len > 0) {
        char c2 = *state->expr;
        if (c2 == '=') {
          *operator = binop_greater_equal;
          state->expr++;
          state->len--;
        }
      }
      return 0;
    }
    break;
  }

  static error_block error = { 666, "Unknown binary operator in expression" };
  return &error;
}

static error_block *EvaluateExpr( expression_result *result,
                                  expression_state *state )
{
  expression_result lresult = { 0 };
  expression_result rresult = { 0 };

  error_block *error = EvalElement( &lresult, state );
  if (error != 0) return error;

  // Now, binary operator?
  for (;;) {
    skip_spaces( state );
    if (state->len == 0) {
      *result = lresult;
      return 0;
    }

    binop operator;
    error = read_binary_operator( state, &operator );
    if (error != 0) return error;

    skip_spaces( state );
    if (state->len == 0) {
      static error_block error = { 666, "Missing operand" };
      return &error;
    }

    error = EvalElement( &rresult, state );
    if (error != 0) return error;

    expression_result opresult;
    error = operator( &opresult, &lresult, &rresult );
    if (error != 0) return error;

    lresult = opresult;
  }

  return 0;
}

static int riscos_strlen( char const *s )
{
  int result = 0;

  while (!terminator( *s++, 0 )) result++;

  return result;
}

// TODO: queue the expression to be evaluated, block the caller
// until it is (releasing the legacy SWIs, if possible)
// IOW Delegate the work to a user mode task.
bool do_OS_EvaluateExpression( svc_registers *regs )
{
  char const *expr = (char*) regs->r[0];
  uint32_t len = riscos_strlen( expr );

#ifdef DEBUG__SHOW_NEW_EVAL
  WriteS( "Evaluate \"" ); Write0( expr ); WriteS( "\"" ); NewLine;
#endif

  expression_result result = {0};
  uint32_t size = 2000;
  if (size < regs->r[2]) {
    size = 2 * regs->r[2];
  }
  char workspace[size];

  expression_state state = { .expr = expr, .len = len, .ws = { .memory = workspace, .length = size } };

  EvaluateExpr( &result, &state );

  if (result.string == 0) {
    regs->r[1] = 0;
    regs->r[2] = result.number;
  }
  else if (result.number <= regs->r[2]) {
    char *buffer = (void*) regs->r[1];
    for (int i = 0; i < result.number; i++) {
      buffer[i] = result.string[i];
    }
    regs->r[2] = result.number;
  }
  else {
    static error_block error = { 484, "Buffer overflow" };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

  return true;
}

#if 1
bool do_OS_GSTrans( svc_registers *regs )
{
  char const *in = (void*) regs->r[0];
  char *buffer = (void*) regs->r[1];
  char *out = buffer;
  uint32_t remaining = regs->r[2] & ~0xe0000000;
  uint32_t flags = regs->r[2] & 0xe0000000;

  bool const copy_quotes = 0 != (flags & (1 << 31));
  bool const ignore_control_codes = 0 != (flags & (1 << 30));

  bool set_top_bit = false;

  while (' ' == *in) { in++; }

  bool quoted_string = false;

  // Don't copy OUTER quotes unless bit 31 is set.
      // If not copying quotes, and the first non-space in the input was a
      // quote, a second teminates the process. Otherwise, quotes stay in
      // the output.

      // e.g. '   "abc"def'             -> 'abc'
      //      'abc "def" ghi'           -> 'abc "def" ghi'
      //      '   "abc "def" ghi"'      -> 'abc ' (with trailing space

  if ('"' == *in && !copy_quotes) {
    quoted_string = true;
    flags = flags & ~(1 << 29);
    in++;
  }

#ifdef DEBUG__SHOW_NEW_GSTRANS
  WriteS( "GSTrans size & flags: " ); WriteNum( regs->r[2] ); NewLine;
  WriteS( "GSTrans in: " ); Write0( in ); Space; WriteNum( buffer ); Space; WriteNum( remaining ); NewLine;
#endif

  while (remaining > 0
      && !terminator( *in, flags )
      && (copy_quotes || !quoted_string || *in != '"')) {
    char c = *in++;

    if (c == '|' && !ignore_control_codes) {
      char next = *in++;

      if (terminator( next, flags )) {
        static error_block error = { 666, "Character missing after |" };
        regs->r[0] = (uint32_t) &error;
        return false;
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
          static error_block error = { 666, "Invalid character after |" };
          regs->r[0] = (uint32_t) &error;
          return false;
        }
      }
    }
    else if (c == '<') {
      if (set_top_bit) {
        static error_block error = { 666, "Missing single character to set top bit of" };
        regs->r[0] = (uint32_t) &error;
        return false;
      }

      bool is_number = true; // As far as we know so far
      int i = 0;
      bool base = 0; // 0 is default, base 10, unless there's an underscore
      uint32_t number = 0;
      if (in[i] == '&') {
        base = 16;
        i++;
      }

      while (in[i] != '>' && !terminator( in[i], flags )) {
        if (in[i] <= ' ') {
          static error_block error = { 666, "Invalid number or variable name" };
          regs->r[0] = (uint32_t) &error;
          return false;
        }
        else if (in[i] == '_' && base == 0 && is_number && number > 1 && number <= 36) {
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
          char copy[i + 1];
          for (int ci = 0; ci < i; ci++) {
            copy[ci] = in[ci];
          }
          copy[i] = '\0'; // Terminate the variable name
#ifdef DEBUG__SHOW_NEW_GSTRANS
          WriteS( "Expanding variable " ); Write0( copy ); WriteS( " in GSTrans" ); NewLine;
#endif

          uint32_t r3 = regs->r[3];
          uint32_t r4 = regs->r[4];
          regs->r[0] = (uint32_t) copy;
          regs->r[1] = (uint32_t) out;
          regs->r[2] = remaining;
          regs->r[3] = 0;
          regs->r[4] = 3;
          bool success = do_OS_ReadVarVal( regs );
          regs->r[3] = r3;
          regs->r[4] = r4;
          if (!success) {
            error_block *error = (void*) regs->r[0];
#ifdef DEBUG__SHOW_NEW_GSTRANS
            WriteS( "ReadVarVal error: " ); Write0( error->desc ); NewLine;
#endif
            if (error->code != 292) return false; // unknown variable -> empty string
          }
          else {
            out += regs->r[2];
            remaining -= regs->r[2];
            in += i+1;
            continue; // No single character to append
          }
        }
      }
      else {
        static error_block error = { 666, "Missing > after value/variable" };
        regs->r[0] = (uint32_t) &error;
        return false;
      }
      in += i + 1;
    }

    if (set_top_bit) {
      set_top_bit = false;
      c = c | 0x80;
    }

    *out++ = c;
    remaining--;
  }

  if ((remaining == 0 || terminator( *in, flags ))
      && (!copy_quotes && quoted_string)) {
    static error_block error = { 253, "String not recognised" };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

  if (set_top_bit) {
    static error_block error = { 666, "No character to set top bit of (|! at end of string)" };
    regs->r[0] = (uint32_t) &error;
    return false;
  }

#ifdef DEBUG__SHOW_NEW_GSTRANS
  WriteS( "GSTrans out: \"" ); WriteN( buffer, out - buffer ); WriteS( "\"" ); NewLine;
#endif

  *out = *in;
  regs->r[0] = (uint32_t) in+1;
  regs->r[2] = out - buffer;
  if (remaining == 0) regs->spsr |= CF; else regs->spsr &= ~CF;

  return true;
}
#endif

