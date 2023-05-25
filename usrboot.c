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

/* This file provides the standard kernel initialisation, running as the
 * first usr32 mode code. Its final act is to run Resources:$.!Boot, which
 * will have been provided by the HAL.
 *
 * TODO: Should !Boot be run on all cores, or just one, with the others
 * waiting for tasks to run?
 * Maybe just provide mechanisms to allow !Boot to manage itself?
 * e.g. Core$Current - current core number (code variable)
 *      Core$GPUInterrupts - core receiving GPU interrupts (on a Pi)
 */

#include "include/types.h"
#include "include/kernel_swis.h"
#include "include/pico_clib.h"

static inline void debug_string_with_length( char const *s, int length )
{
  register int code asm( "r0" ) = TaskOp_DebugString;
  register char const *string asm( "r1" ) = s;
  register int len asm( "r2" ) = length;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (code)
      , "r" (len)
      , "r" (string)
      : "memory" );
}

static inline void debug_string( char const *s )
{
  debug_string_with_length( s, strlen( s ) );
}

static inline void debug_number( uint32_t num )
{
  register int code asm( "r0" ) = TaskOp_DebugNumber;
  register uint32_t number asm( "r1" ) = num;
  asm ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (code)
      , "r" (number)
      : "memory" );
}

#define WriteN( s, n ) debug_string_with_length( s, n )
#define Write0( s ) debug_string( s )
#define WriteS( s ) debug_string_with_length( s, sizeof( s ) - 1 )
#define NewLine debug_string_with_length( "\n\r", 2 )
#define Space debug_string_with_length( " ", 1 )
#define WriteNum( n ) debug_number( (uint32_t) (n) )

bool module_name_match( char const *left, char const *right );

static void Sleep( uint32_t centiseconds )
{
  register uint32_t request asm ( "r0" ) = TaskOp_Sleep;
  register uint32_t time asm ( "r1" ) = centiseconds; // Shift down a lot for testing!

  asm volatile ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      , "r" (time)
      : "lr", "memory" );
}

extern uint32_t _binary_AllMods_start;

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

extern mode_selector_block const only_one_mode;

// Copied from modules.c, fixed by the legacy OS
typedef struct {
  uint32_t offset_to_start;
  uint32_t offset_to_initialisation;
  uint32_t offset_to_finalisation;
  uint32_t offset_to_service_call_handler;
  uint32_t offset_to_title_string;
  uint32_t offset_to_help_string;
  uint32_t offset_to_help_and_command_keyword_table;
  uint32_t swi_chunk;
  uint32_t offset_to_swi_handler;
  uint32_t offset_to_swi_decoding_table;
  uint32_t offset_to_swi_decoding_code;
  uint32_t offset_to_messages_file_name;
  uint32_t offset_to_flags;
} module_header;

static void *pointer_at_offset_from( void *base, uint32_t off )
{
  return (off == 0) ? 0 : ((uint8_t*) base) + off;
}

static inline const char *title_string( module_header *header )
{
  return pointer_at_offset_from( header, header->offset_to_title_string );
}

static module_header *find_rom_module( const char *name )
{
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  while (0 != *rom_module) {
    module_header *header = (void*) (rom_module+1);
    register const char *title = title_string( header );
    if (module_name_match( title, name )) {
      return (void*) (rom_module+1); // Header without size
    }
    rom_module += (*rom_module)/4; // Includes size of length field
  }

  return 0;
}

void init_module( const char *name )
{
  module_header *header = find_rom_module( name );

  if (0 != header) {
    register uint32_t code asm( "r0" ) = 10;
    register module_header *module asm( "r1" ) = header;
    // lr should not be corrupted, we're running in usr32 mode.
    asm volatile ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "cc", "memory" );
  }
}

