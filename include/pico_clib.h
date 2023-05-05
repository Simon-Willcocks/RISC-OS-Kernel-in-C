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

// pico_clib: really, really, the minimum!

static inline int strlen( const char *string )
{
  int result = 0;
  while (*string++ != '\0') result++;
  return result;
}

static inline int strcmp( const char *left, const char *right )
{
  int result = 0;
  while (result == 0) {
    char l = *left++;
    char r = *right++;
    result = l - r;
    if (l == 0 || r == 0) break;
  }
  return result;
}

static inline char *strcpy( char *dest, const char *src )
{
  char *result = dest;
  while (*src != '\0') {
    *dest++ = *src++;
  }
  *dest = *src;
  return result;
}

// Non-conforming implementation, it won't fill dest with nul
// characters unnecessarily.
static inline char *strncpy( char *dest, const char *src, int len )
{
  char *result = dest;
  while (*src != '\0' && --len > 0) {
    *dest++ = *src++;
  }
  *dest = *src;
  return result;
}

