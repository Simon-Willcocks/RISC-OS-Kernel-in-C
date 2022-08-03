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
    workspace.vdu.ChangedBox.left = 0x7fffffff;
    workspace.vdu.ChangedBox.bottom = 0x7fffffff;
    workspace.vdu.ChangedBox.right = 0x80000000;
    workspace.vdu.ChangedBox.top = 0x80000000;
    break;
  default: // Simply read (should be case -1)
    break;
  }
  regs->r[1] = (uint32_t) &workspace.vdu.ChangedBox;
  regs->r[0] = workspace.vdu.ChangedBox.enabled;
  return true;
}

// Horribly incorporate legacy variables so legacy code can access them
// I'd like to move the used ones to a module's workspace and dump the rest.
const uint32_t* vduvarloc[173-128] = { 
  &workspace.vectors.zp.vdu_drivers.ws.GWLCol,           // 0x80 128
  &workspace.vectors.zp.vdu_drivers.ws.GWBRow,
  &workspace.vectors.zp.vdu_drivers.ws.GWRCol,
  &workspace.vectors.zp.vdu_drivers.ws.GWTRow,
  &workspace.vectors.zp.vdu_drivers.ws.TWLCol,
  &workspace.vectors.zp.vdu_drivers.ws.TWBRow,
  &workspace.vectors.zp.vdu_drivers.ws.TWRCol,
  &workspace.vectors.zp.vdu_drivers.ws.TWTRow,
  &workspace.vectors.zp.vdu_drivers.ws.OrgX,
  &workspace.vectors.zp.vdu_drivers.ws.OrgY,
  &workspace.vectors.zp.vdu_drivers.ws.GCsX,
  &workspace.vectors.zp.vdu_drivers.ws.GCsY,
  &workspace.vectors.zp.vdu_drivers.ws.OlderCsX,
  &workspace.vectors.zp.vdu_drivers.ws.OlderCsY,
  &workspace.vectors.zp.vdu_drivers.ws.OldCsX,
  &workspace.vectors.zp.vdu_drivers.ws.OldCsY,
  &workspace.vectors.zp.vdu_drivers.ws.GCsIX,            // 0x90 144
  &workspace.vectors.zp.vdu_drivers.ws.GCsIY,
  &workspace.vectors.zp.vdu_drivers.ws.NewPtX,
  &workspace.vectors.zp.vdu_drivers.ws.NewPtY,
  &workspace.vectors.zp.vdu_drivers.ws.ScreenStart,
  &workspace.vectors.zp.vdu_drivers.ws.DisplayStart,
  &workspace.vectors.zp.vdu_drivers.ws.TotalScreenSize,

  &workspace.vectors.zp.vdu_drivers.ws.GPLFMD,
  &workspace.vectors.zp.vdu_drivers.ws.GPLBMD,
  &workspace.vectors.zp.vdu_drivers.ws.GFCOL,
  &workspace.vectors.zp.vdu_drivers.ws.GBCOL,
  &workspace.vectors.zp.vdu_drivers.ws.TForeCol,
  &workspace.vectors.zp.vdu_drivers.ws.TBackCol,
  &workspace.vectors.zp.vdu_drivers.ws.GFTint,
  &workspace.vectors.zp.vdu_drivers.ws.GBTint,
  &workspace.vectors.zp.vdu_drivers.ws.TFTint,
  &workspace.vectors.zp.vdu_drivers.ws.TBTint,           // 0xa0 160
  &workspace.vectors.zp.vdu_drivers.ws.MaxMode,
  &workspace.vectors.zp.vdu_drivers.ws.GCharSizeX,
  &workspace.vectors.zp.vdu_drivers.ws.GCharSizeY,
  &workspace.vectors.zp.vdu_drivers.ws.GCharSpaceX,
  &workspace.vectors.zp.vdu_drivers.ws.GCharSpaceY,
  &workspace.vectors.zp.vdu_drivers.ws.HLineAddr,
  &workspace.vectors.zp.vdu_drivers.ws.TCharSizeX,
  &workspace.vectors.zp.vdu_drivers.ws.TCharSizeY,
  &workspace.vectors.zp.vdu_drivers.ws.TCharSpaceX,
  &workspace.vectors.zp.vdu_drivers.ws.TCharSpaceY,
  (uint32_t*) &workspace.vectors.zp.vdu_drivers.ws.GcolOraEorAddr,
  &workspace.vectors.zp.vdu_drivers.ws.VIDCClockSpeed
};

uint32_t *const modevarloc[13] = {
  &workspace.vectors.zp.vdu_drivers.ws.ModeFlags,
  &workspace.vectors.zp.vdu_drivers.ws.ScrRCol,
  &workspace.vectors.zp.vdu_drivers.ws.ScrBRow,
  &workspace.vectors.zp.vdu_drivers.ws.NColour,
  &workspace.vectors.zp.vdu_drivers.ws.XEigFactor,
  &workspace.vectors.zp.vdu_drivers.ws.YEigFactor,
  &workspace.vectors.zp.vdu_drivers.ws.LineLength,
  &workspace.vectors.zp.vdu_drivers.ws.ScreenSize,
  &workspace.vectors.zp.vdu_drivers.ws.YShftFactor,
  &workspace.vectors.zp.vdu_drivers.ws.Log2BPP,
  &workspace.vectors.zp.vdu_drivers.ws.Log2BPC,
  &workspace.vectors.zp.vdu_drivers.ws.XWindLimit,
  &workspace.vectors.zp.vdu_drivers.ws.YWindLimit
};

