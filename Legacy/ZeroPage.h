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

/* These are the variables used by various components of the OS that
 * are tightly coupled to the old kernel and the old kernel itself.
 *
 * In time, this should all be made to disappear.
 */

struct DANode {
  uint32_t DANode_Link; //  points to next node (in address order)
  uint32_t DANode_Number; //  number of this area
  uint32_t DANode_Base; //  base address of area (points in middle of doubly-mapped areas)
  uint32_t DANode_Flags; //  various flags
  uint32_t DANode_Size; //  current logical size of area (not counting holes, if Sparse/PMP area)
  uint32_t DANode_MaxSize; //  maximum logical size of area
  uint32_t DANode_Workspace; //  workspace pointer when calling handlers
  uint32_t DANode_Handler; //  pointer to handler routine for area
  uint32_t DANode_Title; //  pointer to area title
  uint32_t DANode_SubLink; //  next node in any disjoint sublist (currently used for Shrinkables only)
  uint32_t DANode_SparseHWM; //  high water mark, if Sparse area (highest base+size claimed for area)
  uint32_t DANode_SortLink; //  next node in alphabetically sorted list
  uint32_t DANode_PMP; //  pointer to physical memory pool - zero if not PMP or has been resized to zero
  uint32_t DANode_PMPSize; //  number of pages currently in phys pool
  uint32_t DANode_PMPMaxSize; //  size of phys memory pool, in pages
};

struct __attribute__(( packed )) OsbyteVars {
  uint8_t VarStart[2]; // &A6,&A7
  uint8_t ROMPtr[2]; // &A8,&A9
  uint8_t ROMInfo[2]; // &AA,&AB
  uint8_t KBTran[2]; // &AC,&AD
  uint8_t VDUvars[2]; // &AE,&AF

  uint8_t CFStime[1]; // &B0
  uint8_t InputStream[1]; // &B1
  uint8_t KeyBdSema[1]; // &B2

  uint8_t ROMPollSema[1]; // &B3
  uint8_t OSHWM[1]; // &B4

  uint8_t RS423mode[1]; // &B5
  uint8_t NoIgnore[1]; // &B6
  uint8_t CFSRFS[1]; // &B7
  uint8_t VULAcopy[2]; // &B8,&B9

  uint8_t ROMatBRK[1]; // &BA
  uint8_t BASICROM[1]; // &BB

  uint8_t ADCchanel[1]; // &BC
  uint8_t ADCmaxchn[1]; // &BD
  uint8_t ADCconv[1]; // &BE

  uint8_t RS423use[1]; // &BF
  uint8_t RS423conflag[1]; // &C0

  uint8_t FlashCount[1]; // &C1
  uint8_t SpacPeriod[1]; // &C2
  uint8_t MarkPeriod[1]; // &C3

  uint8_t KeyRepDelay[1]; // &C4
  uint8_t KeyRepRate[1]; // &C5

  uint8_t ExecFileH[1]; // &C6
  uint8_t SpoolFileH[1]; // &C7

  uint8_t ESCBREAK[1]; // &C8 (200)

  uint8_t KeyBdDisable[1]; // &C9
  uint8_t KeyBdStatus[1]; // &CA

  uint8_t RS423HandShake[1]; // &CB
  uint8_t RS423InputSupr[1]; // &CC
  uint8_t RS423CFSFlag[1]; // &CD

  uint8_t EconetOScall[1]; // &CE
  uint8_t EconetOSrdch[1]; // &CF
  uint8_t EconetOSwrch[1]; // &D0

  uint8_t SpeechSupr[1]; // &D1
  uint8_t SoundSupr[1]; // &D2

  uint8_t BELLchannel[1]; // &D3
  uint8_t BELLinfo[1]; // &D4
  uint8_t BELLfreq[1]; // &D5
  uint8_t BELLdur[1]; // &D6

  uint8_t StartMessSupr[1]; // &D7

