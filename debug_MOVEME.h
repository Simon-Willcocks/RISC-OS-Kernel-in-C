typedef union {
  struct __attribute__(( packed )) {
    uint32_t enable:1;
    uint32_t pmc:2;
    uint32_t lsc:2;
    uint32_t bas:8;
    uint32_t hmc:1;
    uint32_t ssc:2;
    uint32_t lbn:4;
    uint32_t wt:1;
    uint32_t res0_1:3;
    uint32_t mask:1;
    uint32_t res0_2:1;
  };
  uint32_t raw;
} watchpoint_control;
// Set a watchpoint, DBGWVR<0>
asm volatile ( "  mrc p14, 0, %[addr], cr0, cr0, 6" : : [addr] "r" (0x20000344) );

// And enable it.
watchpoint_control enable = { .enable = 1, .pmc = 2, .lsc = 3, .bas = 15, .hmc = 0, .ssc = 0, .lbn = 0, .wt = 0, .mask = 0 };

// Control register, DBGWCR<0>
asm volatile ( "  mrc p14, 0, %[bits], cr0, cr0, 7" : : [bits] "r" (enable.raw) );

typedef union {
  struct __attribute__(( packed )) {
    uint32_t res0_1:14;
    uint32_t spd:2;
    uint32_t res0_2:1;
    uint32_t spme:1;
    uint32_t ste:1;
    uint32_t ttrf:1;
    uint32_t edad:1;
    uint32_t epmad:1;
    uint32_t res0_3:1;
    uint32_t sccd:1;
    uint32_t res0_4:3;
    uint32_t tdcc:1;
    uint32_t mtpme:1;
    uint32_t res0_5:3;
  };
  uint32_t raw;
} secure_debug_control;

// Secure Debug Control Register
secure_debug_control control = { .epmad = 1, .edad = 1, .ste = 1 };
asm volatile ( "  mrc p15, 0, %[bits], cr1, cr3, 1" : : [bits] "r" (control.raw) );

asm volatile ( "  mrc p15, 0, %[bits], cr1, cr1, 1" : : [bits] "r" (3) );

