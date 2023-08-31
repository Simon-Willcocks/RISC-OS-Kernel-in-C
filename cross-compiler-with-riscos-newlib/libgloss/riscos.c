// #define DEBUG__ENABLE

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
#include <string.h>

#define true  (0 == 0)
#define false (0 != 0)
#define assert( b ) while (!(b)) { asm ( "svc 0x17" ); }

typedef struct error_block {
  uint32_t num;
  char desc[];
} error_block;

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
  asm volatile ( "svc 0x20010" : "=r" (env), "=r" (himem), "=r" (time) );
  RISCOS_Environment result = { .env = env, .himem = himem, .time = time };
  return result;
}

static inline void RISCOS_SetMemoryLimit( uint32_t new_limit )
{
  // This works in programs that have nothing to do with the Wimp
  // (as long as it's running?)
  register uint32_t new_size asm ( "r0" ) = new_limit;
  register int32_t next asm ( "r1" ) = -1;
  register uint32_t same_size asm ( "r0" );
  asm volatile ( "svc 0x600ec" : "=r" (same_size), "=r" (next) : "r" (new_size), "r" (next) : "r2", "r3" );
}

#define MAXFD 64
int32_t rofd[MAXFD] = { -1, -1, -1, 0 }; // stdin, stdout, stderr

error_block *__OSFind( int __code, char const *__name, char const *__path, int *handlep )
{
  register uint32_t req             asm( "r0" ) = __code;
  register char const *name         asm( "r1" ) = __name;
  register char const *path         asm( "r2" ) = __path;

  register int32_t handle          asm( "r0" );
  register error_block *error       asm( "r1" );
  asm volatile (
        "svc %[swi]"
    "\n  movvc r1, #0"
    "\n  movvs r1, r0"
    : "=r" (error)
    , "=r" (handle)
    : [swi] "i" (0x2000d) // OS_Find
    , "r" (req)
    , "r" (name)
    , "r" (path) );

  if (handlep != 0) {
    if (error == 0)
      *handlep = handle;
    else
      *handlep = 0;
  }

  return error;
}

error_block *__OSFind_Close( int __fd )
{
  register uint32_t req             asm( "r0" ) = 0;
  register int fd                   asm( "r1" ) = __fd;

  register error_block *error       asm( "r0" );
  asm volatile (
        "svc %[swi]"
    : "=r" (error)
    : [swi] "i" (0x2000d) // OS_Find
    , "r" (req)
    , "r" (fd) );

  return error;
}

#ifdef DEBUG__ENABLE
void write_num( uint32_t n )
{
  static char buf[10];
  char const hex[] = "0123456789abcdef";
  buf[8] = 10; // '\r';
  buf[9] = 13; // '\n';
  for (int i = 7; i >= 0; i--) {
    buf[i] = hex[n & 0xf];
    n = n >> 4;
  }
  _write( 1, buf, 10 );
}

void newline()
{
  _write( 1, "\n\r", 2 );
}
#else
static void inline write_num( uint32_t n ) {}
static void inline newline() {}
#endif

void * _sbrk (ptrdiff_t incr)
{
  extern uint32_t himem;

#ifdef DEBUG__ENABLE
  write( 1, "sbrk: ", 6 );
#endif

  write_num( incr );

  RISCOS_Environment env = OS_GetEnv();
  uint32_t old = himem;

  write_num( env.himem );
  write_num( himem );

  if (incr > 0) { // Never reducing memory, TODO
    uint32_t new_page = (himem + incr + 0xfff) >> 12;
    uint32_t old_page = (himem + 0xfff) >> 12;
    if (old_page != new_page) {
      write_num( new_page << 12 );
      RISCOS_SetMemoryLimit( new_page << 12 );
    }
  }

  himem = himem + incr; // Up or down!

  newline();
#if 0
  register uint32_t *sp asm ( "sp" );
  uint32_t *p = sp;
  for (int i = 0; i < 32; i++) write_num( p[i] );
#endif
  return (void*) old;
}

void _exit( int result )
{
  assert( false );
  __builtin_unreachable();
}

