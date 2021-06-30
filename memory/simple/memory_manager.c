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
  claim_lock( &shared.memory.lock, 1 + workspace.core_number );

  free_block *p = shared.memory.free_blocks;

  while (p->size != 0) {
    p++;
  }

  p->base_page = base_page;
  p->size = size_in_pages;

  release_lock( &shared.memory.lock );
}

static bool aligned( uint32_t b, uint32_t alignment )
{
  return 0 == (b & (alignment - 1));
}

static uint32_t misalignment( uint32_t b, uint32_t alignment )
{
  return alignment - (b & (alignment - 1));
}

// How I think this algorithm will work:
// as memory is allocated, the earlier blocks will become less aligned,
// when a more aligned memory area is needed, there might not be a free_block
// whose base is sufficiently aligned, so the last block will be made into
// an aligned free block by taking the top off an earlier block at the first
// point of alignment.
// In practice, the first free blocks will be least aligned, and the last, most
// aligned.
// This is basically untested, and I'm sure there are dozens of better
// approaches, which I intend to look up later.

// This will not play nicely with freeing pages, but that comes later.
// I anticipate having a linked list of freed blocks, and a page of OS
// memory that I can remap to examine them. With the list structure at
// the start of the freed memory blocks, there will be practically zero OS
// memory overhead.

uint32_t Kernel_allocate_pages( uint32_t size, uint32_t alignment )
{
  uint32_t result = -1;
  uint32_t size_in_pages = size >> 12;
  uint32_t alignment_in_pages = alignment >> 12;

  claim_lock( &shared.memory.lock, 1 + workspace.core_number );

  free_block *p = shared.memory.free_blocks;

  while (p->size != 0
      && (!aligned( p->base_page, alignment_in_pages )
       || p->size < size_in_pages)) {
    p++;
  }

  if (p->size == 0) {
    // Find a big enough block to split, and take the aligned part off into another free block

    free_block *big = shared.memory.free_blocks;
    while (big->size != 0
        && big->size < size_in_pages + misalignment( big->base_page, alignment_in_pages )) {
      big++;
    }

    if (big->size != 0) {
      uint32_t mis = misalignment( big->base_page, alignment_in_pages );
      p->size = big->size - mis;
      p->base_page = big->base_page + mis;
      big->size = mis;
    }
  }

  if (p->size != 0) {
    result = p->base_page << 12;
    p->base_page += size_in_pages;
    p->size -= size_in_pages;

    if (p->size == 0) {
      do {
        *p = *(p+1);
        p++;
      } while (p->size != 0);
    }
  }

  release_lock( &shared.memory.lock );
  return result;
}
