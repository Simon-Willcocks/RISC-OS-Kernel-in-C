#include <stdlib.h>
#include <stdio.h>

// define this in the compiler options to test with and without assertions
#ifdef DLL_VERIFICATION
#include <assert.h>
#warning Assertions turned ON
#else
#warning Assertions turned off, define DLL_VERIFICATION to turn on
#endif

#include "include/doubly_linked_list.h"

typedef struct item item;

struct item {
  item *prev;
  item *next;
  char const *string;
};

void show_list( char const *heading, item *list )
{
  printf( "%s\n", heading );
  if (list == 0) {
    printf( "Empty list\n" );
  }
  else {
    item *i = list;
    do {
      printf( "Item %p (%p, %p): \"%s\"\n", i, i->prev, i->next, i->string );
      i = i->next;
    } while (i != list);
  }

  printf( "Backwards:\n" );
  if (list == 0) {
    printf( "Empty list\n" );
  }
  else {
    item *i = list;
    do {
      i = i->prev;
      printf( "Item %p (%p, %p): \"%s\"\n", i, i->prev, i->next, i->string );
    } while (i != list);
  }
  printf( "\n" );
}

dll_type( item );

item *new_item( char const *s )
{
  item *i = malloc( sizeof( item ) );
  dll_new_item( i );
  i->string = s;
  return i;
}

int main( int argc, char const *argv[] )
{
  item *list = 0;
  for (int a = 1; a < argc; a++) {
    dll_attach_item( new_item( argv[a] ), &list );
  }
  show_list( "Initial list", list );

  item *first_item_detatched = list;
  item *list2 = list->next;
  dll_detatch_item( list );
  show_list( "Detatched head", list );
  show_list( "The rest", list2 );
  list = list2;
  char const *s = list->next->next->string;
  item *to_detatch = list->next->next;
  dll_detatch_item( list->next->next );
  show_list( s, list );
  item *tmp = list->next;
  show_list( "Tail", tmp );
  dll_attach_item( to_detatch, &tmp );
  dll_assert( tmp == to_detatch );
  show_list( "Reattatched at head", tmp );
  show_list( "Reattatched after head", list );
  dll_replace_item( list->next, first_item_detatched, &list );
  show_list( "Replaced list->next", list );


  {
    // single item list
    item *i1 = malloc( sizeof( item ) );
    dll_new_item( i1 );
    i1->string = "1";
    item *i2 = malloc( sizeof( item ) );
    dll_new_item( i2 );
    i2->string = "2";
    show_list( "item 1", i1 );
    show_list( "item 2", i2 );

    item *list = i1;
    show_list( "list", list );
    dll_replace_item( i1, i2, &list );
    show_list( "item 1", i1 );
    show_list( "item 2", i2 );
    show_list( "list", list );
  }

  {
    item *list = 0;
    item *last;
    dll_attach_item( new_item( "FF" ), &list );
    dll_attach_item( new_item( "EE" ), &list );
    dll_attach_item( new_item( "DD" ), &list );
    dll_attach_item( last = new_item( "CC" ), &list );
    dll_attach_item( new_item( "BB" ), &list );
    dll_attach_item( new_item( "AA" ), &list );
    show_list( "Initial list", list );
    item *extracted = list;
    dll_detatch_items_until( &list, last );
    show_list( "Remaining list, starts with DD", list );
    show_list( "Extracted list, starts with AA", extracted );

    item *other_list = 0;
    dll_insert_item_list_at_head( extracted, &other_list );
    show_list( "Inserted into empty list", other_list );

    dll_insert_item_list_at_head( extracted, &list );
    show_list( "Restored list", list );
  }

  return 0;
}
