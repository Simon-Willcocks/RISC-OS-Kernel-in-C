
void __attribute__(( naked, noreturn, section( ".fini" ) )) _fini()
{
  static const struct {
    int code;
    char string[];
  } error_block = { 0xffff, "Unexpected RISC OS newlib exit" };
  register void *error asm( "r0" ) = &error_block;
  register int abex asm( "r1" ) = 0x58454241; // "ABEX"
  register int result asm( "r2" ) = 0xffff;
  asm volatile ( "svc 0x11" : : "r" (error), "r" (abex), "r" (result) ); // OS_Exit
  __builtin_unreachable();
}

