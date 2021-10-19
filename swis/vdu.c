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

void default_os_writec( uint32_t r0, uint32_t r1, uint32_t r2 )
{
  // Do nothing
}

bool do_OS_ChangedBox( svc_registers *regs )
{
  workspace.vdu.ChangedBox.enabled = workspace.vdu.changed_box_tracking_enabled;
  switch (regs->r[0]) {
  case 0: workspace.vdu.changed_box_tracking_enabled = 0; break;
  case 1: workspace.vdu.changed_box_tracking_enabled = 1; break;
  case 2:
    workspace.vdu.ChangedBox.left = 0;
    workspace.vdu.ChangedBox.bottom = 0;
    workspace.vdu.ChangedBox.right = 0;
    workspace.vdu.ChangedBox.top = 0;
    break;
  }
  regs->r[1] = (uint32_t) &workspace.vdu.ChangedBox;
  regs->r[0] = workspace.vdu.ChangedBox.enabled;
  return true;
}

bool do_OS_ReadVduVariables( svc_registers *regs )
{
  uint32_t *var = (void*) regs->r[0];
  uint32_t *val = (void*) regs->r[1];

  while (*var != -1) {
    switch (*var) {
    case 0 ... 12: *val = workspace.vdu.modevars[*var]; break;
    case 128 ... 172: *val = workspace.vdu.vduvars[*var - 128]; break;
    case 256 ... 257: *val = workspace.vdu.textwindow[*var - 256]; break;
    default: for (;;) { asm( "wfi" ); }
    }
    var++;
    val++;
  }
  return true;
}

bool do_OS_ReadModeVariable( svc_registers *regs )
{
  if (regs->r[0] != -1) {
    for (;;) { asm ( "wfi" ); }
  }
  regs->r[2] = workspace.vdu.modevars[regs->r[1]];
  return true;
}

bool do_OS_ReadPoint( svc_registers *regs )
{
  for (;;) { asm( "wfi" ); }
}

bool do_OS_RemoveCursors( svc_registers *regs ) { return true; } // What cursors? FIXME
bool do_OS_RestoreCursors( svc_registers *regs ) { return true; } // What cursors?

static const uint32_t initial_mode_vars[13] = { 0x40, 0xef, 0x86, -1, 1, 1, 0x1e00, 0x7e9000, 0, 5, 5, 0x77f, 0x437 };

void SetInitialVduVars()
{
  memcpy( workspace.vdu.modevars, initial_mode_vars, sizeof( workspace.vdu.modevars ) );
  uint32_t for_drawmod = Kernel_allocate_pages( 4096, 4096 );
  MMU_map_at( (void*) 0x4000, for_drawmod, 4096 );
  for (int i = 0; i < 4096; i+=4) { *(uint32_t*)(0x4000+i) = workspace.core_number; }
}
