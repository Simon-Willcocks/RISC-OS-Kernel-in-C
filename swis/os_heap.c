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

// Implementation from description of heap structures in RISC OS 3 PRMs (1-357)
typedef struct {
  uint32_t magic;
  uint32_t free_offset;
  uint32_t base_offset;
  uint32_t end_offset;
} heap_header;

typedef struct {
  uint32_t offset_of_next_free;
  uint32_t size;
} heap_free_block;

static const uint32_t magic = 0x70616548;

enum { Initialise, Describe, Allocate, Free, ChangeBlockSize, ChangeHeapSize, ReadBlockSize };

error_block BadHeap = { 0x180, "Bad Heap" };
error_block NotEnoughMemory = { 0x184, "Not enough memory (in heap)" };

static inline void *ptr_from_offset( heap_header *head, uint32_t offset )
{
  return (char*) head + offset;
}

static inline uint32_t offset_from_ptr( heap_header *head, void *ptr )
{
  return (char*) ptr - (char*) head;
}

bool do_OS_Heap( svc_registers *regs )
{
  heap_header *b = (void*) regs->r[1];

  if (regs->r[0] != Initialise && b->magic != magic) {
    regs->r[0] = (uint32_t) &BadHeap;
    return false;
  }

  switch (regs->r[0]) {
  case Initialise:
    {
    uint32_t size = regs->r[3];
    b->magic = magic;
    b->free_offset = 0;
    b->base_offset = sizeof( heap_header );
    b->end_offset = size;
    }
    return true;
  case Describe:
    {
    heap_free_block *f = ptr_from_offset( b, b->free_offset );
    uint32_t total_free = b->end_offset - b->base_offset - 4;
    uint32_t largest_available = b->end_offset - b->base_offset;
    while (f != (heap_free_block*) b) {
      total_free += f->size;
      if (f->size > largest_available)
        largest_available = f->size;
      f = ptr_from_offset( b, f->offset_of_next_free );
    }
    regs->r[2] = largest_available;
    regs->r[3] = total_free;
    }
    return true;
  case Allocate:
    {
    // Size of the block of heap that will be allocted to return the requested size
    uint32_t size = (regs->r[3] + 7) & ~3;
    if (size < 8) size = 8;

    uint32_t *result = 0;
    regs->r[2] = 0; // In case of error

    heap_free_block *f = ptr_from_offset( b, b->free_offset );
    uint32_t *reference_to_f = &b->free_offset;
    heap_free_block *best_non_match = f;
    uint32_t *reference_to_best = &b->free_offset;

    // First, look through the freed blocks for an exact match.
    // In case there isn't one, remember the best alternative.
    // "Best" could be closest fit or largest.
    // We will return the best alternative, in whole or part.

    while (f != (heap_free_block*) b && f->size != size) {
      if (f->size > best_non_match->size) {
        best_non_match = f;
        reference_to_best = reference_to_f;
      }
      reference_to_f = &f->offset_of_next_free;
      f = ptr_from_offset( b, f->offset_of_next_free );
    }

    if (f != (heap_free_block*) b) {
      // Found one that matches exactly
      *reference_to_f = f->offset_of_next_free;
      result = &f->size;
    }
    else if (best_non_match->size < size) {
      // None were big enough to allocate the size required, go to unused space, instead.
      uint32_t new_base = b->base_offset + size;
      if (new_base > b->end_offset) {
        regs->r[0] = (uint32_t) &NotEnoughMemory;
        return false;
      }
      result = ptr_from_offset( b, b->base_offset + 4 );
      b->base_offset = new_base;
    }
    else {
      // Best match. If the size is almost the same, return the whole thing, split it otherwise
      if (best_non_match->size > size + 8) {
        *reference_to_best += size;
        heap_free_block *new_free_block = ptr_from_offset( b, *reference_to_best );
        new_free_block->size = best_non_match->size - size;
        new_free_block->offset_of_next_free = best_non_match->offset_of_next_free;
      }
      else {
        *reference_to_best = best_non_match->offset_of_next_free;
      }
      result = &best_non_match->size;
    }

    if (result != 0) {
      result[-1] = size;
    }

    regs->r[2] = (uint32_t) result;
    }
    return true;
  case Free:
    {
    // FIXME
    }
    return true;
  case ChangeBlockSize:
    {
    // FIXME
    }
    return true;
  case ChangeHeapSize:
    {
    // FIXME
    }
    return true;
  case ReadBlockSize:
    {
    regs->r[3] = ((uint32_t*) regs->r[2])[-1];
    }
    return true;
  }

  static error_block bad_reason = { 0x180, "Bad reason code" };
  regs->r[0] = (uint32_t) &bad_reason;

  return false;
}

