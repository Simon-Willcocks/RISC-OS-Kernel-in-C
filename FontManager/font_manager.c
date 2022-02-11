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

/* Re-implementation of parts of the FontManager module from file format. */

/* Shared module, limited functionality. */

/*
  Supported fonts supported: Outline fonts, version 8, IntMetric0.

  Supported SWIs:

  Global/shared:
    Font_FindFont
    Font_LoseFont (does nothing, for now)

  Task(Slot) specific:
    Font_Paint
    Font_SetFont
    Font_CurrentFont

  Font handle -> { Font, size }
  Font -> { Metrics file, Outline font file }

  Task(Slot) remembers:
    * the current font handle
    * the current font colours (do some modules rely on this?)

  (maybe all found fonts, to Lose on exit?)
*/

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing
//   New feature: instead of one private word per core, r12 points to a shared
//   word, initialised by the first core to initialise the module.

#define MODULE_CHUNK "0x40080"
#include "module.h"

NO_start;
//NO_init;
NO_finalise;
NO_service_call;
//NO_title;
NO_help;
NO_keywords;
//NO_swi_handler;
//NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "FontManager";

static inline void clear_VF()
{
  asm ( "msr cpsr_f, #0" );
}

#define WriteS( string ) asm ( "svc 1\n  .string \""string"\"\n  .balign 4" : : : "lr" )

#define NewLine asm ( "svc 3" : : : "lr" )

#define Write0( string ) do { register uint32_t r0 asm( "r0" ) = (uint32_t) (string); asm ( "push { r0-r12, lr }\nsvc 2\n  pop {r0-r12, lr}" : : "r" (r0) ); } while (false)
#define Write13( string ) do { const char *s = (void*) (string); register uint32_t r0 asm( "r0" ); while (31 < (r0 = *s++)) { asm ( "push { r1-r12, lr }\nsvc 0\n  pop {r1-r12, lr}" : : "r" (r0) ); } } while (false)

static void WriteNum( uint32_t number )
{
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (c > '9') c += ('a' - '0' - 10);
    register uint32_t r0 asm( "r0" ) = c;
    asm( "svc 0": : "r" (r0) : "lr", "cc" );
  }
}

static void WriteSmallNum( uint32_t number, int min )
{
  bool started = false;
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (!started && c == '0' && nibble >= min) continue;
    started = true;
    if (c > '9') c += ('a' - '0' - 10);
    register uint32_t r0 asm( "r0" ) = c;
    asm( "svc 0": : "r" (r0) : "lr", "cc" );
  }
}

// This needs a defined struct workspace
C_SWI_HANDLER( c_swi_handler );

typedef struct Font Font;
typedef struct FontHandle FontHandle;

struct Font {
  Font *next;
  void *IntMetrics0;    // Starts with font name
  void *Outlines0;
};

struct FontHandle {
  Font *font;
  uint16_t xsize;
  uint16_t ysize;
};

struct workspace {
  uint32_t lock;

  Font *fonts;
  FontHandle found[256];
};

static void *rma_claim( uint32_t bytes )
{
  // XOS_Module 6 Claim
  register void *memory asm( "r2" );
  register uint32_t code asm( "r0" ) = 6;
  register uint32_t size asm( "r3" ) = bytes;
  asm ( "svc 0x2001e" : "=r" (memory) : "r" (size), "r" (code) : "lr" );

  return memory;
}

static int32_t int16_at( uint8_t *p )
{
  int32_t result = p[1];
  result = (result << 8) | p[0];
  return result;
}

static uint32_t uint16_at( uint8_t *p )
{
  uint32_t result = p[1];
  result = (result << 8) | p[0];
  return result;
}

