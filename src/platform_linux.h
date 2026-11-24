#ifdef platform_h
#error "platform implementation already defined"
#else

#include <pthread.h>


typedef struct {
  pthread_t tid;
  int exit;
} plat_thread_t;

typedef void*(*plat_threadfunc_t)(void*);
#define PLAT_THREAD_FUNC(name,arg) void* name(void* arg)

#define PLATFORM_EXTRA_CR '\r'

// On the host, the grbl thread and the simulator thread share one (or few) real
// CPU cores. grbl's main loop and driver_delay_ms's spin loop run at full host
// speed, so without cooperation they starve the simulator thread and burn a
// whole core (process CPU ~= 2x wall). Yielding the host CPU here does NOT change
// simulation timing: the simulated tick at which every event fires is driven by
// the simulator thread's clock/ISRs, not by how often the grbl thread polls.
// It only lets the scheduler hand cycles to the simulator thread when the grbl
// thread would otherwise busy-spin.
#ifndef WIN32
#include <sched.h>
#define sim_yield() sched_yield()
#else
// SwitchToThread() hands the rest of the timeslice to another ready thread
// (the simulator thread) and returns immediately when none is waiting - the
// Windows equivalent of sched_yield(). windows.h comes in via platform.h.
#define sim_yield() SwitchToThread()
#endif


#endif
