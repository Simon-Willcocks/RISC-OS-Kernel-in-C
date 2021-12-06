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

static bool ReadLegacyModeVariable( uint32_t selector, uint32_t var, uint32_t *val )
{
  uint32_t legacy_mode_vars[21][13] = {
    [20] = { 0, 79, 63, 15, 1, 1, 320, 163840, 6, 2, 2, 639, 511 }
  };

  if (selector >= number_of( legacy_mode_vars ))
    return false;

  uint32_t *modevars = legacy_mode_vars[selector];

  *val = modevars[var];

  return true;
}

static bool ReadCurrentModeVariable( uint32_t selector, uint32_t var, uint32_t *val )
{
  *val = workspace.vdu.modevars[var];

  return true;
}

static uint32_t NColour_from_type( uint32_t type )
{
  static const uint32_t known_types[] = { 0xbadf00d, 1, 3, 15,
                               63, 65535, -1, -1,
                               (1 << 24)-1, 0xbadf00d, 65535, 0xbadf00d,
                               0xbadf00d, 0xbadf00d, 0xbadf00d, 0xbadf00d,
                               4095, 420, 422 };
  if (type >= number_of( known_types )) {
    return 0xbadf00d;
  }
  return known_types[type];
}

static uint32_t Log2BPP_from_type( uint32_t type )
{
  static const uint32_t known_types[] = { 0xbadf00d, 0, 1, 2,
                               3, 4, 5, 5,
                               6, 0xbadf00d, 4, 0xbadf00d,
                               0xbadf00d, 0xbadf00d, 0xbadf00d, 0xbadf00d,
                               4, 7, 7 };
  if (type >= number_of( known_types )) {
    return 0xbadf00d;
  }
  return known_types[type];
}

static bool ReadRO5SpriteModeVariable( uint32_t selector, uint32_t var, uint32_t *val )
{
  union {
    struct __attribute__(( packed )) {
      uint32_t one:1;
      uint32_t three_zeros:3;
      uint32_t xdpi:2;
      uint32_t ydpi:2;
      uint32_t flags:8;
      uint32_t zeros:4;
      uint32_t type:7;
      uint32_t ones:4;
      uint32_t alphamask:1;
    };
    uint32_t raw;
  } specifier = { .raw = selector };

asm( "bkpt 40" );

  return false;
}

static bool ReadEigFromDPI( uint32_t dpi, uint32_t *val )
{
  switch (dpi) {
  case 180: *val = 0; return true;
  case 90: *val = 1; return true;
  case 45: *val = 2; return true;
  case 23: *val = 3; return true;
  case 22: *val = 3; return true;
  default: return false;
  }
}

static bool ReadSpriteModeVariable( uint32_t selector, uint32_t var, uint32_t *val )
{
  union {
    struct __attribute__(( packed )) {
      uint32_t one:1;
      uint32_t xdpi:12;
      uint32_t ydpi:12;
      uint32_t type:4;
      uint32_t alphamask:1;
    };
    uint32_t raw;
  } specifier = { .raw = selector };

  if (specifier.type == 15) {
    return ReadRO5SpriteModeVariable( selector, var, val );
  }

  bool result = true;

  switch (var) {
  case 0: *val = 0x40; break; // No hardware scrolling
  case 1: *val = 0; break;    // Don't know the size
  case 2: *val = 0; break;    // Don't know the size
  case 3: *val = NColour_from_type( specifier.type ); break;
  case 4: result = ReadEigFromDPI( specifier.xdpi, val ); break;
  case 5: result = ReadEigFromDPI( specifier.ydpi, val ); break;
  case 6: *val = 0; break;
  case 7: *val = 0; break;
  case 8: *val = 0; break;
  case 9: *val = Log2BPP_from_type( specifier.type ); break;
  case 10: *val = Log2BPP_from_type( specifier.type ); break; // No "double-pixel" modes
  case 11: *val = 0; break;
  case 12: *val = 0; break;
  default:
    result = false;
  }

  return result;
}

static bool ReadModeSelectorBlockVariable( uint32_t selector, uint32_t var, uint32_t *val )
{
  const mode_selector_block *mode = (void*) selector;

  bool result = true;

  switch (var) {
  case 0: *val = mode->mode_selector_flags; break;
  case 1: *val = mode->xres/8; break; // I think this is characters, not pixels
  case 2: *val = mode->yres/8; break;
  case 3: *val = (1 << mode->log2bpp) - 1; break;
  case 9: *val = mode->log2bpp; break;
  case 10: *val = mode->log2bpp; break; // No "double-pixel" mode->
  case 11: *val = mode->xres; break;
  case 12: *val = mode->yres; break;
  default:
    {
      int i = 0;
      result = false;
      while (mode->mode_variables[i].variable != -1) {
        if (mode->mode_variables[i].variable == var) {
          *val = mode->mode_variables[i].value;
          result = true;
          break;
        }
        i++;
      }
    }
  }

  return result;
}

static bool ReadSpriteAreaModeVariable( uint32_t selector, uint32_t var, uint32_t *val )
{
asm( "bkpt 40" );
  return false;
}

bool do_OS_ReadModeVariable( svc_registers *regs )
{
  // "The C flag is set if variable or mode numbers were invalid"
  bool success = false;

  if (regs->r[1] >= number_of( workspace.vdu.modevars )) {
    success = false;
  }
  else {
    if (0xffffffff == regs->r[0]) {
      // Current mode
      success = ReadCurrentModeVariable( regs->r[0], regs->r[1], &regs->r[2] );
    }
    else if (255 >= regs->r[0]) {
      // Mode number
      success = ReadLegacyModeVariable( regs->r[0], regs->r[1], &regs->r[2] );
    }
    else if (1 == (regs->r[0] & 1)) {
      // Sprite Mode word
      success = ReadSpriteModeVariable( regs->r[0], regs->r[1], &regs->r[2] );
    }
    else if (0 == (regs->r[0] & 2)) {
      // ModeSelectorBlock, or SpriteArea
      uint32_t *p = (void*) regs->r[0];
      if (0 == (*p & 1))
        success = ReadSpriteAreaModeVariable( regs->r[0], regs->r[1], &regs->r[2] );
      else
        success = ReadModeSelectorBlockVariable( regs->r[0], regs->r[1], &regs->r[2] );
    }
    else {
      // Invalid selector
      success = false;
    }
  }

  if (success)
    regs->spsr &= ~CF;
  else
    regs->spsr |=  CF;

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
