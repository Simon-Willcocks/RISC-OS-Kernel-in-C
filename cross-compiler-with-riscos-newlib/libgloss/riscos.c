
#include <_ansi.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <errno.h>
#include <reent.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* Forward prototypes.  */
int	_system		(const char *);
int	_rename		(const char *, const char *);
int	_isatty		(int);
clock_t _times		(struct tms *);
int	_gettimeofday	(struct timeval *, void *);
void	_raise		(void);
int	_unlink		(const char *);
int	_link		(const char *, const char *);
int	_stat		(const char *, struct stat *);
int	_fstat		(int, struct stat *);
void *	_sbrk		(ptrdiff_t);
pid_t	_getpid		(void);
int	_kill		(int, int);
void	_exit		(int);
int	_close		(int);
int	_swiclose	(int);
int	_open		(const char *, int, ...);
int	_swiopen	(const char *, int);
int	_write		(int, const void *, size_t);
_off_t	_lseek		(int, _off_t, int);
_off_t	_swilseek	(int, _off_t, int);
int	_read		(int, void *, size_t);
int	_swiread	(int, void *, size_t);
void	initialise_monitor_handles (void);

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

void * _sbrk (ptrdiff_t incr)
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

void _exit( int result )
{
  assert( false );
  __builtin_unreachable();
}

int _close(int file)
{
  assert( false );
}

/* pointer to array of char * strings that define the current environment variables */
// Maybe store in the TaskSlot for access by all local threads?
char *_environ[] = { 0 };

int _execve (const char *__path, char * const __argv[], char * const __envp[])
{
  assert( false );
}

int _fork()
{
  assert( false );
}

int _fstat(int file, struct stat *st)
{
  assert( false );
}

int _getpid()
{
  assert( false );
}

int _isatty(int file)
{
  assert( false );
}

int _kill(int pid, int sig)
{
  assert( false );
}

int     _link (const char *__path1, const char *__path2)
{
  assert( false );
}

off_t   _lseek (int __fildes, off_t __offset, int __whence)
{
  assert( false );
}

int _open(const char *name, int flags, ...)
{
  assert( false );
}

ssize_t _read (int __fd, void *__buf, size_t __nbyte)
{
  assert( false );
}

int _stat(const char *file, struct stat *st)
{
  assert( false );
}

clock_t _times(struct tms *buf)
{
  assert( false );
}

int _unlink( const char *__path )
{
  assert( false );
}

int _wait(int *status)
{
  assert( false );
}

ssize_t _write (int __fd, const void *__buf, size_t __nbyte)
{
  assert( false );
}

int _gettimeofday(struct timeval *__restrict p, void *__restrict z)
{
  assert( false );
}


