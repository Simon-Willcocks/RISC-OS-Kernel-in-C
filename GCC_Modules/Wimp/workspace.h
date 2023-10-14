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

static uint32_t const maxtasks = 128;

struct autoscroll {
  uint32_t enable;

  uint32_t state; //       ; flags word
  uint32_t handle; //       ; window being scrolled
  uint32_t pz_x0; //       ; pause zone sizes
  uint32_t pz_y0; //
  uint32_t pz_x1; //
  uint32_t pz_y1; //
  uint32_t user_pause; //       ; minimum pause time (cs), or -1 to use default
  uint32_t user_rout; //       ; user routine, or < &8000 to use Wimp-supplied
  uint32_t user_wsptr; //       ; user routine workspace (if above >= &8000)

  uint32_t pause; //       ; minimum pause time (cs) (explicit if default)
  uint32_t rout; //       ; routine (user or Wimp)
  uint32_t wsptr; //       ; workspace (user or Wimp)
  uint32_t next_t; //       ; time when to start autoscrolling, or when next to update
  uint32_t last_t; //       ; time of last update
  uint32_t last_x; //       ; position of mouse at last examination
  uint32_t last_y; //
  uint32_t old_ptr_colours[3]; // used when restoring pointer after autoscroll pointer use
  uint8_t old_ptr_number; //
  uint8_t default_pause; //       ; derived from CMOS (ds)
  uint8_t scrolling; //       ; used to determine next setting of flag bit 8
  uint8_t pausing; //       ; used to determine whether timer is dirty, also a "don't re-enter" flag
};

static uint32_t const autoscr_speed_factor = 5; // -log2 of number of pointer offsets to scroll per centisecond
static uint32_t const autoscr_update_delay = 8; // hardwired minimum interval between updates (cs)
                                                // is necessary to ensure null events have a chance to be seen

struct workspace {
  struct taskinfo *taskstack[maxtasks];
  struct taskinfo *taskpointers[maxtasks];
  struct taskinfo *PollTasks[maxtasks];

  struct wimp_window *allwinds;
  struct wimp_window *activewinds;
  struct wimp_window *oldactivewinds;
  struct wimp_window *openingwinds;

  struct wimp_window heldoverwinds;


  uint8_t dragflag;
  uint8_t dragaction;
  uint8_t addtoolstolist;
  uint8_t dotdash1;
  uint8_t dotdash2;
  uint8_t dotdash;

  struct taskinfo **taskstack; // Grows upwards
  struct taskinfo **PollTaskPtr;

  struct autoscroll autoscroll;
};

