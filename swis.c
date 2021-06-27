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
#include "swis.h"

static const uint32_t VF = (1 << 28);
static const uint32_t Xbit = (1 << 17);

typedef struct __attribute__(( packed )) {
  uint32_t r[13];
  uint32_t lr;
  uint32_t spsr;
} svc_registers;

error_block Error_UnknownSWI = { 1, "Unknown SWI" };

static swi_handler get_swi_handler( uint32_t swi )
{
  swi_handler result = { .module_start = 0, .swi_handler = 0, .private = 0 };
  return result;
}

static bool run_legacy_code( svc_registers *regs, uint32_t svc, swi_handler handler )
{
  register uint32_t legacy_code asm( "r10" ) = handler.swi_handler;
  register uint32_t swi_number asm( "r11" ) = svc & 0x3f;
  register uint32_t private_word_ptr asm( "r12" ) = handler.private;
  register uint32_t failed;
  asm ( "  push { %[regs] }"
      "\n  ldm %[regs], { r0-r9 }"
      "\n  blx r10"
      "\n  pop { r10 }"
      "\n  stm r10, { r0-r9 }"
      "\n  movvs %[failed], #1"
      "\n  movvc %[failed], #0"
      : [failed] "=r" (failed)
      : [regs] "r" (regs)
      , "r" (legacy_code)
      , "r" (swi_number)
      , "r" (private_word_ptr)
      : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9" );
  return failed;
}


static bool do_OS_WriteC( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_WriteS( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Write0( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_NewLine( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

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
static bool do_OS_Heap( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Module( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Claim( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }


static bool do_OS_Release( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
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

static bool do_OS_ServiceCall( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadVduVariables( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadPoint( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_UpCall( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_CallAVector( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
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

static bool do_OS_PrettyPrint( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_Plot( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_WriteN( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_AddToVector( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_WriteEnv( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadArgs( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ReadRAMFsLimits( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ClaimDeviceVector( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ReleaseDeviceVector( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_DelinkApplication( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_RelinkApplication( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
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

static bool do_OS_ConvertStandardDateAndTime( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertDateAndTime( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertHex1( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertHex2( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertHex4( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertHex6( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertHex8( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertCardinal1( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertCardinal2( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertCardinal3( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertCardinal4( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertInteger1( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertInteger2( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertInteger3( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertInteger4( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertBinary1( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertBinary2( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertBinary3( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertBinary4( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertSpacedCardinal1( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertSpacedCardinal2( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertSpacedCardinal3( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertSpacedCardinal4( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertSpacedInteger1( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertSpacedInteger2( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertSpacedInteger3( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertSpacedInteger4( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertFixedNetStation( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertNetStation( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }
static bool do_OS_ConvertFixedFileSize( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool do_OS_ConvertFileSize( svc_registers *regs ) { regs->r[0] = Kernel_Error_UnknownSWI; return false; }

static bool Kernel_go_svc( svc_registers *regs, uint32_t svc )
{
  switch (svc) {
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

  case OS_WriteI ... OS_WriteI+255: { uint32_t r0 = regs->r[0]; regs->r[0] = svc & 0xff; do_OS_WriteC( regs ); regs->r[0] = r0; return true; }

  default: // Find a module that provides the functionality
    regs->r[0] = 0x12345678;
  };

  swi_handler handler = get_swi_handler( svc );
  if (run_legacy_code( regs, svc, handler )) {
    for (;;) asm ( "wfi" );
  }
  else {
    for (;;) asm ( "wfi" );
  }
/*
  register uint32_t swi_number asm( "r11" ) = get_swi_number( regs->lr );
  if (handler.module_start == 0) {
    // Unknown or system provider
    register uint32_t *r0to3and9;
    register uint32_t error_block asm( "r0" );
    asm ( "mov %[p], sp" : [p] "=r" (r0to3and9) );
    error_block = system_swi( r0to3and9 );
    if (error_block) {
      asm ( "  mrs r1, SPSR"
          "\n  orr r1, r1, #(1 << 28)"
          "\n  msr SPSR, r1"
          "\n  add sp, sp, #4"
          "\n  pop { r1-r3, r9-r12, lr }"
          "\n  orr lr, #(1 << 28)"
          "\n  movs pc, lr"
          :
          : "r" (error_block) );
      __builtin_unreachable();
    }
    else {
      asm ( "  mrs r1, SPSR"
          "\n  bic r1, r1, #(1 << 28)"
          "\n  msr SPSR, r1"
          "\n  pop { r0-r3, r9-r12, lr }"
          "\n  movs pc, lr" );
      __builtin_unreachable();
    }
  }

  register uint32_t module_instance asm( "r12" ) = handler.private;

  if (handler.swi_handler == 0) {
    register error_block *error asm( "r0" ) = Kernel_Error_UnknownSWI;
    asm ( "  add sp, sp, #4"
        "\n  pop { r1-r3, r9-r12, lr }"
        "\n  orr lr, #(1 << 28)"
        "\n  movs pc, lr"
        :
        : "r" (error) );
    __builtin_unreachable();
  }
  if ((swi_number & (1 << 17)) != 0) {
    swi_number &= 0x3f;
    asm ( "  mov lr, %[swi_handler]"
        "\n  pop { r0-r3, r9 }"
        "\n  blx lr"
        "\n  pop { r10-r12, lr }"
        "\n  orrvs lr, #(1 << 28)"
        "\n  bicvc lr, #(1 << 28)"
        "\n  movs pc, lr"
        : 
        : [swi_handler] "r" (handler.swi_handler)
        , "r" (swi_number)
        , "r" (module_instance) );
    __builtin_unreachable();
  }
  else {
    swi_number &= 0x3f;
    asm ( "  mov lr, %[swi_handler]"
        "\n  pop { r0-r3, r9 }"
        "\n  mov lr, pc"
        "\n  blx lr"
        "\n  pop { r10-r12, lr }"
        "\n  bicvc lr, #(1 << 28)"
        "\n  movvcs pc, lr"
        "\n  // Here's where the RISC OS error handling code goes, if X bit not set"
        "\n  wfi" // QEMU debugging: break helper_wfi
        : 
        : [swi_handler] "r" (handler.swi_handler)
        , "r" (swi_number)
        , "r" (module_instance) );
    __builtin_unreachable();
  }
*/
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

