#include <stdio.h>
#include <string.h>
#include <assert.h>

#define true (0 == 0)
#define false (0 != 0)
#define bool uint32_t
typedef unsigned uint32_t;

typedef struct Node Node;
typedef __attribute__(( aligned( 4 ) )) union {
  struct {
    uint32_t index:31;
    uint32_t red:1;
  };
  uint32_t raw;
} Link;

static const Link LinkNull = { .index = 0, .red = 0 };
static const Link EmptyTree = { .index = 0, .red = 1 };

struct __attribute__(( aligned( 4 ) )) Node {
  Link left;
  Link right;
  char value;
};

Node nodes[100] = { 0 };
int last_allocated;

char walk[100] = { 0 };
int last_walk;

static inline Node *LinkPtr( Link l )
{
  return l.index == 0 ? 0 : &nodes[l.index];
}

static inline Link MkLink( Node *n, bool red )
{
  Link result = { .index = n - nodes, .red = red };
  return result;
}



static int max_depth( Node *root )
{
  if (root == 0) return -1;
  int left = max_depth( LinkPtr( root->left ) );
  int right = max_depth( LinkPtr( root->right ) );
  if (left > right) return left + 1;
  else return right + 1;
}

static int min_depth( Node *root )
{
  if (root == 0) return -1;
  int left = max_depth( LinkPtr( root->left ) );
  int right = max_depth( LinkPtr( root->right ) );
  if (left < right) return left + 1;
  else return right + 1;
}

static int black_max_depth( Node *root )
{
  if (root == 0) return -1;
  int left = max_depth( LinkPtr( root->left ) ) + 1 - root->left.red;
  int right = max_depth( LinkPtr( root->right ) ) + 1 - root->right.red;
  if (left > right) return left + 1;
  else return right + 1;
}

static int black_min_depth( Node *root )
{
  if (root == 0) return -1;
  int left = max_depth( LinkPtr( root->left ) ) + 1 - root->left.red;
  int right = max_depth( LinkPtr( root->right ) ) + 1 - root->right.red;
  if (left < right) return left + 1;
  else return right + 1;
}

void show_stats( Node *root )
{
  printf( "%d\t%d\t%d\t%d\n", min_depth( root ), max_depth( root ), black_min_depth( root ), black_max_depth( root ) );
}

void show_node( char c, int d, Node *n )
{
  for (int i = 0; i < d; i++) {
    printf( "  " );
  }
  printf( "%c", c );
  for (int i = d; i < 20; i++) {
    printf( "  " );
  }
  printf( "%c\n", n->value );
}

void show_subtree( char c, int d, Node *t )
{
  static const char L[2] = { '-', '=' };
  Node *left = LinkPtr( t->left );
  Node *right = LinkPtr( t->right );
  if (left != 0) show_subtree( L[t->left.red], d + 1, left );
  show_node( c, d, t );
  if (right != 0) show_subtree( L[t->right.red], d + 1, right );
}

void show_tree( Node *root )
{
  show_subtree( '*', 0, root );
}

Node *next_node( char c )
{
  nodes[last_allocated].left = LinkNull;
  nodes[last_allocated].right = LinkNull;
  nodes[last_allocated].value = c;

  return &nodes[last_allocated++];
}

Node *rbfind( Link *parent, char value )
{
  Node *root = LinkPtr( *parent );
  while (root != 0) {
    if (root->value == value) {
      break;
    }
    else if (value > root->value) {
      root = LinkPtr( root->right );
    }
    else {
      root = LinkPtr( root->left );
    }
  }
  return root;
}

void rbinsert( Link *parent, char value )
{
  assert( parent != 0 );

  Node *node = next_node( value );

  Node *root = LinkPtr( *parent );

  while (root != 0) {
    if (root->left.red == root->right.red) {
      // The owner of the parent link is either a 2 or 3-node,
      // with this side being the black link.

      // 2-node or 4-node. The latter must be split
      if (root->left.red && root->right.red) {
        // 4-node, split it, the middle key going to the parent,
        // black links to both children.
        parent->red = 1;
        root->left.red = 0;
        root->right.red = 0;
      }

      assert( !root->left.red );
      assert( !root->right.red );

      // 2-node
      if (value <= root->value) {
        parent = &root->left;
      }
      else {
        parent = &root->right;
      }
    }
    else if (root->left.red) {
      // assert( !parent->red ); The ultimate root link may become a red link

      // 3-node, root holds the right key
      if (value >= root->value) {
        // Easy.
        parent = &root->right;
      }
      else {
        Node *child = LinkPtr( root->left );
        assert( !child->right.red );
        assert( !child->left.red );

        if (value >= child->value) {
          // Middle value of 4-node
          if (LinkPtr( child->left ) == 0) {
            *parent = MkLink( node, 0 );

            node->right = MkLink( root, 1 );
            assert( root->left.raw == MkLink( child, 1 ).raw );
            node->left = root->left; // Red Link to child
            root->left = LinkNull;
            return;
          }
          else {
            parent = &child->right;
          }
        }
        else {
          *parent = MkLink( child, 0 );
          root->left = child->right;
          child->right = MkLink( root, 1 );
          parent = &child->left; // Middle link from 3-node
        }
      }
    }
    else if (root->right.red) {
      // assert( !parent->red ); The ultimate root link may become a red link

      // 3-node, root holds the left key
      if (value <= root->value) {
        // Easy.
        parent = &root->left;
      }
      else {
        Node *child = LinkPtr( root->right );
        assert( !child->left.red );
        assert( !child->right.red );

        if (value <= child->value) {
          if (LinkPtr( child->left ) == 0) {
            // Middle value of 4-node
            *parent = MkLink( node, 0 );

            node->left = MkLink( root, 1 );
            assert( root->right.raw == MkLink( child, 1 ).raw );
            node->right = root->right; // Red Link to child
            root->right = LinkNull;
            return;
          }
          else {
            parent = &child->left;
          }
        }
        else {
          *parent = MkLink( child, 0 );
          root->right = child->left;
          child->left = MkLink( root, 1 );
          parent = &child->right; // Middle link from 3-node
        }
      }
    }

    root = LinkPtr( *parent );
  }

  // Never being added to a 4-node, and the rotation ensures
  // this branch of a 3-node is black.

  *parent = MkLink( node, 1 - parent->red );
}