uint32_t *const textwindowloc[] = {
  (void*) 0xaaab00000,
  (void*) 0xaaab10000
};

bool do_OS_ReadVduVariables( svc_registers *regs )
{
  uint32_t *var = (void*) regs->r[0];
  uint32_t *val = (void*) regs->r[1];

#ifdef DEBUG__SHOW_VDU_VARS
WriteS( "Read Vdu Var " ); WriteNum( *var ); WriteS( " = " );
#endif
  while (*var != -1) {
    switch (*var) {
    case 0 ... 12: *val = *modevarloc[*var]; break;
    case 128 ... 172: *val = *vduvarloc[*var - 128]; break;
    case 192: *val = workspace.vectors.zp.vdu_drivers.ws.CurrentGraphicsVDriver; break;
    case 256:; *val = 1920/8/4-1; break; // Can't find this in zero page...
    case 257:; *val = 30; break; // Can't find this in zero page...
    default: for (;;) { asm( "bkpt 68" ); }
    }
#ifdef DEBUG__SHOW_VDU_VARS
WriteNum( *val ); NewLine;
#endif
    var++;
    val++;
  }
  return true;
}

static bool ReadLegacyModeVariable( uint32_t selector, uint32_t var, uint32_t *val )
{
  static const uint32_t legacy_mode_vars[47][13] = {
    // From 5.28
    [0]  = {	0,	79,	31,	1,	1,	2,	80,	20480,	4,	0,	0,	639,	255 },
    [1]  = {	0,	39,	31,	3,	2,	2,	80,	20480,	4,	1,	1,	319,	255 },
    [2]  = {	0,	19,	31,	15,	3,	2,	160,	40960,	5,	2,	3,	159,	255 },
    [3]  = {	13,	79,	24,	1,	1,	2,	160,	40960,	5,	1,	1,	639,	249 },
    [4]  = {	0,	39,	31,	1,	2,	2,	80,	20480,	4,	0,	1,	319,	255 },
    [5]  = {	0,	19,	31,	3,	3,	2,	80,	20480,	4,	1,	2,	159,	255 },
    [6]  = {	13,	39,	24,	1,	2,	2,	80,	20480,	4,	1,	1,	319,	249 },
    [7]  = {	167,	39,	24,	255,	1,	1,	640,	655360,	5,	3,	3,	639,	499 },
    [8]  = {	0,	79,	31,	3,	1,	2,	160,	40960,	5,	1,	1,	639,	255 },
    [9]  = {	0,	39,	31,	15,	2,	2,	160,	40960,	5,	2,	2,	319,	255 },
    [10] = {	0,	19,	31,	63,	3,	2,	320,	81920,	6,	3,	4,	159,	255 },
    [11] = {	4,	79,	24,	3,	1,	2,	160,	40960,	5,	1,	1,	639,	249 },
    [12] = {	0,	79,	31,	15,	1,	2,	320,	81920,	6,	2,	2,	639,	255 },
    [13] = {	0,	39,	31,	63,	2,	2,	320,	81920,	6,	3,	3,	319,	255 },
    [14] = {	4,	79,	24,	15,	1,	2,	320,	81920,	6,	2,	2,	639,	249 },
    [15] = {	0,	79,	31,	63,	1,	2,	640,	163840,	7,	3,	3,	639,	255 },
    [16] = {	0,	131,	31,	15,	1,	2,	528,	135168,	0,	2,	2,	1055,	255 },
    [17] = {	4,	131,	24,	15,	1,	2,	528,	135168,	0,	2,	2,	1055,	249 },
    [18] = {	0,	79,	63,	1,	1,	1,	80,	40960,	4,	0,	0,	639,	511 },
    [19] = {	0,	79,	63,	3,	1,	1,	160,	81920,	5,	1,	1,	639,	511 },
    [20] = {	0,	79,	63,	15,	1,	1,	320,	163840,	6,	2,	2,	639,	511 },
    [21] = {	0,	79,	63,	63,	1,	1,	640,	327680,	7,	3,	3,	639,	511 },
    [22] = {	0,	95,	35,	15,	0,	1,	384,	110592,	0,	2,	2,	767,	287 },
    [23] = {	48,	143,	55,	1,	1,	1,	144,	129024,	0,	0,	0,	1151,	895 },
    [24] = {	0,	131,	31,	63,	1,	2,	1056,	270336,	0,	3,	3,	1055,	255 },
    [25] = {	0,	79,	59,	1,	1,	1,	80,	38400,	4,	0,	0,	639,	479 },
    [26] = {	0,	79,	59,	3,	1,	1,	160,	76800,	5,	1,	1,	639,	479 },
    [27] = {	0,	79,	59,	15,	1,	1,	320,	153600,	6,	2,	2,	639,	479 },
    [28] = {	0,	79,	59,	63,	1,	1,	640,	307200,	7,	3,	3,	639,	479 },
    [29] = {	0,	99,	74,	1,	1,	1,	100,	60000,	0,	0,	0,	799,	599 },
    [30] = {	0,	99,	74,	3,	1,	1,	200,	120000,	0,	1,	1,	799,	599 },
    [31] = {	0,	99,	74,	15,	1,	1,	400,	240000,	0,	2,	2,	799,	599 },
    [32] = {	0,	99,	74,	63,	1,	1,	800,	480000,	0,	3,	3,	799,	599 },
    [33] = {	0,	95,	35,	1,	1,	2,	96,	27648,	0,	0,	0,	767,	287 },
    [34] = {	0,	95,	35,	3,	1,	2,	192,	55296,	0,	1,	1,	767,	287 },
    [35] = {	0,	95,	35,	15,	1,	2,	384,	110592,	0,	2,	2,	767,	287 },
    [36] = {	0,	95,	35,	63,	1,	2,	768,	221184,	0,	3,	3,	767,	287 },
    [37] = {	0,	111,	43,	1,	1,	2,	112,	39424,	0,	0,	0,	895,	351 },
    [38] = {	0,	111,	43,	3,	1,	2,	224,	78848,	0,	1,	1,	895,	351 },
    [39] = {	0,	111,	43,	15,	1,	2,	448,	157696,	0,	2,	2,	895,	351 },
    [40] = {	0,	111,	43,	63,	1,	2,	896,	315392,	0,	3,	3,	895,	351 },
    [41] = {	0,	79,	43,	1,	1,	2,	80,	28160,	0,	0,	0,	639,	351 },
    [42] = {	0,	79,	43,	3,	1,	2,	160,	56320,	0,	1,	1,	639,	351 },
    [43] = {	0,	79,	43,	15,	1,	2,	320,	112640,	0,	2,	2,	639,	351 },
    [44] = {	0,	79,	24,	1,	1,	2,	80,	16000,	0,	0,	0,	639,	199 },
    [45] = {	0,	79,	24,	3,	1,	2,	160,	32000,	0,	1,	1,	639,	199 },
    [46] = {	0,	79,	24,	15,	1,	2,	320,	64000,	0,	2,	2,	639,	199 }
  };

  if (selector >= number_of( legacy_mode_vars ))
    return false;

  uint32_t const *modevars = legacy_mode_vars[selector];

  // Not all filled in yet
  if (modevars[1] == 0) {
    WriteS( "Unknown mode: " ); WriteNum( selector ); NewLine;
    asm ( "bkpt 22" );
  }

  *val = modevars[var];

  return true;
}

