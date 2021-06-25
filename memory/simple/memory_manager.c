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

#include "kernel.h"

void Kernel_add_free_RAM( uint32_t base_page, uint32_t size_in_pages )
{
  claim_lock( &shared.memory.lock );

  int i = 0;
  while (shared.memory.free_blocks[i].size != 0) {
    i++;
  }

  shared.memory.free_blocks[i].base_page = base_page;
  shared.memory.free_blocks[i].size = size_in_pages;

  release_lock( &shared.memory.lock );
}

uint32_t Kernel_allocate_pages( uint32_t size_in_pages, uint32_t alignment )
{
  uint32_t result = -1;
  claim_lock( &shared.memory.lock );

  // FIXME

  release_lock( &shared.memory.lock );
  return result;
}
