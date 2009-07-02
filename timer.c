/*
    Copyright (C) 2009 Mans Rullgard

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use, copy,
    modify, merge, publish, distribute, sublicense, and/or sell copies
    of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
 */

#include <time.h>
#include <pthread.h>
#include <semaphore.h>

#include "timer.h"

static sem_t sleep_sem;
static struct timespec tstart;

int
timer_init(void)
{
    return sem_init(&sleep_sem, 0, 0);
}

int
timer_start(struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, &tstart);
    *ts = tstart;
    return 0;
}

int
timer_read(struct timespec *ts)
{
    return clock_gettime(CLOCK_REALTIME, ts);
}

int
timer_wait(struct timespec *ts)
{
    return sem_timedwait(&sleep_sem, ts);
}

int
timer_close(void)
{
    sem_destroy(&sleep_sem);
    return 0;
}