void init( uint32_t this_core, uint32_t number_of_cores )
{
  struct workspace **private;
  // Preserve r12, in case we make a function call
  asm volatile ( "mov %[private_word], r12" : [private_word] "=r" (private) );

  bool first_entry = (*private == 0);

  struct workspace *workspace;

  if (first_entry) {
    *private = rma_claim( sizeof( *workspace ) );
  }

  workspace = *private;

  if (first_entry) {
    memset( workspace, 0, sizeof( *workspace ) );

    Font *the_font = rma_claim( sizeof( Font ) );
    the_font->next = 0;
    // The one true font: Trinity.Medium, located in ROM
    // In other words this is going to break he first time a ROM is re-built
    the_font->IntMetrics0 = (void*) 0xfc2f3470;
    the_font->Outlines0 = (void*) 0xfc2f38a0;

    // WIMPSymbol
    //the_font->IntMetrics0 = (void*) 0xfc169388;
    //the_font->Outlines0 = (void*) 0xfc169544;

    workspace->fonts = the_font;
  }

  Write0( "FontManager initialised" ); NewLine;

  if (first_entry) { Write0( "FontManager initialised" ); NewLine; }
}

static bool FindFont( struct workspace *workspace, SWI_regs *regs )
{
  return true;
}

static bool LoseFont( struct workspace *workspace, SWI_regs *regs )
{
  return true;
}

/* Access routines for IntMetrics0 format files (v. 2) */

/*
Components of an IntMetrics0 file:

Header
[ character map ]
[ bbox data ] (bottom left inclusive, top right exclusive)
[ x offsets ] 
[ y offsets ]
[ [ misc data area ]
  [ kern pair data area ]
  [ reserved area 1 ]
  [ reserved area 2 ]
]

*/
typedef struct __attribute__(( packed )) {
  char font_name[40];
  uint32_t constant_16[2];
  uint8_t nlo;
  uint8_t version;
  uint8_t no_bbox_data:1;
  uint8_t no_x_offsets:1;
  uint8_t no_y_offsets:1;
  uint8_t has_character_map:1;
  uint8_t sbz1:1;
  uint8_t has_extra_data:1;
  uint8_t kern_characters_16_bit:1;
  uint8_t sbz2:1;
  uint8_t nhi;
  uint16_t character_map_size;
} IntMetric0;

typedef struct __attribute__(( packed )) {
  int16_t left_inclusive;
  int16_t bottom_inclusive;
  int16_t width;
  int16_t height;
} Font_BBox;                                    // WATCH OUT! L, B, R, T, or L, B, W, H?

typedef struct __attribute__(( packed )) {
  Font_BBox font_max_bbox;
  int16_t default_x_offset;
  int16_t default_y_offset;
  int16_t italic_h_offset;
  int8_t  underline_position;
  int8_t  underline_thickness;
  int16_t cap_height;
  int16_t x_height;
  int16_t descender;
  int16_t ascender;
  int16_t sbz[2];
} IntMetrics0_Misc_Data;

static uint32_t IntMetrics0_num( IntMetric0 *header )
{
  return (header->nhi << 8) | header->nlo;
}

static void *pointer_at_offset_from( void *base, uint32_t off )
{
  return ((uint8_t*) base) + off;
}

static uint8_t *IntMetrics0_character_map( IntMetric0 *header )
{
  if (!header->has_character_map || header->character_map_size == 0) {
    return 0;
  }

  assert (sizeof( IntMetric0 ) == 54);

  return pointer_at_offset_from( header, 54 );
}

static int16_t IntMetrics0_char_index( IntMetric0 *header, uint32_t ch )
{
  uint8_t *map = IntMetrics0_character_map( header );
  if (map != 0) {
    ch = map[ch];
  }
  return ch;
}

static int16_t *IntMetrics0_bboxes( IntMetric0 *header )
{
  if (header->no_bbox_data) return 0;

  uint32_t off = 52;

  if (header->has_character_map) {
    off += 2;   // For the length field
    off += header->character_map_size;
  }
  else
    off += 256;

  return pointer_at_offset_from( header, off );
}

