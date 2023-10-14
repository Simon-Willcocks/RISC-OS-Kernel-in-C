/* Copyright 2023 Simon Willcocks
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

// Dumb file system
// Read-only
// Accesses SD card
// 4k pages
// Allocation blocks powers of two in size
// Lots of wastage
// Files always contiguous on disc

// Filenames DumbFS::$.<start_sector>_<size>
// e.g. Coronation: DumbFS::$.502b18_40000000
// Can probably get this information from FAT, later

// I took a fairly freshly formatted 31GB SD card and
// created two 4GiB files on it, one a video stream, the
// other with searchable values in it. Scanning the disc
// as a whole showed both to be contiguous space on the
// disc. I formatted the second using mkdosfs on Linux.

// I intend to pretend it's an DOSFS image file.

// Video 502b18000_40000000 (stream.dump)
// 4GiB can't be used as FAT, too big (for RISC OS) 603b10000_40000000 (4GiB)
// DOSFS 65bb0000_20000000 (2GiB)
// DOSFS e5bb8000_20000000 (2GiBa)

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing

// Explicitly no SWIs provided (it's the default, anyway)
#define MODULE_CHUNK "0"

#include "module.h"

NO_start;
//NO_init;
NO_finalise;
NO_service_call;
//NO_title;
NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

void __attribute__(( naked, section( ".text.init" ) )) fs_name()
{
  asm volatile ( ".asciz \"DumbFS\"\n  .align" );
}

void __attribute__(( naked, section( ".text.init" ) )) fs_startup()
{
  asm volatile ( ".asciz \"DumbFS startup string\"\n  .align" );
}

void *memcpy(void *dest, const void *src, unsigned len)
{
  uint8_t const *s = src;
  uint8_t *d = dest;
  while (*s != '\0' && len-- > 0) {
    *d++ = *s++;
  }
  return dest;
}

void __attribute__(( naked )) fs_open()
{
  // PRM 2-542
  // In: r0, r1, r3, r6
  // Out: r0, r1, r2, r3, r4
  asm ( "mov r0, #0"
    "\n  mov r1, #0"
    "\n  mov r2, #0"
    "\n  mov r3, #0"
    "\n  mov r4, #0"
  );
}

void __attribute__(( noinline )) c_fs_getbytes( uint32_t *stacked )
{
  WriteS( __func__ );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  NewLine;
}

void __attribute__(( noinline )) c_fs_putbytes( uint32_t *stacked )
{
  WriteS( __func__ );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  NewLine;
}

void __attribute__(( noinline )) c_fs_args( uint32_t *stacked )
{
  WriteS( __func__ );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  NewLine;
}

void __attribute__(( noinline )) c_fs_close( uint32_t *stacked )
{
  WriteS( __func__ );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  NewLine;
}

void __attribute__(( noinline )) c_fs_file( uint32_t *stacked )
{
  WriteS( __func__ ); WriteS( " IN:  " );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  NewLine;
  // Never fails!
  // Bother: Plan A fails: can't report a 4GiB file!
  Write0( (char*) stacked[1] ); NewLine;
  stacked[0] = 2; // File
  stacked[2] = 0xffffc800; // filetype...
  stacked[3] = 0; // timestamp...
  stacked[4] = 0x80000000;
  WriteS( __func__ ); WriteS( " OUT: " );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  NewLine;
}

void __attribute__(( noinline )) c_fs_func( uint32_t *stacked )
{
  WriteS( __func__ ); WriteS( " IN:  " );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  Space; WriteNum( stacked[4] );
  Space; WriteNum( stacked[5] );
  Space; WriteNum( stacked[6] );
  NewLine;
  static char const discname[] = "DumbDisc";
  switch (stacked[0]) {
  case 23: // Canonicalise special field (not supported) and disc name
    {
      if (stacked[1] == 0
       && stacked[2] == 0
       && stacked[3] == 0) {
        stacked[1] = 0; // No special fields
        stacked[2] = 1; // "any non-zero value"
        stacked[3] = 0; // Space needed for special field
        stacked[4] = sizeof( discname ) - 1; // Space needed for disc name
      }
      else {
        // TODO: check stacked[5], [6]
        char *disc = (void*) stacked[4];
        memcpy( disc, discname, sizeof( discname ) - 1 );
        stacked[1] = 0;
        stacked[2] = stacked[4];
        stacked[3] = 0;
        stacked[4] = 0;
      }
    }
    break;
  }

  WriteS( __func__ ); WriteS( " OUT: " );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  Space; WriteNum( stacked[4] );
  Space; WriteNum( stacked[5] );
  Space; WriteNum( stacked[6] );
  NewLine;
}

void __attribute__(( noinline )) c_fs_gbpb( uint32_t *stacked )
{
  WriteS( __func__ );
  Space; WriteNum( stacked[0] );
  Space; WriteNum( stacked[1] );
  Space; WriteNum( stacked[2] );
  Space; WriteNum( stacked[3] );
  NewLine;
}

void __attribute__(( naked )) fs_getbytes()
{
  asm ( "push {" C_CLOBBERED ", lr}" );
  register uint32_t *sp asm ( "sp" );
  c_fs_getbytes( sp );
  asm ( "pop {" C_CLOBBERED ", pc}" );
}

void __attribute__(( naked )) fs_putbytes()
{
  asm ( "push {" C_CLOBBERED ", lr}" );
  register uint32_t *sp asm ( "sp" );
  c_fs_putbytes( sp );
  asm ( "pop {" C_CLOBBERED ", pc}" );
}

void __attribute__(( naked )) fs_args()
{
  asm ( "push {" C_CLOBBERED ", lr}" );
  register uint32_t *sp asm ( "sp" );
  c_fs_args( sp );
  asm ( "pop {" C_CLOBBERED ", pc}" );
}

void __attribute__(( naked )) fs_close()
{
  asm ( "push {" C_CLOBBERED ", lr}" );
  register uint32_t *sp asm ( "sp" );
  c_fs_close( sp );
  asm ( "pop {" C_CLOBBERED ", pc}" );
}

void __attribute__(( naked )) fs_file()
{
  asm ( "push {" C_CLOBBERED ", lr}" );
  register uint32_t *sp asm ( "sp" );
  c_fs_file( sp );
  asm ( "pop {" C_CLOBBERED ", pc}" );
}

void __attribute__(( naked )) fs_func()
{
  // I know that C_CLOBBERED is r0-r3,r12, this stores more
  // I don't think any function uses r7+
  asm ( "push { r0-r6, r12, lr}" );
  register uint32_t *sp asm ( "sp" );
  c_fs_func( sp );
  asm ( "pop { r0-r6, r12, pc}" );
}

void __attribute__(( naked )) fs_gbpb()
{
  asm ( "push {" C_CLOBBERED ", lr}" );
  register uint32_t *sp asm ( "sp" );
  c_fs_gbpb( sp );
  asm ( "pop {" C_CLOBBERED ", pc}" );
}

void __attribute__(( naked, section( ".text.init" ) )) FSIB()
{
  asm (
      "fsib:"
    "\n  .word fs_name - header"
    "\n  .word fs_startup - header"
    "\n  .word fs_open - header"
    "\n  .word fs_getbytes - header"
    "\n  .word fs_putbytes - header"
    "\n  .word fs_args - header"
    "\n  .word fs_close - header"
    "\n  .word fs_file - header"
    "\n  .word 0b000111100110001110000000011111111" // FS Number 255 FIXME
    "\n  .word fs_func - header"
    "\n  .word fs_gbpb - header"
    "\n  .word 0b111"
  );
}

void __attribute__(( section( ".text.init" ) )) register_fs()
{
  error_block *error;
  register uint32_t code asm ( "r0" ) = 12;
  register uint32_t private asm ( "r3" ) = 0x12121212;

  asm volatile ( "adr r1, header"
             "\n  mov r2, #fsib - header"
             "\n  svc %[swi]"
             "\n  movvs %[error], r0"
             "\n  movvc %[error], #0"
             : [error] "=r" (error)
             : [swi] "i" (Xbit | OS_FSControl)
             , "r" (code)
             , "r" (private)
             : "r1", "r2", "lr"
             );

  if (error != 0) {
    WriteS( "Error" );
  }
}

void __attribute__(( naked, section( ".text" ) )) title()
{
  asm ( ".asciz \"DumbFS\"\n  .align" );
}

struct workspace {
  uint32_t placeholder;
};

void __attribute__(( noinline )) c_init( uint32_t this_core, uint32_t number_of_cores, struct workspace **private, char const *args )
{
  register_fs();
  clear_VF();
}

void __attribute__(( naked )) init( uint32_t this_core, uint32_t number_of_cores )
{
  struct workspace **private;
  char const *args;

  // Move r12, r10 into argument registers
  asm volatile (
          "push { lr }"
      "\n  mov %[private_word], r12"
      "\n  mov %[args_ptr], r10" : [private_word] "=r" (private), [args_ptr] "=r" (args) );

  c_init( this_core, number_of_cores, private, args );
  asm ( "pop { pc }" );
}

