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


struct Memory_manager_workspace {
};

struct Memory_manager_shared_workspace {
  uint32_t lock;
  struct {
    uint32_t base_page:16;
    uint32_t size:16; // pages
  } free_blocks[16];
};


void Kernel_add_free_RAM( uint32_t base_page, uint32_t size_in_pages );
uint32_t Kernel_allocate_pages( uint32_t size_in_pages, uint32_t alignment );