static int16_t *IntMetrics0_x_offsets( IntMetric0 *header )
{
  if (header->no_x_offsets) return 0;

  uint32_t off = 52;
  uint32_t n = IntMetrics0_num( header );

  if (header->has_character_map) {
    off += 2;   // For the length field
    off += header->character_map_size;
  }
  else
    off += 256;

  if (!header->no_bbox_data) {
    assert( 8 == sizeof( Font_BBox ) );
    off += n * sizeof( Font_BBox );
  }

  return pointer_at_offset_from( header, off );
}

static int16_t *IntMetrics0_y_offsets( IntMetric0 *header )
{
  if (header->no_y_offsets) return 0;

  uint32_t off = 52;
  uint32_t n = IntMetrics0_num( header );

  if (header->has_character_map) {
    off += 2;   // For the length field
    off += header->character_map_size;
  }
  else
    off += 256;

  if (!header->no_bbox_data) {
    assert( 8 == sizeof( Font_BBox ) );
    off += n * sizeof( Font_BBox );
  }

  if (!header->no_x_offsets) {
    off += sizeof( int16_t ) * n;
  }

  return pointer_at_offset_from( header, off );
}

static uint16_t *IntMetrics0_extra_offsets( IntMetric0 *header )
{
  if (!header->has_extra_data) return 0;

  uint32_t off = 52;
  uint32_t n = IntMetrics0_num( header );

  if (header->has_character_map) {
    off += 2;   // For the length field
    off += header->character_map_size;
  }
  else
    off += 256;

  if (!header->no_bbox_data) {
    assert( 8 == sizeof( Font_BBox ) );
    off += n * sizeof( Font_BBox );
  }

  if (!header->no_x_offsets) {
    off += sizeof( int16_t ) * n;
  }

  if (!header->no_y_offsets) {
    off += sizeof( int16_t ) * n;
  }

  return pointer_at_offset_from( header, off );
}

static IntMetrics0_Misc_Data *IntMetrics0_misc_data( IntMetric0 *header )
{
  uint16_t *offsets = IntMetrics0_extra_offsets( header );

  if (0 == offsets) return 0;

  return pointer_at_offset_from( offsets, offsets[0] );
}

static void *IntMetrics0_kern_pair_data( IntMetric0 *header )
{
  uint16_t *offsets = IntMetrics0_extra_offsets( header );

  if (0 == offsets) return 0;

  return pointer_at_offset_from( offsets, offsets[1] );
}

static int16_t IntMetrics0_x_offset( IntMetric0 *header, uint32_t ch )
{
  int16_t const *offsets = IntMetrics0_x_offsets( header );

  if (0 == offsets) return 0;

  return pointer_at_offset_from( header, offsets[IntMetrics0_char_index( header, ch )] );
}

static int16_t IntMetrics0_y_offset( IntMetric0 *header, uint32_t ch )
{
  int16_t const *offsets = IntMetrics0_y_offsets( header );

  if (0 == offsets) return 0;

  return pointer_at_offset_from( header, offsets[IntMetrics0_char_index( header, ch )] );
}

/* End of access routines for IntMetrics0 format files (v. 2) */

/* Access routines for outline font files v. 8 */

/*
Components of an outline font file v. 8:

Header 
*/

typedef struct __attribute__(( packed )) {
  uint32_t font;        // "FONT", 0x554e4f48                   // 0x00
  uint8_t  bpp;         // 0 for outline fonts
  uint8_t  version;     // 8 for only supported version
  uint16_t design_size;
  Font_BBox font_max_bbox;
  uint32_t offset_to_chunk_offsets;                             // 0x10
  uint32_t number_of_chunks;
  uint32_t number_of_scaffold_index_entries;
  uint32_t all_16_bit:1;
  uint32_t do_not_anti_alias:1;
  uint32_t sbz_flags:30;
  uint32_t sbz[5];                                              // 0x20
  uint16_t scaffold_data[]; // scaffold_data[0] is size of table// 0x34
} OutlineFontFile;

