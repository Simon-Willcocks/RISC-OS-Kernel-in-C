/* Copyright 2021 Simon Willcocks
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

#include "inkernel.h"

typedef struct fs fs;

struct fs {
  uint32_t module;
  uint32_t info;
  uint32_t r12;
  fs *next;
};

bool do_OS_FSControl( svc_registers *regs )
{
  claim_lock( &shared.kernel.fscontrol_lock, workspace.core_number+1 );
  switch (regs->r[0]) {
  case 12: 
    {
      fs *f = (void*) sma_allocate( sizeof( fs ), regs );
      f->module = regs->r[1];
      f->info = regs->r[2];
      f->r12 = regs->r[3];
      f->next = shared.kernel.filesystems;
      return true;
    }
  default:
    return false;
  }
  release_lock( &shared.kernel.fscontrol_lock );
}

