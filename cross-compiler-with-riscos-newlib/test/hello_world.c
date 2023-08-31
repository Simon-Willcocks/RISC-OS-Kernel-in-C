
#include <stdio.h>

int main()
{
  FILE *f = fopen( "/rofs/ram/RamDisc0/Directory/File1", "w" );
  if (f != 0) {
    fprintf( f, "Hello world %d\n", 12345 );
    fclose( f );
  }
  return 0;
}
