Per-CPU Calclock
========================================
A simple profiling tool which utilizes per-CPU variables to minimize interference caused by nested profiling.

## Usage
### At the location where you want to profile:

```c
#include "calclock.h"

KTDEF(some_time_consuming_function);

int foo(void)
{
	ktime_t stopwatch[2];
	...
	ktget(&stopwatch[0]);
	value = some_time_consuming_function();
	ktget(&stopwatch[1]);
	...
	ktput(stopwatch, some_time_consuming_function);
	return output;
}
```
### At the location where you want to combine and see the results:

```c
#include "calclock.h"

KTDEC(some_time_consuming_function);

void exit_module(void)
{
	...
	ktprint(1, some_time_consuming_function);
}
```
### To enable calclock, define `CONFIG_CALCLOCK` when including calclock.h:

```c
#ifndef __CALCLOCK_H
#define __CALCLOCK_H
#include <linux/ktime.h>
#include <linux/percpu.h>

#define CONFIG_CALCLOCK

struct calclock {
	ktime_t time;
	unsigned long long count;
};
...
#else /* !CONFIG_CALCLOCK */
#define ktget(clock)
#define ktput(localclock, funcname)
#define ktprint(depth, funcname)
#endif /* CONFIG_CALCLOCK */

#endif /* __CALCLOCK_H */
```

## DeLFS 7.2.8 module
The measurement points of 6.6.8 DeLFS are kept with the same names, so the
dmesg output can be compared with 6.6.8 runs. They are compiled out unless the
module is built with `make CALCLOCK=1`. The results are printed when the module
is unloaded (`rmmod delfs`).

