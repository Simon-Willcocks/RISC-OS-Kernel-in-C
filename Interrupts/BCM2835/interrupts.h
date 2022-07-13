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
 */

typedef struct {
  TaskSlot *slot;     // Non-traditional
  void (*code)();     // All cases
  uint32_t r12;       // Traditional
} InterruptHandler;

struct Interrupts_workspace {
  uint32_t lock;
  InterruptHandler handlers[2];
};

struct Interrupts_shared_workspace {
};

uint32_t IdentifyInterrupt();