#define REPLACEMENT( modname ) \
  if (0 == strcmp( name, #modname )) { \
    extern uint32_t _binary_Modules_##modname##_start; \
    module_header *header = find_rom_module( #modname ); \
    register uint32_t code asm( "r0" ) = 10; \
    register uint32_t module asm( "r1" ) = 4 + (uint32_t) &_binary_Modules_##modname##_start; \
    register module_header *original asm ( "r2" ) = header; \
 \
    asm volatile ( "svc %[os_module]" \
       : \
       : "r" (code) \
       , "r" (module) \
       , "r" (original) \
       , [os_module] "i" (OS_Module) \
       : "lr", "cc", "memory" ); \
    return true; \
  }

bool excluded( const char *name )
{
  // These modules fail on init, at the moment.
  static const char *excludes[] = { "PCI"               // Data abort fc01ff04 prob. pci_handles

                                  // Wimp filters?
                                  // , "Toolbox"
                                  , "DeviceFS"

                                  // RISC_OSLib ROM modules
                                  , "ScreenModes"       // Writes to ROM? Calls GraphicsV

                                  , "Debugger"
                                  , "BCMSupport"        // Unknown dynamic area
                                  , "Portable"          // Uses OS_MMUControl
                                  , "RTSupport"         // Unknown dynamic area
                                  , "USBDriver"         //  "
                                  , "DWCDriver"         //  "
                                  , "XHCIDriver"        //  "
                                  , "VCHIQ"             //  "
                                  , "BCMSound"          // ???

// Probably don't work, I can't be bothered to see if their problems are solved already
                                  , "SoundDMA"          // Uses OS_Memory
                                  , "SoundChannels"     // ???
                                  , "SoundScheduler"    // Sound_Tuning
                                  // , "TaskManager"       // Initialisation returns an error
                                  , "BCMVideo"          // Tries to use OS_MMUControl
                                  // , "FilterManager"     // Uses Wimp_ReadSysInfo 
                                  , "WaveSynth"         // throws exception
                                  , "StringLib"         // ?
                                  , "Percussion"         // ?
                                  , "IIC"         // ? 0xe200004d
                                  , "SharedSound"       // 0xe200004d
                                  , "DOSFS"             // 0x8600003f
                                  , "SCSIDriver"        // 0x8600003f
                                  , "SCSISoftUSB"       // 0x8600003f
                                  , "SCSIFS"            // 0xe2000001
                                  , "SDIODriver"        // 0x8600003f
                                  , "SDFS"              // 0x8600003f
                                  , "SDCMOS"              // 0x8600003f
                                  , "Internet"          // 0x8600003f
                                  , "Resolver"          // 0x8600003f
                                  , "Net"               // 0x8600003f

// Not checked:
                                  , "BootNet"
                                  , "Freeway"
                                  , "ShareFS"
                                  , "MimeMap"
                                  , "LanManFS"
                                  , "EtherGENET"
                                  , "EtherUSB"
                                  , "DHCP"
                                  , "CDFSDriver"
                                  , "CDFSSoftSCSI"
                                  , "CDFS"
                                  , "CDFSFiler"
                                  , "GPIO"

                                  , "DMAManager"        // Calls OS_Hardware
                                  , "BBCEconet"         // Data abort
                                  , "FSLock"            // Writes CMOS not yet supported
                                  , "FPEmulator"        // OS_ClaimProcessorVector

                                  , "MbufManager"       // 0xe200004d

                                  //, "MessageTrans"     // Breaks the SVC stack when TokNFnd (recurses endlessly) It used to work! - I was initialising it twice
                                  , "ColourPicker"     // Init fails
                                  , "DrawFile"     // Init fails

                                  // , "DragASprite"       // Doesn't return, afaics
                                  , "RamFS"
                                  // , "Filer"             // Doesn't return, afaics
                                  , "VFPSupport"        // Tries to claim processor vector
                                  , "Hourglass"        // OS_ReadPalette
                                  , "InternationalKeyboard" // Probably because there isn't one?
                                  , "NetFS"             // Doesn't return
                                  , "NetPrint"             // Doesn't return
                                  , "NetStatus"             // Doesn't return
                                  , "PipeFS"             // OS_ClaimProcessorVector
                                  , "RTC"               // No ticks? No hardware?
                                  , "ScreenBlanker"        // Doesn't return, afaics
                                  , "ScrSaver"        // Doesn't return, afaics
                                  , "Serial"        // "esources$Path{,_Message} not found
                                  , "SerialDeviceSupport"        // "esources$Path{,_Message} not found
                                  , "ShellCLI"        // "esources$Path{,_Message} not found
                                  , "SoundControl"          // No return
                                  , "BootFX"                    // Calls CallASWIR12 with 0x78440
                                  , "SystemDevices"             // No return
                                  , "TaskWindow"             // 0xfc3428ac Using SvcTable, which doesn't exist any more. Needs replacement.
/**/
  };

  // C Modules that replace ROM modules (experimental)
  //REPLACEMENT( FontManager );
  REPLACEMENT( Portable );
  REPLACEMENT( VFPSupport );
  REPLACEMENT( FPEmulator );

  for (int i = 0; i < number_of( excludes ); i++) {
    if (0 == strcmp( name, excludes[i] ))
      return true;
  }
  return false;
}

void init_modules()
{
  uint32_t *rom_modules = &_binary_AllMods_start;
  uint32_t *rom_module = rom_modules;

  while (0 != *rom_module) {
    asm ( "svc 0x20013" : : : "lr" );
    Sleep( 0 );

    module_header *header = (void*) (rom_module+1);

#ifdef DEBUG__SHOW_MODULE_INIT
    NewLine;
    WriteS( "INIT: " ); Write0( title_string( header ) ); Space;
    WriteNum( rom_module ); Space;
#endif
    if (!excluded( title_string( header ) )) {
#ifdef DEBUG__SHOW_MODULE_INIT
      if (0) {
      if (header->offset_to_service_call_handler != 0) {
        Write0( " services " );
        uint32_t *p = pointer_at_offset_from( header, header->offset_to_service_call_handler );
        if (0xe1a00000 == p[0]) {
          Write0( " with table" );
          uint32_t table_offset = p[-1];

          uint32_t *p = pointer_at_offset_from( header, table_offset );

          NewLine; Write0( "Flags: " ); WriteNum( *p++ ); NewLine; p++; // Skip handler offset
          do {
            NewLine; Write0( "Expects service: " ); WriteNum( *p++ );
          } while (*p != 0);
        }
      }
      NewLine;
#ifdef DEBUG__SHOW_MODULE_COMMANDS_ON_INIT
    show_module_commands( header );
#endif
      }
#endif

      register uint32_t code asm( "r0" ) = 10;
      register module_header *module asm( "r1" ) = header;

      asm volatile ( "svc %[os_module]" : : "r" (code), "r" (module), [os_module] "i" (OS_Module) : "lr", "cc", "memory" );
    }
    else {
#ifdef DEBUG__SHOW_MODULE_INIT
      WriteS( " - excluded" );
      NewLine;
#endif
    }
    rom_module += (*rom_module)/4; // Includes size of length field
  }
}

static inline uint32_t read_var( char const *name, char *value, int size )
{
  register char const *n asm ( "r0" ) = name;
  register char *v asm ( "r1" ) = value;
  register uint32_t s asm ( "r2" ) = size;
  register char const *context asm ( "r3" ) = 0;
  register char const *zero4 asm ( "r4" ) = 0;

  register uint32_t bytes asm ( "r2" );

  asm ( "svc %[swi]"
      : "=r" (bytes)
      : [swi] "i" (OS_ReadVarVal)
      , "r" (n)
      , "r" (v)
      , "r" (s)
      , "r" (context)
      , "r" (zero4) // not converted
      : "memory", "lr" );

  return bytes;
}

static inline void set_var( char const *name, char const *value )
{
  uint32_t length = strlen( value );

  register char const *n asm ( "r0" ) = name;
  register char const *v asm ( "r1" ) = value;
  register uint32_t len asm ( "r2" ) = length;
  register char const *zero3 asm ( "r3" ) = 0;
  register char const *zero4 asm ( "r4" ) = 0;

  register struct error_block *error asm ( "r0" );

  asm volatile ( // volatile in case the output, `error`, is ignored
        "svc %[swi]"
    "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OS_SetVarVal)
      , "r" (n)
      , "r" (v)
      , "r" (len)
      , "r" (zero3)
      , "r" (zero4)
      : "lr", "memory" );

  if (error != 0) {
    asm ( "bkpt 43" );
  }
}

static inline void set_core_var()
{
  // Ah, the beauty of being able to insert any privileged code into your system at will!

  void *core_var_code;

  asm ( "b 0f // Skip variable code"
    "\ncore_var: mov pc, lr   // Write entry point, no effect"
    "\n  push { lr } // Read entry point"
    "\n  mov r0, %[op]"
    "\n  svc %[swi]"
    "\n  pop { pc }"
    "\n0: adr %[code], core_var"
    : [code] "=r" (core_var_code)
    : [swi] "i" (OS_ThreadOp | 0x20000)
    , [op] "i" (TaskOp_CoreNumber)
    : "memory" );

  register char const *n asm ( "r0" ) = "CPU$Core";
  register char const *v asm ( "r1" ) = core_var_code;
  register uint32_t len asm ( "r2" ) = 20; // 5 instructions
  register uint32_t zero3 asm ( "r3" ) = 0;
  register uint32_t zero4 asm ( "r4" ) = 16; // Code variable.

  asm volatile ( // volatile, because outputs ignored (they're only there to indicate they're clobbered)
      "svc %[swi]"
      : "=r" (zero3)
      , "=r" (zero4)
      : [swi] "i" (OS_SetVarVal)
      , "r" (n)
      , "r" (v)
      , "r" (len)
      , "r" (zero3)
      , "r" (zero4)
      : "lr", "memory" );
}

static inline void set_literal_string_var( char const *name, char const *value )
{
  uint32_t length = strlen( value );

  register char const *n asm ( "r0" ) = name;
  register char const *v asm ( "r1" ) = value;
  register uint32_t len asm ( "r2" ) = length;
  register uint32_t zero3 asm ( "r3" ) = 0;
  register uint32_t type asm ( "r4" ) = 4;

  register struct error_block *error asm ( "r0" );

  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"
      : "=r" (error)
      : [swi] "i" (OS_SetVarVal)
      , "r" (n)
      , "r" (v)
      , "r" (len)
      , "r" (zero3)
      , "r" (type)
      : "lr", "memory" );

  if (error != 0) {
    asm ( "bkpt 43" );
  }
}

static inline void Plot( uint32_t type, uint32_t x, uint32_t y )
{
  register uint32_t Rtype asm( "r0" ) = type;
  register uint32_t Rx asm( "r1" ) = x * 2; // pixel units to OS units, just for the tests
  register uint32_t Ry asm( "r2" ) = y * 2;
  asm volatile ( "svc %[swi]" : : [swi] "i" (OS_Plot), "r" (Rtype), "r" (Rx), "r" (Ry) : "lr", "memory" );
}

static inline void Draw_Fill( uint32_t *path, int32_t *transformation_matrix )
{
  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = 0;
  register  int32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 0;
  asm ( "swi 0x60702"
        : 
        : "r" (draw_path)
        , "r" (fill_style)
        , "r" (matrix)
        , "r" (flatness)
        : "lr" );
}

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

static inline void SetColour( uint32_t flags, uint32_t colour )
{
  register uint32_t in_flags asm( "r0" ) = flags;
  register uint32_t in_colour asm( "r1" ) = colour;
  asm ( "swi %[swi]" : 
        : "r" (in_colour)
        , "r" (in_flags)
        , [swi] "i" (OS_SetColour)
        : "lr", "memory" );
}

static inline void SetGraphicsFgColour( uint32_t colour )
{
  Write0( "Setting graphics foreground colour with ColourTrans... " );
  register uint32_t pal asm( "r0" ) = colour;
  register uint32_t Rflags asm( "r3" ) = 0; // FG, no ECFs
  register uint32_t action asm( "r4" ) = 0; // set
  asm volatile ( "svc %[swi]" : : [swi] "i" (0x60743), "r" (pal), "r" (Rflags), "r" (action) : "lr", "cc", "memory" );
}

static inline void SetGraphicsBgColour( uint32_t colour )
{
  Write0( "Setting graphics background colour with ColourTrans... " );
  register uint32_t pal asm( "r0" ) = colour;
  register uint32_t Rflags asm( "r3" ) = 0x80;
  register uint32_t action asm( "r4" ) = 0; // set
  asm volatile ( "svc %[swi]" : : [swi] "i" (0x60743), "r" (pal), "r" (Rflags), "r" (action) : "lr", "cc", "memory" );
}

void Draw_Stroke( uint32_t *path, uint32_t *transformation_matrix )
{
  // Keep this declaration before the first register variable declaration, or
  // -Os will cause the compiler to forget the preceding registers.
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101422
  uint32_t cap_and_join_style[4] =  { 0, 0xa0000, 0, 0 };

  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = 0;
  register uint32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 0;
  register uint32_t thickness asm( "r4" ) = 0x1000;
  register uint32_t *cap_and_join asm( "r5" ) = cap_and_join_style;
  register uint32_t dashes asm( "r6" ) = 0;
  asm ( "swi 0x60704" : 
        : "r" (draw_path)
        , "r" (fill_style)
        , "r" (matrix)
        , "r" (flatness)
        , "r" (thickness)
        , "r" (cap_and_join)
        , "r" (dashes)
        , "m" (cap_and_join_style) // Without this, the array is not initialised
        : "lr", "memory" );
}

// Warning: does not return error status (although a "handle" > 255 is certainly an error)
static inline uint32_t Font_FindFont( const char *name, uint32_t xpoints, uint32_t ypoints, uint32_t xdpi, uint32_t ydpi )
{
  register uint32_t result asm( "r0" );
  register const char *rname asm( "r1" ) = name;
  register uint32_t rxpoints asm( "r2" ) = xpoints;
  register uint32_t rypoints asm( "r3" ) = ypoints;
  register uint32_t rxdpi asm( "r4" ) = xdpi;
  register uint32_t rydpi asm( "r5" ) = ydpi;

  asm volatile ( "swi %[swi]"
        : "=r" (result)
        : "r" (rname)
        , "r" (rxpoints)
        , "r" (rypoints)
        , "r" (rxdpi)
        , "r" (rydpi)
        , [swi] "i" (0x40081)
        : "lr", "memory" );

  return result;
}

static inline void ColourTrans_SetFontColours( uint32_t font, uint32_t fg, uint32_t bg, uint32_t maxdiff )
{
  register uint32_t Rfont asm( "r0" ) = font;
  register uint32_t Rfg asm( "r1" ) = fg;
  register uint32_t Rbg asm( "r2" ) = bg;
  register uint32_t Rmaxdiff asm( "r3" ) = maxdiff;

  asm volatile ( "swi %[swi]"
        :
        : "r" (Rfont)
        , "r" (Rfg)
        , "r" (Rbg)
        , "r" (Rmaxdiff)
        , [swi] "i" (0x20000 | 0x4074F)
        : "lr", "memory" );
}

static inline void usr_OS_ConvertCardinal4( uint32_t number, char *buffer, uint32_t buffer_size, char *old_buffer, char **terminator, uint32_t *remaining_size )
{
  // Do any calculations or variable initialisations here, before
  // declaring any register variables.
  // This is important because the compiler may insert function calls
  // like memcpy or memset, which will corrupt already declared registers.

  // The inputs to the SWI
  register uint32_t n asm( "r0" ) = number;
  register char *buf asm( "r1" ) = buffer;
  register uint32_t s asm( "r2" ) = buffer_size;

  // The outputs from the SWI
  register uint32_t oldbuf asm( "r0" );
  register char *term asm( "r1" );
  register uint32_t rem asm( "r2" );

  // Call the SWI
  asm volatile ( "svc %[swi]"
    : "=r" (oldbuf) // List all the output variables
    , "=r" (term)   // or they will be optimised away.
    , "=r" (rem)
    : [swi] "i" (OS_ConvertCardinal4) // Can use an enum for the SWI number
    , "r" (n)       // List all the input register variables,
    , "r" (buf)     // or they will be optimised away.
    , "r" (s)
    : // If the SWI corrupts any registers, list them here
      // If the function is to be called in a priviledged mode, include "lr"
      "memory"
  );

  // Store the output values
  // Don't worry about the apparent inefficiency, the compiler will
  // optimise out unused values.
  // Again, don't do anything other than simple storage or assignments
  // to non-register variables.
  if (old_buffer != 0) *old_buffer = oldbuf;
  if (terminator != 0) *terminator = term;
  if (remaining_size != 0) *remaining_size = rem;
}

void Font_Paint( uint32_t font, const char *string, uint32_t type, uint32_t startx, uint32_t starty, uint32_t length )
{
  register uint32_t rHandle asm( "r0" ) = font;
  register const char *rString asm( "r1" ) = string;
  register uint32_t rType asm( "r2" ) = type;
  register uint32_t rx asm( "r3" ) = startx;
  register uint32_t ry asm( "r4" ) = starty;
  register uint32_t rBlankArea asm( "r5" ) = 0;
  register uint32_t rMatrix asm( "r6" ) = 0;
  register uint32_t rLength asm( "r7" ) = length;
  asm ( "swi 0x60086" : 
        : "r" (rHandle)
        , "r" (rString)
        , "r" (rType)
        , "r" (rx)
        , "r" (ry)
        , "r" (rMatrix)
        , "r" (rBlankArea)
        , "r" (rLength)
        : "lr" );
}


static const uint32_t sines[] = {
  0x00000, // sin 0
  0x00477, // sin 1
  0x008ef, // sin 2
  0x00d65, // sin 3
  0x011db, // sin 4
  0x0164f, // sin 5
  0x01ac2, // sin 6
  0x01f32, // sin 7
  0x023a0, // sin 8
  0x0280c, // sin 9
  0x02c74, // sin 10
  0x030d8, // sin 11
  0x03539, // sin 12
  0x03996, // sin 13
  0x03dee, // sin 14
  0x04241, // sin 15
  0x04690, // sin 16
  0x04ad8, // sin 17
  0x04f1b, // sin 18
  0x05358, // sin 19
  0x0578e, // sin 20
  0x05bbe, // sin 21
  0x05fe6, // sin 22
  0x06406, // sin 23
  0x0681f, // sin 24
  0x06c30, // sin 25
  0x07039, // sin 26
  0x07438, // sin 27
  0x0782f, // sin 28
  0x07c1c, // sin 29
  0x07fff, // sin 30
  0x083d9, // sin 31
  0x087a8, // sin 32
  0x08b6d, // sin 33
  0x08f27, // sin 34
  0x092d5, // sin 35
  0x09679, // sin 36
  0x09a10, // sin 37
  0x09d9b, // sin 38
  0x0a11b, // sin 39
  0x0a48d, // sin 40
  0x0a7f3, // sin 41
  0x0ab4c, // sin 42
  0x0ae97, // sin 43
  0x0b1d5, // sin 44
  0x0b504, // sin 45
  0x0b826, // sin 46
  0x0bb39, // sin 47
  0x0be3e, // sin 48
  0x0c134, // sin 49
  0x0c41b, // sin 50
  0x0c6f3, // sin 51
  0x0c9bb, // sin 52
  0x0cc73, // sin 53
  0x0cf1b, // sin 54
  0x0d1b3, // sin 55
  0x0d43b, // sin 56
  0x0d6b3, // sin 57
  0x0d919, // sin 58
  0x0db6f, // sin 59
  0x0ddb3, // sin 60
  0x0dfe7, // sin 61
  0x0e208, // sin 62
  0x0e419, // sin 63
  0x0e617, // sin 64
  0x0e803, // sin 65
  0x0e9de, // sin 66
  0x0eba6, // sin 67
  0x0ed5b, // sin 68
  0x0eeff, // sin 69
  0x0f08f, // sin 70
  0x0f20d, // sin 71
  0x0f378, // sin 72
  0x0f4d0, // sin 73
  0x0f615, // sin 74
  0x0f746, // sin 75
  0x0f865, // sin 76
  0x0f970, // sin 77
  0x0fa67, // sin 78
  0x0fb4b, // sin 79
  0x0fc1c, // sin 80
  0x0fcd9, // sin 81
  0x0fd82, // sin 82
  0x0fe17, // sin 83
  0x0fe98, // sin 84
  0x0ff06, // sin 85
  0x0ff60, // sin 86
  0x0ffa6, // sin 87
  0x0ffd8, // sin 88
  0x0fff6, // sin 89
  0x10000  // sin 90
  };        // sin 90, cos 0

static inline uint32_t draw_sin( int deg )
{
  while (deg < 0) { deg += 360; }
  while (deg > 360) { deg -= 360; }
  if (deg > 180) return -draw_sin( deg - 180 );
  if (deg > 90) return draw_sin( 180 - deg );
  return sines[deg];
}

static inline uint32_t draw_cos( int deg )
{
  return draw_sin( deg + 90 );
}

static inline void fill_rect( uint32_t left, uint32_t top, uint32_t w, uint32_t h, uint32_t c )
{
  extern uint32_t frame_buffer;
  uint32_t *screen = &frame_buffer;

  for (uint32_t y = top; y < top + h; y++) {
    uint32_t *p = &screen[y * 1920 + left];
    for (int x = 0; x < w; x++) { *p++ = c; }
  }
}

static inline uint64_t timer_now()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 0, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo) : : "memory"  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static inline uint32_t timer_interrupt_time()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 2, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo)  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static inline void timer_interrupt_at( uint64_t then )
{
  asm volatile ( "mcrr p15, 2, %[lo], %[hi], c14" : : [hi] "r" (then >> 32), [lo] "r" (0xffffffff & then) : "memory" );
}