int _close(int __fd)
{
  if (__fd <= MAXFD
   && rofd[__fd] != 0) { // Not willing to close all files for everyone
    __OSFind_Close( rofd[__fd] );
    rofd[__fd] = 0;
  }
  else {
    asm ( "svc 0x17" );
  }
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
  st->st_dev = 0;       /* dev_t        ID of device containing file */
  st->st_ino = 0;       /* ino_t        Inode number */
  st->st_mode = 0;      /* mode_t       File type and mode */
  st->st_nlink = 0;     /* nlink_t      Number of hard links */
  st->st_uid = 0;       /* uid_t        User ID of owner */
  st->st_gid = 0;       /* gid_t        Group ID of owner */
  st->st_rdev = 0;      /* dev_t        Device ID (if special file) */
  st->st_size = 0;      /* off_t        Total size, in bytes */
  st->st_blksize = 1;   /* blksize_t    Block size for filesystem I/O */
  st->st_blocks = 0;    /* blkcnt_t     Number of 512B blocks allocated */

  st->st_atim.tv_sec = 0;      /* timespec     Time of last access */
  st->st_mtim.tv_sec = 0;      /* timespec     Time of last modification */
  st->st_ctim.tv_sec = 0;      /* timespec     Time of last status change */

  return 0;
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

static uint32_t __convert_to_roname( const char *name, int len, char *buffer )
{
  // "C":
  // /rofs/filing-system/drive/directory1/directory2/filename
  // RISC OS:
  // (filing-system:)(:drive).$.(directory1).(directory2).(filename)

  int ni = 6; // index into name array
  int bi = 0; // index into buffer array

  if (name[0] == '/'
   && name[1] == 'r'
   && name[2] == 'o'
   && name[3] == 'f'
   && name[4] == 's'
   && name[5] == '/') {
    while (ni < len && name[ni] != '/') {
      buffer[bi++] = name[ni++];
    }
    buffer[bi++] = ':';
    buffer[bi++] = ':';
    ni++;
    while (ni < len && name[ni] != '/') {
      buffer[bi++] = name[ni++];
    }
    buffer[bi++] = '.';
    buffer[bi++] = '$';
    buffer[bi++] = '.';
    ni++;
    while (ni < len) {
      while (ni < len && name[ni] != '/') {
        buffer[bi++] = name[ni++];
      }
      if (name[ni] == '/') {
        buffer[bi++] = '.';
        ni++;
      }
    }
    buffer[bi++] = '\0';
  }
  else {
    assert( false );
  }

  return bi;
}

int _open(const char *__name, int __flags, ... )
{
  int result = 0;
  while (rofd[result] != 0) result++;
  if (result >= MAXFD) {
    return -1;
  }

#ifdef DEBUG__ENABLE
  _write( 1, "opening ", 9 );
  _write( 1, __name, strlen( __name ) );
  write_num( result );
#endif

  int len = strlen( __name );
  char roname[len + 1];
  int rolen = __convert_to_roname( __name, len, roname );

  _write( 1, "opening ", 9 );
  _write( 1, roname, strlen( roname ) );

  error_block *error = __OSFind( 0x80, roname, 0, &rofd[result] );
  if (error != 0) {
    // OMG, the error number varies according to the filesystem!
    // Assuming file exists...
    error = __OSFind( 0x88, roname, 0, &rofd[result] );
  }

  assert( error == 0 );

  write_num( rofd[result] );
  newline();

  return result;
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
  char const *buf = __buf;

#ifdef DEBUG__ENABLE
  {
    register char c asm( "r0" ) = 'A' + __fd;
    asm( "svc 0" : : "r" (c) );
  }
  if (__fd > 2)
  {
    uint32_t riscos_descriptor = rofd[__fd];
    write_num( riscos_descriptor );
    register uint32_t c asm( "r0" ) = riscos_descriptor + 48;
    asm( "svc 0" : : "r" (c) );
  }

  asm ( "svc %[swi]" : : [swi] "i" (256 + '"') );
#endif
  if (__fd <= MAXFD) {
    if (0 == rofd[__fd]){
      asm( "svc %[swi]" : : [swi] "i" (0x100 + 'z') );
    }
    else if (-1 == rofd[__fd]) {
      for (int i = 0; i < __nbyte; i++) {
        register uint32_t c asm( "r0" ) = buf[i];
        asm ( "svc 0" : : "r" (c) );
        if (c == 10) asm ( "svc 0x10d" ); // Unix-style TODO Really? What about VDU sequences. This is a bad idea.
      }
    }
    else { 
      // OS_GBPB
      register uint32_t req             asm( "r0" ) = 2;
      register uint32_t handle          asm( "r1" ) = rofd[__fd];
      register void const *buffer       asm( "r2" ) = buf;
      register uint32_t size            asm( "r3" ) = __nbyte;
      register error_block *error       asm( "r0" );
      asm volatile (
            "svc %[swi]"
        "\n  movvc r0, #0"
        : "=r" (error)
        : [swi] "i" (0x2000c) // OS_GBPB
        , "r" (req)
        , "r" (handle)
        , "r" (buffer)
        , "r" (size) );
    }
  }
  else {
    asm ( "svc 0x17" );
  }
#ifdef DEBUG__ENABLE
  asm ( "svc %[swi]" : : [swi] "i" (256 + '"') );
#endif
  return __nbyte;
}

int _gettimeofday(struct timeval *__restrict p, void *__restrict z)
{
  assert( false );
}


