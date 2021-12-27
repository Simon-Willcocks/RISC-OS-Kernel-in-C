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

static void new_absolute_point( svc_registers *regs )
{
  workspace.vdu.plot_points[2] = workspace.vdu.plot_points[1];
  workspace.vdu.plot_points[1] = workspace.vdu.plot_points[0];
  workspace.vdu.plot_points[0].x = (int32_t) regs->r[1];
  workspace.vdu.plot_points[0].y = (int32_t) regs->r[2];
}

static void new_relative_point( svc_registers *regs )
{
  workspace.vdu.plot_points[2] = workspace.vdu.plot_points[1];
  workspace.vdu.plot_points[1] = workspace.vdu.plot_points[0];
  workspace.vdu.plot_points[0].x += (int32_t) regs->r[1];
  workspace.vdu.plot_points[0].y += (int32_t) regs->r[2];
}

typedef void (*plotter)( uint32_t left, uint32_t right, uint32_t y );

static void fg_plotter( uint32_t left, uint32_t right, uint32_t y )
{
}

static void bg_plotter( uint32_t left, uint32_t right, uint32_t y )
{
}

static void invert_plotter( uint32_t left, uint32_t right, uint32_t y )
{
}

static void solid_line( plotter plot )
{
}

bool do_OS_Plot( svc_registers *regs )
{
asm ( "bkpt 4" );
  if (0 == (regs->r[0] & 4))
    new_relative_point( regs );
  else
    new_absolute_point( regs );

  plotter plot;
  switch (regs->r[0] & 3) {
  case 0: return true; // Moved, nothing to plot
  case 1: plot = fg_plotter; break;
  case 2: plot = invert_plotter; break;
  case 3: plot = bg_plotter; break;
  }
  switch (regs->r[0] / 8) {
  case 0: solid_line( plot ); break;
  }
  return true;
}
