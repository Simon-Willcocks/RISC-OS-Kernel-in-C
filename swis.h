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
 