static uint32_t OutlineFontFile_chunks_offsets( OutlineFontFile *file )
{
  return pointer_at_offset_from( file, file->offset_to_chunk_offsets );
}

static void ShowScaffoldLineX( uint8_t *entry )
{
}

struct Scaffold {
  uint16_t coord:12;
  uint16_t link_index:3;
  uint16_t linear:1;
  uint8_t width;
};

static struct Scaffold ReadScaffold( uint8_t *entry )
{
  union {
    struct {
      uint16_t coord:12;
      uint16_t link_index:3;
      uint16_t linear:1;
    } bits;
    uint16_t raw;
  } scaffold = { .raw = uint16_at( entry ) };

  struct Scaffold result = { .coord = scaffold.bits.coord,
                             .link_index = scaffold.bits.link_index,
                             .linear = scaffold.bits.linear,
                             .width = entry[2] };

  WriteSmallNum( result.coord, 1 ); Write0( " " );
  WriteSmallNum( result.link_index, 1 ); Write0( " " );
  WriteSmallNum( result.linear, 1 ); Write0( " " );
  Write0( " width " ); WriteSmallNum( result.width, 1 ); NewLine;

  return result;
}

static void ShowScaffoldEntry( uint8_t *entry, uint32_t base )
{
  // Pointer "entry" points to the byte after the base, whether it's one or two bytes
  Write0( "Scaffolding, base char: " ); WriteSmallNum( base, 1 ); NewLine;

  // Read the flags.
  uint8_t base_x_scaffolds = entry[0];
  uint8_t base_y_scaffolds = entry[1];
  uint8_t local_x_scaffolds = entry[2];
  uint8_t local_y_scaffolds = entry[3];


  struct Scaffold x_scaffold[8];
  struct Scaffold y_scaffold[8];
  uint8_t x_scoffold_set = 0;
  uint8_t y_scoffold_set = 0;

  // Reading Fonts04 as documentation
  // No rendermatrix yet... TODO
  uint8_t *local_scaffolds = entry + 4;

  if (local_x_scaffolds != 0) {
    Write0( "Local X scaffolds:" ); NewLine;
    for (int i = 0; i < 8; i++) {
      if (0 != (local_x_scaffolds & (1 << i))) {
        x_scaffold[i] = ReadScaffold( local_scaffolds );
        local_scaffolds += 3;
      }
    }
  }
  else {
    Write0( "No local X scaffolds" ); NewLine;
  }

  if (local_x_scaffolds != 0) {
    Write0( "Local Y scaffolds:" ); NewLine;
    for (int i = 0; i < 8; i++) {
      if (0 != (local_y_scaffolds & (1 << i))) {
        y_scaffold[i] = ReadScaffold( local_scaffolds );
        local_scaffolds += 3;
      }
    }
  }
  else {
    Write0( "No local Y scaffolds" ); NewLine;
  }
}

static void PaintChar( Font *font, uint32_t ch )
{
  IntMetric0 const * const metrics = font->IntMetrics0;
  OutlineFontFile const * const outline_font = font->Outlines0;

  uint32_t max_char = IntMetrics0_num( metrics );

  uint16_t index = IntMetrics0_char_index( metrics, ch );
  Write0( "Index: " ); WriteNum( index ); NewLine;
  if (index > max_char) {
    Write0( "Character out of range" ); NewLine;
    return;
  }

  uint32_t offset = (char*) outline_font->scaffold_data - (char*) outline_font;

  if (outline_font->scaffold_data[ch] != 0) {
    uint16_t data = outline_font->scaffold_data[ch];
    uint16_t off;
    bool base_8bit;
    if (outline_font->all_16_bit) {
      off = data;
      base_8bit = false;
    }
    else {
      base_8bit = 0 == (0x8000 & data);
      off = data & 0x7fff;
    }
    uint16_t *scaffolding = pointer_at_offset_from( outline_font->scaffold_data, off );
    register c asm( "r0" ) = ch;
    asm( "svc 0" : : "r" (c) : "lr" );
    Write0( " " ); WriteSmallNum( ch, 1 ); Write0( " " ); WriteSmallNum( off + offset, 1 ); Write0( " " ); WriteSmallNum( scaffolding, 1 ); NewLine;
    if (base_8bit) {
      uint32_t base = *(uint8_t*) scaffolding;
      ShowScaffoldEntry( pointer_at_offset_from( scaffolding, 1 ), base );
    }
    else {
      uint32_t base = uint16_at( scaffolding );
      ShowScaffoldEntry( pointer_at_offset_from( scaffolding, 2 ), base );
    }
  }
}