void __attribute__(( naked )) returned_to_root()
{
  asm ( "bkpt 0x7777" );
}

static inline int32_t timer_get_countdown()
{
  int32_t timer;
  asm volatile ( "mrc p15, 0, %[t], c14, c2, 0" : [t] "=r" (timer) );
  return timer;
}

static inline uint32_t timer_status()
{
  uint32_t bits;
  asm volatile ( "mrc p15, 0, %[config], c14, c2, 1" : [config] "=r" (bits) );
  return bits;
}

static inline void Send_Service_PostInit()
{
  register uint32_t service asm ( "r1" ) = 0x73;
  asm ( "svc %[swi]"
    :
    : [swi] "i" (OS_ServiceCall)
    , "r" (service)
    : "memory", "r0", "r2", "r3", "r4", "r5", "r6", "r7", "r8" );
}

static inline void Send_Service_ModeChange()
{
  register uint32_t service asm ( "r1" ) = 0x46;
  register void const *mode asm ( "r2" ) = &only_one_mode;
  register uint32_t monitor_type asm ( "r3" ) = 0;

  asm ( "svc %[swi]"
    :
    : [swi] "i" (OS_ServiceCall)
    , "r" (service)
    , "r" (mode)
    , "r" (monitor_type)
    : "memory", "r0", "r4", "r5", "r6", "r7", "r8" );
  // Registers corrupted by readvduvars2 in Wimp01 https://www.riscosopen.org/tracker/tickets/555
  // However "other registers up to R8 may be modified if the service was claimed" PRM1-256
}

