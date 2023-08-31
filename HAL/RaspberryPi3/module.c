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

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing
//   New feature: instead of one private word per core, r12 points to a shared
//   word, initialised by the first core to initialise the module.

// Explicitly no SWIs provided (it's the default, anyway)
#define MODULE_CHUNK "0"

#include "module.h"

#include "include/taskop.h"

NO_start;
//NO_init;
NO_finalise;
//NO_service_call;
//NO_title;
//NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char title[] = "Raspberry Pi 3 HAL";
const char help[] = "HAL\t0.01";

typedef struct {
  uint32_t       control;
  uint32_t       res1;
  uint32_t       timer_prescaler;
  uint32_t       GPU_interrupts_routing;
  uint32_t       Performance_Monitor_Interrupts_routing_set;
  uint32_t       Performance_Monitor_Interrupts_routing_clear;
  uint32_t       res2;
  uint32_t       Core_timer_access_LS_32_bits; // Access first when reading/writing 64 bits.
  uint32_t       Core_timer_access_MS_32_bits;
  uint32_t       Local_Interrupt_routing0;
  uint32_t       Local_Interrupts_routing1;
  uint32_t       Axi_outstanding_counters;
  uint32_t       Axi_outstanding_IRQ;
  uint32_t       Local_timer_control_and_status;
  uint32_t       Local_timer_write_flags;
  uint32_t       res3;
  uint32_t       Core_timers_Interrupt_control[4];
  uint32_t       Core_Mailboxes_Interrupt_control[4];
  uint32_t       Core_IRQ_Source[4];
  uint32_t       Core_FIQ_Source[4];
  struct {
    uint32_t       Mailbox[4]; // Write only!
  } Core_write_set[4];
  struct {
    uint32_t       Mailbox[4]; // Read/write
  } Core_write_clear[4];
} QA7;

// Note: Alignment is essential for device area, so that GCC doesn't
// generate multiple strb instructions to write a single word.
// Or I could be professional and make every device access an inline
// routine call. Nah!
struct __attribute__(( packed, aligned( 256 ) )) gpio {
  uint32_t gpfsel[6];  // 0x00 - 0x14
  uint32_t res18;
  uint32_t gpset[2];   // 0x1c, 0x20
  uint32_t res24;
  uint32_t gpclr[2];
  uint32_t res30;     // 0x30
  uint32_t gplev[2];
  uint32_t res3c;
  uint32_t gpeds[2];   // 0x40
  uint32_t res48;
  uint32_t gpren[2];
  uint32_t res54;
  uint32_t gpfen[2];
  uint32_t res60;     // 0x60
  uint32_t gphen[2];
  uint32_t res6c;
  uint32_t gplen[2];    // 0x70
  uint32_t res78;
  uint32_t gparen[2];
  uint32_t res84;
  uint32_t gpafen[2];
  uint32_t res90;     // 0x90
  uint32_t gppud;
  uint32_t gppudclk[2];
  uint32_t resa0;
  uint32_t resa4;
  uint32_t resa8;
  uint32_t resac;
  uint32_t test;
};

typedef struct __attribute__(( packed )) {
  uint32_t data;                                // 0x00
  uint32_t receive_status_error_clear;          // 0x04
  uint32_t res0[4];
  uint32_t flags;                               // 0x18
  uint32_t res1[2];
  uint32_t integer_baud_rate_divisor;           // 0x24
  uint32_t fractional_baud_rate_divisor;        // 0x28
  uint32_t line_control;                        // 0x2c
  uint32_t control;                             // 0x30
  uint32_t interrupt_fifo_level_select;         // 0x34
  uint32_t interrupt_mask_set_clear;            // 0x38
  uint32_t raw_interrupt_status;                // 0x3c
  uint32_t masked_interrupt_status;             // 0x40
  uint32_t interrupt_clear;                     // 0x44
  uint32_t dma_control;                         // 0x48
  uint32_t res2[(0x80-0x4c)/4];
  uint32_t test_control;                        // 0x80
  uint32_t integration_test_input;              // 0x84
  uint32_t integration_test_output;             // 0x88
  uint32_t test_data;                           // 0x8c
} UART;

typedef struct {
  uint32_t value; // Request or Response, depending if from or to ARM,
                  // (Pointer & 0xfffffff0) | Channel 0-15
  uint32_t res1;
  uint32_t res2;
  uint32_t res3;
  uint32_t peek;  // Doesn't remove the value from the FIFO
  uint32_t sender;// ??
  uint32_t status;// bit 31: Tx full, 30: Rx empty
  uint32_t config;
} GPU_mailbox;

typedef struct __attribute__(( packed )) {
  uint32_t to0x200[0x200/4];
  // 0x200
  union {
    struct {
      uint32_t basic_pending;
      uint32_t pending1;
      uint32_t pending2;
      uint32_t fiq_control;
      uint32_t enable_irqs1;
      uint32_t enable_irqs2;
      uint32_t enable_basic;
      uint32_t disable_irqs1;
      uint32_t disable_irqs2;
      uint32_t disable_basic;
    };
    uint32_t to0x400[0x200/4];
  };
  // 0x400
  union {
    struct {
      uint32_t load;
      uint32_t value;
      uint32_t control;
      uint32_t irq;
      uint32_t irq_raw;
      uint32_t irq_masked;
      uint32_t pre_deivider;
      uint32_t counter;
    } regular_timer;
    uint32_t to0x880[0x480/4];
  };
  // 0x880
  GPU_mailbox mailbox[2]; // ARM may read mailbox 0, write mailbox 1.

} GPU;

struct workspace {
  uint32_t lock;

  GPU *gpu; // Interrupts, mailboxes, etc.
  struct gpio *gpio;
  UART        *uart;
  QA7         *qa7;

  void *mailbox_request;
  uint32_t fb_physical_address;
  uint32_t *frame_buffer;
  uint32_t graphics_driver_id;
  uint32_t ticks_per_interval;

  struct uart_task_stack {
    uint32_t stack[64];
  } uart_task_stack;

  uint32_t wimp_started;
  uint32_t wimp_poll_word;

  struct core_workspace {
    struct workspace *shared;
    uint8_t core;
    int8_t first_reported_irq;
    int8_t last_reported_irq;
    uint8_t res;
    struct console_stack {
      uint64_t stack[64];
    } console_stack;
    struct ticker_stack {
      uint64_t stack[64];
    } ticker_stack;
    struct tickerv_stack {
      uint64_t stack[4];
    } tickerv_stack;
    uint8_t queued;
    uint8_t queue[15];
    uint32_t x;
    uint32_t y;
    char display[40][60];
  } core_specific[];
};

static int core( struct core_workspace *cws )
{
  return cws->core;
}

typedef struct {
  uint32_t va;
  uint32_t pa;
} dma_memory;


static uint32_t lock_for_dma( uint32_t address, uint32_t bytes )
{
  register uint32_t addr asm( "r0" ) = address;
  register uint32_t size asm( "r1" ) = bytes;
  register uint32_t physical asm( "r0" );

  asm ( "svc 0xfc" : "=r" (physical) : "r" (size), "r" (addr) : "lr" );

  return physical;
}

static dma_memory rma_claim_for_dma( uint32_t bytes, uint32_t alignment )
{
  dma_memory result;

  // FIXME: Loop, allocating blocks and attempting to lock the memory for DMA, then release all those that couldn't be locked.
  result.va = (uint32_t) rma_claim( bytes + alignment );
  result.pa = lock_for_dma( result.va, bytes + alignment );

  return result;
}

static struct workspace *new_workspace( uint32_t number_of_cores )
{
  uint32_t required = sizeof( struct workspace ) + number_of_cores * sizeof( struct core_workspace );

  struct workspace *memory = rma_claim( required );

  memset( memory, 0, required );

  for (int i = 0; i < number_of_cores; i++) {
    memory->core_specific[i].core = i;
  }

  return memory;
}

enum fb_colours {
  Black   = 0xff000000,
  Grey    = 0xff888888,
  Blue    = 0xff0000ff,
  Green   = 0xff00ff00,
  Red     = 0xffff0000,
  Yellow  = 0xffffff00,
  Magenta = 0xff00ffff,
  White   = 0xffffffff };

static inline void set_pixel( uint32_t x, uint32_t y, uint32_t colour, struct workspace *ws )
{
  ws->frame_buffer[x + y * 1920] = colour;
}

