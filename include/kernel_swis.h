/* Copyright 2022 Simon Willcocks
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

#ifndef __RISCOS_KERNEL_SWIS
#define __RISCOS_KERNEL_SWIS

static const uint32_t NF = (1 << 31);
static const uint32_t ZF = (1 << 30);
static const uint32_t CF = (1 << 29);
static const uint32_t VF = (1 << 28);

static const uint32_t Xbit = (1 << 17);

// Copy of the registers stored for an SVC instruction; doesn't include
// the user stack pointer, or link registers.
typedef struct __attribute__(( packed )) svc_registers {
  uint32_t r[13];
  uint32_t lr;
  uint32_t spsr;
} svc_registers;

enum VarTypes { VarType_String = 0,
                VarType_Number,
                VarType_Macro,
                VarType_Expanded,
                VarType_LiteralString,
                VarType_Code = 16,
                VarType_None = 31 }; // internal use only

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

/* 60 */ OS_FindMemMapEntries, OS_SetColour,
/* 64 */ OS_Pointer = 0x64, OS_ScreenMode, OS_DynamicArea,
/* 68 */ OS_Memory = 0x68, OS_ClaimProcessorVector, OS_Reset, OS_MMUControl,
/* 6c */ OS_ResyncTime, OS_PlatformFeatures, OS_SynchroniseCodeAreas, OS_CallASWI,
/* 70 */ OS_AMBControl, OS_CallASWIR12, OS_SpecialControl, OS_EnterUSR32,
/* 74 */ OS_EnterUSR26, OS_VIDCDivider, OS_NVMemory,
         OS_Hardware = 0x7a, OS_IICOp, 
/* 7c */ OS_LeaveOS, OS_ReadLine32, OS_SubstituteArgs32, OS_HeapSort32,
 
/* c0 */ OS_ConvertStandardDateAndTime = 0xc0, OS_ConvertDateAndTime,
/* d0 */ OS_ConvertHex1 = 0xd0, OS_ConvertHex2, OS_ConvertHex4, OS_ConvertHex6,
/* d4 */ OS_ConvertHex8, OS_ConvertCardinal1, OS_ConvertCardinal2, OS_ConvertCardinal3,
/* d8 */ OS_ConvertCardinal4, OS_ConvertInteger1, OS_ConvertInteger2, OS_ConvertInteger3,
/* dc */ OS_ConvertInteger4, OS_ConvertBinary1, OS_ConvertBinary2, OS_ConvertBinary3,
/* e0 */ OS_ConvertBinary4, OS_ConvertSpacedCardinal1, OS_ConvertSpacedCardinal2, OS_ConvertSpacedCardinal3,
/* e4 */ OS_ConvertSpacedCardinal4, OS_ConvertSpacedInteger1, OS_ConvertSpacedInteger2, OS_ConvertSpacedInteger3,
/* e8 */ OS_ConvertSpacedInteger4, OS_ConvertFixedNetStation, OS_ConvertNetStation, OS_ConvertFixedFileSize,
/* ec */ OS_ConvertFileSize,

// New SWIs for C kernel, if they duplicate another solution, one or the other approach may be discarded.
/* f8 */ OS_MSTime = 0xf8, OS_VduCommand = 0xfb, // update the current graphics state for this task (not really, fixme)
/* fc */ OS_LockForDMA = 0xfc, OS_ReleaseDMALock, OS_MapDevicePages, OS_FlushCache, // For screen updates, etc.

/* 100-1ff */ OS_WriteI = 0x100,

         OSTask_NumberOfCores = 0x300, // Fails (no such...) if no support for multi-tasking
         // FIXME: have a block of function pointers that can be used by user code
         OSTask_RegisterSWITargets,

         OSTask_CreateTask,
         OSTask_CreateTaskSeparate,
         OSTask_Exit,

         OSTask_Sleep,
         OSTask_WaitUntilWoken,
         OSTask_Wake,

         OSTask_GetHandle,
         OSTask_LockClaim,      // UNTESTED! (Maybe unwritten?)
         OSTask_LockRelease,    // UNTESTED! (Maybe unwritten?)

         OSTask_AppMemoryTop,   // r0 = new top (>= 0x8000), or 0 to read

         OSTask_RelinquishControl,
         OSTask_RunThisForMe,   // Get the Task to run code in its context, return when it relinquishes control back to this Task
         OSTask_ReleaseTask,    // Resume the controlled Task with a new context
         OSTask_GetRegisters,
         OSTask_SetRegisters,
         OSTask_ChildCommand,
         OSTask_ResumeParent,

         OSTask_WaitForInterrupt,
         OSTask_InterruptIsOff,
         OSTask_NumberOfInterruptSources,
  
         OSTask_DebugString, OSTask_DebugNumber, OSTask_DebugShowTasks,
         OSTask_GetDebugPipe,
  
         OSTask_CoreNumber, OSTask_CoreNumberString,

         OSTask_CallLegacySWI,     // For internal use only
  
         OSTask_QueueCreate = OSTask_NumberOfCores + 32,
         OSTask_QueueWait,
         OSTask_QueueWaitCore,
         OSTask_QueueWaitSWI,
         OSTask_QueueWaitCoreAndSWI,

         OSTask_PipeCreate = OSTask_NumberOfCores + 48,
         OSTask_PipeWaitForSpace,  // Block task until N bytes may be written
         OSTask_PipeSpaceFilled,   // I've filled this many bytes
         OSTask_PipeSetSender,     // Another task is going to take over filling this pipe
         OSTask_PipeUnreadData,    // Useful, in case data can be dropped or consolidated (e.g. mouse movements)
         OSTask_PipeNoMoreData,    // I'm done filling the pipe
         OSTask_PipeWaitForData,   // Block task until N bytes may be read (or WaitUntilEmpty, NoMoreData called)
         OSTask_PipeDataConsumed,  // I don't need the first N bytes that were written any more
         OSTask_PipeSetReceiver,   // Another task is going to take over listening at this pipe
         OSTask_PipeNotListening,  // I don't want any more data, thanks
         OSTask_PipeWaitUntilEmpty,// Block task until all bytes have been consumed TODO?
};

#endif

