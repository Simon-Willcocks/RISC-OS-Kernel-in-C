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

// Compile with -static, or risk the wrath of the 64-bit pointer!
// Seriously, without -static, there's address randomisation, and functions
// and strings (for example) find themselves above the 4GiB 32-bit limit.
// The stack is still in high memory, so don't pass pointers to non-static
// local variables. (Or you might be able to do something with setarch...)

#include "inkernel.h"
#include <sys/mman.h>

uint32_t heap[10240] = { 0 };
uint32_t *heap_top = heap;

void *rma_allocate( uint32_t size, svc_registers *regs )
{
  uint32_t *result = heap_top;
  heap_top += (size + 3) / 4;
  return result;
}

struct core_workspace workspace = {};
struct shared_workspace volatile shared = {};

bool test_SetVarVal( svc_registers *regs, const char *name, const char *value, int length, uint32_t context, int type, uint32_t expected_error )
{
  regs->r[0] = (uint64_t) name;
  regs->r[1] = (uint64_t) value;
  regs->r[2] = length;
  regs->r[3] = context;
  regs->r[4] = type;
  if (!do_OS_SetVarVal( regs )) {
    error_block *e = (void *) (uint64_t) (regs->r[0]);
    if (expected_error != e->code) {
      printf( "FAILED: OS_SetVarVal %s\n", (char *) (uint64_t) (regs->r[0]+4) );
      return false;
    }
  }
  else {
    if (regs->r[0] != (uint64_t) name)
      return false;
    if (regs->r[1] != (uint64_t) value)
      return false;
    if (regs->r[2] != length)
      return false;
  }
  return true;
}

bool test_ReadVarVal( svc_registers *regs, const char *name, const char *buffer, uint32_t size, uint32_t context, int type, uint32_t expected_error )
{
  regs->r[0] = (uint64_t) name;
  regs->r[1] = (uint64_t) buffer;
  regs->r[2] = size;
  regs->r[3] = context;
  regs->r[4] = type;
  if (!do_OS_ReadVarVal( regs )) {
    error_block *e = (void *) (uint64_t) (regs->r[0]);
    if (expected_error != e->code) {
      printf( "FAILED: OS_ReadVarVal %s\n", (char *) (uint64_t) (regs->r[0]+4) );
      return false;
    }
  }
  else {
    if (regs->r[0] != (uint64_t) name)
      return false;
    if (regs->r[1] != (uint64_t) buffer)
      return false;
  }
  return true;
}

int main()
{
  svc_registers regs;
  int fails = 0;

  if (!test_SetVarVal( &regs, "Run$Path", "ADFS::$.", 0, 0, 0, 0 )) {
    printf( "FAILED: OS_SetVarVal %d\n", __LINE__ );
    fails++;
  }

  // Test for existence (exists):
  if (!test_ReadVarVal( &regs, "Run$Path", 0, -1, 0, 0, 0x1e4 )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }
  if (regs.r[2] != (uint32_t) ~strlen( "ADFS::$." )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }

  // Test for existence (does not exist):
  if (!test_ReadVarVal( &regs, "Peanutbutter", 0, -1, 0, 0, 0x124 )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }
  if (regs.r[2] != 0) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }

  // Read value (simple string)
  static char buffer[256];
  if (!test_ReadVarVal( &regs, "Run$Path", buffer, sizeof( buffer ), 0, 0, 0 )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }

  if (0 != strcmp( buffer, "ADFS::$." )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }

  // Add some more variables, they're stored in alphabetical order
  if (!test_SetVarVal( &regs, "AAAAA", "aaaaaaa", 0, 0, 0, 0 )) {
    printf( "FAILED: OS_SetVarVal %d\n", __LINE__ );
    fails++;
  }

  if (!test_SetVarVal( &regs, "MMMMM", "mmmmmmmmmm", 0, 0, 0, 0 )) {
    printf( "FAILED: OS_SetVarVal %d\n", __LINE__ );
    fails++;
  }

  if (!test_SetVarVal( &regs, "ZZZ", "zzzzzzz", 0, 0, 0, 0 )) {
    printf( "FAILED: OS_SetVarVal %d\n", __LINE__ );
    fails++;
  }

  if (!test_ReadVarVal( &regs, "Run$Path", buffer, sizeof( buffer ), 0, 0, 0 )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }

  if (0 != strcmp( buffer, "ADFS::$." )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }

  if (!test_ReadVarVal( &regs, "MMMMM", buffer, sizeof( buffer ), 0, 0, 0 )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }

  if (0 != strcmp( buffer, "mmmmmmmmmm" )) {
    printf( "FAILED: OS_ReadVarVal %d\n", __LINE__ );
    fails++;
  }

  // Additional tests to be done:
  // Non-string variables.
  // String variables with GSTrans codes in.
  // Macro variables (expanded by GSTrans on reading)
  // Code variables (extremely scary, but kind of cool!)

  return fails;
}
