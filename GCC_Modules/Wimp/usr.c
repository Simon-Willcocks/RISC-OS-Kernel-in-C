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

// This file contains usr32 mode code
#include "usr.h"

enum {
  windowicon_workarea = -1,
  windowicon_back = -2,
  windowicon_close = -3,
  windowicon_title = -4,
  windowicon_toggle = -5,
  windowicon_up = -6,
  windowicon_verticalbar = -7,
  windowicon_down = -8,
  windowicon_resize = -9,
  windowicon_left = -10,
  windowicon_horizbar = -11,
  windowicon_right = -12,
  windowicon_outerframe = -13,
  windowicon_iconise = -14,
  windowicon_bothbars = -15
};

enum {
  iconposn_back           = 1,
  iconposn_close          = 2,
  iconposn_title          = 3,
  iconposn_toggle         = 4,
  iconposn_vscroll        = 5,
  iconposn_resize         = 6,
  iconposn_hscroll        = 7,
  iconposn_iconise        = 8
}

struct task_data {
  uint32_t task_flagword;
  uint32_t task_slotptr;

  uint32_t task_wimpver;        // R0 on entry to Wimp_Initialise
  uint32_t task_pollword;       // R3 on entry to Wimp_Poll(Idle)
  uint32_t task_fpblock;        // FP register save block

  uint32_t task_file;           // File handle for swap file.
  uint32_t task_filename;       // File name for swap file.
  uint32_t task_extent;         // File extent / Slot size.

  uint32_t task_windows;        // Number of open windows.
  uint32_t task_priority;       // Priority for swap out.

  uint32_t task_eventtime;

  uint32_t task_messages;       // messages list / =-1 for all
  uint32_t task_messagessize;   // size of the list
};

enum {
  priority_iconbar  =     1<<0,   // 1
  priority_old      =     1<<1,   // 2
  priority_pollword =     1<<2,   // 4
  priority_idle     =     1<<3,   // 8
  priority_windows  =     1<<4,   // 16
  priority_null     =     1<<5,   // 32
  priority_top      =     1<<20
};

typedef struct wimp_window wimp_window;

typedef struct wimp_window_in_child_list wimp_window_in_child_list;

struct wimp_window_in_child_list {
  wimp_window_in_child_list *next;
  wimp_window_in_child_list *prev;
};

static uint32_t const window_tag = 0x646e6957;

struct wimp_window {
  uint32_t guardword; // 0x646e6957
  uint32_t taskhandle;
  wimp_window *next;
  wimp_window *prev;
  wimp_window_in_child_list *in_child_list;
  wimp_window_in_child_list *in_old_child_list;
  struct icon *icons;
};

// Wimp windows can be in multiple lists, which makes using
// doubly_linked_lists a bit messy.
// If this happens often, we should probably modify the structure

wimp_window_in_child_list *child( wimp_window *w )
{
  return &w->in_child_list;
}

wimp_window *child_wimp_window( wimp_window_in_child_list *c )
{
  return (void*) ((uint8_t*) c) - (uint8_t*) child( 0 );
}


void initptrs( struct workspace *ws ) // Wimp02, 2823-3056
{
  static uint32_t const nullptr = -1;
  static uint32_t const nullptr2 = -2;

  struct workspace init = {
    .freepool = nullptr2,
    .singletaskhandle = nullptr,
    .backwindow = nullptr,
    .commandhandle = nullptr,
    .redrawhandle = nullptr,
    .caretdata = nullptr,
    .ghostcaretdata = nullptr,
    .selectionwindow = nullptr,
    .menucaretwindow = nullptr,
    .pendingtask = nullptr,
    .border_iconselected = nullptr,
    .border_windowselected = nullptr,

    .dotdash1 = 0xfc,
    .dotdash2 = 0xf9
  };

  *ws = init;
  ws->taskSP = &ws->taskstack;
}