static const uint8_t *read_12bit_pair( uint8_t const *v, int16_t* x, int16_t* y )
{
  *x = v[0] | ((v[1] & 0xf) <<8);
  if (0 != (*x & 0x800)) *x |= 0xf000;
  *y = (v[2] << 4) | ((v[1] & 0xf0) >> 4);
  if (0 != (*y & 0x800)) *y |= 0xf000;
  return v + 3;
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

static void SetColour( uint32_t flags, uint32_t colour )
{
  register uint32_t in_flags asm( "r0" ) = flags;
  register uint32_t in_colour asm( "r1" ) = colour;
  asm ( "swi %[swi]" : 
        : "r" (in_colour)
        , "r" (in_flags)
        , [swi] "i" (0x20061) // (OS_SetColour)
        : "lr", "cc" );
}

static void SetGraphicsFgColour( uint32_t colour )
{
  Write0( "Setting graphics foreground colour with ColourTrans... " );
  register uint32_t pal asm( "r0" ) = colour;
  register uint32_t Rflags asm( "r3" ) = 0; // FG, no ECFs
  register uint32_t action asm( "r4" ) = 0; // set
  asm ( "svc %[swi]" : : [swi] "i" (0x60743), "r" (pal), "r" (Rflags), "r" (action) : "lr", "cc" );
}

static void Draw_Fill( uint32_t *path, uint8_t style, int32_t *transformation_matrix )
{
  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = style;
  register  int32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 0;
  asm ( "swi 0x60702"
        : 
        : "r" (draw_path)
        , "r" (fill_style)
        , "r" (matrix)
        , "r" (flatness)
        : "lr", "cc" );
}

void Draw_Stroke( uint32_t *path, uint32_t thick, uint32_t *transformation_matrix )
{
  // Keep this declaration before the first register variable declaration, or
  // -Os will cause the compiler to forget the preceding registers.
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101422
  uint32_t cap_and_join_style[4] =  { 0, 0xa0000, 0, 0 };

  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = 0;
  register uint32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 0;
  register uint32_t thickness asm( "r4" ) = thick;
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
        : "lr" );
}

static uint8_t const *make_path( uint8_t const *next_byte, uint32_t *path )
{
  bool terminated = false;
  do {
    uint8_t code = *next_byte++;
    switch (3 & code) {
    case 0:
      {
        Write0( "Term " ); WriteSmallNum( code, 2 ); NewLine;
        terminated = true;
        *path = 0;
      }
      break;
    case 1:
      {
        Write0( "Move " );
        int16_t x;
        int16_t y;
        next_byte = read_12bit_pair( next_byte, &x, &y );
        WriteSmallNum( x, 4 ); Write0( ", " ); WriteSmallNum( y, 4 ); NewLine;
        *path++ = 2; *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
      }
      break;
    case 2:
      {
        Write0( "Line " );
        int16_t x;
        int16_t y;
        next_byte = read_12bit_pair( next_byte, &x, &y );
        WriteSmallNum( x, 4 ); Write0( ", " ); WriteSmallNum( y, 4 ); NewLine;
        *path++ = 8; *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
      }
      break;
    case 3:
      {
        Write0( "Curve " );
        int16_t x;
        int16_t y;
        *path++ = 6;
        next_byte = read_12bit_pair( next_byte, &x, &y );
        WriteSmallNum( x, 4 ); Write0( ", " ); WriteSmallNum( y, 4 ); Write0( "; " );
        *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
        next_byte = read_12bit_pair( next_byte, &x, &y );
        WriteSmallNum( x, 4 ); Write0( ", " ); WriteSmallNum( y, 4 ); Write0( "; " );
        *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
        next_byte = read_12bit_pair( next_byte, &x, &y );
        WriteSmallNum( x, 4 ); Write0( ", " ); WriteSmallNum( y, 4 ); NewLine;
        *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
      }
    }
  } while (!terminated);

  return next_byte;
}

