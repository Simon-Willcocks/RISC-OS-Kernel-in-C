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

bool Kernel_Error_UnknownSWI( svc_registers *regs )
{
  static error_block error = { 0x1e6, "Unknown SWI" }; // Could be "SWI name not known", or "SWI &3333 not known"
  regs->r[0] = (uint32_t) &error;
  return false;
}

bool Kernel_Error_UnimplementedSWI( svc_registers *regs )
{
  static error_block error = { 0x999, "Unimplemented SWI" };
  regs->r[0] = (uint32_t) &error;

  asm ( "bkpt 77" );
  return false;
}

static uint32_t word_align( void *p )
{
  return (((uint32_t) p) + 3) & ~3;
}

static bool do_OS_WriteS( svc_registers *regs )
{
  char *s = (void*) regs->lr;
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

static bool do_OS_WriteN( svc_registers *regs )
{
  const char *string = (void*) regs->r[0];
  int n = regs->r[1];

  bool result = true;
  for (int i = 0; i < n && result; i++) {
    regs->r[0] = string[i];
    result = do_OS_WriteC( regs );
  }

  if (result) {
    regs->r[0] = (uint32_t) string;
  }
  regs->r[1] = n;

  return result;
}


static bool do_OS_Control( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_Exit( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SetEnv( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_IntOn( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_IntOff( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_CallBack( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_EnterOS( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_BreakPt( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_BreakCtrl( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_UnusedSWI( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_UpdateMEMC( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SetCallBack( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReadUnsigned( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool gs_space_is_terminator( uint32_t flags )
{
  return (0 != (flags & (1 << 29)));
}

static bool gs_quotes_are_special( uint32_t flags )
{
  return (0 == (flags & (1 << 31)));
}

static bool gs_translate_control_codes( uint32_t flags )
{
  return (0 == (flags & (1 << 30)));
}

static bool terminator( char c, uint32_t flags )
{
  switch (c) {
  case   0: return true;
  case  10: return true;
  case  13: return true;
  case ' ': return gs_space_is_terminator( flags );
  default: return false;
  }
}

static bool do_OS_GSInit( svc_registers *regs )
{
  regs->spsr &= ~CF;
  const char *string = (void*) regs->r[0];
  uint32_t flags = regs->r[2];
  while (!terminator( *string, flags ) && *string == ' ') {
    string++;
  }
  regs->r[0] = (uint32_t) string;
  regs->r[1] = *string;
  regs->r[2] = flags & 0xe0000000;
  return true;
}

static bool Error_BadString( svc_registers *regs )
{
  static error_block error = { 0xfd, "String not recognised" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool do_OS_GSRead( svc_registers *regs )
{
  uint32_t flags = regs->r[2] & 0xe0000000;
  //const char *var = regs->r[2] & ~0xe0000000;
  const char *string = (void*) regs->r[0];

  bool set_top_bit = false;
  char c;
  do {
    c = *string++;
    if (c == '"' && gs_quotes_are_special( flags )) {
      // Remove double quotes. Should I check for pairs? Do <> values in quotes not get expanded? FIXME
      c = *string++;
    }
    if (c == '|' && gs_translate_control_codes( flags )) {
      c = *string++;
      if (terminator( c, flags )) {
        return Error_BadString( regs );
      }
      else if (c >= '@' && c <= 'Z') c -= '@';
      else if (c >= 'a' && c <= 'z') c -= '`';
      else if (c == '?') c = 127;
      else if (c == '!') set_top_bit = true;
      else if (c == '[' || c == '{') c = 27;
      else if (c == '\\') c = 28;
      else if (c == ']' || c == '}') c = 29;
      else if (c == '^' || c == '~') c = 30;
      else if (c == '_' || c == '`') c = 31;
    }
    else if (c == '<') {
      for (;;) { asm ( "bkpt 77\nwfi" ); }
    }
  } while (set_top_bit);

  if (set_top_bit) c = c | 0x80;

  regs->r[1] = c;

  if (terminator( c, flags )) {
    regs->spsr |= CF;
  }
  regs->r[0] = (uint32_t) string;
  return true;
}

bool do_OS_GSTrans( svc_registers *regs )
{
  char *buffer = (void*) regs->r[1];
  uint32_t maxsize = regs->r[2] & ~0xe0000000;
  uint32_t translated = 0;
  regs->r[2] &= 0xe0000000;     // flags
  bool result = do_OS_GSInit( regs );
  while (result && 0 == (regs->spsr & CF) && translated < maxsize) {
    result = do_OS_GSRead( regs );
    if (result && buffer != 0) {
      buffer[translated] = regs->r[1];
    }
    translated++;
  }
  if (buffer != 0) *buffer++ = '\0';
  regs->r[1] = (uint32_t) buffer;
  regs->r[2] = translated;

  if (translated == maxsize)
    regs->spsr |=  CF;
  else
    regs->spsr &= ~CF;

  return result;
}

static bool do_OS_BinaryToDecimal( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReadEscapeState( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_EvaluateExpression( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadPalette( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_SWINumberToString( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SWINumberFromString( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ValidateAddress( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_CallAfter( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_CallEvery( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_RemoveTickerEvent( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_InstallKeyHandler( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_CheckModeValid( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }


static bool do_OS_ClaimScreenMemory( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadMonotonicTime( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SubstituteArgs( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

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

static bool do_OS_WriteEnv( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadArgs( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadRAMFsLimits( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ClaimDeviceVector( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ReleaseDeviceVector( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool comparison_routine_says_less( uint32_t v1, uint32_t v2, uint32_t workspace, uint32_t routine )
{
  register uint32_t r0 asm( "r0" ) = v1;
  register uint32_t r1 asm( "r1" ) = v2;
  register uint32_t r12 asm( "r12" ) = workspace;
  asm goto ( "blx %[routine]"
         "\n  blt %l[less]"
         : : "r" (r0), "r" (r1), "r" (r12), [routine] "r" (routine)
         : "r2", "r3"
         : less );

  return false;
less:
  return true;
}

static bool do_OS_HeapSort( svc_registers *regs )
{
  // Not the proper implementation FIXME
  int elements = regs->r[0];
  uint32_t *array = (void*) (regs->r[1] & ~0xe0000000);
  uint32_t flags = regs->r[1] >> 29;
  switch (regs->r[2]) {
  case 3: // Pointers to integers (DrawMod)
    {
    if (flags != 0)
      return Kernel_Error_UnimplementedSWI( regs );
    int **array = (void*) (regs->r[1] & ~0xe0000000);
    for (int i = 0; i < elements-1; i++) {
      int lowest = *array[i];
      int **p = 0;
      for (int j = i+1; j < elements; j++) {
        if (*array[j] < lowest) {
          lowest = *array[j];
          p = &array[j];
        }
      }

      if (p != 0) {
        // Swap pointers
        int32_t *l = *p;
        *p = array[i];
        array[i] = l;
      }
    }
    }
    break;
  default: return Kernel_Error_UnimplementedSWI( regs );
  }

  return true;
}

static bool do_OS_ExitAndDie( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ReadMemMapInfo( svc_registers *regs )
{
  regs->r[0] = 4096;
  regs->r[1] = 64 << 20; // FIXME Lying, but why is this being used?
  return true;
}

static bool do_OS_ReadMemMapEntries( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SetMemMapEntries( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_AddCallBack( svc_registers *regs )
{
  transient_callback *callback = workspace.kernel.transient_callbacks_pool;
  if (callback == 0) {
    callback = rma_allocate( sizeof( transient_callback ), regs );
  }
  else {
    workspace.kernel.transient_callbacks_pool = callback->next;
  }
  // Most recently requested gets called first, I don't know if that's right or not.
  callback->next = workspace.kernel.transient_callbacks;
  workspace.kernel.transient_callbacks = callback;
  callback->code = regs->r[0];
  callback->private_word = regs->r[1];
  return true;
}


static bool do_OS_ReadDefaultHandler( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_SetECFOrigin( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

typedef struct {
  uint32_t mode_selector_flags;
  uint32_t xres;
  uint32_t yres;
  uint32_t log2bpp;
  uint32_t frame_rate;
  struct {
    uint32_t variable;
    uint32_t value;
  } mode_variables[];
} mode_selector_block;

static const mode_selector_block only_one_mode = { .mode_selector_flags = 1, .xres = 1920, .yres = 1080, .log2bpp = 32, .frame_rate = 60, { { -1, 0 } } };


// OS_ReadSysInfo 6 values
// Not all of these will be needed or supported.
enum OS_ReadSysInfo_6 {
OSRSI6_CamEntriesPointer                       = 0,
OSRSI6_MaxCamEntry                             = 1,
OSRSI6_PageFlags_Unavailable                   = 2,
OSRSI6_PhysRamTable                            = 3,
OSRSI6_ARMA_Cleaner_flipflop                   = 4, // Unused in HAL kernels
OSRSI6_TickNodeChain                           = 5,
OSRSI6_ROMModuleChain                          = 6,
OSRSI6_DAList                                  = 7,
OSRSI6_AppSpaceDANode                          = 8,
OSRSI6_Module_List                             = 9,
OSRSI6_ModuleSHT_Entries                       = 10,
OSRSI6_ModuleSWI_HashTab                       = 11,
OSRSI6_IOSystemType                            = 12,
OSRSI6_L1PT                                    = 13,
OSRSI6_L2PT                                    = 14,
OSRSI6_UNDSTK                                  = 15,
OSRSI6_SVCSTK                                  = 16,
OSRSI6_SysHeapStart                            = 17,

// These are used by ROL, but conflict with our allocations

OSRSI6_ROL_KernelMessagesBlock                 = 18,
OSRSI6_ROL_ErrorSemaphore                      = 19,
OSRSI6_ROL_MOSdictionary                       = 20,
OSRSI6_ROL_Timer_0_Latch_Value                 = 21,
OSRSI6_ROL_FastTickerV_Counts_Per_Second       = 22,
OSRSI6_ROL_VecPtrTab                           = 23,
OSRSI6_ROL_NVECTORS                            = 24,
OSRSI6_ROL_IRQSTK                              = 25,
OSRSI6_ROL_SWIDispatchTable                    = 26, // JTABLE-SWIRelocation?
OSRSI6_ROL_SWIBranchBack                       = 27, // DirtyBranch?

// Our allocations which conflict with the above

OSRSI6_Danger_SWIDispatchTable                 = 18, // JTABLE-SWIRelocation (Relocated base of OS SWI dispatch table)
OSRSI6_Danger_Devices                          = 19, // Relocated base of IRQ device head nodes
OSRSI6_Danger_DevicesEnd                       = 20, // Relocated end of IRQ device head nodes
OSRSI6_Danger_IRQSTK                           = 21,
OSRSI6_Danger_SoundWorkSpace                   = 22, // workspace (8K) and buffers (2*4K)
OSRSI6_Danger_IRQsema                          = 23,

// Safe versions of the danger allocations
// Only supported by OS 5.17+, so if backwards compatability is required code
// should (safely!) fall back on the danger versions

OSRSI6_SWIDispatchTable                        = 64, // JTABLE-SWIRelocation (Relocated base of OS SWI dispatch table)
OSRSI6_Devices                                 = 65, // Relocated base of IRQ device head nodes
OSRSI6_DevicesEnd                              = 66, // Relocated end of IRQ device head nodes
OSRSI6_IRQSTK                                  = 67,
OSRSI6_SoundWorkSpace                          = 68, // workspace (8K) and buffers (2*4K)
OSRSI6_IRQsema                                 = 69,

// New ROOL allocations

OSRSI6_DomainId                                = 70, // current Wimp task handle
OSRSI6_OSByteVars                              = 71, // OS_Byte vars (previously available via OS_Byte &A6/VarStart)
OSRSI6_FgEcfOraEor                             = 72,
OSRSI6_BgEcfOraEor                             = 73,
OSRSI6_DebuggerSpace                           = 74,
OSRSI6_DebuggerSpace_Size                      = 75,
OSRSI6_CannotReset                             = 76,
OSRSI6_MetroGnome                              = 77, // OS_ReadMonotonicTime
OSRSI6_CLibCounter                             = 78,
OSRSI6_RISCOSLibWord                           = 79,
OSRSI6_CLibWord                                = 80,
OSRSI6_FPEAnchor                               = 81,
OSRSI6_ESC_Status                              = 82,
OSRSI6_ECFYOffset                              = 83,
OSRSI6_ECFShift                                = 84,
OSRSI6_VecPtrTab                               = 85,
OSRSI6_NVECTORS                                = 86,
OSRSI6_CAMFormat                               = 87, // 0 = 8 bytes per entry, 1 = 16 bytes per entry
OSRSI6_ABTSTK                                  = 88,
OSRSI6_PhysRamtableFormat                      = 89  // 0 = addresses are in byte units, 1 = addresses are in 4KB units
};

// Testing. Is this read-only?
// I don't think so, we need to update MetroGnome, don't we? Still, this will do as the initial values.
static const uint32_t SysInfo[] = {
  [OSRSI6_CamEntriesPointer]                       = 0,
  [OSRSI6_MaxCamEntry]                             = 1,
  [OSRSI6_PageFlags_Unavailable]                   = 2,
  [OSRSI6_PhysRamTable]                            = 3,
  [OSRSI6_ARMA_Cleaner_flipflop]                   = 4, // Unused in HAL kernels
  [OSRSI6_TickNodeChain]                           = 5,
  [OSRSI6_ROMModuleChain]                          = 6,
  [OSRSI6_DAList]                                  = 7,
  [OSRSI6_AppSpaceDANode]                          = 8,
  [OSRSI6_Module_List]                             = 9,
  [OSRSI6_ModuleSHT_Entries]                       = 10,
  [OSRSI6_ModuleSWI_HashTab]                       = 11,
  [OSRSI6_IOSystemType]                            = 12,
  [OSRSI6_L1PT]                                    = 13,
  [OSRSI6_L2PT]                                    = 14,
  [OSRSI6_UNDSTK]                                  = 
        sizeof( workspace.kernel.undef_stack ) + (uint32_t) &workspace.kernel.undef_stack,
  [OSRSI6_SVCSTK]                                  = 
        sizeof( workspace.kernel.svc_stack ) + (uint32_t) &workspace.kernel.svc_stack,
  [OSRSI6_SysHeapStart]                            = 17,

// Safe versions of the danger allocations
// Only supported by OS 5.17+, so if backwards compatability is required code
// should (safely!) fall back on the danger versions

  [OSRSI6_SWIDispatchTable]                        = 64, // JTABLE-SWIRelocation (Relocated base of OS SWI dispatch table)
  [OSRSI6_Devices]                                 = 65, // Relocated base of IRQ device head nodes
  [OSRSI6_DevicesEnd]                              = 66, // Relocated end of IRQ device head nodes
  [OSRSI6_IRQSTK]                                  = 67,
  [OSRSI6_SoundWorkSpace]                          = 68, // workspace (8K) and buffers (2*4K)
  [OSRSI6_IRQsema]                                 = &workspace.vectors.zp.IRQsema,

// New ROOL allocations

  [OSRSI6_DomainId]                                = 0x66600666, // current Wimp task handle
  [OSRSI6_OSByteVars]                              = 71, // OS_Byte vars (previously available via OS_Byte &A6/VarStart)
  [OSRSI6_FgEcfOraEor]                             = 72,
  [OSRSI6_BgEcfOraEor]                             = 73,
  [OSRSI6_DebuggerSpace]                           = 74,
  [OSRSI6_DebuggerSpace_Size]                      = 75,
  [OSRSI6_CannotReset]                             = 76,
  [OSRSI6_MetroGnome]                              = 77, // OS_ReadMonotonicTime
  [OSRSI6_CLibCounter]                             = (uint32_t) &workspace.vectors.zp.CLibCounter,
  [OSRSI6_RISCOSLibWord]                           = (uint32_t) &workspace.vectors.zp.RISCOSLibWord,
  [OSRSI6_CLibWord]                                = (uint32_t) &workspace.vectors.zp.CLibWord,
  [OSRSI6_FPEAnchor]                               = 81,
  [OSRSI6_ESC_Status]                              = 82,
  [OSRSI6_ECFYOffset]                              = 83,
  [OSRSI6_ECFShift]                                = 84,
  [OSRSI6_VecPtrTab]                               = 85,
  [OSRSI6_NVECTORS]                                = 86,
  [OSRSI6_CAMFormat]                               = 87, // 0 = 8 bytes per entry, 1 = 16 bytes per entry
  [OSRSI6_ABTSTK]                                  = 88,
  [OSRSI6_PhysRamtableFormat]                      = 89  // 0 = addresses are in byte units, 1 = addresses are in 4KB units
};

bool read_kernel_value( svc_registers *regs )
{
  static error_block error = { 0x333, "ReadSysInfo 6 unknown code" };

  if (regs->r[1] == 0) {
    // Single value, number in r2, result to r2
    regs->r[2] = SysInfo[regs->r[2]];
    return true;
  }

  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool do_OS_ReadSysInfo( svc_registers *regs )
{
  static error_block error = { 0x333, "ReadSysInfo unknown code" };

  // Probably just ChkKernelVersion (code 1)

  switch (regs->r[0]) {
  case 1:
    {
      regs->r[0] = (uint32_t) &only_one_mode;
      regs->r[1] = 7;
      regs->r[2] = 0;
      return true;
    }
  case 6: return read_kernel_value( regs );

  default: { return Kernel_Error_UnknownSWI( regs ); }
  }

  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool do_OS_Confirm( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_CRC( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_PrintChar( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ChangeRedirection( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_RemoveCallBack( svc_registers *regs )
{
  // This is not at all reentrant, and I'm not sure how you could make it so...
  transient_callback **cp = &workspace.kernel.transient_callbacks;
  while (*cp != 0 && ((*cp)->code != regs->r[0] || (*cp)->private_word != regs->r[1])) {
    cp = &(*cp)->next;
  }
  if ((*cp) != 0) {
    transient_callback *callback = (*cp);
    *cp = callback->next;
    callback->next = workspace.kernel.transient_callbacks_pool;
    workspace.kernel.transient_callbacks_pool = callback;
  }
  return true;
}


static bool do_OS_FindMemMapEntries( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

typedef union {
  struct {
    uint32_t action:3; // Set, OR, AND, EOR, Invert, Unchanged, AND NOT, OR NOT.
    uint32_t use_transparency:1;
    uint32_t background:1;
    uint32_t ECF_pattern:1; // Unlikely to be supported
    uint32_t text_colour:1; // As opposed to graphics colour
    uint32_t read_colour:1; // As opposed to setting it
  };
  uint32_t raw;
} OS_SetColour_Flags;

static bool do_OS_SetColour( svc_registers *regs )
{
  OS_SetColour_Flags flags = { .raw = regs->r[0] };

  if (flags.action != 0) {
    return Kernel_Error_UnimplementedSWI( regs );
  }

  union {
    struct {
      uint32_t A:8;
      uint32_t R:8;
      uint32_t G:8;
      uint32_t B:8;
    };
    uint32_t raw;
  } os_colour = { .raw = regs->r[1] };
  uint32_t colour = (255 << 24) | (os_colour.R << 16) | (os_colour.G << 8) | os_colour.B;

  if (flags.background)
    workspace.vdu.vduvars[154 - 128] = colour;
  else
    workspace.vdu.vduvars[153 - 128] = colour;
  return true;
}

static bool do_OS_Pointer( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ScreenMode( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_Memory( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ClaimProcessorVector( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_Reset( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_MMUControl( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool buffer_too_small(  svc_registers *regs )
{
  static error_block error = { 0x1e4, "Buffer overflow" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

static bool write_converted_character( svc_registers *regs, char c )
{
  *((char *) regs->r[1]++) = c;
  regs->r[2] --;
  if (regs->r[2] == 0) return buffer_too_small( regs );
  return true;
}

// This is a lot of work for little gain, and could be fixed by a Convert module, which can use existing code.
static bool do_OS_ConvertStandardDateAndTime( svc_registers *regs )
{
  for (const char *p = "No ConvertStandardDateAndTime"; *p != 0; p++) {
    if (!write_converted_character( regs, *p )) return false;
  }
  if (!write_converted_character( regs, '\0' )) return false;
  return true;
}

static bool do_OS_ConvertDateAndTime( svc_registers *regs )
{
  for (const char *p = "No ConvertDateAndTime"; *p != 0; p++) {
    if (!write_converted_character( regs, *p )) return false;
  }
  if (!write_converted_character( regs, '\0' )) return false;
  return true;
}

static const char hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

static bool hex_convert( svc_registers *regs, int digits )
{
  uint32_t n = regs->r[0];

  regs->r[0] = regs->r[1];

  for (int i = digits; i > 0; i--) {
    if (!write_converted_character( regs, hex[(n >> (4*i))&0xf] )) return false;
  }

  return write_converted_character( regs, '\0' );
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
    if (!write_converted_character( regs, '0' + n % 10 )) return false;
  }

  return result;
}

static bool convert_decimal( svc_registers *regs, uint32_t mask )
{
  uint32_t n = regs->r[0] & mask;
  regs->r[0] = regs->r[1];

  if (recursive_convert_decimal( regs, n ))
    return write_converted_character( regs, '\0' );

  return false;
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
    if (!write_converted_character( regs, '-' )) return false;
    n = sign_bit - n;
  }

  return recursive_convert_decimal( regs, n );
}

static bool do_OS_ConvertInteger1( svc_registers *regs )
{
  return convert_signed_decimal( regs, (1 << 7) );
}

static bool do_OS_ConvertInteger2( svc_registers *regs )
{
  return convert_signed_decimal( regs, (1 << 15) );
}

static bool do_OS_ConvertInteger3( svc_registers *regs )
{
  return convert_signed_decimal( regs, (1 << 23) );
}

static bool do_OS_ConvertInteger4( svc_registers *regs )
{
  return convert_signed_decimal( regs, (1ul << 31) );
}


static bool do_OS_ConvertBinary1( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary2( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary3( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertBinary4( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertSpacedCardinal1( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal2( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal3( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedCardinal4( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertSpacedInteger1( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger2( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger3( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertSpacedInteger4( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_ConvertFixedNetStation( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertNetStation( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }
static bool do_OS_ConvertFixedFileSize( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

static bool do_OS_FlushCache( svc_registers *regs )
{
  claim_lock( &shared.mmu.lock );
  clean_cache_to_PoC(); // FIXME
  release_lock( &shared.mmu.lock );
  return true;
}

static bool do_OS_ConvertFileSize( svc_registers *regs ) { return Kernel_Error_UnimplementedSWI( regs ); }

typedef bool (*swifn)( svc_registers *regs );

static swifn os_swis[256] = {
  [OS_WriteC] =  do_OS_WriteC,
  [OS_WriteS] =  do_OS_WriteS,
  [OS_Write0] =  do_OS_Write0,
  [OS_NewLine] =  do_OS_NewLine,

  [OS_ReadC] =  do_OS_ReadC,
  [OS_CLI] =  do_OS_CLI,
  [OS_Byte] =  do_OS_Byte,
  [OS_Word] =  do_OS_Word,

  [OS_File] =  do_OS_File,
  [OS_Args] =  do_OS_Args,
  [OS_BGet] =  do_OS_BGet,
  [OS_BPut] =  do_OS_BPut,

  [OS_GBPB] =  do_OS_GBPB,
  [OS_Find] =  do_OS_Find,
  [OS_ReadLine] =  do_OS_ReadLine,
  [OS_Control] =  do_OS_Control,

  [OS_GetEnv] =  do_OS_GetEnv,
  [OS_Exit] =  do_OS_Exit,
  [OS_SetEnv] =  do_OS_SetEnv,
  [OS_IntOn] =  do_OS_IntOn,

  [OS_IntOff] =  do_OS_IntOff,
  [OS_CallBack] =  do_OS_CallBack,
  [OS_EnterOS] =  do_OS_EnterOS,
  [OS_BreakPt] =  do_OS_BreakPt,

  [OS_BreakCtrl] =  do_OS_BreakCtrl,
  [OS_UnusedSWI] =  do_OS_UnusedSWI,
  [OS_UpdateMEMC] =  do_OS_UpdateMEMC,
  [OS_SetCallBack] =  do_OS_SetCallBack,

  [OS_Mouse] =  do_OS_Mouse,
  [OS_Heap] =  do_OS_Heap,
  [OS_Module] =  do_OS_Module,
  [OS_Claim] =  do_OS_Claim,

  [OS_Release] =  do_OS_Release,
  [OS_ReadUnsigned] =  do_OS_ReadUnsigned,
  [OS_GenerateEvent] =  do_OS_GenerateEvent,
  [OS_ReadVarVal] =  do_OS_ReadVarVal,

  [OS_SetVarVal] =  do_OS_SetVarVal,
  [OS_GSInit] =  do_OS_GSInit,
  [OS_GSRead] =  do_OS_GSRead,
  [OS_GSTrans] =  do_OS_GSTrans,

  [OS_BinaryToDecimal] =  do_OS_BinaryToDecimal,
  [OS_FSControl] =  do_OS_FSControl,
  [OS_ChangeDynamicArea] =  do_OS_ChangeDynamicArea,
  [OS_GenerateError] =  do_OS_GenerateError,

  [OS_ReadEscapeState] =  do_OS_ReadEscapeState,
  [OS_EvaluateExpression] =  do_OS_EvaluateExpression,
  [OS_SpriteOp] =  do_OS_SpriteOp,
  [OS_ReadPalette] =  do_OS_ReadPalette,

  [OS_ServiceCall] =  do_OS_ServiceCall,
  [OS_ReadVduVariables] =  do_OS_ReadVduVariables,
  [OS_ReadPoint] =  do_OS_ReadPoint,
  [OS_UpCall] =  do_OS_UpCall,

  [OS_CallAVector] =  do_OS_CallAVector,
  [OS_ReadModeVariable] =  do_OS_ReadModeVariable,
  [OS_RemoveCursors] =  do_OS_RemoveCursors,
  [OS_RestoreCursors] =  do_OS_RestoreCursors,

  [OS_SWINumberToString] =  do_OS_SWINumberToString,
  [OS_SWINumberFromString] =  do_OS_SWINumberFromString,
  [OS_ValidateAddress] =  do_OS_ValidateAddress,
  [OS_CallAfter] =  do_OS_CallAfter,

  [OS_CallEvery] =  do_OS_CallEvery,
  [OS_RemoveTickerEvent] =  do_OS_RemoveTickerEvent,
  [OS_InstallKeyHandler] =  do_OS_InstallKeyHandler,
  [OS_CheckModeValid] =  do_OS_CheckModeValid,


  [OS_ChangeEnvironment] =  do_OS_ChangeEnvironment,
  [OS_ClaimScreenMemory] =  do_OS_ClaimScreenMemory,
  [OS_ReadMonotonicTime] =  do_OS_ReadMonotonicTime,
  [OS_SubstituteArgs] =  do_OS_SubstituteArgs,

  [OS_PrettyPrint] =  do_OS_PrettyPrint,
  [OS_Plot] =  do_OS_Plot,
  [OS_WriteN] =  do_OS_WriteN,
  [OS_AddToVector] =  do_OS_AddToVector,

  [OS_WriteEnv] =  do_OS_WriteEnv,
  [OS_ReadArgs] =  do_OS_ReadArgs,
  [OS_ReadRAMFsLimits] =  do_OS_ReadRAMFsLimits,
  [OS_ClaimDeviceVector] =  do_OS_ClaimDeviceVector,

  [OS_ReleaseDeviceVector] =  do_OS_ReleaseDeviceVector,
  [OS_DelinkApplication] =  do_OS_DelinkApplication,
  [OS_RelinkApplication] =  do_OS_RelinkApplication,
  [OS_HeapSort] =  do_OS_HeapSort,

  [OS_ExitAndDie] =  do_OS_ExitAndDie,
  [OS_ReadMemMapInfo] =  do_OS_ReadMemMapInfo,
  [OS_ReadMemMapEntries] =  do_OS_ReadMemMapEntries,
  [OS_SetMemMapEntries] =  do_OS_SetMemMapEntries,

  [OS_AddCallBack] =  do_OS_AddCallBack,
  [OS_ReadDefaultHandler] =  do_OS_ReadDefaultHandler,
  [OS_SetECFOrigin] =  do_OS_SetECFOrigin,
  [OS_SerialOp] =  do_OS_SerialOp,


  [OS_ReadSysInfo] =  do_OS_ReadSysInfo,
  [OS_Confirm] =  do_OS_Confirm,
  [OS_ChangedBox] =  do_OS_ChangedBox,
  [OS_CRC] =  do_OS_CRC,

  [OS_ReadDynamicArea] =  do_OS_ReadDynamicArea,
  [OS_PrintChar] =  do_OS_PrintChar,
  [OS_ChangeRedirection] =  do_OS_ChangeRedirection,
  [OS_RemoveCallBack] =  do_OS_RemoveCallBack,


  [OS_FindMemMapEntries] =  do_OS_FindMemMapEntries,
  [OS_SetColour] =  do_OS_SetColour,
  [OS_Pointer] =  do_OS_Pointer,
  [OS_ScreenMode] =  do_OS_ScreenMode,

  [OS_DynamicArea] =  do_OS_DynamicArea,
  [OS_Memory] =  do_OS_Memory,
  [OS_ClaimProcessorVector] =  do_OS_ClaimProcessorVector,
  [OS_Reset] =  do_OS_Reset,

  [OS_MMUControl] =  do_OS_MMUControl,

  [OS_ConvertStandardDateAndTime] =  do_OS_ConvertStandardDateAndTime,
  [OS_ConvertDateAndTime] =  do_OS_ConvertDateAndTime,

  [OS_ConvertHex1] =  do_OS_ConvertHex1,
  [OS_ConvertHex2] =  do_OS_ConvertHex2,
  [OS_ConvertHex4] =  do_OS_ConvertHex4,
  [OS_ConvertHex6] =  do_OS_ConvertHex6,

  [OS_ConvertHex8] =  do_OS_ConvertHex8,
  [OS_ConvertCardinal1] =  do_OS_ConvertCardinal1,
  [OS_ConvertCardinal2] =  do_OS_ConvertCardinal2,
  [OS_ConvertCardinal3] =  do_OS_ConvertCardinal3,

  [OS_ConvertCardinal4] =  do_OS_ConvertCardinal4,
  [OS_ConvertInteger1] =  do_OS_ConvertInteger1,
  [OS_ConvertInteger2] =  do_OS_ConvertInteger2,
  [OS_ConvertInteger3] =  do_OS_ConvertInteger3,

  [OS_ConvertInteger4] =  do_OS_ConvertInteger4,
  [OS_ConvertBinary1] =  do_OS_ConvertBinary1,
  [OS_ConvertBinary2] =  do_OS_ConvertBinary2,
  [OS_ConvertBinary3] =  do_OS_ConvertBinary3,

  [OS_ConvertBinary4] =  do_OS_ConvertBinary4,
  [OS_ConvertSpacedCardinal1] =  do_OS_ConvertSpacedCardinal1,
  [OS_ConvertSpacedCardinal2] =  do_OS_ConvertSpacedCardinal2,
  [OS_ConvertSpacedCardinal3] =  do_OS_ConvertSpacedCardinal3,

  [OS_ConvertSpacedCardinal4] =  do_OS_ConvertSpacedCardinal4,
  [OS_ConvertSpacedInteger1] =  do_OS_ConvertSpacedInteger1,
  [OS_ConvertSpacedInteger2] =  do_OS_ConvertSpacedInteger2,
  [OS_ConvertSpacedInteger3] =  do_OS_ConvertSpacedInteger3,

  [OS_ConvertSpacedInteger4] =  do_OS_ConvertSpacedInteger4,
  [OS_ConvertFixedNetStation] =  do_OS_ConvertFixedNetStation,
  [OS_ConvertNetStation] =  do_OS_ConvertNetStation,
  [OS_ConvertFixedFileSize] =  do_OS_ConvertFixedFileSize,

  [OS_FlushCache] = do_OS_FlushCache, // This could be called by each core on centisecond interrupts, maybe?

  [OS_ConvertFileSize] =  do_OS_ConvertFileSize
};

static bool Kernel_go_svc( svc_registers *regs, uint32_t svc )
{
  switch (svc & ~Xbit) {
  case 0 ... 255:
    if (os_swis[svc & ~Xbit] != 0)
      return os_swis[svc & ~Xbit]( regs );
    else
      return Kernel_Error_UnknownSWI( regs );

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

static void do_svc_and_transient_callbacks( svc_registers *regs, uint32_t lr )
{
  regs->spsr &= ~VF;
  uint32_t number = get_swi_number( lr );
  if (Kernel_go_svc( regs, number )) {
    // Worked
    regs->spsr &= ~VF;
  }
  else if ((number & Xbit) != 0) {
    // Error
    // for (;;) { asm ( "wfi" ); }
    regs->spsr |= VF;
  }
  else {
    // Call error handler
*(uint8_t*) 0x98989898 = 42; // Cause a data abort
    for (;;) { asm ( "wfi" ); }
  }

uint32_t test = regs->spsr;
  while (workspace.kernel.transient_callbacks != 0) {
    transient_callback *callback = workspace.kernel.transient_callbacks;

    // In case the callback registers a callback, make a private copy of the
    // callback details and sort out the list before making the call. 
    transient_callback latest = *callback;

    callback->next = workspace.kernel.transient_callbacks_pool;
    workspace.kernel.transient_callbacks_pool = callback;
    workspace.kernel.transient_callbacks = latest.next;

    // Callbacks preserve all registers and return by mov pc, lr
    register uint32_t private_word asm ( "r12" ) = latest.private_word;
    register uint32_t code asm ( "r14" ) = latest.code;
    asm ( "blx r14" : : "r" (private_word), "r" (code) : "memory" );
  }

if (test != regs->spsr) {
*(uint8_t*) 0x94949494 = 42; // Cause a data abort
}
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

  svc_registers *regs;
  uint32_t lr;
  asm ( "  srsdb sp!, #0x13"
      "\n  push { r0-r12 }"
      "\n  mov %[regs], sp"
      "\n  mov %[lr], lr"
      : [regs] "=r" (regs)
      , [lr] "=r" (lr)
      );

  do_svc_and_transient_callbacks( regs, lr );

  asm ( "pop { r0-r12 }"
    "\n  rfeia sp!" );

  __builtin_unreachable();
}

