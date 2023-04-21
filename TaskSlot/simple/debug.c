/* Copyright 2021-2023 Simon Willcocks
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

/* Handling the kernel debug pipes. */

#include "common.h"

static char *pipe_space( int len )
{
  uint32_t written;

  // Allocate space in the pipe for our string, allowing for being interrupted
  // This can still get screwed up, if the receiver gets scheduled. Can that happen?
  do {
    written = workspace.kernel.debug_written;
    if (written + len > workspace.kernel.debug_space.available) {
      //if (workspace.kernel.frame_buffer_initialised) asm ( "bkpt 2" : : "r" (workspace.kernel.debug_written), "r" (len), "r" (written) );
      return 0; // No space.
    }
  } while (written != change_word_if_equal( &workspace.kernel.debug_written, written, written+len ));

  assert( written < 0x2000 );

  return workspace.kernel.debug_space.location + written;
}

void SVCWriteN( char const *s, int len )
{
  os_pipe *pipe = (os_pipe*) workspace.kernel.debug_pipe;

  if (pipe == 0 || this_is_debug_receiver()) return; // Too early, or receiver is in a SWI

  char *location = pipe_space( len );

  if (location != 0) {
    for (int i = 0; i < len; i++) { location[i] = s[i]; };
  }
}

void SVCWrite0( char const *s )
{
  if (s == 0) s = "<NULL>";
  int len = 0;
  for (len = 0; (s[len] != '\0' && s[len] != '\n' && s[len] != '\r'); len++) {};
  WriteN( s, len );
}

void SVCWriteNum( uint32_t n )
{
  os_pipe *pipe = (os_pipe*) workspace.kernel.debug_pipe;

  if (pipe == 0 || this_is_debug_receiver()) return; // Too early, or receiver is in a SWI

  char *location = pipe_space( 8 );

  if (location != 0) {
    for (int i = 7; i >= 0; i--) {
      location[i] = hex[n & 0xf]; n = n >> 4;
    }
  }
}