static void ShowCharacter( uint8_t const *ch, uint32_t *matrix )
{
  // Note: the flags word are for versions 8+
  union {
    struct {
      uint8_t coords_12bit:1;
      uint8_t data_1bpp:1;
      uint8_t initial_pixel_black:1;
      uint8_t outline:1;
      uint8_t composite:1;
      uint8_t has_accent:1;
      uint8_t codes_16bit:1;
      uint8_t sbz:1;
    };
    uint8_t raw;
  } character = { .raw = *ch };

  if (character.coords_12bit) { Write0( "12 bit coordinates" ); NewLine; }
  if (character.data_1bpp) { Write0( "1 bit per pixel (or outline)" ); NewLine; }
  if (character.initial_pixel_black) { Write0( "Initial pixel black" ); NewLine; }
  if (character.outline) { Write0( "Outline" ); NewLine; }
  if (character.composite) { Write0( "composite" ); NewLine; }
  if (character.has_accent) { Write0( "Has accent" ); NewLine; }
  if (character.codes_16bit) { Write0( "16-bit character codes" ); NewLine; }

  uint8_t const *next_byte = ch+1;
  uint16_t base_character = 0; // Only important if character.composite

  if (character.outline) {
    if (character.composite) {
      if (character.codes_16bit) {
        base_character = uint16_at( next_byte );
        next_byte += 2;
      }
      else {
        base_character = *next_byte++;
      }
    }

    if (character.has_accent) {
      return;
      asm ( "bkpt 1" ); // TODO FIXME
    }
  }

  Font_BBox bbox = { 0 };
  if (character.outline == 0 || character.composite == 0) {
    if (character.coords_12bit) {
      Write0( "12-bits BBox: " );
      WriteSmallNum( next_byte[0], 2 ); 
      WriteSmallNum( next_byte[1], 2 ); 
      WriteSmallNum( next_byte[2], 2 ); 
      WriteSmallNum( next_byte[3], 2 ); 
      WriteSmallNum( next_byte[4], 2 ); 
      WriteSmallNum( next_byte[5], 2 ); 
      NewLine;

      next_byte = read_12bit_pair( next_byte, &bbox.left_inclusive, &bbox.bottom_inclusive );
      next_byte = read_12bit_pair( next_byte, &bbox.width, &bbox.height );
    }
    else {
      bbox.left_inclusive = *(int8_t*)next_byte++;
      bbox.bottom_inclusive = *(int8_t*)next_byte++;
      bbox.width = *(int8_t*)next_byte++;
      bbox.height = *(int8_t*)next_byte++;
    }

    Write0( "BBox: " );
    WriteSmallNum( bbox.left_inclusive, 4 ); Write0( ", " );
    WriteSmallNum( bbox.bottom_inclusive, 4 ); Write0( ", " );
    WriteSmallNum( bbox.width, 4 ); Write0( ", " );
    WriteSmallNum( bbox.height, 4 ); NewLine;
  }

    SetColour( 0, 0x00e50000 );

  uint32_t path[64];

  next_byte = make_path( next_byte, path );
  Draw_Fill( path, 0x32, matrix );
  if (0 != (next_byte[-1] & 8)) {
    SetColour( 0, 0xe5000000 );
  matrix[4] += 64*256;
    next_byte = make_path( next_byte, path );
    Draw_Stroke( path, 0x18, matrix );
    SetColour( 0, 0x00e50000 );
  }
  matrix[4] += 64*256;

  assert( character.sbz == 0 );
}