typedef struct {
  uint32_t code;
  char message[];
} error_block;

static inline error_block *OSCLI( const char *command )
{
WriteS( "OSCLI " ); Write0( command ); NewLine;
  register const char *c asm( "r0" ) = command;
  register error_block *result;

  asm volatile ( "svc %[swi]"
             "\n  movvc %[error], #0"
             "\n  movvs %[error], r0"
             : [error] "=r" (result)
             : [swi] "i" (OS_CLI | Xbit)
             , "r" (c)
             : "lr", "cc", "memory" );

  return result;
}


static inline void SetApplicationMemory( uint32_t limit )
{
  register uint32_t handler asm ( "r0" ) = 0;
  register uint32_t new_limit asm ( "r1" ) = limit;
  register uint32_t r2 asm ( "r2" ) = 0;
  register uint32_t r3 asm ( "r3" ) = 0;

  asm volatile (
      "svc %[swi]"
      :
      : [swi] "i" (OS_ChangeEnvironment | Xbit)
      , "r" (handler)
      , "r" (new_limit)
      , "r" (r2)
      , "r" (r3)
      : "lr", "cc", "memory" );
}

void __attribute__(( noreturn )) UsrBoot()
{
  set_core_var(); // <CPU$Core>, read-only code variable.

  uint32_t core_number;

  asm ( 
    "\n  mov r0, %[op]"
    "\n  svc %[swi]"
    "\n  mov %[core], r0"
    : [core] "=r" (core_number)
    : [swi] "i" (OS_ThreadOp | 0x20000)
    , [op] "i" (TaskOp_CoreNumber)
    : "memory" );

  core_number = 0; // FIXME

  Sleep( 0 ); // Run HAL callbacks and/or tasks

  asm volatile ( "svc 0x13" ); // OS_IntOn

  {
    extern uint32_t _binary_Modules_MTWimp_start;
    //module_header *header = find_rom_module( "MultiTaskingWindowManager" );
    register uint32_t code asm( "r0" ) = 10;
    register uint32_t module asm( "r1" ) = 4 + (uint32_t) &_binary_Modules_MTWimp_start;

    asm volatile ( "svc %[os_module]"
       :
       : "r" (code)
       , "r" (module)
       , [os_module] "i" (OS_Module)
       : "lr", "cc", "memory" );
  }

  if (core_number == 0) {
    init_modules();

    //init_module( "UtilityModule" );
    //init_module( "FileSwitch" ); // needed by...

    extern uint32_t _binary_Modules_DumbFS_start;
    register uint32_t code asm( "r0" ) = 10;
    register uint32_t module asm( "r1" ) = 4 + (uint32_t) &_binary_Modules_DumbFS_start;

    asm volatile ( "svc %[os_module]"
       :
       : "r" (code)
       , "r" (module)
       , [os_module] "i" (OS_Module)
       : "lr", "cc", "memory" );

    // OSCLI( "Fat32FS" );
    OSCLI( "info DumbFS:603b10000_40000000" );
  }
  else {
    init_module( "UtilityModule" );
    init_module( "FileSwitch" ); // needed by...
    init_module( "ResourceFS" ); // needed by...
    init_module( "BASIC" );
  }
  WriteS( "Modules initialised" ); NewLine;

  Send_Service_PostInit();
  WriteS( "Post-init done" ); NewLine;
  Send_Service_ModeChange();
  WriteS( "Mode changed done" ); NewLine;

  SetApplicationMemory( 0xA8000 );

  WriteS( "About to run Resources:$.!Boot\n" );

  error_block *err = OSCLI( "Resources:$.!Boot.!Run" ); // FIXME Take out the .!Run when do_CLI fixed
  //error_block *err = OSCLI( "Desktop" );

  WriteS( "Failed to run Resources:$.!Boot, or it returned" );
for (;;) { asm volatile ( "mov r0, #255\n  svc 0xf9" : : : "r0" ); Sleep( 10 ); WriteS( "." ); }

  if (err != 0) for (;;) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  if (err != 0) { for (;;) { Sleep( 0 ); } for (;;) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); }

  __builtin_unreachable();
}