  uint8_t SoftKeyLen[1]; // &D8

  uint8_t PageModeLineCount[1]; // &D9

  uint8_t VDUqueueItems[1]; // &DA

  uint8_t TABch[1]; // &DB
  uint8_t ESCch[1]; // &DC

  uint8_t IPbufferCh[4]; // &DD,&DE,&DF,&E0
  uint8_t RedKeyCh[4]; // &E1,&E2,&E3,&E4

  uint8_t ESCaction[1]; // &E5
  uint8_t ESCeffect[1]; // &E6

  uint8_t u6522IRQ[1]; // &E7
  uint8_t s6850IRQ[1]; // &E8
  uint8_t s6522IRQ[1]; // &E9

  uint8_t TubeFlag[1]; // &EA

  uint8_t SpeechFlag[1]; // &EB

  uint8_t WrchDest[1]; // &EC
  uint8_t CurEdit[1]; // &ED

  uint8_t KeyBase[1]; // &EE
  uint8_t Shadow[1]; // &EF
  uint8_t Country[1]; // &F0

  uint8_t UserFlag[1]; // &F1

  uint8_t SerULAreg[1]; // &F2

  uint8_t TimerState[1]; // &F3

  uint8_t SoftKeyConsist[1]; // &F4

  uint8_t PrinterDrivType[1]; // &F5
  uint8_t PrinterIgnore[1]; // &F6

  uint8_t BREAKvector[3]; // &F7,&F8,&F9

  uint8_t MemDriver[1]; // &FA - where the VDU drivers write to
  uint8_t MemDisplay[1]; // &FB - where we display from

  uint8_t LangROM[1]; // &FC

  uint8_t LastBREAK[1]; // &FD

  uint8_t KeyOpt[1]; // &FE

  uint8_t StartOptions[1]; // &FF

  uint8_t SerialInHandle[1]; // Handle for serial input stream  (0 if not open currently)
  uint8_t SerialOutHandle[1]; // Handle for serial output stream (-----------""----------)

        // AlignSpace

  uint8_t EventSemaphores[32]; // One byte for each of 32 events

  uint8_t TimerAlpha[8]; // As used by time (bottom 5 bytes)
  uint8_t TimerBeta[8]; // ................................
// both aligned to word boundaries

  uint8_t RealTime[8]; // 5-byte fast real-time

  uint8_t PrinterActive[4]; // Handle/active flag for printer (word aligned)

  uint8_t IntervalTimer[5]; // Up Counter synchronous with TIME.
// Event generated when Zero is reached
// bottom byte aligned to word boundary

  uint8_t SecondsTime[1]; // the soft copy (centi-)seconds of the RTC
  uint8_t CentiTime[1]; // """"""""""""""""""""""""""""""""""""""""

  uint8_t FlashState[1]; // which flash colours are we using

  uint8_t SecondsDirty[1]; // the dirty flag for start up!

  uint8_t MinTick[1]; // the minutes odd/even state

  uint8_t DCDDSRCopy[1]; // copy of ACIA bits to check for change

  uint8_t TVVertical[1]; // *TV first parameter

  uint8_t TVInterlace[1]; // *TV second parameter

  uint8_t CentiCounter[1]; // Counter for VDU CTRL timing

  uint8_t Alphabet[1]; // Current alphabet number

  uint8_t Keyboard[1]; // Current keyboard number

  uint8_t KeyAlphabet[1]; // Alphabet associated with current keyboard

  char PrinterPrefix[sizeof( "PrinterType$" )]; // -1?

  char PrinterTypeName[sizeof( "PrinterType$" )+6];

        // AlignSpace

  uint8_t SerialFlags[4]; // New serial flags

  uint8_t XONXOFFChar[1]; // Character to send before rest (0 if none)
};


