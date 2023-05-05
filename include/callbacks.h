/* Copyright 2022 Simon Willcocks
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
 *
 */

typedef struct callback callback;

struct callback {
  uint32_t code;
  uint32_t private_word;
  callback *next;
  callback *prev;
};

#ifndef MPSAFE_DLL_TYPE
#include "mpsafe_dll.h"
#endif

MPSAFE_DLL_TYPE( callback )

static inline void *rma_allocate( uint32_t size );
static inline callback *alloc_callback( int size )
{
  return rma_allocate( size );
}

static inline callback *callback_new( callback **pool )
{
  return mpsafe_fill_and_detach_callback_at_head( pool, alloc_callback, 64 );
}

static inline void release_callback( callback **pool, callback *c )
{
  mpsafe_insert_callback_at_tail( pool, c );
}