static void ShowChunk( Font *font, uint32_t *chunk, int index )
{
  // File format requires chunks are work aligned.
  assert( 0 == (3 & (uint32_t) chunk) );

  union {
    struct {
      uint32_t horizontal_subpixel_placement:1;
      uint32_t vertical_subpixel_placement:1;
      uint32_t sbz1:5;
      uint32_t dependency_bytes:1;
      uint32_t sbz2:23;
      uint32_t sbo:1;
    };
    uint32_t raw;
  } flags = { .raw = *chunk };

uint32_t matrix[6] = { 0x2000, 0, 0, 0x2000, 0x1000, 0x4000 * index };
  OutlineFontFile const * const outline_font = font->Outlines0;

  Write0( "Chunk" ); NewLine;
  uint32_t *next_word = chunk + 1;
  for (int i = 0; i < 32; i++) { // 32 characters
    Write0( "Character: " ); WriteSmallNum( i, 1 ); Write0( " " );
    WriteSmallNum( (uint32_t) pointer_at_offset_from( next_word, next_word[i] ) - (uint32_t) outline_font, 4 ); NewLine;
    if (next_word[i] != 0) {
      ShowCharacter( pointer_at_offset_from( next_word, next_word[i] ), matrix );
    }
  }

  uint8_t const *bytes = (void*) (next_word + 8);

  if (flags.dependency_bytes) {
    Write0( "Dependency bytes" ); NewLine;
    //asm ( "bkpt 1" ); // TODO
  }

  for (int i = 0; i < 100; i++) {
    int16_t x;
    int16_t y;
    read_12bit_pair( bytes+i, &x, &y );
    WriteSmallNum( x, 1 ); Write0( ", " ); WriteSmallNum( y, 1 ); NewLine;
  }
}

static void ShowFont( Font *font )
{
  IntMetric0 const * const metrics = font->IntMetrics0;
  OutlineFontFile const * const outline_font = font->Outlines0;

  Write0( "Font: " ); Write13( metrics->font_name ); NewLine;

  Write0( "BBox: " );

  WriteSmallNum( outline_font->font_max_bbox.left_inclusive, 4 ); Write0( ", " );
  WriteSmallNum( outline_font->font_max_bbox.bottom_inclusive, 4 ); Write0( ", " );
  WriteSmallNum( outline_font->font_max_bbox.width, 4 ); Write0( ", " );
  WriteSmallNum( outline_font->font_max_bbox.height, 4 ); NewLine;

  uint32_t max_char = IntMetrics0_num( metrics );
  Write0( "Number of chars: " ); WriteNum( max_char ); NewLine;

  Write0( "Number of chunks: " ); WriteSmallNum( outline_font->number_of_chunks, 1 ); NewLine;
  uint32_t *chunks = OutlineFontFile_chunks_offsets( outline_font );

  Write0( "File size: " ); WriteSmallNum( chunks[outline_font->number_of_chunks], 1 ); NewLine;
  for (int i = 0; i < outline_font->number_of_chunks; i++) {
    Write0( "Chunk " ); WriteSmallNum( i, 1 ); Write0( " " ); WriteSmallNum( chunks[i], 1 ); Write0( " " ); WriteNum( pointer_at_offset_from( outline_font, chunks[i] ) ); NewLine;
    ShowChunk( font, pointer_at_offset_from( outline_font, chunks[i] ), i );
  }

  Write0( "Number of scaffold indices: " ); WriteSmallNum( outline_font->number_of_scaffold_index_entries, 1 ); 
  Write0( ", size " ); WriteSmallNum( outline_font->scaffold_data[0], 1 ); NewLine;

  for (int i = 1; i < outline_font->number_of_scaffold_index_entries; i++) {
    if (outline_font->scaffold_data[i] != 0) {
      register c asm( "r0" ) = i;
      asm( "svc 0" : : "r" (c) : "lr" );
      Write0( " " ); WriteSmallNum( i, 1 ); Write0( " " ); WriteSmallNum( outline_font->scaffold_data[i], 1 ); NewLine;
    }
  }

  uint8_t *entry = pointer_at_offset_from( outline_font->scaffold_data, 2 * outline_font->number_of_scaffold_index_entries );
  if (entry[0] == 0) Write0( "Always draw scaffolding" );
  else { Write0( "Skeleton threshold " ); WriteSmallNum( entry[0], 1 ); }

  Write0( ((char*) &outline_font->scaffold_data[0]) + outline_font->scaffold_data[0] ); NewLine;
}

