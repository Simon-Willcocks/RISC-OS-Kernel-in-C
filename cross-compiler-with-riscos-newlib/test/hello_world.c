
#include <stdio.h>
#include <unistd.h>

int main()
{
  write( 1, "Hello world\n", 12 );
  return 0;
}
