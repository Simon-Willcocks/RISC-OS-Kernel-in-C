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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned        bool;
#define true  (0 == 0)
#define false (0 != 0)

#define number_of( arr ) (sizeof( arr ) / sizeof( arr[0] ))

typedef struct core_workspace core_workspace;
typedef struct shared_workspace shared_workspace;

typedef struct variable variable;

struct Kernel_workspace {
  variable *variables; // Should be shared?
};

struct Kernel_shared_workspace {
};

extern struct core_workspace {
  uint32_t core_number;
  struct Kernel_workspace kernel;
} workspace;

extern struct shared_workspace {
  struct Kernel_shared_workspace kernel;
} volatile shared;

static const uint32_t NF = (1 << 31);
static const uint32_t ZF = (1 << 30);
static const uint32_t CF = (1 << 29);
static const uint32_t VF = (1 << 28);

static const uint32_t Xbit = (1 << 17);

typedef struct __attribute__(( packed )) {
  uint32_t r[13];
  uint32_t lr;
  uint32_t spsr;
} svc_registers;

enum {
/* 00 */ OS_WriteC, OS_WriteS, OS_Write0, OS_NewLine,
/* 04 */ OS_ReadC, OS_CLI, OS_Byte, OS_Word,
/* 08 */ OS_File, OS_Args, OS_BGet, OS_BPut,
/* 0c */ OS_GBPB, OS_Find, OS_ReadLine, OS_Control,
/* 10 */ OS_GetEnv, OS_Exit, OS_SetEnv, OS_IntOn,
/* 14 */ OS_IntOff, OS_CallBack, OS_EnterOS, OS_BreakPt,
/* 18 */ OS_BreakCtrl, OS_UnusedSWI, OS_UpdateMEMC, OS_SetCallBack,
/* 1c */ OS_Mouse, OS_Heap, OS_Module, OS_Claim,

/* 20 */ OS_Release, OS_ReadUnsigned, OS_GenerateEvent, OS_ReadVarVal,
/* 24 */ OS_SetVarVal, OS_GSInit, OS_GSRead, OS_GSTrans,
/* 28 */ OS_BinaryToDecimal, OS_FSControl, OS_ChangeDynamicArea, OS_GenerateError,
/* 2c */ OS_ReadEscapeState, OS_EvaluateExpression, OS_SpriteOp, OS_ReadPalette,
/* 30 */ OS_ServiceCall, OS_ReadVduVariables, OS_ReadPoint, OS_UpCall,
/* 34 */ OS_CallAVector, OS_ReadModeVariable, OS_RemoveCursors, OS_RestoreCursors,
/* 38 */ OS_SWINumberToString, OS_SWINumberFromString, OS_ValidateAddress, OS_CallAfter,
/* 3c */ OS_CallEvery, OS_RemoveTickerEvent, OS_InstallKeyHandler, OS_CheckModeValid,

/* 40 */ OS_ChangeEnvironment, OS_ClaimScreenMemory, OS_ReadMonotonicTime, OS_SubstituteArgs,
/* 44 */ OS_PrettyPrint, OS_Plot, OS_WriteN, OS_AddToVector,
/* 48 */ OS_WriteEnv, OS_ReadArgs, OS_ReadRAMFsLimits, OS_ClaimDeviceVector,
/* 4c */ OS_ReleaseDeviceVector, OS_DelinkApplication, OS_RelinkApplication, OS_HeapSort,
/* 50 */ OS_ExitAndDie, OS_ReadMemMapInfo, OS_ReadMemMapEntries, OS_SetMemMapEntries,
/* 54 */ OS_AddCallBack, OS_ReadDefaultHandler, OS_SetECFOrigin, OS_SerialOp,

/* 58 */ OS_ReadSysInfo, OS_Confirm, OS_ChangedBox, OS_CRC,
/* 5c */ OS_ReadDynamicArea, OS_PrintChar, OS_ChangeRedirection, OS_RemoveCallBack,

/* 60 */ OS_FindMemMapEntries, OS_SetColour, OS_Pointer, OS_ScreenMode,
/* 64 */ OS_DynamicArea, OS_Memory, OS_ClaimProcessorVector, OS_Reset,
/* 68 */ OS_MMUControl,
 
/* c0 */ OS_ConvertStandardDateAndTime = 0xc0, OS_ConvertDateAndTime,
/* d0 */ OS_ConvertHex1 = 0xd0, OS_ConvertHex2, OS_ConvertHex4, OS_ConvertHex6,
/* d4 */ OS_ConvertHex8, OS_ConvertCardinal1, OS_ConvertCardinal2, OS_ConvertCardinal3,
/* d8 */ OS_ConvertCardinal4, OS_ConvertInteger1, OS_ConvertInteger2, OS_ConvertInteger3,
/* dc */ OS_ConvertInteger4, OS_ConvertBinary1, OS_ConvertBinary2, OS_ConvertBinary3,
/* e0 */ OS_ConvertBinary4, OS_ConvertSpacedCardinal1, OS_ConvertSpacedCardinal2, OS_ConvertSpacedCardinal3,
/* e4 */ OS_ConvertSpacedCardinal4, OS_ConvertSpacedInteger1, OS_ConvertSpacedInteger2, OS_ConvertSpacedInteger3,
/* e8 */ OS_ConvertSpacedInteger4, OS_ConvertFixedNetStation, OS_ConvertNetStation, OS_ConvertFixedFileSize,
/* ec */ OS_ConvertFileSize,
/* 100-1ff */ OS_WriteI = 0x100 };
 
// OS SWIs implemented other than in swis.c:

// Implemented in os_heap.c:
bool do_OS_Heap( svc_registers *regs );

// modules.c:
bool do_OS_Module( svc_registers *regs );
bool do_OS_ServiceCall( svc_registers *regs );

bool do_OS_CallAVector( svc_registers *regs );
bool do_OS_Claim( svc_registers *regs );
bool do_OS_Release( svc_registers *regs );
bool do_OS_AddToVector( svc_registers *regs );
bool do_OS_DelinkApplication( svc_registers *regs );
bool do_OS_RelinkApplication( svc_registers *regs );
bool do_OS_GetEnv( svc_registers *regs );

// Vectored SWIs (do nothing but call the appropriate vectors)
bool do_OS_Find( svc_registers *regs );

// swis/os_fscontrol.c
bool do_OS_FSControl( svc_registers *regs );

// memory/

bool do_OS_ChangeDynamicArea( svc_registers *regs );
bool do_OS_ReadDynamicArea( svc_registers *regs );
bool do_OS_DynamicArea( svc_registers *regs );

// swis/vdu.c
void SetInitialVduVars();
bool do_OS_ChangedBox( svc_registers *regs );
bool do_OS_ReadVduVariables( svc_registers *regs );
bool do_OS_ReadPoint( svc_registers *regs );
bool do_OS_ReadModeVariable( svc_registers *regs );
bool do_OS_RemoveCursors( svc_registers *regs );
bool do_OS_RestoreCursors( svc_registers *regs );


// swis/varvals.c
enum { VarType_String = 0,
       VarType_Number,
       VarType_Macro,
       VarType_Expanded,
       VarType_LiteralString,
       VarType_Code = 16 } VarTypes;
bool do_OS_ReadVarVal( svc_registers *regs );
bool do_OS_SetVarVal( svc_registers *regs );

void *rma_allocate( uint32_t size, svc_registers *regs );

typedef struct {
  uint32_t code;
  char text[];
} error_block;