// Call func for each item, sorted lowest to highest.
// Returning false terminates the walk immediately.
bool walk_tree( Node *root, bool (*func)( Node * ) )
{
  if (root != 0) {
    if (!walk_tree( LinkPtr( root->left ), func )) return false;
    if (!func( root )) return false;
    if (!walk_tree( LinkPtr( root->right ), func )) return false;
  }
  return true;
}

// Depth-first scan of the tree; the nodes passed to
// func can be safely inserted into a different tree,
// as long as you forget about the original when this
// function returns.
void extract_items( Node *root, void (*func)( Node * ) )
{
  if (root != 0) {
    extract_items( LinkPtr( root->left ), func );
    extract_items( LinkPtr( root->right ), func );
    // No subtree any more
    func( root );
  }
}

void reset()
{
  last_allocated = 1;
  last_walk = 0;
}

void insert_string( Link *root, char const *str )
{
  while (*str != '\0') {
    printf( "Adding %c\n", *str );
    rbinsert( root, *str++ );
    show_tree( LinkPtr( *root ) ); printf( "\n\n" );
  }
}

bool next_char( Node *n )
{
  walk[last_walk++] = n->value;
  return true;
}

void make_string( Node *root )
{
  walk_tree( root, next_char );
}

bool test1()
{
  reset();
  Link tree = EmptyTree;
  rbinsert( &tree, 'A' );
  make_string( LinkPtr( tree ) );
  walk[last_walk++] = '\0';
  printf( "%s\t%d\t%s\n", __func__, max_depth( LinkPtr( tree ) ), walk );
  show_stats( LinkPtr( tree ) );
  show_tree( LinkPtr( tree ) );

  return strcmp( walk, "A" ) == 0;
}

bool test2()
{
  reset();
  Link tree = EmptyTree;
  insert_string( &tree, "ASE" );
  make_string( LinkPtr( tree ) );
  walk[last_walk++] = '\0';
  printf( "%s\t%d\t%s\n", __func__, max_depth( LinkPtr( tree ) ), walk );
  show_stats( LinkPtr( tree ) );
  show_tree( LinkPtr( tree ) );

  return strcmp( walk, "AES" ) == 0;
}

bool test3()
{
  reset();
  Link tree = EmptyTree;
  insert_string( &tree, "ASEARCHINGEXAMPLE" );
  make_string( LinkPtr( tree ) );
  walk[last_walk++] = '\0';
  printf( "%s\t%d\t%s\n", __func__, max_depth( LinkPtr( tree ) ), walk );
  show_stats( LinkPtr( tree ) );
  show_tree( LinkPtr( tree ) );

  return strcmp( walk, "AAACEEEGHILMNPRSX" ) == 0;
}

bool test4()
{
  reset();
  Link tree = EmptyTree;
  insert_string( &tree, "ASEARCHINGEXAMPLE" );
  make_string( LinkPtr( tree ) );
  walk[last_walk++] = '\0';
  printf( "%s\t%d\t%s\n", __func__, max_depth( LinkPtr( tree ) ), walk );
  show_stats( LinkPtr( tree ) );
  show_tree( LinkPtr( tree ) );
  Node *n = rbfind( &tree, 'I' );
  if (n == 0 || n->value != 'I') return false;
  n = rbfind( &tree, '!' );
  if (n != 0) return false;

  return strcmp( walk, "AAACEEEGHILMNPRSX" ) == 0;
}

int main()
{
  assert( sizeof( uint32_t ) == 4 );
  if (!test1()) return 1;
  if (!test2()) return 2;
  if (!test3()) return 3;
  if (!test4()) return 4;
  return 0;
}

