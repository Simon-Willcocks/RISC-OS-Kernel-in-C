extern void __attribute__(( naked, noreturn )) _start();

void __attribute__(( naked, noreturn, section( ".init" ) )) _init()
{
  asm volatile ( "svc 0x10\n  mov sp, r2" ); // Set SP to top of available RAM
  _start();
  __builtin_unreachable();
}