typedef struct {
uint32_t vectors_and_fiq[0x40];

union {
struct {
uint32_t IRQ1V; //  &100
uint8_t  ESC_Status; //  &104
uint8_t  LatchBSoftCopy; //  &105
uint8_t  IOCControlSoftCopy; //  &106
uint8_t  CannotReset; //  &107
uint32_t IRQsema; //  &108
uint32_t MetroGnome; //  &10C
uint32_t MemorySpeed; //  &110
uint32_t MEMC_CR_SoftCopy; //  &114
uint32_t ResetIndirection; //  &118
// Now all internal definitions
// Up to here is initialized on reset
// Next come handler variables
uint32_t MemLimit;
uint32_t UndHan;
uint32_t PAbHan;
uint32_t DAbHan;
uint32_t AdXHan;
uint32_t ErrHan;
uint32_t ErrBuf;
uint32_t ErrHan_ws;
uint32_t CallAd_ws; //  smart Rs ordering:
uint32_t CallAd; //  can do LDMIA of r12, pc
uint32_t CallBf;
uint32_t BrkAd_ws;
uint32_t BrkAd;
uint32_t BrkBf;
uint32_t EscHan_ws;
uint32_t EscHan;
uint32_t EvtHan_ws;
uint32_t EvtHan;
// The next lot of workspace is in the space vacated by the small soft CAM map area
// (256 words) which is no longer adequate, so we can reuse it
// JordanWS        #       0
uint32_t Serv_SysChains; // anchor for block handling 'system' service numbers, in range 1 to 255
uint32_t Serv_UsrChains; // anchor for block handling 'user' service numbers, > 255
uint32_t Serv_AwkwardChain; // anchor for chain handling non-compliant modules (no service table)
uint32_t DAList; //  Pointer to first node on dynamic area list
                // AlignSpace 16
uint32_t al[3]; // align
uint32_t AMBControl_ws; //  workspace anchor word for AMBControl
uint32_t DynArea_ws; //  workspace anchor word for data structures to accelerate OS SWIs for dynamic areas
uint32_t Oscli_CmdHashSum; // for hashed command lookup
uint32_t Oscli_CmdHashLists; // anchor for hashed command lists structure

               // AlignSpace 16   ; skipped bit must start on 16-byte boundary (ClearPhysRAM does 4 words at a time for skipped areas)

uint32_t VideoPhysAddr; //  Address of video RAM (in the case of DRAM-only machines,
uint32_t VideoSizeFlags; //  this is actually a chunk out of DRAM)
uint32_t DRAMPhysAddrA; //  Next the DRAM
uint32_t DRAMSizeFlagsA;
uint32_t PhysRamTable[32]; // ??

uint32_t LxPTUsed; //  Amount of memory used for L2PT (short desc) or L3PT (long desc)
uint32_t SoftCamMapSize; //  Amount of memory (in bytes) used for soft CAM map
                                // (whole number of pages)
                // AlignSpace
uint32_t HAL_StartFlags;
uint32_t HAL_Descriptor;
uint32_t HAL_Workspace;
uint32_t HAL_WsSize;

uint32_t ICache_NSets;
uint32_t ICache_Size;
uint8_t  ICache_LineLen;
uint8_t  ICache_Associativity;
uint8_t  Cache_Type;
uint8_t  Cache_Flags;

uint32_t DCache_NSets;
uint32_t DCache_Size;
uint8_t  DCache_LineLen;
uint8_t  DCache_Associativity;
uint8_t  ProcessorArch;
uint8_t  ProcessorType; //  Processor type (handles 600 series onwards)
uint32_t DCache_IndexBit;
uint32_t DCache_IndexSegStart;
uint32_t DCache_RangeThreshold;
                // AlignSpace
uint32_t ProcessorFlags; //  Processor flags (IMB, Arch4 etc)
                // AlignSpace
uint32_t MMU_PPLTrans;
uint32_t MMU_PCBTrans;
uint32_t MMU_PPLAccess;
uint32_t Proc_Cache_CleanInvalidateAll;
uint32_t Proc_Cache_CleanInvalidateRange;
uint32_t Proc_Cache_CleanAll;
uint32_t Proc_Cache_CleanRange;
uint32_t Proc_Cache_InvalidateAll;
uint32_t Proc_Cache_InvalidateRange;
uint32_t Proc_Cache_RangeThreshold;
uint32_t Proc_Cache_Examine;
uint32_t Proc_ICache_InvalidateAll;
uint32_t Proc_ICache_InvalidateRange;
uint32_t Proc_TLB_InvalidateAll;
uint32_t Proc_TLB_InvalidateEntry;
uint32_t Proc_DSB_ReadWrite;
uint32_t Proc_DSB_Write;
uint32_t Proc_DSB_Read;
uint32_t Proc_DMB_ReadWrite;
uint32_t Proc_DMB_Write;
uint32_t Proc_DMB_Read;
uint32_t Proc_IMB_Full;
uint32_t Proc_IMB_Range;
uint32_t Proc_IMB_List;
uint32_t Proc_MMU_Changing;
uint32_t Proc_MMU_ChangingEntry;
uint32_t Proc_MMU_ChangingUncached;
uint32_t Proc_MMU_ChangingUncachedEntry;
uint32_t Proc_MMU_ChangingEntries;
uint32_t Proc_MMU_ChangingUncachedEntries;
uint32_t Cache_Lx_Info; //  Cache level ID register
uint32_t DCache[2];
uint32_t ICache[2];
uint32_t Cache_HALDevice; //  Pointer to any HAL cache device we're using
uint32_t IOAllocPtr; //  current lowpoint of mapped I/O space (also upper limit on DAs)
uint32_t IOAllocLimit; //  current lowest allowed I/O space (DA creation may move this up)
uint32_t IOAllocTop; //  high end of I/O space
uint32_t PhysIllegalMask; //  mask of invalid bits in upper word of physical addresses
};
uint32_t block1[0x80];
};

uint32_t  CompatibilityPageEnabled; //  0 or 1 as appropriate, a byte, but use a word to align
                // AlignSpace
// IICBus_Count       *    5 ; 5 buses is enough for all current machines
uint32_t IICBus_Base[5][3];        // #    IICBus_Size*IICBus_Count
uint32_t PageTable_PageFlags; //  Page flags used for page tables. L2PT uses this directly, L1PT adds in PageFlags_Unavailable.
                // AlignSpace 16   ; skipped bit must end on 16-byte boundary (ClearPhysRAM does 4 words at a time for skipped areas)
// SkippedTablesEnd #      0
// NVRAM support
uint8_t  NVRamSize; //  Size of NVRam (E2ROM & CMOS) fitted in 256byte units
uint8_t  NVRamBase; //  Base of NVRam
uint8_t  NVRamSpeed; //  Clock hold time in 0.5us units
uint8_t  NVRamPageSize; //  Page size for writing (log2)
uint8_t  NVRamWriteSize; //  Size of writable region (256byte units)
                   // AlignSpace
struct DANode AppSpaceDANode; // Dummy area node for application space (not on list)
struct DANode FreePoolDANode; // Area node for free pool
struct DANode SysHeapDANode; // Area node for system heap
uint32_t CDASemaphore; //  Semaphore for OS_ChangeDynamicArea - non-zero => routine threaded
uint32_t MMUControlSoftCopy; //  Soft copy of ARM control register
uint32_t IRQMax; //  from HAL_IRQMax
uint32_t DeviceCount; //  size of our table of devices in the system heap
uint32_t DeviceTable; //  pointer to table

// Unused
uint32_t ProcVec_Branch0; //  Branch through zero
uint32_t ProcVec_UndInst; //  Undefined instruction vector
uint32_t ProcVec_SWI; //  SWI vector
uint32_t ProcVec_PrefAb; //  Prefetch abort vector
uint32_t ProcVec_DataAb; //  Data abort vector
uint32_t ProcVec_AddrEx; //  not used (was Address exception vector on 26-bit-only ARMs)
uint32_t ProcVec_IRQ; //  IRQ vector
uint32_t ProcVecPreVeneers[4];

uint32_t ExtendedROMFooter; //  Pointer to the extended ROM footer structure. 0 if not initialised, -1 if not found.
uint32_t CPUFeatures[2];

uint8_t free[0xb4]; // e0]; // Kernel/hdr/ExportVals/values

uint32_t CamMapCorruptDebugBlock[16]; // somewhere to dump registers in case of emergency
uint32_t MaxCamEntry32; //  maximum index into the cam map which has a
                                // 32bit physical address, for easy detection by
                                // page number (all RAM banks with 32bit
                                // addresses come first)
uint8_t padding[0x24]; // Kernel/hdr/ExportVals/values

uint32_t CamEntriesPointer; //  points to where CAM soft copy is
uint32_t MaxCamEntry; //  maximum index into the cam map, ie
                                // 511 for 16MByte machines, 383 for 12MBytes
                                // 255 for 8MBytes, otherwise 127
uint32_t RAMLIMIT; //  Number of pages of RAM
uint32_t ROMPhysAddr;
uint32_t HiServ_ws;
uint32_t HiServ;
uint32_t SExitA;
uint32_t SExitA_ws;
uint32_t UpCallHan_ws;
uint32_t UpCallHan;
uint32_t ROMModuleChain; //  pointer to head of ROM module chain
// now a section that it's handy to have in simply loadable places
             // AlignSpace 16
uint8_t KeyWorkSpace[0x200];

uint32_t ChocolateCBBlocks; //  -> array of quick access blocks for Callback
uint32_t ChocolateSVBlocks; //  -> array of quick access blocks for software vectors
uint32_t ChocolateTKBlocks; //  -> array of quick access blocks for tickers
uint32_t ChocolateMRBlocks; //  -> array of blocks for ROM module nodes (reduces no. of individual blocks in heap)
uint32_t ChocolateMABlocks; //  -> array of blocks for active module nodes (reduces no. of individual blocks in heap)
uint32_t ChocolateMSBlocks; //  -> array of blocks for module SWI hash nodes (reduces no. of individual blocks in heap)
// !!!! Free Space (40 bytes)
uint8_t OldSWIHashspace[10];
uint32_t Module_List;
uint32_t Curr_Active_Object;
// Vector Claim & Release tables etc
uint32_t VecPtrTab[96];
uint32_t ExceptionDump;
uint8_t spare[68];
            // AlignSpace  16 ; Ensures we can MOV rn, #OsbyteVars if <=&1000
struct OsbyteVars OsbyteVars;
                            // (and stored in) OS_Bytes &A6,&A7. SKS

uint32_t BuffInPtrs[10];
uint32_t BuffOutPtrs[10];

uint32_t VariableList;
// Oscli stuff
uint32_t OscliCBtopUID;
uint32_t OscliCBbotUID;
uint32_t OscliCBcurrend;
uint32_t ReturnCode;
uint32_t RCLimit;
uint32_t SpriteSize; //  saved on startup for Sprite code
uint32_t TickNodeChain;
uint32_t PIRQ_Chain;
uint32_t PFIQasIRQ_Chain;
// Workspace
uint8_t  EnvTime[5];
uint8_t  RedirectInHandle;
uint8_t  RedirectOutHandle;
uint8_t  MOShasFIQ;
uint8_t  FIQclaim_interlock;
uint8_t  CallBack_Flag;

uint8_t  MonitorLeadType; //  some function of the monitor lead inputs, as yet undetermined
uint8_t  MentionCMOSReset; //  non zero reports CMOS resets prior to the start banner
                // AlignSpace
uint32_t DUMPER[17];
uint32_t removed_PxxxIRQ_Chain;
uint32_t Page_Size;
uint8_t CMOSRAMCache[256];

uint8_t ModuleSHT_Padding0[12];
uint32_t ModuleSWI_HashTab[128];
uint32_t SysVars_StickyPointers[11];

uint32_t Abort32_dumparea[6]; // ;info for OS_ReadSysInfo 7 - 32-bit PSR, fault address, 32-bit PC (room for two copies)

uint32_t Help_guard; // for *help, guard against foreground re-entrancy (multiple taskwindows)
uint32_t Help_msgdescr[4]; // for *help, 4 words MessageTrans descriptor
uint32_t PCI_status; // bit 0 = 1 if PCI exists or 0 if PCI does not exist, bits 1..31 reserved (0)
uint32_t IOMD_NoInterrupt; // no. of irq devices for extant IOMD
uint32_t IOMD_DefaultIRQ1Vcode; // default irq code start address (ROM) for extant IOMD
uint32_t IOMD_DefaultIRQ1Vcode_end; // default irq code end address (ROM)
uint32_t IOMD_Devices; // default irq devices table address (ROM)

uint8_t ModuleSHT_Padding1[752-12-4*128-11*4-6*4-5*4-4-4*4];
//was:
//OldIRQ1Vspace       # 752
uint32_t CallBack_Vector;
// interruptible heap manager workspace
uint32_t HeapSavedReg_R0;
uint32_t HeapSavedReg_R1;
uint32_t HeapSavedReg_R2;
uint32_t HeapSavedReg_R3;
uint32_t HeapSavedReg_R4;
uint32_t HeapSavedReg_R5;
uint32_t HeapSavedReg_R13;
uint32_t HeapReturnedReg_R0;
uint32_t HeapReturnedReg_R1;
uint32_t HeapReturnedReg_R2;
uint32_t HeapReturnedReg_R3;
uint32_t HeapReturnedReg_R4;
uint32_t HeapReturnedReg_R5;
uint32_t HeapReturnedReg_R13;
uint32_t HeapReturnedReg_PSR; //  also acts as interlock
uint8_t RawMachineID[8];//        #  8                ; 64 bits for unique machine ID
uint8_t KernelMessagesBlock[20]; //               ; 5 Words for messagetrans message block.
uint8_t  ErrorSemaphore; //  Error semaphore to avoid looping on error translation.
uint8_t  PortableFlags; // 
        // AlignSpace
uint8_t MOSConvertBuffer[12]; //               ; Enough romm for 8 hex digits.
uint32_t AbortIndirection; //  Pointer to list of addresses and trap routines
uint32_t PreVeneerRegDump[17]; //    #  17*4             ; room for r0-r15, spsr
uint32_t CachedErrorBlocks; //  pointer to sysheap node holding the error block cache
uint32_t PrinterBufferAddr; //  holds address of printer buffer
uint32_t PrinterBufferSize; //  size of printer buffer - not to be confused with PrintBuffSize
                            // which is the (constant) default size for the MOS's smallish buffer

uint8_t pad_to_fe8[0xfe8 - 0xfd8];

// Words for old tools of assorted varieties
// Don't move the following as their positions are assumed by other modules
//                        ^       &FE8
uint8_t  CLibCounter; //  Counter for Shared C Library tmpnam function
        // AlignSpace
// ECN 17-Feb-92
// Added RISCOSLibWord and CLibWord. The ROM RISCOSLib and CLib must continue
// to work even when they are killed since ROM apps are hard linked to the
// ROM libraries. They cannot use the private word since the block pointed
// to by this will be freed.
uint32_t RISCOSLibWord;
uint32_t CLibWord;
uint32_t FPEAnchor;
uint32_t DomainId; //  SKS added for domain identification
uint32_t Modula2_Private; //  MICK has FFC and uses it it in USR mode
uint8_t VduDriverWorkSpace[0x3000];
uint32_t DebuggerSpace[1024]; 
} LegacyZeroPage;

