/* Copyright 2023 Simon Willcocks
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

/* Conversion of the WindowManager to C */

const unsigned module_flags = 3;
// Bit 0: 32-bit compatible
// Bit 1: Multiprocessing

#define MODULE_CHUNK "0x400c0"

#include "module.h"
#include "include/taskop.h"

NO_start;
//NO_init;
NO_finalise;
NO_service_call;
//NO_title;
NO_help;
NO_keywords;
NO_swi_handler;
NO_swi_names;
NO_swi_decoder;
NO_messages_file;

const char __attribute__(( section( ".text" ) )) title[] = "WindowManager";

#include "workspace.h"

static inline struct workspace *new_workspace( uint32_t number_of_cores )
{
  uint32_t required = sizeof( struct workspace );

  struct workspace *memory = rma_claim( required );

  memset( memory, 0, required );

  return memory;
}

static inline void set_application_memory( uint32_t initial_size )
{
  register uint32_t number asm( "r0" ) = 0;
  register uint32_t size asm( "r1" ) = initial_size + 0x8000; // ChangeEnvironment expects the upper limit.
  register uint32_t r2 asm( "r2" ) = 0; // read/ignored
  register uint32_t r3 asm( "r3" ) = 0; // read/ignored
  register error_block *error asm( "r0" );

  asm volatile ( "svc %[swi]"
    : "=r" (error)
    , "=r" (r2)
    , "=r" (r3)
    : [swi] "i" (Xbit | OS_ChangeEnvironment)
    , "r" (number)
    , "r" (size)
    , "r" (r2)
    , "r" (r3)
    : "lr", "cc", "memory" );
}

static inline void initialise_heap( void* heap_base, uint32_t heap_size )
{
  register uint32_t cmd asm( "r0" ) = 0;
  register void *base asm( "r1" ) = heap_base;
  register uint32_t size asm( "r3" ) = heap_size;
  register error_block *error asm( "r0" );

  asm volatile ( "svc %[swi]"
    : "=r" (error)
    : [swi] "i" (Xbit | OS_Heap)
    , "r" (cmd)
    , "r" (base)
    , "r" (size)
    : "lr", "cc", "memory" );
}

static inline void *heap_allocate( uint32_t bytes )
{
  register uint32_t cmd asm( "r0" ) = 2; // Get Heap Block
  register uint32_t base asm( "r1" ) = 0x8000;
  register uint32_t size asm( "r3" ) = bytes;
  register void *allocation asm( "r2" );
  register error_block *error asm( "r0" );

  asm volatile ( "svc %[swi]"
    : "=r" (error)
    , "=r" (allocation)
    : [swi] "i" (Xbit | OS_Heap)
    , "r" (cmd)
    , "r" (base)
    , "r" (size)
    : "lr", "cc" );

  return allocation;
}

static inline void start_task( server *server, void (*task)( uint32_t handle, uint32_t *queue ) )
{
  static uint32_t const initial_stack_size = 6 << 10;
  uint32_t *stack_base = heap_allocate( initial_stack_size );

  register uint32_t request asm ( "r0" ) = TaskOp_CreateThread;
  register void *code asm ( "r1" ) = task;
  register void *stack asm ( "r2" ) = stack_base + (initial_stack_size / sizeof( uint32_t ) );
  register uint32_t *queue asm ( "r3" ) = &server->queue;
  register uint32_t handle asm( "r0" );

  asm volatile ( "svc %[swi]"
      : "=r" (handle)
      : [swi] "i" (OS_ThreadOp)
      , "r" (request)
      , "r" (code)
      , "r" (stack)
      , "r" (queue)
      : "lr", "cc", "memory" );

  server->task = handle;
}

// To run in usr32 mode:
extern void __attribute__(( noinline, noreturn )) readvarval_task( uint32_t handle, uint32_t *queue );
extern void __attribute__(( noinline, noreturn )) setvarval_task( uint32_t handle, uint32_t *queue );
extern void __attribute__(( noinline, noreturn )) gstrans_task( uint32_t handle, uint32_t *queue );
extern void __attribute__(( noinline, noreturn )) oscli_task( uint32_t handle, uint32_t *queue );

void __attribute__(( noinline )) c_init( uint32_t this_core, uint32_t number_of_cores, struct workspace **private, char const *args )
{
  bool first_entry = (*private == 0);

  struct workspace *workspace;

  if (first_entry) {
    *private = new_workspace( number_of_cores );
  }

  workspace = *private;

  static uint32_t const initial_size = 32 << 10; // 32KiB, to start with

  set_application_memory( initial_size );

  initialise_heap( (void*) 0x8000, initial_size );

  start_task( &workspace->readvarval, adr( readvarval_task ) );
  start_task( &workspace->setvarval, adr( setvarval_task ) );
  //start_task( &workspace->gstrans, adr( gstrans_task ) );
  //start_task( &workspace->oscli, adr( oscli_task ) );

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

