#include "memory.h"

unsigned char siheap[SINTER_HEAP_SIZE] = { 0 };

struct sientry sistack[SINTER_STACK_ENTRIES] = { 0 };

struct sientry *sistack_bottom = sistack;
struct sientry *sistack_limit = sistack;
struct sientry *sistack_top = sistack;
