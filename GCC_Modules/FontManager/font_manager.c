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

/*
  Painting a character (outline format only):

  A character will have a bounding box, or use the bounding box of the font.
  A character may have an associated base character,
  it may have an accent (like a base character, but with an offset).
  All three will contain a path description in design coordinates.
  All three may contain strokes to be drawn thin if the output is too small.

  Find the character from its Unicode/ASCII code, if neccessary its base
  and accent characters as well. (This involves checking the Encodings, I
  think. TODO, when strings come out as characters, but the wrong ones!)

  Build two Draw paths, one for filling, the other for stroking.

  So:
  error_block *MakeCharPaths( Font const *font,
                              uint32_t ch,
                              uint32_t *fill_path,
                              uint32_t *stroke_path,
                              Font_BBox *bb );

  Recurse to paint the base and the accent, if present.

  The passed in path arrays will contain the number of free words in the
  first word.

  A font could be written to infinitely recurse, or simply have very
  complicated paths, so MakeCharPaths will have to have the ability to return
  an error (or throw an exception). (The font could be malicious, if we
  ever get any security that makes it worthwhile.)


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

static void WriteSmallNum( uint32_t number, int min = 1 )
{
  char buf[8];
  char *p = &buf[7];
  int chars = 0;
  while (number != 0 || chars < min) {
    char c = '0' + (number & 0xf)
    if (c > '9') c += ('a' - '0' - 10);
    number = number >> 4;
    *p-- = c;
    chars++;
  }
  debug_string_with_length( p, chars );
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

static int32_t int16_at( uint8_t const *p )
{
  int32_t result = p[1];
  result = (result << 8) | p[0];
  return result;
}

static uint32_t uint16_at( uint8_t const *p )
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
    the_font->IntMetrics0 = (void*) FONT_METRICS + 36; // 0xfc2f3470; strings latest.bin -t x | grep Trinity.Medium.Int
    the_font->Outlines0 = (void*) FONT_OUTLINE + 36; // 0xfc2f38a0;   strings latest.bin -t x | grep Trinity.Medium.Out

    // WIMPSymbol
    //the_font->IntMetrics0 = (void*) 0xfc169388;
    //the_font->Outlines0 = (void*) 0xfc169544;

    workspace->fonts = the_font;
  }

  if (first_entry) { Write0( "FontManager initially initialised" ); NewLine; }
  else { Write0( "FontManager initialised" ); NewLine; }

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

static uint32_t IntMetrics0_num( IntMetric0 const *header )
{
  return (header->nhi << 8) | header->nlo;
}

static void const *pointer_at_offset_from( void const *base, uint32_t off )
{
  return ((uint8_t*) base) + off;
}

static uint8_t const *IntMetrics0_character_map( IntMetric0 const *header )
{
  if (!header->has_character_map || header->character_map_size == 0) {
    return 0;
  }

  assert (sizeof( IntMetric0 ) == 54);

  return pointer_at_offset_from( header, 54 );
}

static int16_t IntMetrics0_char_index( IntMetric0 const *header, uint32_t ch )
{
  uint8_t const *map = IntMetrics0_character_map( header );
  if (map != 0) {
    ch = map[ch];
  }
  return ch;
}

static int16_t const *IntMetrics0_bboxes( IntMetric0 const *header )
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

static int16_t const *IntMetrics0_x_offsets( IntMetric0 const *header )
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

static int16_t const *IntMetrics0_y_offsets( IntMetric0 const *header )
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

static uint16_t const *IntMetrics0_extra_offsets( IntMetric0 const *header )
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

static IntMetrics0_Misc_Data const *IntMetrics0_misc_data( IntMetric0 const *header )
{
  uint16_t const *offsets = IntMetrics0_extra_offsets( header );

  if (0 == offsets) return 0;

  return pointer_at_offset_from( offsets, offsets[0] );
}

static void const *IntMetrics0_kern_pair_data( IntMetric0 const *header )
{
  uint16_t const *offsets = IntMetrics0_extra_offsets( header );

  if (0 == offsets) return 0;

  return pointer_at_offset_from( offsets, offsets[1] );
}

static int16_t IntMetrics0_x_offset( IntMetric0 const *header, uint32_t ch )
{
  int16_t const *offsets = IntMetrics0_x_offsets( header );

  if (0 == offsets) return 0;

  return *(int16_t*) pointer_at_offset_from( header, offsets[IntMetrics0_char_index( header, ch )] );
}

static int16_t IntMetrics0_y_offset( IntMetric0 const *header, uint32_t ch )
{
  int16_t const *offsets = IntMetrics0_y_offsets( header );

  if (0 == offsets) return 0;

  return *(int16_t*) pointer_at_offset_from( header, offsets[IntMetrics0_char_index( header, ch )] );
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

static uint32_t OutlineFontFile_chunks_offsets( OutlineFontFile const *file )
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

#ifdef DEBUG__VERBOSE
  WriteSmallNum( result.coord, 1 ); Write0( " " );
  WriteSmallNum( result.link_index, 1 ); Write0( " " );
  WriteSmallNum( result.linear, 1 ); Write0( " " );
  Write0( " width " ); WriteSmallNum( result.width, 1 ); NewLine;
#endif
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

// Read the coordinates at v, 8 or 12 bits
static const uint8_t *read_font_coord_pair( uint8_t const *v, bool wide, int16_t* x, int16_t* y )
{
  if (wide) {
    *x = v[0] | ((v[1] & 0xf) <<8);
    if (0 != (*x & 0x800)) *x |= 0xf000;
    *y = (v[2] << 4) | ((v[1] & 0xf0) >> 4);
    if (0 != (*y & 0x800)) *y |= 0xf000;
    return v + 3;
  }
  else {
    *x = *(int8_t*) (v);
    *y = *(int8_t*) (v+1);
    return v + 2;
  }
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
#ifdef DEBUG__VERBOSE
  Write0( "Setting graphics foreground colour with ColourTrans... " );
#endif
  register uint32_t pal asm( "r0" ) = colour;
  register uint32_t Rflags asm( "r3" ) = 0; // FG, no ECFs
  register uint32_t action asm( "r4" ) = 0; // set
  asm ( "svc %[swi]" : : [swi] "i" (0x60743), "r" (pal), "r" (Rflags), "r" (action) : "lr", "cc" );
}

static void Font_Draw_TransformPath( uint32_t *path, int32_t *transformation_matrix )
{
  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t output asm( "r1" ) = 0; // Overwrite
  register  int32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t zero asm( "r3" ) = 0;
  asm ( "swi 0x6070a"
        :
        : "r" (draw_path)
        , "r" (output)
        , "r" (matrix)
        , "r" (zero)
        : "lr", "cc" );
}

static void Font_Draw_Fill( uint32_t *path, int32_t *transformation_matrix )
{
  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = 0x30; // V8 non-zero filling
  register  int32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 0;   // Set to 100 results in access to 0x5000...? FIXME
  asm ( "swi 0x60702"
        :
        : "r" (draw_path)
        , "r" (fill_style)
        , "r" (matrix)
        , "r" (flatness)
        : "lr", "cc" );
}

void Font_Draw_Stroke( uint32_t *path, int32_t *transformation_matrix )
{
  register uint32_t *draw_path asm( "r0" ) = path;
  register uint32_t fill_style asm( "r1" ) = 0x18;
  register int32_t *matrix asm( "r2" ) = transformation_matrix;
  register uint32_t flatness asm( "r3" ) = 100;
  register uint32_t thickness asm( "r4" ) = 0;
  register uint32_t *cap_and_join asm( "r5" ) = 0;
  register uint32_t dashes asm( "r6" ) = 0;
  asm ( "swi 0x60704" :
        : "r" (draw_path)
        , "r" (fill_style)
        , "r" (matrix)
        , "r" (flatness)
        , "r" (thickness)
        , "r" (cap_and_join)
        , "r" (dashes)
        : "lr", "cc" );
}

// On entry, the path array must be initialised with the number of
// remaining usable elements in the array at index 0.
// If the array is too small, the draw path will be prematurely terminated
// and null returned, since the path wasn't completed.
// The returned value is the address of the font terminator byte, or null.
static uint8_t const *FontToDrawPath( uint8_t const *next_byte, bool wide, uint32_t *path )
{
  uint32_t remaining_space = path[0];
  static uint32_t const termination_space = 1;
  bool terminated = remaining_space <= termination_space;
  while (!terminated) {
    uint8_t code = *next_byte++;
    switch (3 & code) {
    case 0:
      {
        // Term
        next_byte--;
        terminated = true;
      }
      break;
    case 1:
    case 2:
      {
        // Move
        int16_t x;
        int16_t y;
        next_byte = read_font_coord_pair( next_byte, wide, &x, &y );
        if (remaining_space >= 3 + termination_space) {
          remaining_space -= 3; // code, x, y

          if (1 == (3 & code))
            *path++ = 2; // Move
          else
            *path++ = 8; // Line
          *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
        }
        else {
          next_byte = 0;
          terminated = true;
        }
      }
      break;
    case 3:
      {
        // Curve
        int16_t x;
        int16_t y;
        if (remaining_space >= 7 + termination_space) {
          remaining_space -= 7; // code, control 1, control 2, endpoint x, y

          *path++ = 6;
          next_byte = read_font_coord_pair( next_byte, wide, &x, &y );
          *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
          next_byte = read_font_coord_pair( next_byte, wide, &x, &y );
          *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
          next_byte = read_font_coord_pair( next_byte, wide, &x, &y );
          *path++ = ((int32_t) x) << 8; *path++ = ((int32_t) y) << 8;
        }
        else {
          next_byte = 0;
          terminated = true;
        }
      }
    }
  }

  if (remaining_space <= termination_space) {
    next_byte = 0; // in case given a too small array
  }
  else {
    *path++ = 0; // End path
  }

  return next_byte;
}

typedef struct {        // 32-bit word
  uint32_t horizontal_subpixel_placement:1;
  uint32_t vertical_subpixel_placement:1;
  uint32_t sbz1:5;
  uint32_t dependency_bytes:1;
  uint32_t sbz2:23;
  uint32_t sbo:1;
} FontChunkFlags;

typedef struct {        // Single byte
  uint8_t coords_12bit:1;
  uint8_t data_1bpp:1;
  uint8_t initial_pixel_black:1;
  uint8_t outline:1;
  uint8_t composite:1;
  uint8_t has_accent:1;
  uint8_t codes_16bit:1;
  uint8_t sbz:1;
} FontCharacterFlags;

FontCharacterFlags read_font_character_flags( uint8_t byte )
{
  union {
    FontCharacterFlags flags;
    uint8_t raw;
  } flags_byte = { .raw = byte };
  return flags_byte.flags;
}

// aka BuildCharPath/BuildPath/MakePath?
static error_block *MakeCharPaths( Font const *font,
                               uint32_t ch,
                               uint32_t *fill_path,
                               uint32_t *stroke_path,
                               Font_BBox *bbox )
{
  IntMetric0 const * const metrics = font->IntMetrics0;
  OutlineFontFile const * const outline_font = font->Outlines0;

  // On entry, the path arrays must be initialised with the number of
  // remaining usable elements in the array at index 0. (See Paint.)

  uint32_t max_char = IntMetrics0_num( metrics );

  uint16_t index = IntMetrics0_char_index( metrics, ch );

#ifdef DEBUG__VERBOSE
Write0( "Character: " ); WriteSmallNum( ch, 1 ); Write0( ", index " ); WriteSmallNum( index, 1 ); NewLine;
#endif

  uint32_t *chunks = OutlineFontFile_chunks_offsets( outline_font );

#ifdef DEBUG__VERBOSE
Write0( "Chunk offset: " ); WriteSmallNum( chunks[ch / 32], 1 ); NewLine;
#endif

  if (outline_font->number_of_chunks < ch / 32) {
    // No such char FIXME
    fill_path[0] = 0;
    stroke_path[0] = 0;
    return 0;
  }

  if (0 == chunks[ch / 32]) {
    // No such char FIXME
    fill_path[0] = 0;
    stroke_path[0] = 0;
    return 0;
  }

  uint32_t *chunk = pointer_at_offset_from( outline_font, chunks[ch / 32] );

  // File format requires chunks are word aligned.
  assert( 0 == (3 & (uint32_t) chunk) );

  uint32_t *char_offsets = chunk + 1;

  uint8_t *char_data = pointer_at_offset_from( char_offsets, char_offsets[ch % 32] );
#ifdef DEBUG__VERBOSE
Write0( "Char offset: " ); WriteNum( char_offsets[ch % 32] ); NewLine;
#endif

  if (0 == char_offsets[ch % 32]) {
    // No such char FIXME
    fill_path[0] = 0;
    stroke_path[0] = 0;
    return 0;
  }

  // Note: the flags byte is only in versions 8+
  FontCharacterFlags character = read_font_character_flags( char_data[0] );

#ifdef DEBUG__VERBOSE
  if (character.coords_12bit) { Write0( "12 bit coordinates" ); NewLine; }
  if (character.data_1bpp) { Write0( "1 bit per pixel (or outline)" ); NewLine; }
  if (character.initial_pixel_black) { Write0( "Initial pixel black" ); NewLine; }
  if (character.outline) { Write0( "Outline" ); NewLine; }
  if (character.composite) { Write0( "composite" ); NewLine; }
  if (character.has_accent) { Write0( "Has accent" ); NewLine; }
  if (character.codes_16bit) { Write0( "16-bit character codes" ); NewLine; }
#endif

  uint8_t const *next_byte = char_data+1;
  uint16_t base_character = 0; // Only important if character.composite

  uint16_t accent_character = 0; // Only important if character.has_accent
  int16_t accentx;
  int16_t accenty;

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
      if (character.codes_16bit) {
        accent_character = uint16_at( next_byte );
        next_byte += 2;
      }
      else {
        accent_character = *next_byte++;
      }
      
      next_byte = read_font_coord_pair( next_byte, character.coords_12bit, &accentx, &accenty );
    }
  }

  if (character.outline == 0 || character.composite == 0) {
    next_byte = read_font_coord_pair( next_byte, character.coords_12bit,
                                      &bbox->left_inclusive, &bbox->bottom_inclusive );
    next_byte = read_font_coord_pair( next_byte, character.coords_12bit,
                                      &bbox->width, &bbox->height );

#ifdef DEBUG__VERBOSE
    Write0( "BBox: " );
    WriteSmallNum( bbox->left_inclusive, 4 ); Write0( ", " );
    WriteSmallNum( bbox->bottom_inclusive, 4 ); Write0( ", " );
    WriteSmallNum( bbox->width, 4 ); Write0( ", " );
    WriteSmallNum( bbox->height, 4 ); NewLine;
#endif
  }
  else {
    *bbox = outline_font->font_max_bbox;
  }

  next_byte = FontToDrawPath( next_byte, character.coords_12bit, fill_path );
  if (next_byte != 0) {
    next_byte = FontToDrawPath( next_byte, character.coords_12bit, stroke_path );
  }

  if (next_byte == 0) {
    Write0( "Path creation failed" ); NewLine;
    // FIXME Report error?
  }

  assert( character.sbz == 0 );

  return 0;
}

#define DEBUG__SHOW_FONT_PATHS
#ifdef DEBUG__SHOW_FONT_PATHS
static void DebugPrintPath( uint32_t *p )
{
  uint32_t code;
  do {
    code = *p++;
    switch (code) {
      case 0:
        Write0( "End." );
        break;
      case 1:
        Write0( "Pointer... " ); WriteNum( *p++ );
        break;
      case 2:
        Write0( "Move to " ); WriteNum( *p++ ); Write0( ", " ); WriteNum( *p++ );
        break;
      case 3:
        Write0( "Move to (no winding)" ); WriteNum( *p++ ); Write0( ", " ); WriteNum( *p++ );
        break;
      case 4:
        Write0( "Close with gap" );
        break;
      case 5:
        Write0( "Close with line" );
        break;
      case 6:
        Write0( "Curve via (" );
        WriteNum( *p++ ); Write0( ", " ); WriteNum( *p++ ); Write0( "), (" );
        WriteNum( *p++ ); Write0( ", " ); WriteNum( *p++ ); Write0( "), to (" );
        WriteNum( *p++ ); Write0( ", " ); WriteNum( *p++ ); Write0( ")" );
        break;
      case 7:
        Write0( "Gap to " ); WriteNum( *p++ ); Write0( ", " ); WriteNum( *p++ );
        break;
      case 8:
        Write0( "Line to " ); WriteNum( *p++ ); Write0( ", " ); WriteNum( *p++ );
        break;
    }

    NewLine;
  } while (code != 0);
  NewLine;
}
#endif

static bool FindFont( struct workspace *workspace, SWI_regs *regs )
{
  return true;
}

static bool LoseFont( struct workspace *workspace, SWI_regs *regs )
{
  return true;
}

static bool ReadInfo( struct workspace *workspace, SWI_regs *regs )
{
  Font *font = workspace->fonts;
  OutlineFontFile const * const outline_font = font->Outlines0;
  regs->r[1] = -3;  // FIXME completely made up
  regs->r[2] = -3;
  regs->r[3] = 13;
  regs->r[4] = 13;
  return true;
}

static bool Paint( struct workspace *workspace, SWI_regs *regs )
{
  // One true font
  Font *font = workspace->fonts;

  Write0( "Paint \"" ); Write0( regs->r[1] ); Write0( "\"" ); NewLine;

  char *p = (void*) regs->r[1];
  char ch;
  //int32_t matrix[6] = { 0x2000, 0, 0, 0x2000, 0x1000, 0x4000 };

  // TODO: Put into a FoundFont structure
  uint32_t point_size = 12 * 16; // 1/16ths of a point
  uint32_t dpi = 180; // OS Units per inch
  uint32_t const points_per_inch = 72;

  OutlineFontFile const * const outline_font = font->Outlines0;
  uint32_t design_size = outline_font->design_size;
  uint32_t fp_zoom = ((point_size * dpi * 0x1000)/points_per_inch) / design_size;

#ifdef DEBUG__VERBOSE
Write0( "FP Zoom: " ); WriteNum( fp_zoom ); NewLine;
#endif

  int32_t x = regs->r[3];
  int32_t y = regs->r[4];

  if (0 == (regs->r[2] & (1 << 4))) {
    // Coordinates are in millipoints, not OS units
    x = (x / 400);
    y = (y / 400);
  }

  int32_t matrix[6] = { fp_zoom, 0, 0, fp_zoom, x * 256 / 2, y * 256 / 2 }; // Internal draw units FIXME
  while ((ch = *p++) >= ' ') {
    uint32_t fill_path[128];
    uint32_t stroke_path[64];
    fill_path[0] = sizeof( fill_path )/sizeof( fill_path[0] ) - 1;
    stroke_path[0] = sizeof( stroke_path )/sizeof( stroke_path[0] ) - 1;
    Font_BBox bbox;

    MakeCharPaths( font, ch, fill_path, stroke_path, &bbox );

#ifdef DEBUG__SHOW_FONT_PATHS
Write0( "Fill path" ); NewLine; DebugPrintPath( fill_path );
Write0( "Stroke path" ); NewLine; DebugPrintPath( stroke_path );
#endif
//Font_Draw_TransformPath( fill_path, matrix );
//Font_Draw_TransformPath( stroke_path, matrix );

    Font_Draw_Fill( fill_path, matrix );
    Font_Draw_Stroke( stroke_path, matrix );
    matrix[4] += 25600;
    Font_Draw_Stroke( stroke_path, matrix );
    matrix[4] -= 25600;

    matrix[4] += 256 * (matrix[0] * bbox.width) / 0x10000; // x multiplier
    // FIXME: non-horizontal drawing
  }
  return true;
}

static bool ConverttoOS( struct workspace *workspace, SWI_regs *regs )
{
  regs->r[1] = regs->r[1]/400;
  regs->r[2] = regs->r[2]/400;
  return true;
}

static bool CurrentFont( struct workspace *workspace, SWI_regs *regs )
{
  WriteS( "CurrentFont" );
  regs->r[0] = 0x77;
  regs->r[1] = 0xff000000;
  regs->r[2] = 0x00ff0000;
  regs->r[3] = 14;
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

static bool SetFontColours( struct workspace *workspace, SWI_regs *regs )
{
  Write0( "SetFontColours " ); WriteNum( regs->r[0] );
  Write0( ", BG: " ); WriteNum( regs->r[1] );
  Write0( ", FG: " ); WriteNum( regs->r[2] );
  Write0( ", off: " ); WriteNum( regs->r[3] );
  NewLine;
  return true;
}

static bool SetColourTable( struct workspace *workspace, SWI_regs *regs )
{
  Write0( "SetColourTable" ); NewLine;
  return true;
}

static bool SwitchOutputToBuffer( struct workspace *workspace, SWI_regs *regs )
{
  Write0( "SwitchOutputToBuffer FIXME" ); NewLine;
  return true;
}

static bool FontScanString( struct workspace *workspace, SWI_regs *regs )
{
  Write0( "FontScanString FIXME" ); NewLine;
  return true;
}

bool __attribute__(( noinline )) c_swi_handler( struct workspace *workspace, SWI_regs *regs )
{
  NewLine; Write0( "Handling Font SWI " ); WriteNum( MODULE_CHUNK + regs->number ); NewLine;

  switch (regs->number) {
  case 0x01: return FindFont( workspace, regs );
  case 0x02: return LoseFont( workspace, regs );
  case 0x04: return ReadInfo( workspace, regs );
  case 0x06: return Paint( workspace, regs );
  case 0x08: return ConverttoOS( workspace, regs );
  case 0x09: regs->r[1] *= 400; regs->r[2] *= 400; return true;
  case 0x0b: return CurrentFont( workspace, regs );
  case 0x0f: regs->r[1] = 400; regs->r[2] = 400; return true;
  case 0x12: return SetFontColours( workspace, regs );
  case 0x13: return SetPalette( workspace, regs );
  case 0x1e: return SwitchOutputToBuffer( workspace, regs );
  case 0x21: return FontScanString( workspace, regs );
  case 0x22: return SetColourTable( workspace, regs );
  default: WriteNum( regs->number );
  }
  static const error_block error = { 0x1e6, "FontManager SWI unsupported by C implementation (sorry)" };
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


// The following is documentation of the assembler, it's not likely to be
// correct!

typedef struct {
  int32_t XX;
  int32_t YX;
  int32_t XY;
  int32_t YY;
  int32_t X;
  int32_t Y;
} umatrix;

typedef struct {
  int32_t XX;
  int32_t YX;
  int32_t XY;
  int32_t YY;
  int32_t X;
  int32_t Y;
  int32_t coord_shift;
} matrix;

typedef struct {
  int32_t mantissa;
  int32_t exponent;
} fp;

typedef struct {
  fp XX;
  fp YX;
  fp XY;
  fp YY;
  fp X;
  fp Y;
} fpmatrix;

typedef struct {
  int32_t x0;
  int32_t y0;
  int32_t x1;
  int32_t y1;
} box;

typedef struct {
  char name[12];
} short_string;

typedef struct {
  char name[11]; // 10 chars, terminator, space for flags after
} leaf_name;

// The objects in the cache form a tree, with a single link list for
// the top level items (fonts only?), and pointers to sub-items.
// 
typedef struct {
  uint32_t size:28;
  uint32_t marker:1;    // Maybe not valid in all objects
  uint32_t ischar:1;    // ditto
  uint32_t claimed:1;
  uint32_t locked:1;

  object_header *link;  // Next object at top level
  header **backlink;    // Link back to the pointer to this object. TCBO1?
  void *anchor;
} object_header;

typedef struct {
  object_header header;
  umatrix       unscaled;
  matrix        metricsmatrix;
  fpmatrix      scaled;
} matrixblock;

typedef struct {
  object_header header;
  uint32_t flags;

} cache_chunk;

typedef struct {
  object_header header;
  union {
    cache_chunk *pointers[];
    uint32_t offsets[];
  };
} pixo;

typedef struct {
  object_header header;
// hdr_usage       #       4               ; font usage count
  uint32_t usage;
//  
// hdr_metricshandle  #    1               ; metrics
  char metricshandle;
// hdr4_pixelshandle  #    1               ; 4 bpp (old or new format or outlines)
  char pixelshandle1;
// hdr1_pixelshandle  #    1               ; 1 bpp
  char pixelshandle4;
// hdr_flags          #    1               ; bit 0 set => swap over x/y subpixel posns
  char flags;

// hdr_name        #       40
  char name[40];
// hdr_nameend     #       0
// hdr_xsize       #       4               ; x-size of font (1/16ths point)
  int32_t xsize;
// hdr_ysize       #       4               ; y-size of font
  int32_t ysize;

// hdr_nchars      #       4               ; number of defined characters       ) read from metrics file header
  int32_t nchars;
// hdr_metflags    #       1               ; metrics flags                      )
  char metflags;
// hdr_masterfont  #       1               ; used for 4bpp and outline masters
  char masterfont;
// hdr_masterflag  #       1               ; is this a 'proper' master font?
  char masterflag;      // enum { normal, ramscaled, master } - FindFont called with font name, {26, handle}, r2 == -1 (the last being undocumented?)
// hdr_skelthresh  #       1               ; skeleton line threshold (pixels)
  char skelthresh;

// hdr_encoding    #       12              ; lower-cased zero-padded version of /E parameter
  short_string encoding;
// hdr_base        #       4               ; base encoding number (for setleafnames_R6)
  uint32_t base;

// hdr_xmag        #       4
  uint32_t xmag;
// hdr_ymag        #       4               ; these are only used for 4-bpp bitmaps
  uint32_t ymag;

// hdr_xscale      #       4               ; = psiz * xres * xscaling * 16
  uint32_t xscale;
// hdr_yscale      #       4               ; = psiz * yres * yscaling * 16
  uint32_t yscale;
// hdr_xres        #       4               ; 0,0 => variable resolution (pixelmatrix derived from res. at the time)
  uint32_t xres;
// hdr_yres        #       4
  uint32_t yres;
// hdr_filebbox    #       4               ; held in byte form in old-style file
  uint32_t filebbox;

//                 ASSERT  (@-hdr_xscale)=(fpix_index-fpix_xscale)

// hdr_threshold1  #       4               ; max height for scaled bitmaps
  uint32_t threshold1;
// hdr_threshold2  #       4               ; max height for 4-bpp
  uint32_t threshold2;
// hdr_threshold3  #       4               ; max cached bitmaps from outlines
  uint32_t threshold3;
// hdr_threshold4  #       4               ; max width for subpixel scaling
  uint32_t threshold4;
// hdr_threshold5  #       4               ; max height for subpixel scaling
  uint32_t threshold5;

// hdr_MetOffset   #       4               ; from fmet_chmap onwards
  uint32_t MetOffset;
// hdr_MetSize     #       4
  uint32_t MetSize;
// hdr_PixOffset   #       4               ; from fpix_index onwards
  uint32_t PixOffset;
// hdr_PixSize     #       4
  uint32_t PixSize;
// hdr_scaffoldsize   #    4               ; from fnew_tablesize onwards
  uint32_t scaffoldsize;



// hdr_designsize  #       4               ; design size (for the outline file)
  uint32_t designsize;
// hdr_rendermatrix  #     mat_end         ; if no paint matrix, transforms from design units -> pixels << 9
  matrix rendermatrix;
// hdr_bboxmatrix  #       mat_end         ; transforms from design units -> 1/1000pt
  matrix bboxmatrix;
// hdr_resXX       #       8               ; xres << 9 / 72000   : the resolution matrix
  fp resXX;
// hdr_resYY       #       8               ; yres << 9 / 72000   : (floating point)
  fp resYY;


// hdr_metaddress  #       4               ; base of file data, or 0 if not ROM
  uint32_t metaddress;
// hdr_oldkernsize #       4               ; cached size of old-style kerning table (for ReadFontMetrics)
  uint32_t oldkernsize;


  struct {

// hdr4_leafname   #       11              ; 10-char filename, loaded as 3 words
    leaf_name leafname;
// hdr4_flags      #       1               ; flags given by pp_*
    uint8_t flags;

// hdr4_PixOffStart  #     4               ; offset to chunk offset array in file
    uint32_t PixOffStart;
// hdr4_nchunks    #       4               ; number of chunks in file
    uint32_t nchunks;
// hdr4_nscaffolds #       4               ; number of scaffold index entries
    uint32_t nscaffolds;
// hdr4_address    #       4               ; for ROM-based fonts
    uint32_t address;
// hdr4_boxx0      #       4               ;
    uint32_t boxx0;
// hdr4_boxy0      #       4               ; separate copies for 4-bpp and 1-bpp
    uint32_t boxy0;
// hdr4_boxx1      #       4               ; (used internally in cachebitmaps)
    uint32_t boxx1;
// hdr4_boxy1      #       4               ;
    uint32_t boxy1;

  } hdr4;

  struct {
// hdr1_leafname   #       11              ; 10-char filename, loaded as 3 words
    leaf_name leafname;
// hdr1_flags      #       1               ; flags given by pp_*
    uint8_t flags;
// hdr1_PixOffStart  #     4               ; offset to chunk offset array in file
    uint32_t PixOffStart;
// hdr1_nchunks    #       4               ; number of chunks in file
    uint32_t nchunks;
// hdr1_nscaffolds #       4               ; number of scaffold index entries
    uint32_t nscaffolds;
// hdr1_address    #       4               ; for ROM-based fonts
    uint32_t address;
// hdr1_boxx0      #       4               ;
    uint32_t boxx0;
// hdr1_boxy0      #       4               ; Font_ReadInfo returns whichever
    uint32_t boxy0;
// hdr1_boxx1      #       4               ; box happens to be defined
    uint32_t boxx1;
// hdr1_boxy1      #       4               ;
    uint32_t boxy1;

  } hdr1;

// hdr_MetricsPtr  #       4               ; 1 set for all characters
  uint32_t MetricsPtr;
// hdr_Kerns       #       4               ; only cached if needed
  uint32_t Kerns;
// hdr_Charlist    #       4
  uint32_t Charlist;
// hdr_Scaffold    #       4
  uint32_t Scaffold;
// hdr_PathName    #       4               ; block containing (expanded) pathname
  uint32_t PathName;
// hdr_PathName2   #       4               ; 0 unless shared font pixels used
  uint32_t PathName2;
// hdr_FontMatrix  #       4               ; derived font matrix, or font matrix (unscaled and scaled)
  uint32_t FontMatrix;
// hdr_mapindex    #       4*4             ; master font's list of mappings (target encoding / private base)
  uint32_t mapindex[4];

// hdr4_PixoPtr    #       4
// Pointer to array of nchunks chunk pointers (or nchunks+1 offsets?)
  pixo *hdr4_PixoPtr;
// hdr1_PixoPtr    #       4               ; pointer to block containing file offsets and pointers to chunks
  pixo *hdr1_PixoPtr;

// hdr_transforms  #       0               ; chain of different transforms pointing to chunks
// hdr4_transforms #       4*8             ; 4-bpp versions
  uint32_t hdr4_transforms[8];
// hdr1_transforms #       4*8             ; 1-bpp versions
  uint32_t hdr1_transforms[8];
// hdr_transformend  #     0
// nhdr_ptrs       *       (@-hdr_MetricsPtr)/4
} cache_font_header;

typedef struct {
  object_header header;
} cache_header;



struct {
  error_block *error; // zero if no error
  const char *font_file;
  // input leafname modified unless error or...
  bool data_not_found;
  bool file_not_found;
} ScanFontDir_res;

ScanFontDir_res ScanFontDir( cache_font_header *header, char *leaf_ptr )
{
  if (leaf_ptr == &header->hdr1_leafname) {
  }
  else { // (leaf_ptr == &header->hdr4_leafname)
  }

  if (header->masterflag == 
}

typedef struct {
  char encoding[12];
} encoding_id;

typedef struct {
  encoding_id encbuf;
  void *paintmatrix;
  void *transformptr;
  void *font_cache;
  void *font_cache_end;
} workspace;

typedef struct {
  error_block *error; // zero if no error
  bool match_found;
  uint32_t handle;
  cache_font_header *header;
} MatchFont_res;

error_block *FindMaster( workspace *ws, const char *name, cache_font_header *header )
{
}

static void MarkPixos( pixo *p, uint32_t n, bool claimed )
{
  for (uint32_t i = 0; i < n; i++) {
    cache_chunk *cc = p->pointers[i];
    cc->claimed = claimed ? 1 : 0;
    if (cc->pix_flags
  }
}

error_block *ClaimFont( workspace *ws, const char *name, cache_font_header *header )
{
  error_block *result = 0;

  if (header->masterflag == normal) {
    if (header->masterfont != 0) {
      result = FindMaster( ws, name, header );
      if (result) return result;

      // assert (header->masterfont != 0) ?
    }
  }

  header->usage ++;

  // markfontclaimed_R7
  if (header->usage != 1) { // Not already claimed
    header->header->claimed = 1;
    header->header->locked = 1;

    uint32_t font_cache = ws->font_cache;
    uint32_t font_cache_end = ws->font_cache_end;

    uint32_t *pp = &header->MetricsPtr; // First pointer in header
    while (pp < &header->hdr4_PixoPtr) { // Follows last one?
      uint32_t p = *pp++;
      if (p > font_cache && font_cache_end > p) { // In cache
        header *h = (void*) p;
        h->claimed = 1;
      }
    }

    // Mark the pixo entries as used, as well.
    MarkPixos( header->hdr4_PixoPtr, header->hdr4.nchunks, true );
    MarkPixos( header->hdr1_PixoPtr, header->hdr1.nchunks, true );
  }
}

typedef struct {
  error_block *error; // zero if no error
  uint32_t handle;
  uint32_t xres;
  uint32_t yres;
} FindFont_res;

FindFont_res Int_FindFont( worspace *ws,
        const char *name,
        uint32_t xpoints, uint32_t ypoints,
        uint32_t xres, uint32_t yres )
{
  FindFont_res result = { .error = 0 };

  ws->paintmatrix = 0;
  ws->transformptr = 0;

  result.error = SetModeData( ws, name, xpoints, ypoints, xres, yres );
  if (result.error) return result;
  result.error = DefaultRes( ws, name, xpoints, ypoints, xres, yres );
  if (result.error) return result;

  uint32_t handle;
  cache_font_header *header;

  {
    MatchFont_res res = MatchFont( ws, name, xpoints, ypoints, xres, yres );
    if (res.error) { result.error = res.error; return result; }

    if (!res.match_found) {
      
    }
    header = res.header;
    handle = res.handle;
  }

  result.error = ClaimFont( ws, name, header );
  if (result.error) return result;

  return result;
}

FindFont_res FindFont( worspace *ws,
        const char *name,
        uint32_t xpoints, uint32_t ypoints,
        uint32_t xres, uint32_t yres )
{
  FindFont_res result = { .error = 0 };

  // Fill in a variable in workspace
  result.error = GetEncodingId( ws, name, xpoints, ypoints, xres, yres );
  if (result.error) return result;

  return Int_FindFont;
}
