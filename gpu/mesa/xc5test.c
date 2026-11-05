/*
 * The OpenGL test of osmesa-test.c as a library, for the XCover5: the
 * kernel inserts /private/var/xc5/lib/libxc5test.dylib into backboardd
 * (boot-arg xc5gl=1, bsd/dev/xc5_drop.c), which needs no launchd job in the
 * root image. A thread of its own runs the test once (osmesa-test's main(),
 * which waits 20 s first) and leaves backboardd alone otherwise. Results go
 * to the kernel log through XC5GPU's Log selector (herodrm_log).
 */
#include <pthread.h>

#define main xc5_osmesa_test_main
#include "osmesa-test.c"
#undef main

static void *
xc5test_run(void *arg)
{
   (void)arg;
   xc5_osmesa_test_main();
   return NULL;
}

__attribute__((constructor)) static void
xc5test_start(void)
{
   pthread_t t;
   if (pthread_create(&t, NULL, xc5test_run, NULL) == 0)
      pthread_detach(t);
}