static bool Paint( struct workspace *workspace, SWI_regs *regs )
{
  Write0( "Paint \\\"" );
  Write0( regs->r[1] );
  Write0( "\\\"" );
  NewLine;
  char *p = (void*) regs->r[1];
  char ch;
  // One true font
  ShowFont( workspace->fonts );
  while ((ch = *p++) >= ' ') {
    PaintChar( workspace->fonts, ch );
  }
  return true;
}

static bool SetPalette( struct workspace *workspace, SWI_regs *regs )
{
  Write0( "SetPalette BG: " ); WriteNum( regs->r[1] );
  Write0( ", FG: " ); WriteNum( regs->r[2] );
  Write0( ", off: " ); WriteNum( regs->r[3] );
  Write0( ", BG BGR: " ); WriteNum( regs->r[4] );
  Write0( ", FG BGR: " ); WriteNum( regs->r[5] );
  NewLine;
  return true;
}

static bool SetColourTable( struct workspace *workspace, SWI_regs *regs )
{
  Write0( "SetColourTable" ); NewLine;
  return true;
}

bool __attribute__(( noinline )) c_swi_handler( struct workspace *workspace, SWI_regs *regs )
{
  NewLine; Write0( "Handling Font SWI " ); WriteNum( regs->number ); NewLine;

  switch (regs->number) {
  case 0x01: return FindFont( workspace, regs );
  case 0x02: return LoseFont( workspace, regs );
  case 0x06: return Paint( workspace, regs );
  case 0x13: return SetPalette( workspace, regs );
  case 0x22: return SetColourTable( workspace, regs );
  }
  static const error_block error = { 0x1e6, "Bad FontManager SWI" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

char const swi_names[] = { "Font" 
          "\0CacheAddr" 
          "\0FindFont" 
          "\0LoseFont" 
          "\0ReadDefn" 
          "\0ReadInfo" 
          "\0StringWidth" 
          "\0Paint" 
          "\0Caret" 
          "\0ConverttoOS" 
          "\0Converttopoints"
          "\0SetFont"
          "\0CurrentFont"
          "\0FutureFont"
          "\0FindCaret"
          "\0CharBBox"
          "\0ReadScaleFactor"
          "\0SetScaleFactor"
          "\0ListFonts"
          "\0SetFontColours"
          "\0SetPalette"
          "\0ReadThresholds"
          "\0SetThresholds"
          "\0FindCaretJ"
          "\0StringBBox"
          "\0ReadColourTable"
          "\0MakeBitmap"
          "\0UnCacheFile"
          "\0SetFontMax"
          "\0ReadFontMax"
          "\0ReadFontPrefix"
          "\0SwitchOutputToBuffer"
          "\0ReadFontMetrics"
          "\0DecodeMenu"
          "\0ScanString"
          "\0SetColourTable"
          "\0CurrentRGB"
          "\0FutureRGB"
          "\0ReadEncodingFilename"
          "\0FindField"
          "\0ApplyFields"
          "\0LookupFont"
          "\0" };
