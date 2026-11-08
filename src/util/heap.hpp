#ifndef UTIL_HEAP_HPP
#define UTIL_HEAP_HPP

#if defined(__UCLIBC__) || defined(__GLIBC__)
#include <malloc.h>
#endif

// Hand freed heap memory back to the kernel after a streaming client leaves.
// uClibc-ng malloc-standard (dlmalloc 2.7) only trims automatically when the top
// chunk exceeds M_TRIM_THRESHOLD, so after a burst RSS stays at its high-water
// mark; an explicit trim at disconnect time is cheap and bounded. musl returns
// memory on its own, so this is a no-op there.
inline void release_free_heap() {
#if defined(__UCLIBC__) || defined(__GLIBC__)
  malloc_trim(0);
#endif
}

#endif // UTIL_HEAP_HPP
