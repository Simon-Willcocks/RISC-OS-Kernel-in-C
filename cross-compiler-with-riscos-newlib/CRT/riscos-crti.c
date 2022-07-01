#include <sys/types.h>

extern void _start( char const *env );

void __attribute__(( naked, noreturn, section (".init") )) _init()
{
  // TODO Use the OS to allocate a stack for this thread, rather than
  // using himem. I'd like a OS_ThreadOp to allocate a virtual memory
  // area that can grow downwards as needed.

  // Initial stack starts at the top of the slot memory
  register char const *env asm ( "r0" );
  register uint32_t himem asm ( "r1" );
  register uint64_t *time asm ( "r2" );
  asm volatile ( "svc 0x10" : "=r" (env)        // OS_GetEnv
		            , "=r" (himem)
			    , "=r" (time) );
  asm volatile ( "mov sp, %[stacktop]" : 
                            : [stacktop] "r" (himem) );
  _start( env );
  __builtin_unreachable();
}
