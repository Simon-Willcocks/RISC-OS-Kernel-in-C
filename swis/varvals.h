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

#include "kernel.h"
#include "include/types.h"
#include "include/kernel_swis.h"
#include "include/taskop.h"
#include "include/pipeop.h"

typedef struct variable variable;

// Memory split between stack and heap
static const uint32_t stack_top = 0x9000;

struct globals {
  variable *head;
  // For scan_string:
  PipeSpace string;
  PipeSpace buffer;
};

static uint32_t const heap = stack_top + sizeof( struct globals );
static uint32_t const top = 0x10000; // initial top of heap

void c_environment_vars_task( uint32_t handle, uint32_t queue );
