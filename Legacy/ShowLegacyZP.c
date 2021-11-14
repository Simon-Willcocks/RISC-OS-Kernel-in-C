typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

#include "ZeroPage.h"
#include <stdio.h>

#define SHOW( e ) printf( #e"\t%04x\t0x%x\n", (uint32_t) (long) &lzp.e - (uint32_t) (long) &lzp, (uint32_t) sizeof( lzp.e ) );

int main()
{
LegacyZeroPage lzp;

SHOW( IRQ1V );
SHOW( ESC_Status );
SHOW( LatchBSoftCopy );
SHOW( IOCControlSoftCopy );
SHOW( CannotReset );
SHOW( IRQsema );
SHOW( MetroGnome );
SHOW( MemorySpeed );
SHOW( MEMC_CR_SoftCopy );
SHOW( ResetIndirection );

SHOW( MemLimit );
SHOW( UndHan );
SHOW( PAbHan );
SHOW( DAbHan );
SHOW( AdXHan );
SHOW( ErrHan );
SHOW( ErrBuf );
SHOW( ErrHan_ws );
SHOW( CallAd_ws );
SHOW( CallAd );
SHOW( CallBf );
SHOW( BrkAd_ws );
SHOW( BrkAd );
SHOW( BrkBf );
SHOW( EscHan_ws );
SHOW( EscHan );
SHOW( EvtHan_ws );
SHOW( EvtHan );

SHOW( Serv_SysChains );
SHOW( Serv_UsrChains );
SHOW( Serv_AwkwardChain );
SHOW( DAList );
printf( "AlignSpace 16\n" );
SHOW( AMBControl_ws );
SHOW( DynArea_ws );
SHOW( Oscli_CmdHashSum );
SHOW( Oscli_CmdHashLists );

printf( "AlignSpace 16   ; skipped bit must start on 16-byte boundary (ClearPhysRAM does 4 words at a time for skipped areas)\n" );

SHOW( VideoPhysAddr );
SHOW( VideoSizeFlags );
SHOW( DRAMPhysAddrA );
SHOW( DRAMSizeFlagsA );
SHOW( PhysRamTable );
SHOW( LxPTUsed );
SHOW( SoftCamMapSize );
                                // (whole number of pages)
printf( "AlignSpace\n" );
SHOW( HAL_StartFlags );
SHOW( HAL_Descriptor );
SHOW( HAL_Workspace );
SHOW( HAL_WsSize );

SHOW( ICache_NSets );
SHOW( ICache_Size );
SHOW( ICache_LineLen );
SHOW( ICache_Associativity );
SHOW( Cache_Type );
SHOW( Cache_Flags );

SHOW( DCache_NSets );
SHOW( DCache_Size );
SHOW( DCache_LineLen );
SHOW( DCache_Associativity );
SHOW( ProcessorArch );
SHOW( ProcessorType );

SHOW( DCache_IndexBit );

SHOW( DCache_IndexSegStart );
SHOW( DCache_RangeThreshold );
printf( "AlignSpace\n" );
SHOW( ProcessorFlags );
printf( "AlignSpace\n" );
SHOW( MMU_PPLTrans );
SHOW( MMU_PCBTrans );
SHOW( MMU_PPLAccess );
SHOW( Proc_Cache_CleanInvalidateAll );
SHOW( Proc_Cache_CleanInvalidateRange );
SHOW( Proc_Cache_CleanAll );
SHOW( Proc_Cache_CleanRange );
SHOW( Proc_Cache_InvalidateAll );
SHOW( Proc_Cache_InvalidateRange );
SHOW( Proc_Cache_RangeThreshold );
SHOW( Proc_Cache_Examine );
SHOW( Proc_ICache_InvalidateAll );
SHOW( Proc_ICache_InvalidateRange );
SHOW( Proc_TLB_InvalidateAll );
SHOW( Proc_TLB_InvalidateEntry );
SHOW( Proc_DSB_ReadWrite );
SHOW( Proc_DSB_Write );
SHOW( Proc_DSB_Read );
SHOW( Proc_DMB_ReadWrite );
SHOW( Proc_DMB_Write );
SHOW( Proc_DMB_Read );
SHOW( Proc_IMB_Full );
SHOW( Proc_IMB_Range );
SHOW( Proc_IMB_List );
SHOW( Proc_MMU_Changing );
SHOW( Proc_MMU_ChangingEntry );
SHOW( Proc_MMU_ChangingUncached );
SHOW( Proc_MMU_ChangingUncachedEntry );
SHOW( Proc_MMU_ChangingEntries );
SHOW( Proc_MMU_ChangingUncachedEntries );
SHOW( Cache_Lx_Info );
SHOW( DCache[2] );
SHOW( ICache[2] );
SHOW( Cache_HALDevice );
SHOW( IOAllocPtr );
SHOW( IOAllocLimit );
SHOW( IOAllocTop );
SHOW( PhysIllegalMask );
SHOW( DebuggerSpace );
SHOW( CompatibilityPageEnabled );
printf( "AlignSpace\n" );
SHOW( IICBus_Base );
SHOW( PageTable_PageFlags );
printf( "AlignSpace 16   ; skipped bit must end on 16-byte boundary (ClearPhysRAM does 4 words at a time for skipped areas)\n" );
// NVRAM support
SHOW( NVRamSize );
SHOW( NVRamBase );
SHOW( NVRamSpeed );
SHOW( NVRamPageSize );
SHOW( NVRamWriteSize );
printf( "AlignSpace\n" );
SHOW( AppSpaceDANode );
SHOW( FreePoolDANode );
SHOW( SysHeapDANode );
SHOW( CDASemaphore );
SHOW( MMUControlSoftCopy );
SHOW( IRQMax );
SHOW( DeviceCount );
SHOW( DeviceTable );

SHOW( ProcVec_Branch0 );
SHOW( ProcVec_UndInst );
SHOW( ProcVec_SWI );
SHOW( ProcVec_PrefAb );
SHOW( ProcVec_DataAb );
SHOW( ProcVec_AddrEx );
SHOW( ProcVec_IRQ );
SHOW( ProcVecPreVeneers );
SHOW( ExtendedROMFooter );
SHOW( CPUFeatures );
SHOW( CamMapCorruptDebugBlock );
SHOW( MaxCamEntry32 );
SHOW( CamEntriesPointer );
SHOW( MaxCamEntry );
SHOW( RAMLIMIT );
SHOW( ROMPhysAddr );
SHOW( HiServ_ws );
SHOW( HiServ );
SHOW( SExitA );
SHOW( SExitA_ws );
SHOW( UpCallHan_ws );
SHOW( UpCallHan );
SHOW( ROMModuleChain );
printf( "AlignSpace 16\n" );
SHOW( KeyWorkSpace );

SHOW( ChocolateCBBlocks );
SHOW( ChocolateSVBlocks );
SHOW( ChocolateTKBlocks );
SHOW( ChocolateMRBlocks );
SHOW( ChocolateMABlocks );
SHOW( ChocolateMSBlocks );

SHOW( OldSWIHashspace );

SHOW( Module_List );
SHOW( Curr_Active_Object );

SHOW( VecPtrTab );
SHOW( ExceptionDump );
printf( "AlignSpace  16 ; Ensures we can MOV rn, #OsbyteVars if <=&1000\n" );
SHOW( OsbyteVars );
                            // (and stored in) OS_Bytes &A6,&A7. SKS
SHOW( BuffInPtrs );
SHOW( BuffOutPtrs );

SHOW( VariableList );
// Oscli stuff
SHOW( OscliCBtopUID );
SHOW( OscliCBbotUID );
SHOW( OscliCBcurrend );
SHOW( ReturnCode );
SHOW( RCLimit );
SHOW( SpriteSize );
SHOW( TickNodeChain );
SHOW( PIRQ_Chain );
SHOW( PFIQasIRQ_Chain );
// Workspace
SHOW( EnvTime );
SHOW( RedirectInHandle );
SHOW( RedirectOutHandle );
SHOW( MOShasFIQ );
SHOW( FIQclaim_interlock );
SHOW( CallBack_Flag );

SHOW( MonitorLeadType );
SHOW( MentionCMOSReset );
printf( "AlignSpace\n" );
SHOW( DUMPER );
SHOW( removed_PxxxIRQ_Chain );
SHOW( Page_Size );
SHOW( CMOSRAMCache );

SHOW( ModuleSHT_Padding0 );
SHOW( ModuleSWI_HashTab );
SHOW( SysVars_StickyPointers );

SHOW( Abort32_dumparea );
SHOW( Help_guard );
SHOW( Help_msgdescr );
SHOW( PCI_status );
SHOW( IOMD_NoInterrupt );
SHOW( IOMD_DefaultIRQ1Vcode );
SHOW( IOMD_DefaultIRQ1Vcode_end );
SHOW( IOMD_Devices );
SHOW( ModuleSHT_Padding1 );
SHOW( CallBack_Vector );

// interruptible heap manager workspace
SHOW( HeapSavedReg_R0 );
SHOW( HeapSavedReg_R1 );
SHOW( HeapSavedReg_R2 );
SHOW( HeapSavedReg_R3 );
SHOW( HeapSavedReg_R4 );
SHOW( HeapSavedReg_R5 );
SHOW( HeapSavedReg_R13 );
SHOW( HeapReturnedReg_R0 );
SHOW( HeapReturnedReg_R1 );
SHOW( HeapReturnedReg_R2 );
SHOW( HeapReturnedReg_R3 );
SHOW( HeapReturnedReg_R4 );
SHOW( HeapReturnedReg_R5 );
SHOW( HeapReturnedReg_R13 );
SHOW( HeapReturnedReg_PSR );
SHOW( RawMachineID );
SHOW( KernelMessagesBlock );
SHOW( ErrorSemaphore );
SHOW( PortableFlags );
printf( "AlignSpace\n" );
SHOW( MOSConvertBuffer );
SHOW( AbortIndirection );
SHOW( PreVeneerRegDump );
SHOW( CachedErrorBlocks );
SHOW( PrinterBufferAddr );
SHOW( PrinterBufferSize );
                            // which is the (constant) default size for the MOS's smallish buffer
// Words for old tools of assorted varieties
// Don't move the following as their positions are assumed by other modules
                        // ^       &FE8
SHOW( CLibCounter );
printf( "AlignSpace\n" );
// ECN 17-Feb-92
// Added RISCOSLibWord and CLibWord. The ROM RISCOSLib and CLib must continue
// to work even when they are killed since ROM apps are hard linked to the
// ROM libraries. They cannot use the private word since the block pointed
// to by this will be freed.
SHOW( RISCOSLibWord );
SHOW( CLibWord );
SHOW( FPEAnchor );
SHOW( DomainId );
SHOW( Modula2_Private );
SHOW( VduDriverWorkSpace );
SHOW( DebuggerSpace );
return 0;
}