static bool ReadCurrentModeVariable( uint32_t selector, uint32_t var, uint32_t *val )
{
  *val = *modevarloc[var];

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
      uint32_t xeig:2;
      uint32_t yeig:2;
      uint32_t flags:8;
      uint32_t zeros:4;
      uint32_t type:7;
      uint32_t ones:4;
      uint32_t alphamask:1;
    };
    uint32_t raw;
  } specifier = { .raw = selector };

  switch (var) {
  case 0: *val = (selector & 0x0000ff00); return true;
  case 1: *val = 0; return true;
  case 2: *val = 0; return true;
  case 3: *val = 0xffffffff; return true;
  case 4: *val = specifier.xeig; return true;
  case 5: *val = specifier.yeig; return true;
  case 6: *val = 0; return true;
  case 7: *val = 0; return true;
  case 8: *val = 0; return true;
  case 9: *val = 0; return true;
  case 10: *val = 0; return true;
  case 11: *val = 0; return true;
  case 12: *val = 0; return true;
  }

  return true;
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
      uint32_t xdpi:13;
      uint32_t ydpi:13;
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

#ifdef DEBUG__SHOW_VDU_VARS
Write0( __func__ ); Write0( " " ); WriteNum( selector ); Write0( " " ); WriteNum( var ); NewLine;
#endif
  bool result = true;

  switch (var) {
  case 0: *val = mode->mode_selector_flags; break;
  case 1: *val = mode->xres/8; break; // I think this is characters, not pixels
  case 2: *val = mode->yres/8; break;
  case 3: *val = (1 << (1 << mode->log2bpp)) - 1; break;
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
Write0( __func__ ); Space; WriteNum( selector ); Space; WriteNum( var ); NewLine;
asm( "bkpt 40" );
  return false;
}

bool do_OS_ReadModeVariable( svc_registers *regs )
{
  // "The C flag is set if variable or mode numbers were invalid"
  bool success = false;

  if (regs->r[1] >= number_of( modevarloc )) {
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

#ifdef DEBUG__SHOW_MODE_VARIABLE_READS
  if (success) { Write0( __func__ ); Space; WriteNum( regs->r[0] ); Space; WriteNum( regs->r[1] ); NewLine; }
#endif
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

