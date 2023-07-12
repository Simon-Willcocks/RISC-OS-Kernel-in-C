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

#include "common.h"

static void run_handler( uint32_t code, uint32_t private )
{
  // Very trustingly, run module code
  register uint32_t p asm ( "r12" ) = private;
  register uint32_t c asm ( "r14" ) = code;
  asm volatile ( "blx r14" : : "r" (p), "r" (c) : "cc", "memory" );
}

// This implementation allows callbacks to be run on multiple cores,
// which could cause problems. Rather use a mpsafe_foreach function?
void run_transient_callbacks()
{
  transient_callback *latest;

  do {
    latest = mpsafe_detach_callback_at_head( &shared.task_slot.transient_callbacks );
    if (latest != 0) {
#ifdef DEBUG__SHOW_TRANSIENT_CALLBACKS
  WriteS( "Call transient callback: " ); WriteNum( latest->code ); WriteS( ", " ); WriteNum( latest->private_word ); NewLine;
#endif
      run_handler( latest->code, latest->private_word );
      mpsafe_insert_callback_at_tail( &shared.kernel.callbacks_pool, latest );
    }
  } while (latest != 0);
}

static inline bool equal_callback( callback *a, callback *b )
{
  return a->code == b->code && a->private_word == b->private_word;
}

bool do_OS_RemoveCallBack( svc_registers *regs )
{
    asm ( "bkpt 0x1999" ); // Untested

  transient_callback cb = { .code = regs->r[0], .private_word = regs->r[1] };
  transient_callback *found = mpsafe_find_and_remove_callback( &shared.task_slot.transient_callbacks, &cb, equal_callback );
  if (found != 0) {
    mpsafe_insert_callback_at_tail( &shared.kernel.callbacks_pool, found );
  }
  else {
    asm ( "bkpt 0x1001" ); // Error?
  }
  return true;
}

void set_transient_callback( uint32_t code, uint32_t private )
{
#ifdef DEBUG__SHOW_TRANSIENT_CALLBACKS
  WriteS( "New transient callback: " ); WriteNum( code ); WriteS( ", " ); WriteNum( private ); NewLine;
#endif
  transient_callback *callback = callback_new( &shared.kernel.callbacks_pool );

  if (callback == 0) {
    asm ( "bkpt 0x1002" ); // FIXME
  }

  callback->code = code;
  callback->private_word = private;

  mpsafe_insert_callback_at_head( &shared.task_slot.transient_callbacks, callback );
}

bool do_OS_AddCallBack( svc_registers *regs )
{
  set_transient_callback( regs->r[0], regs->r[1] );

  return true;
}

