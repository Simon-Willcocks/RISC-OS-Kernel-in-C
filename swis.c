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

error_block Error_UnknownSWI = { 1, "Unknown SWI" };

static bool do_OS_WriteC( svc_registers *regs )
{
  svc_registers tmp;
  tmp.r[0] = regs->r[0];
  tmp.r[9] = 3;
  
  bool result = do_OS_CallAVector( &tmp );

  if (!result) {
    regs->r[0] = tmp.r[0]; // Error code
  }
  return result;
}

static uint32_t word_align( void *p )
{
  return (((uint32_t) p) + 3) & ~3;
}

static bool do_OS_WriteS( svc_registers *regs )
{
  const char *s = (void*) regs->lr;
  uint32_t r0 = regs->r[0];
  bool result = true;

  while (*s != '\0' && result) {
    regs->r[0] = *s++;
    result = do_OS_WriteC( regs );
  }

  regs->lr = word_align( s );
  if (result) regs->r[0] = r0;

  return result;
}

static bool do_OS_Write0( svc_registers *regs )
{
  const char *s = (void*) regs->r[0];
  bool result = true;

  while (*s != '\0' && result) {
    regs->r[0] = *s++;
    result = do_OS_WriteC( regs );
  }
  if (result) {
    regs->r[0] = (uint32_t) s+1;
  }

  return result;
}

static bool do_OS_NewLine( svc_registers *regs )
{
  bool result;
  regs->r[0] = '\r';
  result = do_OS_WriteC( regs );
  if (result) {
    regs->r[0] = '\n';
    result = do_OS_WriteC( regs );
  }

  return result;
}