// Taken and translated to C from RiscOS.Sources.Kernel.s.vdu.vdufont1
static char const system_font_from_space[256 -32][8] = {
  { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, //ISO  "space"
  { 0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00 }, //ISO  "exclamation mark"
  { 0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00 }, //ISO  "quotation mark"
  { 0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00 }, //ISO  "number sign"
  { 0x0C,0x3F,0x68,0x3E,0x0B,0x7E,0x18,0x00 }, //ISO  "dollar sign"
  { 0x60,0x66,0x0C,0x18,0x30,0x66,0x06,0x00 }, //ISO  "percent sign"
  { 0x38,0x6C,0x6C,0x38,0x6D,0x66,0x3B,0x00 }, //ISO  "ampersand"
  { 0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00 }, //ISO  "apostrophe" (vertical)
  { 0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00 }, //ISO  "left parenthesis"
  { 0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00 }, //ISO  "right parenthesis"
  { 0x00,0x18,0x7E,0x3C,0x7E,0x18,0x00,0x00 }, //ISO  "asterisk"
  { 0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00 }, //ISO  "plus sign"
  { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30 }, //ISO  "comma"
  { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 }, //ISO  "hyphen-minus"
  { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 }, //ISO  "full stop"
  { 0x00,0x06,0x0C,0x18,0x30,0x60,0x00,0x00 }, //ISO  "solidus"
  { 0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0x00 }, //ISO  "digit zero"
  { 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 }, //ISO  "digit one"
  { 0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00 }, //ISO  "digit two"
  { 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 }, //ISO  "digit three"
  { 0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00 }, //ISO  "digit four"
  { 0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 }, //ISO  "digit five"
  { 0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00 }, //ISO  "digit six"
  { 0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00 }, //ISO  "digit seven"
  { 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 }, //ISO  "digit eight"
  { 0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00 }, //ISO  "digit nine"
  { 0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x00 }, //ISO  "colon"
  { 0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x30 }, //ISO  "semicolon"
  { 0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00 }, //ISO  "less-than sign"
  { 0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00 }, //ISO  "equals sign"
  { 0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00 }, //ISO  "greater-than sign"
  { 0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00 }, //ISO  "question mark"
  { 0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00 }, //ISO  "commercial at"
  { 0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 }, //ISO  "Latin capital letter A"
  { 0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00 }, //ISO  "Latin capital letter B"
  { 0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00 }, //ISO  "Latin capital letter C"
  { 0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00 }, //ISO  "Latin capital letter D"
  { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00 }, //ISO  "Latin capital letter E"
  { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00 }, //ISO  "Latin capital letter F"
  { 0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter G"
  { 0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 }, //ISO  "Latin capital letter H"
  { 0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00 }, //ISO  "Latin capital letter I"
  { 0x3E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00 }, //ISO  "Latin capital letter J"
  { 0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00 }, //ISO  "Latin capital letter K"
  { 0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00 }, //ISO  "Latin capital letter L"
  { 0x63,0x77,0x7F,0x6B,0x6B,0x63,0x63,0x00 }, //ISO  "Latin capital letter M"
  { 0x66,0x66,0x76,0x7E,0x6E,0x66,0x66,0x00 }, //ISO  "Latin capital letter N"
  { 0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter O"
  { 0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00 }, //ISO  "Latin capital letter P"
  { 0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00 }, //ISO  "Latin capital letter Q"
  { 0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00 }, //ISO  "Latin capital letter R"
  { 0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00 }, //ISO  "Latin capital letter S"
  { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 }, //ISO  "Latin capital letter T"
  { 0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter U"
  { 0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00 }, //ISO  "Latin capital letter V"
  { 0x63,0x63,0x6B,0x6B,0x7F,0x77,0x63,0x00 }, //ISO  "Latin capital letter W"
  { 0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00 }, //ISO  "Latin capital letter X"
  { 0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00 }, //ISO  "Latin capital letter Y"
  { 0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00 }, //ISO  "Latin capital letter Z"
  { 0x7C,0x60,0x60,0x60,0x60,0x60,0x7C,0x00 }, //ISO  "left square bracket"
  { 0x00,0x60,0x30,0x18,0x0C,0x06,0x00,0x00 }, //ISO  "reverse solidus"
  { 0x3E,0x06,0x06,0x06,0x06,0x06,0x3E,0x00 }, //ISO  "right square bracket"
  { 0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00 }, //ISO  "circumflex accent"
  { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF }, //ISO  "low line"
  { 0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00 }, //ISO  "grave accent"
  { 0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00 }, //ISO  "Latin small letter a"
  { 0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00 }, //ISO  "Latin small letter b"
  { 0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00 }, //ISO  "Latin small letter c"
  { 0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00 }, //ISO  "Latin small letter d"
  { 0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00 }, //ISO  "Latin small letter e"
  { 0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00 }, //ISO  "Latin small letter f"
  { 0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C }, //ISO  "Latin small letter g"
  { 0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00 }, //ISO  "Latin small letter h"
  { 0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00 }, //ISO  "Latin small letter i"
  { 0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x70 }, //ISO  "Latin small letter j"
  { 0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00 }, //ISO  "Latin small letter k"
  { 0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 }, //ISO  "Latin small letter l"
  { 0x00,0x00,0x36,0x7F,0x6B,0x6B,0x63,0x00 }, //ISO  "Latin small letter m"
  { 0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00 }, //ISO  "Latin small letter n"
  { 0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin small letter o"
  { 0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60 }, //ISO  "Latin small letter p"
  { 0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x07 }, //ISO  "Latin small letter q"
  { 0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x00 }, //ISO  "Latin small letter r"
  { 0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00 }, //ISO  "Latin small letter s"
  { 0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00 }, //ISO  "Latin small letter t"
  { 0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00 }, //ISO  "Latin small letter u"
  { 0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00 }, //ISO  "Latin small letter v"
  { 0x00,0x00,0x63,0x6B,0x6B,0x7F,0x36,0x00 }, //ISO  "Latin small letter w"
  { 0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00 }, //ISO  "Latin small letter x"
  { 0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C }, //ISO  "Latin small letter y"
  { 0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00 }, //ISO  "Latin small letter z"
  { 0x0C,0x18,0x18,0x70,0x18,0x18,0x0C,0x00 }, //ISO  "left curly bracket"
  { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00 }, //ISO  "vertical line"
  { 0x30,0x18,0x18,0x0E,0x18,0x18,0x30,0x00 }, //ISO  "right curly bracket"
  { 0x31,0x6B,0x46,0x00,0x00,0x00,0x00,0x00 }, //ISO  "tilde"
  { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF }, //BFNT "Solid block"
  { 0x3C,0x66,0x60,0xF8,0x60,0x66,0x3C,0x00 }, //ISO  "euro sign"
  { 0x1C,0x36,0x00,0x63,0x6B,0x7F,0x63,0x00 }, //ISO  "Latin capital letter W with circumflex"
  { 0x1C,0x36,0x00,0x6B,0x6B,0x7F,0x36,0x00 }, //ISO  "Latin small letter w with circumflex"
  { 0x06,0x01,0x06,0x61,0x96,0x60,0x90,0x60 }, // "83"
  { 0x05,0x05,0x07,0x61,0x91,0x60,0x90,0x60 }, // "84"
  { 0x3C,0x66,0x00,0x66,0x3C,0x18,0x18,0x00 }, //ISO  "Latin capital letter Y with circumflex"
  { 0x3C,0x66,0x00,0x66,0x66,0x3E,0x06,0x3C }, //ISO  "Latin small letter y with circumflex"
  { 0x07,0x01,0x02,0x64,0x94,0x60,0x90,0x60 }, // "87"
  { 0x06,0x09,0x06,0x69,0x96,0x60,0x90,0x60 }, // "88"
  { 0x06,0x09,0x07,0x61,0x96,0x60,0x90,0x60 }, // "89"
  { 0x06,0x09,0x0F,0x69,0x99,0x60,0x90,0x60 }, // "8A"
  { 0x0E,0x09,0x0E,0x69,0x9E,0x60,0x90,0x60 }, // "8B"
  { 0x00,0x00,0x00,0x00,0x00,0xDB,0xDB,0x00 }, //ISO  "horizontal ellipsis"
  { 0xF1,0x5B,0x55,0x51,0x00,0x00,0x00,0x00 }, //ISO  "trade mark sign"
  { 0xC0,0xCC,0x18,0x30,0x60,0xDB,0x1B,0x00 }, //ISO  "per mille sign"
  { 0x00,0x00,0x3C,0x7E,0x7E,0x3C,0x00,0x00 }, //ISO  "bullet"
  { 0x0C,0x18,0x18,0x00,0x00,0x00,0x00,0x00 }, //ISO  "left single quotation mark"
  { 0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00 }, //ISO  "right single quotation mark"
  { 0x00,0x0C,0x18,0x30,0x30,0x18,0x0C,0x00 }, //ISO  "single left-pointing angle quotation mark"
  { 0x00,0x30,0x18,0x0C,0x0C,0x18,0x30,0x00 }, //ISO  "single right-pointing angle quotation mark"
  { 0x1B,0x36,0x36,0x00,0x00,0x00,0x00,0x00 }, //ISO  "left double quotation mark"
  { 0x36,0x36,0x6C,0x00,0x00,0x00,0x00,0x00 }, //ISO  "right double quotation mark"
  { 0x00,0x00,0x00,0x00,0x00,0x36,0x36,0x6C }, //ISO  "double low-9 quotation mark"
  { 0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0x00 }, //ISO  "en dash"
  { 0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00 }, //ISO  "em dash"
  { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 }, //ISO  "minus sign"
  { 0x77,0xCC,0xCC,0xCF,0xCC,0xCC,0x77,0x00 }, //ISO  "Latin capital ligature OE"
  { 0x00,0x00,0x6E,0xDB,0xDF,0xD8,0x6E,0x00 }, //ISO  "Latin small ligature oe"
  { 0x18,0x18,0x7E,0x18,0x18,0x18,0x18,0x18 }, //ISO  "dagger"
  { 0x18,0x18,0x7E,0x18,0x7E,0x18,0x18,0x18 }, //ISO  "double dagger"
  { 0x3C,0x66,0x60,0xF6,0x66,0x66,0x66,0x00 }, //ISO  "Latin small ligature fi"
  { 0x3E,0x66,0x66,0xF6,0x66,0x66,0x66,0x00 }, //ISO  "Latin small ligature fl"
  { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, //ISO  "no-break space"
  { 0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00 }, //ISO  "inverted exclamation mark"
  { 0x08,0x3E,0x6B,0x68,0x6B,0x3E,0x08,0x00 }, //ISO  "cent sign"
  { 0x1C,0x36,0x30,0x7C,0x30,0x30,0x7E,0x00 }, //ISO  "pound sign"
  { 0x00,0x66,0x3C,0x66,0x66,0x3C,0x66,0x00 }, //ISO  "currency sign"
  { 0x66,0x3C,0x18,0x18,0x7E,0x18,0x18,0x00 }, //ISO  "yen sign"
  { 0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00 }, //ISO  "broken bar"
  { 0x3C,0x60,0x3C,0x66,0x3C,0x06,0x3C,0x00 }, //ISO  "section sign"
  { 0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, //ISO  "diaeresis"
  { 0x3C,0x42,0x99,0xA1,0xA1,0x99,0x42,0x3C }, //ISO  "copyright sign"
  { 0x1C,0x06,0x1E,0x36,0x1E,0x00,0x3E,0x00 }, //ISO  "feminine ordinal indicator"
  { 0x00,0x33,0x66,0xCC,0xCC,0x66,0x33,0x00 }, //ISO  "left-pointing double angle quotation mark"
  { 0x7E,0x06,0x00,0x00,0x00,0x00,0x00,0x00 }, //ISO  "not sign"
  { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 }, //ISO  "soft hyphen"
  { 0x3C,0x42,0xB9,0xA5,0xB9,0xA5,0x42,0x3C }, //ISO  "registered sign"
  { 0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, //ISO  "macron"
  { 0x3C,0x66,0x3C,0x00,0x00,0x00,0x00,0x00 }, //ISO  "degree sign"
  { 0x18,0x18,0x7E,0x18,0x18,0x00,0x7E,0x00 }, //ISO  "plus-minus sign"
  { 0x38,0x04,0x18,0x20,0x3C,0x00,0x00,0x00 }, //ISO  "superscript two"
  { 0x38,0x04,0x18,0x04,0x38,0x00,0x00,0x00 }, //ISO  "superscript three"
  { 0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00 }, //ISO  "acute accent"
  { 0x00,0x00,0x33,0x33,0x33,0x33,0x3E,0x60 }, //ISO  "micro sign"
  { 0x03,0x3E,0x76,0x76,0x36,0x36,0x3E,0x00 }, //ISO  "pilcrow sign"
  { 0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00 }, //ISO  "middle dot"
  { 0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x30 }, //ISO  "cedilla"
  { 0x10,0x30,0x10,0x10,0x38,0x00,0x00,0x00 }, //ISO  "superscript one"
  { 0x1C,0x36,0x36,0x36,0x1C,0x00,0x3E,0x00 }, //ISO  "masculine ordinal indicator"
  { 0x00,0xCC,0x66,0x33,0x33,0x66,0xCC,0x00 }, //ISO  "right-pointing double angle quotation mark"
  { 0x40,0xC0,0x40,0x48,0x48,0x0A,0x0F,0x02 }, //ISO  "vulgar fraction one quarter"
  { 0x40,0xC0,0x40,0x4F,0x41,0x0F,0x08,0x0F }, //ISO  "vulgar fraction one half"
  { 0xE0,0x20,0xE0,0x28,0xE8,0x0A,0x0F,0x02 }, //ISO  "vulgar fraction three quarters"
  { 0x18,0x00,0x18,0x18,0x30,0x66,0x3C,0x00 }, //ISO  "inverted question mark"
  { 0x30,0x18,0x3C,0x66,0x7E,0x66,0x66,0x00 }, //ISO  "Latin capital letter A with grave"
  { 0x0C,0x18,0x3C,0x66,0x7E,0x66,0x66,0x00 }, //ISO  "Latin capital letter A with acute"
  { 0x18,0x66,0x3C,0x66,0x7E,0x66,0x66,0x00 }, //ISO  "Latin capital letter A with circumflex"
  { 0x36,0x6C,0x3C,0x66,0x7E,0x66,0x66,0x00 }, //ISO  "Latin capital letter A with tilde"
  { 0x66,0x00,0x3C,0x66,0x7E,0x66,0x66,0x00 }, //ISO  "Latin capital letter A with diaeresis"
  { 0x3C,0x66,0x3C,0x66,0x7E,0x66,0x66,0x00 }, //ISO  "Latin capital letter A with ring above"
  { 0x3F,0x66,0x66,0x7F,0x66,0x66,0x67,0x00 }, //ISO  "Latin capital letter AE (ash)"
  { 0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x60 }, //ISO  "Latin capital letter C with cedilla"
  { 0x30,0x18,0x7E,0x60,0x7C,0x60,0x7E,0x00 }, //ISO  "Latin capital letter E with grave"
  { 0x0C,0x18,0x7E,0x60,0x7C,0x60,0x7E,0x00 }, //ISO  "Latin capital letter E with acute"
  { 0x3C,0x66,0x7E,0x60,0x7C,0x60,0x7E,0x00 }, //ISO  "Latin capital letter E with circumflex"
  { 0x66,0x00,0x7E,0x60,0x7C,0x60,0x7E,0x00 }, //ISO  "Latin capital letter E with diaeresis"
  { 0x30,0x18,0x7E,0x18,0x18,0x18,0x7E,0x00 }, //ISO  "Latin capital letter I with grave"
  { 0x0C,0x18,0x7E,0x18,0x18,0x18,0x7E,0x00 }, //ISO  "Latin capital letter I with acute"
  { 0x3C,0x66,0x7E,0x18,0x18,0x18,0x7E,0x00 }, //ISO  "Latin capital letter I with circumflex"
  { 0x66,0x00,0x7E,0x18,0x18,0x18,0x7E,0x00 }, //ISO  "Latin capital letter I with diaeresis"
  { 0x78,0x6C,0x66,0xF6,0x66,0x6C,0x78,0x00 }, //ISO  "Latin capital letter ETH"
  { 0x36,0x6C,0x66,0x76,0x7E,0x6E,0x66,0x00 }, //ISO  "Latin capital letter N with tilde"
  { 0x30,0x18,0x3C,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter O with grave"
  { 0x0C,0x18,0x3C,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter O with acute"
  { 0x18,0x66,0x3C,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter O with circumflex"
  { 0x36,0x6C,0x3C,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter O with tilde"
  { 0x66,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter O with diaeresis"
  { 0x00,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00 }, //ISO  "multiply sign"
  { 0x3D,0x66,0x6E,0x7E,0x76,0x66,0xBC,0x00 }, //ISO  "Latin capital letter O with slash"
  { 0x30,0x18,0x66,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter U with grave"
  { 0x0C,0x18,0x66,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter U with acute"
  { 0x3C,0x66,0x00,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter U with circumflex"
  { 0x66,0x00,0x66,0x66,0x66,0x66,0x3C,0x00 }, //ISO  "Latin capital letter U with diaeresis"
  { 0x0C,0x18,0x66,0x66,0x3C,0x18,0x18,0x00 }, //ISO  "Latin capital letter Y with acute"
  { 0xF0,0x60,0x7C,0x66,0x7C,0x60,0xF0,0x00 }, //ISO  "Latin capital letter THORN"
  { 0x3C,0x66,0x66,0x6C,0x66,0x66,0x6C,0xC0 }, //ISO  "Latin small letter sharp s"
  { 0x30,0x18,0x3C,0x06,0x3E,0x66,0x3E,0x00 }, //ISO  "Latin small letter a with grave"
  { 0x0C,0x18,0x3C,0x06,0x3E,0x66,0x3E,0x00 }, //ISO  "Latin small letter a with acute"
  { 0x18,0x66,0x3C,0x06,0x3E,0x66,0x3E,0x00 }, //ISO  "Latin small letter a with circumflex"
  { 0x36,0x6C,0x3C,0x06,0x3E,0x66,0x3E,0x00 }, //ISO  "Latin small letter a with tilde"
  { 0x66,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00 }, //ISO  "Latin small letter a with diaeresis"
  { 0x3C,0x66,0x3C,0x06,0x3E,0x66,0x3E,0x00 }, //ISO  "Latin small letter a with ring above"
  { 0x00,0x00,0x3F,0x0D,0x3F,0x6C,0x3F,0x00 }, //ISO  "Latin small letter ae (ash)"
  { 0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x60 }, //ISO  "Latin small letter c with cedilla"
  { 0x30,0x18,0x3C,0x66,0x7E,0x60,0x3C,0x00 }, //ISO  "Latin small letter e with grave"
  { 0x0C,0x18,0x3C,0x66,0x7E,0x60,0x3C,0x00 }, //ISO  "Latin small letter e with acute"
  { 0x3C,0x66,0x3C,0x66,0x7E,0x60,0x3C,0x00 }, //ISO  "Latin small letter e with cirumflex"
  { 0x66,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00 }, //ISO  "Latin small letter e with diaeresis"
  { 0x30,0x18,0x00,0x38,0x18,0x18,0x3C,0x00 }, //ISO  "Latin small letter i with grave"
  { 0x0C,0x18,0x00,0x38,0x18,0x18,0x3C,0x00 }, //ISO  "Latin small letter i with acute"
  { 0x3C,0x66,0x00,0x38,0x18,0x18,0x3C,0x00 }, //ISO  "Latin small letter i with circumflex"
  { 0x66,0x00,0x38,0x18,0x18,0x18,0x3C,0x00 }, //ISO  "Latin small letter i with diaeresis"
  { 0x18,0x3E,0x0C,0x06,0x3E,0x66,0x3E,0x00 }, //ISO  "Latin small letter eth"
  { 0x36,0x6C,0x00,0x7C,0x66,0x66,0x66,0x00 }, //ISO  "Latin small letter n with tilde"
  { 0x30,0x18,0x00,0x3C,0x66,0x66,0x3C,0x00 }, //ISO  "Latin small letter o with grave"
  { 0x0C,0x18,0x00,0x3C,0x66,0x66,0x3C,0x00 }, //ISO  "Latin small letter o with acute"
  { 0x3C,0x66,0x00,0x3C,0x66,0x66,0x3C,0x00 }, //ISO  "Latin small letter o with circumflex"
  { 0x36,0x6C,0x00,0x3C,0x66,0x66,0x3C,0x00 }, //ISO  "Latin small letter o with tilde"
  { 0x66,0x00,0x00,0x3C,0x66,0x66,0x3C,0x00 }, //ISO  "Latin small letter o with diaeresis"
  { 0x00,0x18,0x00,0xFF,0x00,0x18,0x00,0x00 }, //ISO  "divide sign"
  { 0x00,0x02,0x3C,0x6E,0x76,0x66,0xBC,0x00 }, //ISO  "Latin small letter o with slash"
  { 0x30,0x18,0x66,0x66,0x66,0x66,0x3E,0x00 }, //ISO  "Latin small letter u with grave"
  { 0x0C,0x18,0x66,0x66,0x66,0x66,0x3E,0x00 }, //ISO  "Latin small letter u with acute"
  { 0x3C,0x66,0x00,0x66,0x66,0x66,0x3E,0x00 }, //ISO  "Latin small letter u with circumflex"
  { 0x66,0x00,0x66,0x66,0x66,0x66,0x3E,0x00 }, //ISO  "Latin small letter u with diaeresis"
  { 0x0C,0x18,0x66,0x66,0x66,0x3E,0x06,0x3C }, //ISO  "Latin small letter y with acute"
  { 0x60,0x60,0x7C,0x66,0x7C,0x60,0x60,0x00 }, //ISO  "Latin small letter thorn"
  { 0x66,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C } //ISO  "Latin small letter y with diaeresis"
};

static inline void show_character( uint32_t x, uint32_t y, unsigned char c, uint32_t colour, struct workspace *ws )
{
  if ((x | y) & (1 << 31)) asm( "bkpt 3" ); // -ve coordinates? No thanks!

  uint32_t dx = 0;
  uint32_t dy = 0;

  for (dy = 0; dy < 8; dy++) {
    for (dx = 0; dx < 8; dx++) {
      if (0 != (system_font_from_space[c-32][dy] & (0x80 >> dx)))
        set_pixel( x+dx, y+dy, colour, ws );
      else
        set_pixel( x+dx, y+dy, Black, ws );
    }
  }
}

#define TOP 400

static void show_character_at( int cx, int cy, char ch, int core, uint32_t colour, struct workspace *ws )
{
  int x = cx * 8 + core * (60 * 8) + 4;
  int y = cy * 8 + TOP;
  show_character( x, y, ch, colour, ws );
}

static void new_line( struct core_workspace *workspace )
{
  workspace->x = 0;
  workspace->y++;
  if (workspace->y == 40)
    workspace->y = 0;

  char *row = workspace->display[workspace->y];
  for (int x = 0; x < 59; x++)
    row[x] = ' ';
}

static inline void add_to_display( char c, struct core_workspace *workspace )
{
  if (core( workspace ) == 0) {
    // Duplicate core 0 output on uart (no checks for overflows)
    if (c < ' ' && c != '\r' && c != '\n') {
      workspace->shared->uart->data = '|';
      workspace->shared->uart->data = c + '@';
    }
    else
      workspace->shared->uart->data = c;
  }
  return; // So singlestep doesn't take so long!

  if (workspace->x == 58 || c == '\n') {
    new_line( workspace );
  }
  if (c == '\r') {
    workspace->x = 0;
  }

  if (c != '\n' && c != '\r') {
    workspace->display[workspace->y][workspace->x++] = c;
  }
}

static inline void add_string( const char *s, struct core_workspace *workspace )
{
  while (*s != 0) {
    add_to_display( *s++, workspace );
  }
}

static inline void add_num( uint32_t number, struct core_workspace *workspace )
{
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (c > '9') c += ('a' - '0' - 10);
    add_to_display( c, workspace );
  }
}

static inline void add_green_num( uint32_t number, struct core_workspace *workspace )
{
  for (int nibble = 7; nibble >= 0; nibble--) {
    char c = '0' + ((number >> (nibble*4)) & 0xf);
    if (c > '9') c += ('a' - '0' - 10);
    add_to_display( c + 128, workspace );
  }
}

static void update_display( struct core_workspace *workspace )
{
  for (int y = 1; y <= 40; y++) {
    char *row = workspace->display[(y + workspace->y) % 40];
    for (int x = 0; x < 60; x++) {
      char c = row[x];
      if (c < ' ')
        show_character_at( x, y, c + '@', core( workspace ), Red, workspace->shared );
      else if (c > 128)
        show_character_at( x, y, c - 128, core( workspace ), Green, workspace->shared );
      else
        show_character_at( x, y, c, core( workspace ), White, workspace->shared );
    }
  }

  if (0 != workspace->shared->frame_buffer) {
    asm ( "svc 0xff" : : : "lr", "cc" );
  }
}

// FIXME This is totally in the wrong place!
// It's a legacy of when I was using the output stream for debugging
// data.
void __attribute__(( noinline )) C_WrchV_handler( char c, struct core_workspace *workspace )
{
  static const uint8_t bytes[32] = { 1, 2, 1, 1,  1, 1, 1, 1,
                                     1, 1, 1, 1,  1, 1, 1, 1,
                                     1, 2, 3, 6,  1, 1, 2, 10,
                                     9, 6, 1, 1,  5, 5, 1, 3 };
  const uint8_t *parameter_bytes = bytes;

  if (workspace->queued != 0) {
    workspace->queue[workspace->queued] = c;
    workspace->queued++;
  }
  else if (c < ' ') {
    // VDU codes.
    workspace->queue[0] = c;
    workspace->queued = 1;
  }

  if (workspace->queued != 0) {
    if (workspace->queued == parameter_bytes[workspace->queue[0]]) {
      // Got all the bytes we need to perform the action
      workspace->queued = 0;

      switch (workspace->queue[0]) {
      case 0: break; // Do nothing
      case 10: add_to_display( c, workspace ); break;       // Line feed
      case 13: add_to_display( c, workspace ); break;       // Carriage return
      default:
        {
          register uint32_t code asm( "r0" ) = workspace->queue[0];
          register void *params asm( "r1" ) = &workspace->queue[1];
          // FIXME handle errors
          // OS_VduCommand
          asm ( "svc 0x200fb" : : "r" (code), "r" (params) : "lr", "cc" );
        }
      }
    }
  }
  else {
    add_to_display( c, workspace );
  }

  clear_VF();
}

static void __attribute__(( naked )) WrchV_handler( char c )
{
  // OS_WriteC must preserve all registers, C will ensure the callee saved registers are preserved.
  asm ( "push { "C_CLOBBERED" }" );
  register struct core_workspace *workspace asm( "r12" );
  C_WrchV_handler( c, workspace );
  // Intercepting call (pops pc from the stack)
  asm ( "bvc 0f\n  bkpt #2\n0:" );
  asm ( "pop { "C_CLOBBERED", pc }" );
}

static void __attribute__(( noinline )) C_MouseV_handler( uint32_t *regs, struct workspace *workspace )
{
  // FIXME
  regs[0] = 100;        // x
  regs[1] = 100;        // y
  regs[2] = 0;          // Buttons
  regs[3] = 0;          // Time
}

static void __attribute__(( naked )) MouseV_handler()
{
  uint32_t *regs;
  asm ( "push { "C_CLOBBERED" }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  register struct workspace *workspace asm( "r12" );
  C_MouseV_handler( regs, workspace );
  // Intercepting call (pops pc from the stack)
  asm ( "bvc 0f\n  bkpt #2\n0:" );
  asm ( "pop { "C_CLOBBERED", pc }" );
}

static void __attribute__(( naked )) IrqV_handler();

typedef enum { HANDLER_PASS_ON, HANDLER_INTERCEPTED, HANDLER_FAILED } handled;

static void GraphicsV_ReadItems( uint32_t item, uint32_t *buffer, uint32_t len )
{
  switch (item) {
  case 4:
    {
      for (int i = 0; i < len; i++) {
        WriteS( "GraphicsV control list item: " ); WriteNum( buffer[i] );
      }
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
    }
    break;
  default:
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  };
}

handled __attribute__(( noinline )) C_GraphicsV_handler( uint32_t *regs, struct workspace *workspace )
{
  union {
    struct {
      uint32_t code:16;
      uint32_t head:8;
      uint32_t driver:8;
    };
    uint32_t raw;
  } command = { .raw = regs[4] };

  if (command.driver != workspace->graphics_driver_id) {
    clear_VF();
    return HANDLER_PASS_ON;
  }

  Write0( "GraphicsV for HAL " ); WriteNum( command.raw ); NewLine;

  switch (command.code) {
  case 0:
    break; // Null reason code for when vector has been claimed
  case 1:
    WriteS( "VSync interrupt occurred " ); 
    break; // VSync interrupt occurred 	BG 	SVC/IRQ
  case 2:
    WriteS( "Set mode " ); 
    break; // Set mode 	FG2 	SVC
  case 3:
    WriteS( "Obsolete3 (was Set interlace) " ); 
    break; // Obsolete3 (was Set interlace) 	FG 	SVC
  case 4:
    WriteS( "Set blank " ); 
    break; // Set blank 	FG/BG 	SVC
  case 5:
    WriteS( "Update pointer " ); 
    break; // Update pointer 	FG/BG 	SVC/IRQ
  case 6:
    WriteS( "Set DAG " ); 
    break; // Set DAG 	FG/BG 	SVC/IRQ
  case 7:
    WriteS( "Vet mode " ); 
    break; // Vet mode 	FG 	SVC
  case 8:  // Features 	FG 	SVC
    {
      regs[0] = 0x18; // No VSyncs, separate frame store, not variable frame store
      regs[1] = 0x20;
      regs[2] = 0;
      regs[4] = 0;
    }
    break;
  case 9:
    WriteS( "Framestore information " ); // Framestore information 	FG 	SVC
    {
      regs[0] = workspace->fb_physical_address;
      regs[1] = 8 << 20; // FIXME
    }
    break;
  case 10:
    WriteS( "Write palette entry " ); 
    break; // Write palette entry 	FG/BG 	SVC/IRQ
  case 11:
    WriteS( "Write palette entries " ); 
    break; // Write palette entries 	FG/BG 	SVC/IRQ
  case 12:
    WriteS( "Read palette entry " ); 
    break; // Read palette entry 	FG 	SVC
  case 13:
    WriteS( "Render " ); 
    break; // Render 	FG 	SVC
  case 14:
    WriteS( "IIC op " ); 
    break; // IIC op 	FG 	SVC
  case 15:
    WriteS( "Select head " ); 
    break; // Select head 	FG 	SVC
  case 16:
    WriteS( "Select startup mode " ); 
    break; // Select startup mode 	FG 	SVC
  case 17:
    WriteS( "List pixel formats " );
    break; // List pixel formats 	FG 	SVC
  case 18:
    GraphicsV_ReadItems( regs[0], (void*) regs[1], regs[2] );
    break; // Read info 	FG 	SVC
  case 19:
    WriteS( "Vet mode 2 " ); 
    break; // Vet mode 2 	FG 	SVC
  }

  regs[4] = 0; // Indicate to caller that call was intercepted

  return HANDLER_INTERCEPTED;
}

static void __attribute__(( naked )) GraphicsV_handler( char c )
{
  uint32_t *regs;
  asm ( "push { r0-r9, r12 }\n  mov %[regs], sp" : [regs] "=r" (regs) );
  asm ( "push {lr}" ); // Normal return address, to continue down the list

  register struct workspace *workspace asm( "r12" );
  handled result = C_GraphicsV_handler( regs, workspace );
  switch (result) {
  case HANDLER_FAILED: // Intercepted, but failed
  case HANDLER_INTERCEPTED:
    if (result == HANDLER_FAILED)
      set_VF();
    else
      clear_VF();
    asm ( "pop {lr}\n  pop { r0-r9, r12, pc }" );
    break;
  case HANDLER_PASS_ON:
    asm ( "pop {lr}\n  pop { r0-r9, r12 }\n  mov pc, lr" );
    break;
  }
}

static void *map_device_page( uint32_t physical_address )
{
  register uint32_t phys asm( "r0" ) = physical_address;
  register uint32_t pages asm( "r1" ) = 1;
  register void *result asm( "r0" );
  asm ( "svc 0xfe" : "=r" (result) : "r" (phys), "r" (pages) : "lr", "cc" );
  return result;
}

#define LED_BLINK_TIME 0x10000000

static inline void led_init( struct workspace *workspace )
{
  volatile struct gpio *g = workspace->gpio;
  // gpfsel[pin / 10] ... << 3 * (pin % 10)
  g->gpfsel[2] = (g->gpfsel[2] & ~(7 << (2 * 3))) | (1 << (2 * 3)); // GPIO pin 22
  g->gpfsel[2] = (g->gpfsel[2] & ~(7 << (7 * 3))) | (1 << (7 * 3)); // GPIO pin 27

  // Never before needed, but LED not getting bright. (Copied from IsambardOS)
  g->gppud = 0;
  asm volatile ( "dsb sy" );
  for (int i = 0; i < 150; i++) { asm volatile( "nop" ); }
  g->gppudclk[0] |= 1 << 4;
  asm volatile ( "dsb sy" );
  for (int i = 0; i < 150; i++) { asm volatile( "nop" ); }
  g->gppud = 0;
  asm volatile ( "dsb sy" );
  g->gppudclk[0] &= ~(1 << 4);
  // End.

  asm volatile ( "dsb sy" );
}

void led_on( struct workspace *workspace )
{
  // Probably overkill on the dsbs, but we're alternating between mailboxes and gpio
  asm volatile ( "dsb" );
  workspace->gpio->gpset[0] = (1 << 22);
  asm volatile ( "dsb" );
}

void led_off( struct workspace *workspace )
{
  asm volatile ( "dsb" );
  workspace->gpio->gpclr[0] = (1 << 22);
  asm volatile ( "dsb" );
}

void led_blink( struct workspace *workspace, int n )
{
  // Count the blinks! Extra short = 0, Long = 5

  if (n == 0) {
    led_on( workspace );
    for (uint64_t i = 0; i < LED_BLINK_TIME / 4; i++) { asm volatile ( "" ); }
    led_off( workspace );
    for (uint64_t i = 0; i < LED_BLINK_TIME; i++) { asm volatile ( "" ); }
  }
  else {
    while (n >= 5) {
      led_on( workspace );
      for (uint64_t i = 0; i < LED_BLINK_TIME * 4; i++) { asm volatile ( "" ); }
      led_off( workspace );
      for (uint64_t i = 0; i < LED_BLINK_TIME; i++) { asm volatile ( "" ); }
      n -= 5;
    }
    while (n > 0) {
      led_on( workspace );
      for (uint64_t i = 0; i < LED_BLINK_TIME; i++) { asm volatile ( "" ); }
      led_off( workspace );
      for (uint64_t i = 0; i < LED_BLINK_TIME; i++) { asm volatile ( "" ); }
      n --;
    }
  }
  for (uint64_t i = 0; i < 4 * LED_BLINK_TIME; i++) { asm volatile ( "" ); }
}

void show_word( int x, int y, uint32_t number, uint32_t colour, struct workspace *ws )
{
  static const char hex[] = "0123456789abcdef";
  for (int nibble = 0; nibble < 8; nibble++) {
    show_character( x+64-nibble*8, y, hex[number & 0xf], colour, ws );
    number = number >> 4;
  }
}

// TODO Take that code out of DynamicArea, and have another SWI for defining the screen, including the width and height...
static uint32_t *map_screen_into_memory( uint32_t address )
{
  register uint32_t r0 asm ( "r0" ) = 30;
  register uint32_t r1 asm ( "r1" ) = address;
  register uint32_t r2 asm ( "r2" ) = 8 << 20;  // Allows access to slightly more RAM than needed, FIXME (1920x1080x4 = 0x7e9000)
  // TODO Add a few more virtual lines, so that we're allocated the full 8MiB.
  register uint32_t *base asm ( "r1" );

  asm ( "svc %[os_dynamicarea]" : "=r" (base) : [os_dynamicarea] "i" (0x66), "r" (r0), "r" (r1), "r" (r2) : "lr", "cc" );

  return base;
}

static inline void stop_and_blink( struct workspace *workspace )
{
  bool on = true;
  for (;;) {
    for (int i = 0; i < 0x10000000; i++) { asm volatile( "" ); }

    if (on) led_off( workspace );
    else led_on( workspace );
    on = !on;
  }
}

// FIXME: Don't busy-wait for responses, have a module that handles
// GPU mailbox communications asynchronously!
uint32_t initialise_frame_buffer( struct workspace *workspace )
{
  GPU_mailbox volatile *mailbox = &workspace->gpu->mailbox[0];

  const int width = 1920;
  const int height = 1080;

  static const int space_to_claim = 26 * sizeof( uint32_t );
  static const uint32_t alignment = 2 << 20; // 2 MB aligned (more for long descriptor translation tables than short ones)

  dma_memory tag_memory = rma_claim_for_dma( space_to_claim, 16 );

  while (0 != (0xf & tag_memory.pa)) { tag_memory.pa++; tag_memory.va++; }

  uint32_t volatile *dma_tags = (void*) tag_memory.va;

  // Note: my initial sequence of tags, 0x00040001, 0x00048003, 0x00048004, 
  // 0x00048005, 0x00048006 didn't get a valid size value from QEMU.

  int index = 0;
  dma_tags[index++] = space_to_claim;
  dma_tags[index++] = 0;
  dma_tags[index++] = 0x00048005;    // Colour depth
  dma_tags[index++] = 4;
  dma_tags[index++] = 0;
  dma_tags[index++] = 32;
  dma_tags[index++] = 0x00048006;    // Pixel order
  dma_tags[index++] = 4;
  dma_tags[index++] = 0;
  dma_tags[index++] = 0;             // 0 = BGR, 1 = RGB
  dma_tags[index++] = 0x00048003;    // Set physical (display) width/height
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  dma_tags[index++] = width;
  dma_tags[index++] = height;
  dma_tags[index++] = 0x00048004;    // Set virtual (buffer) width/height
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  dma_tags[index++] = width;
  dma_tags[index++] = height + 13;    // Some hidden lines so that we are allocated whole MiB. FIXME for non-1080p
  // Despite a line of 1920 pixels being about 8k, the allocated amount varies enormously
  // 1088 results in 0x7f8000 (32KiB less than 8 MiB)
  // 1089 results in 0x816000 (88KiB more than 8 MiB)
  // 1093 is, by definition more than 8MB, so qemu, returning a closer size than the real hardware, will still work
  // It's safer to map in less than is allocated than more, since the ARM could corrupt GPU memory in the latter case
  // Mapping 0x800000 of the 0x816000 simply means 88KiB of memory won't be accessable by anyone.
  // Maybe we can use some of it for mouse pointers or something, as long as the GPU isn't used to clear the screen?
  dma_tags[index++] = 0x00040001;    // Allocate buffer
  dma_tags[index++] = 8;
  dma_tags[index++] = 0;
  int buffer_tag = index;
  dma_tags[index++] = alignment;
  dma_tags[index++] = 0;
  dma_tags[index++] = 0;             // End tag

  asm volatile ( "dsb sy" );
  asm ( "svc 0xff" : : : "lr", "cc" );

  uint32_t request = 8 | tag_memory.pa;

  while (dma_tags[buffer_tag] == alignment) {
    mailbox[1].value = request;
    asm volatile ( "dsb" );

    //workspace->gpio[0x28/4] = (1 << 22); // Clr
    led_on( workspace );

    uint32_t response;

    do {
      uint32_t countdown = 0x10000;
      while ((mailbox[0].status & (1 << 30)) != 0 && --countdown > 0) { asm volatile ( "dsb" ); } // Empty?
      if (countdown == 0) break;

      response = mailbox[0].value;
      if (response != request) stop_and_blink( workspace );
    } while (response != request);

    asm ( "svc 0xff" : : : "lr", "cc" );
  }

  led_off( workspace );
  asm volatile ( "dsb" );

  return (dma_tags[buffer_tag] & ~0xc0000000);
}

static uint32_t GraphicsV_DeviceNumber( char const *name )
{
  // OS_ScreenMode 65
  register uint32_t code asm( "r0" ) = 64;
  register uint32_t flags asm( "r1" ) = 0;
  register char const *driver_name asm( "r2" ) = name;
  register uint32_t allocated asm( "r0" );
  asm ( "svc 0x20065" : "=r" (allocated) : "r" (code), "r" (flags), "r" (driver_name) : "lr" );
  return allocated;
}

static void GraphicsV_DeviceReady( uint32_t number )
{
  // OS_ScreenMode 65
  register uint32_t code asm( "r0" ) = 65;
  register uint32_t driver_number asm( "r1" ) = number;
  asm ( "svc 0x20065" : : "r" (code), "r" (driver_number) : "lr" );
}

// Not static, or it won't be seen by inline assembler
void __attribute__(( noinline )) c_start_display( struct core_workspace *workspace )
{
  if (0 != workspace->shared->graphics_driver_id) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );

  WriteS( "BCM28xx" ); NewLine;

  workspace->shared->graphics_driver_id = GraphicsV_DeviceNumber( "BCM28xx" );

  {
    // This handler is not core-specific
    void *handler = GraphicsV_handler;
    register uint32_t vector asm( "r0" ) = 42;
    register void *routine asm( "r1" ) = handler;
    register struct workspace *handler_workspace asm( "r2" ) = workspace->shared;
    asm ( "svc %[swi]" : : [swi] "i" (OS_Claim | Xbit), "r" (vector), "r" (routine), "r" (handler_workspace) : "lr" );
  }

  WriteS( "HAL obtained GraphicsV" ); NewLine;
  GraphicsV_DeviceReady( workspace->shared->graphics_driver_id );
  WriteS( "Graphics Driver Ready" ); NewLine;

  WriteS( "HAL initialised frame buffer" ); NewLine;
}

static void __attribute__(( naked )) start_display()
{
  asm ( "push { "C_CLOBBERED", lr }"
    "\n  mov r0, r12"
    "\n  bl c_start_display"
    "\n  pop { "C_CLOBBERED", pc }" );
}

static inline
uint64_t timer_now()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 0, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo) : : "memory"  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static inline
uint32_t timer_interrupt_time()
{
  uint32_t hi, lo;

  asm volatile ( "mrrc p15, 2, %[lo], %[hi], c14" : [hi] "=r" (hi), [lo] "=r" (lo)  );

  uint64_t now;
  now = hi;
  now = now << 32;
  now = now | lo;

  return now;
}

static inline
void timer_interrupt_at( uint64_t then )
{
  asm volatile ( "mcrr p15, 2, %[lo], %[hi], c14" : : [hi] "r" (then >> 32), [lo] "r" (0xffffffff & then) : "memory" );
}

static inline
void timer_set_countdown( int32_t timer )
{
  asm volatile ( "mcr p15, 0, %[t], c14, c2, 0" : : [t] "r" (timer) );
  // Clear interrupt and enable timer
  asm volatile ( "mcr p15, 0, %[config], c14, c2, 1" : : [config] "r" (1) );
}

static inline
int32_t timer_get_countdown()
{
  int32_t timer;
  asm volatile ( "mrc p15, 0, %[t], c14, c2, 0" : [t] "=r" (timer) );
  return timer;
}

static inline
uint32_t timer_status()
{
  uint32_t bits;
  asm volatile ( "mrc p15, 0, %[config], c14, c2, 1" : [config] "=r" (bits) );
  return bits;
}

static inline
bool timer_interrupt_active()
{
  return (timer_status() & 4) != 0;
}

static int __attribute__(( noinline )) C_IrqV_handler( struct core_workspace *workspace )
{
  // This is where we will use the hardware to identify which devices have
  // tried to interrupt the processor.
  QA7 volatile *qa7 = workspace->shared->qa7;

  memory_read_barrier();

  // Source is: QA7 core interrupt source; bit 8 is GPU interrupt, bit 0 is physical timer

  uint32_t source = qa7->Core_IRQ_Source[core( workspace )];
  bool found = false;
  GPU *gpu = workspace->shared->gpu;

  // TODO is the basic_pending register still a thing?
  // TODO ignore interrupts that come from the GPU! They may be masked, but do they still show as pending?

  memory_read_barrier();

  // There are a few speedups possible
  //  e.g. test bits by seeing if (int32_t) (source << (32-irq)) is -ve, or zero (skip the rest of the bits)
  //  count leading zeros instruction...

  int last_reported_irq = workspace->last_reported_irq;
  int irq = last_reported_irq;
  bool last_possibility = false;

  // Write0( "IRQ " ); WriteNum( source ); Space; WriteNum( gpu->pending1 ); Space; WriteNum( gpu->pending1 ); NewLine;
  if (0) {
    register uint32_t r0 asm( "r5" ) = source;
    register uint32_t r1 asm( "r6" ) = gpu->pending1;
    register uint32_t r2 asm( "r7" ) = gpu->pending2;
    asm ( " .word 0xffffffff" : : "r"(r0), "r"(r1), "r"(r2) );
  }
  do {
    irq++;
    last_possibility = (irq == last_reported_irq);
    // WriteNum( irq ); Space;
    if (irq >= 0 && irq < 64) {
      if (0 == (source & (1 << 8))) {
        // Nothing from GPU, don't need to check anything under 64
        irq = 63;
      }
      else {
        uint32_t pending;
        if (irq < 32) {
          pending = gpu->pending1;
        }
        else {
          pending = gpu->pending2;
        }
        // We only get here with irq & 0x1f non-zero if the previous reported was in this range
        assert( (0 != (irq & 0x1f)) == (irq == last_reported_irq+1) );
        pending = pending >> (irq & 0x1f);
        while (pending != 0 && 0 == (pending & 1)) {
          irq++;
          pending = pending >> 1;
        }
        found = pending != 0;
        if (!found) {
          irq = irq | 0x1f;
        }
        // Next time round will be in next 32-bit chunk
        assert( found || 0x1f == (irq & 0x1f) );
      }
    }
    else if (irq == 72) {
      // Covered by 0..63
    }
    else if (irq < 76) {
      // 64 CNTPSIRQ
      // 65 CNTPNSIRQ
      // 66 CNTHPIRQ
      // 67 CNTVIRQ
      // 68 Mailbox 0
      // 69 Mailbox 1
      // 70 Mailbox 2
      // 71 Mailbox 3
      // 72 (GPU, be more specific, see above)
      // 73 PMU 
      // 74 AXI outstanding (core 0 only)
      // 75 Local timer 
      found = (0 != (source & (1 << (irq & 0x1f))));
    }
    else
      irq = -1; // Wrap around to 0 on the next loop

    // Check each possible source once, but stop if found
  } while (!found && !last_possibility);

  // if (irq == -1) { asm( "bkpt 777\n add %[s], %[p1], %[p2]" : : [s] "r" (source), [p1]"r" (gpu->pending1), [p2]"r" (gpu->pending2) : "lr" ); }

  if (found) {
    workspace->last_reported_irq = irq;

    return irq;
  }
  else {
    return -1;
  }
}

static void __attribute__(( naked )) IrqV_handler()
{
  // IrqV contains no information in the registers on entry, except r12
  asm ( "push { "C_CLOBBERED" }" );
  register struct core_workspace *workspace asm( "r12" );
  register uint32_t *sp asm( "sp" ); // Location of r0
  *sp = C_IrqV_handler( workspace );
  // Intercepting call (pops pc from the stack)
  asm ( "pop { "C_CLOBBERED", pc }" );
}

// Returns with interrupts disabled for this core, enable the source
// and call Task_WaitForInterrupt asap.
static void disable_interrupts()
{
  asm volatile ( "svc %[swi]"
      :
      : [swi] "i" (OS_IntOff)
      : "lr" );
}

// Decouple the TickerV from the actual interrupt that causes it.
// Unlike the documentation, PRM 1-99, enabling interrupts during
// the vector call will not allow another call.
static void tickerv_task( uint32_t handle, struct core_workspace *ws )
{
  //QA7 volatile *qa7 = ws->shared->qa7;
  int this_core = core( ws );
  int ticks = 0;
  for (;;) {
    Task_WaitUntilWoken();

    ticks++;
    if (ticks % 10 == 0) show_word( this_core * 1920/4, 60, ticks, Green, ws->shared ); 
/*
{
  register uint32_t request asm ( "r0" ) = 255;

  asm volatile ( "svc %[swi]"
      :
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      : "lr", "cc" );
}
    for (int i = 0; i < 4; i++) {
      Space; WriteNum( qa7->Core_write_clear[i].Mailbox[3-i] );
    }
    NewLine;
*/
    // Vector is called with interrupts disabled
    asm ( "svc %[swi]" : : [swi] "i" (OS_IntOff) );
    asm ( 
      "\n  mov r9, #0x1c// TickerV"
      "\n  svc %[swi]"
      : : [swi] "i" (Xbit | OS_CallAVector) : "r9", "lr" );
    asm ( "svc %[swi]" : : [swi] "i" (OS_IntOn) );
  }
}

static void timer_interrupt_task( uint32_t handle, struct core_workspace *ws, int device )
{
  int this_core = core( ws );

  struct workspace *shared = ws->shared;
  uint32_t ticks_per_interval = shared->ticks_per_interval;
  QA7 volatile *qa7 = shared->qa7;

  uint32_t tickerv_handle;

WriteS( "Timer interrupt task" ); NewLine;
  {
    uint64_t *stack = (void*) ((&ws->tickerv_stack)+1);

    register uint32_t request asm ( "r0" ) = TaskOp_CreateThread;
    register void *code asm ( "r1" ) = tickerv_task;
    register void *stack_top asm ( "r2" ) = stack;
    register struct core_workspace *cws asm( "r3" ) = ws;

    register uint32_t handle asm ( "r0" );

    asm volatile ( "svc %[swi]"
        : "=r" (handle) 
        : [swi] "i" (OS_ThreadOp)
        , "r" (request)
        , "r" (code)
        , "r" (stack_top)
        , "r" (cws)
        : "lr" );

    tickerv_handle = handle;
  }

  WriteS( "Timer task claiming interrupt and entering loop " ); WriteNum( handle ); NewLine;

  disable_interrupts();

  memory_write_barrier(); // About to write to QA7

  // Let the generic ARM timer interrupt this core
  qa7->Core_timers_Interrupt_control[this_core] = 15; // 2; // Generic ARM timer irq

  memory_write_barrier(); // About to write to something else

  timer_set_countdown( ticks_per_interval );

  memory_write_barrier(); // Maybe needed?

  const uint32_t tick_divider = 10;
  uint32_t ticks = 0;

  do {
    Task_WaitForInterrupt( device );

    int32_t timer = timer_get_countdown();
    uint32_t missed_ticks = 0;

    while (timer < 0) {
      timer += ticks_per_interval;
      missed_ticks++;
    }
    // TODO: Report missed ticks?

    timer_set_countdown( timer );

    {
    GPU volatile *gpu = shared->gpu;
    if (0 != (gpu->basic_pending & 1)) {
      WriteS( "IRQ still outstanding!" ); NewLine;
    }
    else {
      WriteS( "." );
      asm ( 
        "\n  mov r0, #0xff"
        "\n  svc %[swi]"
        : : [swi] "i" (Xbit | OS_ThreadOp) : "r0", "lr" );
    }
    }

    // If we wanted to enable interrupts we would ensure the
    // source of the interrupt was disabled, then call:
    // interrupt_is_off( device );
    ticks += missed_ticks;

    if (ticks >= tick_divider) Task_WakeTask( tickerv_handle );

    while (ticks >= tick_divider) {
      ticks -= tick_divider;
    }
  } while (true); // Could check a flag in ws, in case of shutdown
}

static uint32_t start_timer_interrupt_task( struct core_workspace *ws, int device )
{
  uint64_t *stack = (void*) ((&ws->ticker_stack)+1);

  register uint32_t request asm ( "r0" ) = TaskOp_CreateThread + 0x100; // In separate slot
  register void *code asm ( "r1" ) = timer_interrupt_task;
  register void *stack_top asm ( "r2" ) = stack;
  register struct core_workspace *workspace asm( "r3" ) = ws;
  register uint32_t dev asm( "r4" ) = device;

  register uint32_t handle asm ( "r0" );

  asm volatile ( "svc %[swi]"
      : "=r" (handle) 
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      , "r" (code)
      , "r" (stack_top)
      , "r" (workspace)
      , "r" (dev)
      : "lr" );

  return handle;
}

// TODO This will be the interrupt from the GPU, ths HAL should report a
// larger number of interrupts, including one for each of the GPU interrupt
// sources.
static void uart_interrupt_task( uint32_t handle, struct core_workspace *ws, int device )
{
  int this_core = core( ws );

  QA7 volatile *qa7 = ws->shared->qa7;
  UART volatile *uart = ws->shared->uart;
  GPU volatile *gpu = ws->shared->gpu;

  WriteS( "Listening to UART" ); NewLine;
  uart->control = 0x31; // enable, tx & rx

  disable_interrupts();

  memory_write_barrier(); // About to write to QA7

  // FIXME: This belongs in a section that knows about interrupt mapping

  qa7->GPU_interrupts_routing = this_core * 5; // FIQ and IRQ to this core

  memory_write_barrier(); // About to write to something else

  if (device < 32) {
    gpu->enable_irqs1 = (1 << device);
  }
  else {
    gpu->enable_irqs2 = (1 << (device-32));
  }

  memory_write_barrier(); // About to write to something else

  uart->control |= (1 << 9); // Receive interrupt enable

  memory_write_barrier(); // Maybe needed?

  do {
    Task_WaitForInterrupt( device );

    uint32_t c = uart->data;
    char buffer[2] = { c, 0 };
    // This is naughty, the call may block the task. But this is simply
    // a toy device handler.
    WriteN( buffer, 1 ); NewLine;
  } while (true); // Could check a flag in ws, in case of shutdown
}

static uint32_t start_uart_interrupt_task( struct core_workspace *ws, int device )
{
  register uint32_t request asm ( "r0" ) = TaskOp_CreateThread;
  register void *code asm ( "r1" ) = uart_interrupt_task;
  register void *stack_top asm ( "r2" ) = (&ws->shared->uart_task_stack+1);
  register struct core_workspace *workspace asm( "r3" ) = ws;
  register uint32_t dev asm( "r4" ) = device;

  register uint32_t handle asm ( "r0" );

  asm volatile ( "svc %[swi]"
      : "=r" (handle) 
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      , "r" (code)
      , "r" (stack_top)
      , "r" (workspace)
      , "r" (dev)
      : "lr" );

  return handle;
}

#include "include/pipeop.h"

static void __attribute__(( noreturn )) console_task( uint32_t handle, struct core_workspace *ws, uint32_t read_pipe )
{
  PipeSpace data = { 0, 0, 0 };
  add_string( "Starting console task, pipe: ", ws );
  add_num( read_pipe, ws );
  add_string( "\r\n", ws );

  for (;;) {
    if (data.available == 0) {
      data = PipeOp_WaitForData( read_pipe, 1 );

      if (data.location == 0) {
        add_string( "PipeOp_WaitForData returned zero location!", ws ); update_display( ws );
        for (;;) asm ( "wfi" );
      }
      if (data.available == 0) {
        add_string( "PipeOp_WaitForData returned zero bytes", ws ); update_display( ws );
        for (;;) asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); // FIXME
      }
    }
    while (data.available > 0) {
      char *s = data.location;

      for (int i = 0; i < data.available; i++)
        add_to_display( s[i], ws );

      data = PipeOp_DataConsumed( read_pipe, data.available );
    }
    update_display( ws );
  }
}

static uint32_t start_console_task( struct core_workspace *ws, uint32_t pipe )
{
  uint64_t *stack = (void*) ((&ws->console_stack)+1);

  register uint32_t request asm ( "r0" ) = TaskOp_CreateThread;
  register void *code asm ( "r1" ) = console_task;
  register void *stack_top asm ( "r2" ) = stack;
  register struct core_workspace *workspace asm( "r3" ) = ws;
  register uint32_t read_pipe asm( "r4" ) = pipe;

  register uint32_t handle asm ( "r0" );

  asm volatile ( "svc %[swi]"
      : "=r" (handle) 
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      , "r" (code)
      , "r" (stack_top)
      , "r" (workspace)
      , "r" (read_pipe)
      : "lr" );

  return handle;
}

static const int board_interrupt_sources = 64 + 12; // 64 GPU, 12 ARM peripherals (BCM2835-ARM-Peripherals.pdf, QA7)

// args: debug_pipe to open for reading FIXME: OS_GetEnv?

void __attribute__(( noinline )) c_init( uint32_t this_core, uint32_t number_of_cores, struct workspace **private, char const *args )
{
  bool first_entry = (*private == 0);

  struct workspace *workspace;

  if (first_entry) {
    *private = new_workspace( number_of_cores );
  }

  workspace = *private;

  // Map this addresses into all cores
  workspace->gpu = map_device_page( 0x3f00b000 );

  workspace->gpio = map_device_page( 0x3f200000 );

  if (first_entry) {
    led_init( workspace );
    //led_blink( 0x04040404, workspace );
  }

  workspace->uart = map_device_page( 0x3f201000 );
  workspace->qa7 = map_device_page( 0x40000000 );

  workspace->uart->data = '0' + this_core;

  if (first_entry) {
    asm volatile ( "dsb" );

    workspace->fb_physical_address = initialise_frame_buffer( workspace );
  }

  workspace->frame_buffer = map_screen_into_memory( workspace->fb_physical_address );

show_word( this_core * (1920/4), 16, this_core*0x11111111, first_entry ? Red : Green, workspace ); 
show_word( this_core * (1920/4), 32, (uint32_t) workspace->gpio, first_entry ? Red : Green, workspace ); 
  QA7 volatile *qa7 = workspace->qa7;
show_word( this_core * (1920/4), 48, (uint32_t) &qa7->Core_write_clear[this_core], first_entry ? Red : Green, workspace ); 

  workspace->core_specific[this_core].shared = workspace;
  workspace->core_specific[this_core].queued = 0; // VDU code queue size, including character that started it filling
  workspace->core_specific[this_core].x = 0;
  workspace->core_specific[this_core].y = 0;
  for (int y = 0; y < 40; y++) {
    for (int x = 0; x < 60; x++) {
      workspace->core_specific[this_core].display[y][x] = ' ';
    }
  }

  {
    void *handler = IrqV_handler;
    register uint32_t vector asm( "r0" ) = 2;
    register void *routine asm( "r1" ) = handler;
    register struct core_workspace *handler_workspace asm( "r2" ) = &workspace->core_specific[this_core];
    asm ( "svc %[swi]" : : [swi] "i" (OS_Claim | Xbit), "r" (vector), "r" (routine), "r" (handler_workspace) : "lr" );
  }

  {
    void *handler = WrchV_handler;
    register uint32_t vector asm( "r0" ) = 3;
    register void *routine asm( "r1" ) = handler;
    register struct core_workspace *handler_workspace asm( "r2" ) = &workspace->core_specific[this_core];
    asm ( "svc %[swi]" : : [swi] "i" (OS_Claim | Xbit), "r" (vector), "r" (routine), "r" (handler_workspace) : "lr" );

    add_string( "HAL obtained WrchV\n\r", &workspace->core_specific[this_core] );
  }

  {
    void *handler = MouseV_handler;
    register uint32_t vector asm( "r0" ) = 0x1a;
    register void *routine asm( "r1" ) = handler;
    register struct core_workspace *handler_workspace asm( "r2" ) = &workspace->core_specific[this_core];
    asm ( "svc %[swi]" : : [swi] "i" (OS_Claim | Xbit), "r" (vector), "r" (routine), "r" (handler_workspace) : "lr" );

    add_string( "HAL obtained MouseV\n\r", &workspace->core_specific[this_core] );
  }

  {
    uint32_t pipe = 0;
    char const *p = args;
    // Skip filename
    while (*p > ' ') p++;
    while (*p == ' ') p++;
    for (int i = 0; i < 8; i++) {
      char c = p[i];
      if (c >= 'a' && c <= 'f') c = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') c = c - 'A' + 10;
      else if (c >= '0' && c <= '9') c = c - '0';
      else asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); // FIXME
      pipe = (pipe << 4) | c;
    }

    if (pipe != 0) {
add_string( "starting console task ", &workspace->core_specific[this_core] );
add_num( pipe, &workspace->core_specific[this_core] );
      uint32_t handle = start_console_task( &workspace->core_specific[this_core], pipe );
      handle = handle; // unused variable
    }
  }

#define QEMU
  workspace->qa7->timer_prescaler = 0x06AAAAAB;

  // Enable timer, no interrupts yet. (It is shared between all cores.)

#ifdef QEMU
  const uint32_t clock_frequency = 62500000;
#else
  const uint32_t clock_frequency = 1000000; // Pi3 with default prescaler - 1MHz (checked manually over 60s)
#endif
  // For information only. CNTFRQ
  asm volatile ( "mcr p15, 0, %[bits], c14, c0, 0" : : [bits] "r" (clock_frequency) );

  // No event stream, EL0 accesses not trapped to undefined: CNTHCTL
  asm volatile ( "mcr p15, 0, %[config], c14, c1, 0" : : [config] "r" (0x303) );

  if (first_entry) {
    workspace->ticks_per_interval = clock_frequency / 1000; // milliseconds

#ifdef QEMU
    const int slower = 1000;
    Write0( "Slowing timer ticks by: " ); WriteNum( slower ); NewLine;
    workspace->ticks_per_interval = workspace->ticks_per_interval * slower;
#endif
    Write0( "Timer ticks per interval: " ); WriteNum( workspace->ticks_per_interval ); NewLine;

    Task_RegisterInterruptSources( board_interrupt_sources );

    memory_write_barrier(); // About to write to QA7
    workspace->qa7->GPU_interrupts_routing = this_core;
    workspace->qa7->Core_IRQ_Source[this_core] = 0xffd;
  }
  else {
    workspace->qa7->Core_IRQ_Source[this_core] = 0xd;
  }

  GPU *gpu = workspace->gpu;
  Write0( "IRQs enabled " ); WriteNum( gpu->enable_basic ); Space; WriteNum( gpu->enable_irqs1 ); Space; WriteNum( gpu->enable_irqs2 ); NewLine;

  if (0) {
    uint32_t handle = start_timer_interrupt_task( &workspace->core_specific[this_core], 64 );
    Write0( "Timer task: " ); WriteNum( handle ); NewLine;
  }
  else {
    WriteS( "No timer interrupts" ); NewLine;
  }

  if (first_entry) {
    uint32_t handle = start_uart_interrupt_task( &workspace->core_specific[this_core], 57 );
    Write0( "UART task: " ); WriteNum( handle ); NewLine;
  }
  else {
    WriteS( "No uart interrupts" ); NewLine;
  }

  if (first_entry) {
    // Involves calculation, don't assign directly to the register variable
    struct core_workspace *cws = &workspace->core_specific[this_core];

    register void *callback asm( "r0" ) = start_display;
    register struct core_workspace *ws asm( "r1" ) = cws;
    asm( "svc %[swi]" : : [swi] "i" (OS_AddCallBack | 0x20000), "r" (callback), "r" (ws) );
  }

show_word( this_core * (1920/4), 96, 0x11111111, first_entry ? Red : Green, workspace ); 

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

#include "Resources.h"

void register_files( uint32_t *regs )
{
  register void const *files asm ( "r0" ) = resources;
  register uint32_t service asm ( "r1" ) = regs[1];
  register uint32_t code asm ( "r2" ) = regs[2];
  register uint32_t workspace asm ( "r3" ) = regs[3];
  asm ( "mov lr, pc"
    "\n  mov pc, r2"
    :
    : "r" (files)
    , "r" (service)
    , "r" (code)
    , "r" (workspace)
    : "lr" );
}

void __attribute__(( naked )) service_call()
{
  asm ( "teq r1, #0x77"
    "\n  teqne r1, #0x50"
    "\n  teqne r1, #0x60"
    "\n  movne pc, lr" );


  // This is extremely minimal, and not all that efficient!
  // Object to mode changes. All of them.
  asm ( "teq r1, #0x77"
    "\n  moveq r1, #0"
    "\n  moveq r2, #0"
    "\n  moveq pc, lr" );

  asm ( "teq r1, #0x50"
    "\n  bne 0f"
    "\n  ldr r12, [r12]"
    "\n  mov r1, #0"
    "\n  adr r3, vidc_list"
/* VIDC List:
https://www.riscosopen.org/wiki/documentation/show/Service_ModeExtension
0 	3 (list format)
1 	Log2BPP mode variable
2 	Horizontal sync width (pixels)
3 	Horizontal back porch (pixels)
4 	Horizontal left border (pixels)
5 	Horizontal display size (pixels)
6 	Horizontal right border (pixels)
7 	Horizontal front porch (pixels)
8 	Vertical sync width (rasters)
9 	Vertical back porch (rasters)
10 	Vertical top border (rasters)
11 	Vertical display size (rasters)
12 	Vertical bottom border (rasters)
13 	Vertical front porch (rasters)
14 	Pixel rate (kHz)
15 	Sync/polarity flags:
Bit 0: Invert H sync
Bit 1: Invert V sync
Bit 2: Interlace flags (bits 3 and 4) specified, else kernel decides interlacing1
Bit 3: Interlace sync1
Bit 4: Interlace fields1
16+ 	Optional list of VIDC control list items (2 words each)
N 	-1 (terminator) 
 */
    "\nvidc_list: .word 3, 5, 0, 0, 0, 1920, 0, 0, 0, 0, 0, 1080, 0, 0, 8000, 0, -1"
    "\n  0:" );

  asm ( "teq r1, #0x60" // Service_ResourceFSStarting
    "\n  bne 0f"
    "\n  push { "C_CLOBBERED", lr }"
    "\n  mov r0, sp"
    "\n  bl register_files"
    "\n  pop { "C_CLOBBERED", pc }"
    "\n  0:" );

    asm (
    "\n  bkpt %[line]" : : [line] "i" (__LINE__) );
}

