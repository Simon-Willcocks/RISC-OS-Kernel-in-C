#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>

extern int main();
extern void exit( int );

typedef struct {
  char const *env;
  uint32_t himem;
  uint64_t *time; // Only 5 bytes valid in legacy RO
} RISCOS_Environment;

static RISCOS_Environment OS_GetEnv()
{
  register char const *env asm ( "r0" );
  register uint32_t himem asm ( "r1" );
  register uint64_t *time asm ( "r2" );
  asm volatile ( "svc 0x10" : "=r" (env), "=r" (himem), "=r" (time) );
  RISCOS_Environment result = { .env = env, .himem = himem, .time = time };
  return result;
}

static void RISCOS_SetMemoryLimit( uint32_t new_limit )
{
  register uint32_t code asm ( "r0" ) = 0;
  register uint32_t himem asm ( "r1" ) = new_limit;
  register uint32_t same_code asm ( "r0" );
  register uint32_t old_limit asm ( "r1" );
  asm volatile ( "svc 0x40" : "=r" (same_code), "=r" (old_limit) : "r" (code), "r" (himem) : "r2", "r3" );
}

void * sbrk (ptrdiff_t incr)
{
  RISCOS_Environment env = OS_GetEnv();

  if (incr != 0) {
    env.himem += ((incr + 0xfff) & ~0xfff);
    RISCOS_SetMemoryLimit( env.himem );
  }

  return (void*) env.himem;
}

#define true  (0 == 0)
#define false (0 != 0)
#define assert( b ) while (!(b)) { }

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/times.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <stdio.h>

void exit( int result )
{
  assert( false );
  __builtin_unreachable();
}

int close(int file)
{
  assert( false );
}

/* pointer to array of char * strings that define the current environment variables */
// Maybe store in the TaskSlot for access by all local threads?
char **environ = { "arg0", 0 };

int execve (const char *__path, char * const __argv[], char * const __envp[])
{
  assert( false );
}

int fork()
{
  assert( false );
}

int fstat(int file, struct stat *st)
{
  assert( false );
}

int getpid()
{
  assert( false );
}

int isatty(int file)
{
  assert( false );
}

int kill(int pid, int sig)
{
  assert( false );
}

int     link (const char *__path1, const char *__path2)
{
  assert( false );
}

off_t   lseek (int __fildes, off_t __offset, int __whence)
{
  assert( false );
}

int open(const char *name, int flags, ...)
{
  assert( false );
}

_READ_WRITE_RETURN_TYPE read (int __fd, void *__buf, size_t __nbyte)
{
  assert( false );
}

int stat(const char *file, struct stat *st)
{
  assert( false );
}

clock_t times(struct tms *buf)
{
  assert( false );
}

int unlink( const char *__path )
{
  assert( false );
}

int wait(int *status)
{
  assert( false );
}

_READ_WRITE_RETURN_TYPE write (int __fd, const void *__buf, size_t __nbyte)
{
  assert( false );
}

int gettimeofday(struct timeval *__restrict p, void *__restrict z)
{
  assert( false );
}