static bool do_OS_ReadC( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_CLI( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Byte( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Word( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_File( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Args( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_BGet( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_BPut( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_GBPB( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Find( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadLine( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Control( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_GetEnv( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Exit( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SetEnv( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_IntOn( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_IntOff( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_CallBack( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_EnterOS( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_BreakPt( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_BreakCtrl( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_UnusedSWI( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_UpdateMEMC( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SetCallBack( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_Mouse( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ReadUnsigned( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_GenerateEvent( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadVarVal( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_SetVarVal( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_GSInit( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_GSRead( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_GSTrans( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_BinaryToDecimal( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_FSControl( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ChangeDynamicArea( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_GenerateError( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ReadEscapeState( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_EvaluateExpression( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SpriteOp( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadPalette( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }


static bool do_OS_ReadVduVariables( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadPoint( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_UpCall( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ReadModeVariable( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_RemoveCursors( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_RestoreCursors( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_SWINumberToString( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SWINumberFromString( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ValidateAddress( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_CallAfter( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_CallEvery( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_RemoveTickerEvent( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_InstallKeyHandler( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_CheckModeValid( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }


static bool do_OS_ChangeEnvironment( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ClaimScreenMemory( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadMonotonicTime( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SubstituteArgs( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_PrettyPrint( svc_registers *regs )
{
  const char *s = (void*) regs->r[0];
  const char *dictionary = (void*) regs->r[1];
  if (dictionary == 0) {
    static const char internal[] =
        "Syntax: *\x1b\0"; // FIXME
    dictionary = internal;
  }

  uint32_t r0 = regs->r[0];
  bool result = true;

  while (*s != '\0' && result) {
    if (*s == '\x1b') {
      s++;
      regs->r[0] = (uint32_t) "!!!PrettyPrint needs implementing!!!";
      result = do_OS_WriteS( regs );
    }
    else {
      regs->r[0] = *s++;
      result = do_OS_WriteC( regs );
    }
  }

  if (result) regs->r[0] = r0;

  return result;
}

static bool do_OS_Plot( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_WriteN( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_WriteEnv( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadArgs( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadRAMFsLimits( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ClaimDeviceVector( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ReleaseDeviceVector( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_HeapSort( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ExitAndDie( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadMemMapInfo( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadMemMapEntries( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SetMemMapEntries( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_AddCallBack( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadDefaultHandler( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SetECFOrigin( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SerialOp( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }


static bool do_OS_ReadSysInfo( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Confirm( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ChangedBox( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_CRC( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ReadDynamicArea( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_PrintChar( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ChangeRedirection( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_RemoveCallBack( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }


static bool do_OS_FindMemMapEntries( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_SetColour( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Pointer( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ScreenMode( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_DynamicArea( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Memory( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ClaimProcessorVector( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Reset( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_MMUControl( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

#ifdef NO_CONVERT_MODULE
// This is a lot of work for little gain, and could be fixed by a Convert module, which can use existing code.
static bool do_OS_ConvertStandardDateAndTime( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertDateAndTime( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static const char hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

static bool buffer_too_small(  svc_registers *regs )
{
  static error_block error = { 0x1e4, "Buffer overflow" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool hex_convert( svc_registers *regs, int digits )
{
  uint32_t n = regs->r[0];

  regs->r[0] = regs->r[1];

  for (int i = digits; i > 0; i--) {
    *((char *) regs->r[1]++) = hex[(n >> (4*i))&0xf];
    regs->r[2] --;
    if (regs->r[2] == 0) return buffer_too_small( regs );
  }
  return true;
}

static bool do_OS_ConvertHex1( svc_registers *regs )
{
  return hex_convert( regs, 1 );
}

static bool do_OS_ConvertHex2( svc_registers *regs )
{
  return hex_convert( regs, 2 );
}

static bool do_OS_ConvertHex4( svc_registers *regs )
{
  return hex_convert( regs, 4 );
}

static bool do_OS_ConvertHex6( svc_registers *regs )
{
  return hex_convert( regs, 6 );
}

static bool do_OS_ConvertHex8( svc_registers *regs )
{
  return hex_convert( regs, 8 );
}

static bool recursive_convert_decimal( svc_registers *regs, uint32_t n )
{
  uint32_t d = n / 10;
  bool result = true;

  if (d > 0)
    result = recursive_convert_decimal( regs, d );

  if (result) {
    *((char *) regs->r[1]++) = '0' + n % 10;
    regs->r[2] --;
    if (regs->r[2] == 0) result = buffer_too_small( regs );
  }

  return result;
}

static bool convert_decimal( svc_registers *regs, uint32_t mask )
{
  uint32_t n = regs->r[0] & mask;
  regs->r[0] = regs->r[1];

  return recursive_convert_decimal( regs, n );
}

static bool do_OS_ConvertCardinal1( svc_registers *regs )
{
  return convert_decimal( regs, 0xff );
}

static bool do_OS_ConvertCardinal2( svc_registers *regs )
{
  return convert_decimal( regs, 0xffff );
}

static bool do_OS_ConvertCardinal3( svc_registers *regs )
{
  return convert_decimal( regs, 0xffffff );
}

static bool do_OS_ConvertCardinal4( svc_registers *regs )
{
  return convert_decimal( regs, 0xffffffff );
}

static bool convert_signed_decimal( svc_registers *regs, uint32_t sign_bit )
{
  uint32_t n = regs->r[0] & (sign_bit - 1);

  if (0 != (regs->r[0] & sign_bit)) {
    *((char *) regs->r[1]++) = '-';
    regs->r[2] --;
    if (regs->r[2] == 0) return buffer_too_small( regs );
    n = sign_bit - n;
  }

  return recursive_convert_decimal( regs, n );
}

static bool do_OS_ConvertInteger1( svc_registers *regs )
{
  return convert_decimal( regs, (1 << 7) );
}

static bool do_OS_ConvertInteger2( svc_registers *regs )
{
  return convert_decimal( regs, (1 << 15) );
}

static bool do_OS_ConvertInteger3( svc_registers *regs )
{
  return convert_decimal( regs, (1 << 23) );
}

static bool do_OS_ConvertInteger4( svc_registers *regs )
{
  return convert_decimal( regs, (1ul << 31) );
}


static bool do_OS_ConvertBinary1( svc_registers *regs )
static bool do_OS_ConvertBinary2( svc_registers *regs )
static bool do_OS_ConvertBinary3( svc_registers *regs )
static bool do_OS_ConvertBinary4( svc_registers *regs )

static bool do_OS_ConvertSpacedCardinal1( svc_registers *regs )
static bool do_OS_ConvertSpacedCardinal2( svc_registers *regs )
static bool do_OS_ConvertSpacedCardinal3( svc_registers *regs )
static bool do_OS_ConvertSpacedCardinal4( svc_registers *regs )

static bool do_OS_ConvertSpacedInteger1( svc_registers *regs )
static bool do_OS_ConvertSpacedInteger2( svc_registers *regs )
static bool do_OS_ConvertSpacedInteger3( svc_registers *regs )
static bool do_OS_ConvertSpacedInteger4( svc_registers *regs )

static bool do_OS_ConvertFixedNetStation( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertNetStation( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertFixedFileSize( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertFileSize( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
#endif

static bool Kernel_go_svc( svc_registers *regs, uint32_t svc )
{
  switch (svc & ~Xbit) {
  case OS_WriteC: return do_OS_WriteC( regs );
  case OS_WriteS: return do_OS_WriteS( regs );
  case OS_Write0: return do_OS_Write0( regs );
  case OS_NewLine: return do_OS_NewLine( regs );

  case OS_ReadC: return do_OS_ReadC( regs );
  case OS_CLI: return do_OS_CLI( regs );
  case OS_Byte: return do_OS_Byte( regs );
  case OS_Word: return do_OS_Word( regs );

  case OS_File: return do_OS_File( regs );
  case OS_Args: return do_OS_Args( regs );
  case OS_BGet: return do_OS_BGet( regs );
  case OS_BPut: return do_OS_BPut( regs );

  case OS_GBPB: return do_OS_GBPB( regs );
  case OS_Find: return do_OS_Find( regs );
  case OS_ReadLine: return do_OS_ReadLine( regs );
  case OS_Control: return do_OS_Control( regs );

  case OS_GetEnv: return do_OS_GetEnv( regs );
  case OS_Exit: return do_OS_Exit( regs );
  case OS_SetEnv: return do_OS_SetEnv( regs );
  case OS_IntOn: return do_OS_IntOn( regs );

  case OS_IntOff: return do_OS_IntOff( regs );
  case OS_CallBack: return do_OS_CallBack( regs );
  case OS_EnterOS: return do_OS_EnterOS( regs );
  case OS_BreakPt: return do_OS_BreakPt( regs );

  case OS_BreakCtrl: return do_OS_BreakCtrl( regs );
  case OS_UnusedSWI: return do_OS_UnusedSWI( regs );
  case OS_UpdateMEMC: return do_OS_UpdateMEMC( regs );
  case OS_SetCallBack: return do_OS_SetCallBack( regs );

  case OS_Mouse: return do_OS_Mouse( regs );
  case OS_Heap: return do_OS_Heap( regs );
  case OS_Module: return do_OS_Module( regs );
  case OS_Claim: return do_OS_Claim( regs );


  case OS_Release: return do_OS_Release( regs );
  case OS_ReadUnsigned: return do_OS_ReadUnsigned( regs );
  case OS_GenerateEvent: return do_OS_GenerateEvent( regs );
  case OS_ReadVarVal: return do_OS_ReadVarVal( regs );

  case OS_SetVarVal: return do_OS_SetVarVal( regs );
  case OS_GSInit: return do_OS_GSInit( regs );
  case OS_GSRead: return do_OS_GSRead( regs );
  case OS_GSTrans: return do_OS_GSTrans( regs );

  case OS_BinaryToDecimal: return do_OS_BinaryToDecimal( regs );
  case OS_FSControl: return do_OS_FSControl( regs );
  case OS_ChangeDynamicArea: return do_OS_ChangeDynamicArea( regs );
  case OS_GenerateError: return do_OS_GenerateError( regs );

  case OS_ReadEscapeState: return do_OS_ReadEscapeState( regs );
  case OS_EvaluateExpression: return do_OS_EvaluateExpression( regs );
  case OS_SpriteOp: return do_OS_SpriteOp( regs );
  case OS_ReadPalette: return do_OS_ReadPalette( regs );

  case OS_ServiceCall: return do_OS_ServiceCall( regs );
  case OS_ReadVduVariables: return do_OS_ReadVduVariables( regs );
  case OS_ReadPoint: return do_OS_ReadPoint( regs );
  case OS_UpCall: return do_OS_UpCall( regs );

  case OS_CallAVector: return do_OS_CallAVector( regs );
  case OS_ReadModeVariable: return do_OS_ReadModeVariable( regs );
  case OS_RemoveCursors: return do_OS_RemoveCursors( regs );
  case OS_RestoreCursors: return do_OS_RestoreCursors( regs );

  case OS_SWINumberToString: return do_OS_SWINumberToString( regs );
  case OS_SWINumberFromString: return do_OS_SWINumberFromString( regs );
  case OS_ValidateAddress: return do_OS_ValidateAddress( regs );
  case OS_CallAfter: return do_OS_CallAfter( regs );

  case OS_CallEvery: return do_OS_CallEvery( regs );
  case OS_RemoveTickerEvent: return do_OS_RemoveTickerEvent( regs );
  case OS_InstallKeyHandler: return do_OS_InstallKeyHandler( regs );
  case OS_CheckModeValid: return do_OS_CheckModeValid( regs );


  case OS_ChangeEnvironment: return do_OS_ChangeEnvironment( regs );
  case OS_ClaimScreenMemory: return do_OS_ClaimScreenMemory( regs );
  case OS_ReadMonotonicTime: return do_OS_ReadMonotonicTime( regs );
  case OS_SubstituteArgs: return do_OS_SubstituteArgs( regs );

  case OS_PrettyPrint: return do_OS_PrettyPrint( regs );
  case OS_Plot: return do_OS_Plot( regs );
  case OS_WriteN: return do_OS_WriteN( regs );
  case OS_AddToVector: return do_OS_AddToVector( regs );

  case OS_WriteEnv: return do_OS_WriteEnv( regs );
  case OS_ReadArgs: return do_OS_ReadArgs( regs );
  case OS_ReadRAMFsLimits: return do_OS_ReadRAMFsLimits( regs );
  case OS_ClaimDeviceVector: return do_OS_ClaimDeviceVector( regs );

  case OS_ReleaseDeviceVector: return do_OS_ReleaseDeviceVector( regs );
  case OS_DelinkApplication: return do_OS_DelinkApplication( regs );
  case OS_RelinkApplication: return do_OS_RelinkApplication( regs );
  case OS_HeapSort: return do_OS_HeapSort( regs );

  case OS_ExitAndDie: return do_OS_ExitAndDie( regs );
  case OS_ReadMemMapInfo: return do_OS_ReadMemMapInfo( regs );
  case OS_ReadMemMapEntries: return do_OS_ReadMemMapEntries( regs );
  case OS_SetMemMapEntries: return do_OS_SetMemMapEntries( regs );

  case OS_AddCallBack: return do_OS_AddCallBack( regs );
  case OS_ReadDefaultHandler: return do_OS_ReadDefaultHandler( regs );
  case OS_SetECFOrigin: return do_OS_SetECFOrigin( regs );
  case OS_SerialOp: return do_OS_SerialOp( regs );


  case OS_ReadSysInfo: return do_OS_ReadSysInfo( regs );
  case OS_Confirm: return do_OS_Confirm( regs );
  case OS_ChangedBox: return do_OS_ChangedBox( regs );
  case OS_CRC: return do_OS_CRC( regs );

  case OS_ReadDynamicArea: return do_OS_ReadDynamicArea( regs );
  case OS_PrintChar: return do_OS_PrintChar( regs );
  case OS_ChangeRedirection: return do_OS_ChangeRedirection( regs );
  case OS_RemoveCallBack: return do_OS_RemoveCallBack( regs );


  case OS_FindMemMapEntries: return do_OS_FindMemMapEntries( regs );
  case OS_SetColour: return do_OS_SetColour( regs );
  case OS_Pointer: return do_OS_Pointer( regs );
  case OS_ScreenMode: return do_OS_ScreenMode( regs );

  case OS_DynamicArea: return do_OS_DynamicArea( regs );
  case OS_Memory: return do_OS_Memory( regs );
  case OS_ClaimProcessorVector: return do_OS_ClaimProcessorVector( regs );
  case OS_Reset: return do_OS_Reset( regs );

  case OS_MMUControl: return do_OS_MMUControl( regs );

#ifdef NO_CONVERT_MODULE
  case OS_ConvertStandardDateAndTime: return do_OS_ConvertStandardDateAndTime( regs );
  case OS_ConvertDateAndTime: return do_OS_ConvertDateAndTime( regs );

  case OS_ConvertHex1: return do_OS_ConvertHex1( regs );
  case OS_ConvertHex2: return do_OS_ConvertHex2( regs );
  case OS_ConvertHex4: return do_OS_ConvertHex4( regs );
  case OS_ConvertHex6: return do_OS_ConvertHex6( regs );

  case OS_ConvertHex8: return do_OS_ConvertHex8( regs );
  case OS_ConvertCardinal1: return do_OS_ConvertCardinal1( regs );
  case OS_ConvertCardinal2: return do_OS_ConvertCardinal2( regs );
  case OS_ConvertCardinal3: return do_OS_ConvertCardinal3( regs );

  case OS_ConvertCardinal4: return do_OS_ConvertCardinal4( regs );
  case OS_ConvertInteger1: return do_OS_ConvertInteger1( regs );
  case OS_ConvertInteger2: return do_OS_ConvertInteger2( regs );
  case OS_ConvertInteger3: return do_OS_ConvertInteger3( regs );

  case OS_ConvertInteger4: return do_OS_ConvertInteger4( regs );
  case OS_ConvertBinary1: return do_OS_ConvertBinary1( regs );
  case OS_ConvertBinary2: return do_OS_ConvertBinary2( regs );
  case OS_ConvertBinary3: return do_OS_ConvertBinary3( regs );

  case OS_ConvertBinary4: return do_OS_ConvertBinary4( regs );
  case OS_ConvertSpacedCardinal1: return do_OS_ConvertSpacedCardinal1( regs );
  case OS_ConvertSpacedCardinal2: return do_OS_ConvertSpacedCardinal2( regs );
  case OS_ConvertSpacedCardinal3: return do_OS_ConvertSpacedCardinal3( regs );

  case OS_ConvertSpacedCardinal4: return do_OS_ConvertSpacedCardinal4( regs );
  case OS_ConvertSpacedInteger1: return do_OS_ConvertSpacedInteger1( regs );
  case OS_ConvertSpacedInteger2: return do_OS_ConvertSpacedInteger2( regs );
  case OS_ConvertSpacedInteger3: return do_OS_ConvertSpacedInteger3( regs );

  case OS_ConvertSpacedInteger4: return do_OS_ConvertSpacedInteger4( regs );
  case OS_ConvertFixedNetStation: return do_OS_ConvertFixedNetStation( regs );
  case OS_ConvertNetStation: return do_OS_ConvertNetStation( regs );
  case OS_ConvertFixedFileSize: return do_OS_ConvertFixedFileSize( regs );

  case OS_ConvertFileSize: return do_OS_ConvertFileSize( regs );
#endif

  case OS_WriteI ... OS_WriteI+255:
    {
      uint32_t r0 = regs->r[0];
      bool result;
      regs->r[0] = svc & 0xff;
      result = do_OS_WriteC( regs );
      if (result) {
        regs->r[0] = r0;
      }
      return result;
    }
  };

  return do_module_swi( regs, svc );
}

void __attribute__(( naked, noreturn )) Kernel_default_svc()
{
  // Some SWIs preserve all registers
  // SWIs have the potential to update the first 10 registers
  // The implementations are passed values in r11 and r12, which must not
  // be seen by the caller, and r10 may also be corrupted.
  // The SVC stack pointer should be maintained by the implementation.

  // C functions may corrupt r0-r3, r9, and r10-12, and r14 (lr)

  // Gordian knot time.
  // Store r0-r12 on the stack, plus the exception return details (srs)
  // Call C functions to find and call the appropriate handler, storing the returned
  // r0-r9 over the original values on return (and updating the stored SPSR flags).
  // The savings of not always having to save r4-r8 (into non-shared, cached memory)
  // will be minor compared to messing about trying to avoid it.

  register svc_registers *regs asm ( "r0" );
  register uint32_t lr;
  asm ( "  srsdb sp!, #0x13"
      "\n  push { r0-r12 }"
      "\n  mov %[regs], sp"
      "\n  mov %[lr], lr"
      : [regs] "=r" (regs)
      , [lr] "=r" (lr)
      );

  regs->spsr &= ~VF;
  uint32_t number = get_swi_number( lr );
  if (Kernel_go_svc( regs, number )) {
    // Worked
    regs->spsr &= ~VF;
  }
  else if ((number & Xbit) != 0) {
    // Error
    regs->spsr |= VF;
  }
  else {
    // Call error handler
  }

  asm ( "pop { r0-r12 }"
    "\n  rfeia sp!" );

  __builtin_unreachable();
}

