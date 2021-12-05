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
    default: for (;;) { asm( "bkpt 68" ); }
    }
#ifdef DEBUG__SHOW_VDU_VARS
WriteS( "Read Vdu Var " ); WriteNum( *var ); WriteS( " = " ); WriteNum( *val ); NewLine;
#endif
    var++;
    val++;
  }
  return true;
}

bool do_OS_ReadModeVariable( svc_registers *regs )
{
  uint32_t legacy_mode_vars[21][13] = {
    [20] = { 0, 79, 63, 15, 1, 1, 320, 163840, 6, 2, 2, 639, 511 }
  };
  uint32_t *modevars;
  if (regs->r[0] == -1)
    modevars = workspace.vdu.modevars;
  else
    modevars = legacy_mode_vars[regs->r[0]];

  regs->r[2] = modevars[regs->r[1]];
  return true;
}

bool do_OS_ReadPoint( svc_registers *regs )
{
  for (;;) { asm( "bkpt 67" ); }
}

bool do_OS_RemoveCursors( svc_registers *regs ) { return true; } // What cursors? FIXME
bool do_OS_RestoreCursors( svc_registers *regs ) { return true; } // What cursors?

static const uint32_t initial_mode_vars[13] = { 
  0x40,         // Mode flags Hardware scroll never used
  1920 / 8,     // Character columns
  1080 / 8,     // Character rows
  0xffffffff,   // Maximum logical colour (?)
  0,            // XEigFactor
  0,            // YEigFactor
  1920 * 4,     // Line length
  1920*1080*4,  // Screen size
  0,            // YShiftFactor (not used)
  5,            // Log2BPP (32bpp)
  5,            // Log2BPC
  1920-1,       // XWindLimit
  1080-1 };     // YWindLimit

void SetInitialVduVars()
{
  memcpy( workspace.vdu.modevars, initial_mode_vars, sizeof( workspace.vdu.modevars ) );
  uint32_t for_drawmod = Kernel_allocate_pages( 4096, 4096 );
  MMU_map_at( (void*) 0x4000, for_drawmod, 4096 );
  for (int i = 0; i < 4096; i+=4) { *(uint32_t*)(0x4000+i) = workspace.core_number; }
}