/*

; Copyright 1996 Acorn Computers Ltd
;
; Licensed under the Apache License, Version 2.0 (the "License");
; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
;     http://www.apache.org/licenses/LICENSE-2.0
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS,
; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
; See the License for the specific language governing permissions and
; limitations under the License.
;
ARM stand alone Macro Assembler Version 2.00
AMBControl_ws  at  00000180
ARMA_Cleaner_flipflop at 00000184
SyncCodeA_sema (byte) at 00000188
Oscli_CmdHashSum      at 0000018C
Oscli_CmdHashLists    at 00000190
Serv_SysChains        at 00000194
Serv_UsrChains        at 00000198
Serv_AwkwardChain     at 0000019C
VideoPhysAddr held at  000001A0
LCD_Active flag byte held at  00000259
ProcessorType  at  0000025B
ProcessorFlags at  0000025C
Free space after ProcVec = 00000020
Free space after EnvString = 000000E0
Free space after CamMap debug block = 00000024
KeyWorkSpace          at 00000590
ChocolateCBBlocks     at 00000790
ChocolateSVBlocks     at 00000794
ChocolateTKBlocks     at 00000798
ChocolateMRBlocks     at 0000079C
ChocolateMABlocks     at 000007A0
ChocolateMSBlocks     at 000007A4
Module_List           at 000007D0
ModuleSWI_HashTab     at 00000C40
SysVars_StickyPtrs    at 00000E40
Abort32_dumparea      at 00000E6C
Help_guard            at 00000E84
PCI_status            at 00000E98
IOMD_NoInterrupt      at 00000E9C

**WARNING** compiling in code to trace some SysHeap node statistics (mjsSysHeapNodesTrace TRUE)

mjsSHNodesTrace_ws    at 00000EAC
Label Export_BgEcfOraEor has the value &000004C0
Label Export_FgEcfOraEor has the value &00000480
Label Export_BranchToSWIExit has the value &01F037FC
Label Export_DomainId has the value &00000FF8
Label Export_ESC_Status has the value &00000104
Label Export_IRQsema has the value &00000108
Label Export_LatchBSoftCopy has the value &00000105
Label Export_MEMC_CR_SoftCopy has the value &00000114
Label Export_RedirectInHandle has the value &00000AE1
Label Export_RedirectOutHandle has the value &00000AE2
Label Export_ScratchSpace has the value &00004000
Label ScratchSpaceSize has the value &00004000
Label Export_SoundDMABuffers has the value &01F06000
Label Export_SoundDMABufferSize has the value &00001000
Label Export_SoundWorkSpace has the value &01F04000
Label Export_SVCSTK has the value &01C02000
Label Export_SvcTable has the value &01F033FC
Label Export_SysHeapStart has the value &01C02000
Label Export_VduDriverWorkSpace has the value &00001000
Label VDWSSize has the value &00003000
Label ScreenBlankFlag has the value &0000047C
Label ScreenBlankDPMSState has the value &0000047D
*/
