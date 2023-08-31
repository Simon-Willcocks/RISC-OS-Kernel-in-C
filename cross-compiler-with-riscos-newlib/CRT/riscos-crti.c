// #define DEBUG__ENABLE
#include <sys/types.h>

#define assert( b ) while (!(b)) { asm ( "svc 0x17" ); }

extern void _start( char const *env );

uint32_t himem; // Used by sbrk, it seems it should keep track by the byte!

#ifdef DEBUG__ENABLE
void write_num( uint32_t n );
void newline();
#else
static void inline write_num( uint32_t n ) {}
static void inline newline() {}
#endif

// Enough initial stack to call sbrk safely
struct {
  uint32_t s[31];
} tinystack;

extern uint32_t _end;

void __attribute__(( naked, noreturn, section (".init") )) _init()
{
  // TODO Use the OS to allocate a stack for this thread, rather than
  // using himem. I'd like a OS_ThreadOp to allocate a virtual memory
  // area that can grow downwards as needed.

  // Allocate initial stack
  register char const *env asm ( "r0" );
  register uint32_t mem asm ( "r1" );
  register uint64_t *time asm ( "r2" );
  asm volatile ( "svc 0x10" : "=r" (env)        // OS_GetEnv
		            , "=r" (mem)
			    , "=r" (time) );

  static uint32_t const stack_size = 0x8000;
  uint32_t top = (uint32_t) &_end;
  uint32_t stacktop = (top + stack_size + 0xfff) & ~0xfff; // Aligned, at least stack_size

  asm volatile ( "mov sp, %[stacktop]" : 
                            : [stacktop] "r" ((&tinystack)+1) );

  himem = mem;
  write_num( mem );
  write_num( top );
  write_num( stacktop );

  // Call _sbrk directly, less stack used than brk
  uint32_t before = _sbrk( stacktop - mem );

  assert( before == mem );
  assert( himem == stacktop );

  asm volatile ( "mov sp, %[stacktop]" : 
                            : [stacktop] "r" (himem) );
  _start( env );
  __builtin_unreachable();
}
