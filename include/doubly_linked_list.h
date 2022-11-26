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
 *
 */

// For any struct with next and prev fields that are pointers to that type
// of structure.

#ifdef DLL_VERIFICATION
#define dll_assert( x ) assert( x )
#else
#define dll_assert( x )
#endif

#define dll_type( T ) \
/* Initialise the item as a list of one item. */ \
static inline void dll_new_##T( T *i ) { \
  i->next = i; \
  i->prev = i; \
} \
 \
/* Attach the item as the head of the list. (If you want it at the tail, */ \
/* follow up with `l = l->next;`, if you want it after the head, declare */ \
/* a list `T *tmp = l->next;`, then attach it to that list. (Remember to */ \
/* check for an empty list!) */ \
static inline void dll_attach_##T( T *i, T **l ) { \
  dll_assert( i->next == i && i->prev == i ); \
  T *head = *l; \
  (*l) = i; \
  if (head != 0) { \
    i->next = head; \
    i->prev = head->prev; \
    i->prev->next = i; \
    head->prev = i; \
  } \
} \
 \
/* Detatch the item from any list it is in (if it is the head of a */ \
/* list, it will effectively detatch the rest of the list instead!) */ \
static inline void dll_detatch_##T( T *i ) { \
  dll_assert( i->prev->next == i ); \
  dll_assert( i->next->prev == i ); \
  i->prev->next = i->next; \
  i->next->prev = i->prev; \
  i->next = i; \
  i->prev = i; \
  dll_assert( i->next == i && i->prev == i ); \
} \
 \
/* Move the item from list 1 to list 2 (which should be pointers \
   to the head of the list). */ \
static inline void dll_move_##T( T *i, T **l1, T **l2 ) { \
  if ((*l1) == i) { \
    (*l1) = i->next; \
    if ((*l1) == i) { \
      (*l1) = 0; \
    } \
  } \
  if ((*l2) == 0) (*l2) = i; \
  i->prev->next = i->next; \
  i->next->prev = i->prev; \
  i->next = (*l2); \
  i->prev = (*l2)->prev; \
  (*l2)->prev = i; \
  (*l2)->prev->next = i; \
} \
 \
/* Replace item 1 with item 2 in whatever list it may be in. */ \
/* It will not update the pointer to the head of the list */ \
static inline void dll_replace_##T( T *i1, T *i2, T **l ) { \
  dll_assert( i1 != i2 ); \
  dll_assert( i2->next == i2 ); \
  dll_assert( i2->prev == i2 ); \
  if (i1->next == i1) { \
    /* Only item in the list */ \
    dll_assert( *l == i1 ); \
    *l = i2; \
  } \
  else { \
    i2->prev = i1->prev; \
    i2->next = i1->next; \
    i2->prev->next = i2; \
    i2->next->prev = i2; \
    i1->prev = i1; \
    i1->next = i1; \
    if (*l == i1) *l = i2; \
  } \
}

