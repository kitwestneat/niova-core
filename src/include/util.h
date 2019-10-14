/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */
#ifndef NIOVA_UTIL_H
#define NIOVA_UTIL_H 1

#include <stdio.h>

#include "common.h"
#include "log.h"

#define	DECL_AND_FMT_STRING(name, len, fmt, ...)                \
char name[len + 1];                                             \
{                                                               \
    int rc = snprintf(name, len, fmt, ##__VA_ARGS__);           \
    FATAL_IF((rc > len), "rc=%d, requested len=%u", rc, len);   \
}

#define niova_malloc malloc
#define niova_calloc calloc
#define niova_free   free

#define niova_unstable_clock(dest) clock_gettime(CLOCK_MONOTONIC, (dest))
#define niova_stable_clock(dest) clock_gettime(CLOCK_MONOTONIC_RAW, (dest))

/**
 * BSD timespec macros
 */
#ifndef timespecclear
#define timespecclear(tsp)		((tsp)->tv_sec = (tsp)->tv_nsec = 0)
#endif

#ifndef timespecisset
#define timespecisset(tsp)		((tsp)->tv_sec || (tsp)->tv_nsec)
#endif

#ifndef timespeccmp
#define timespeccmp(tsp, usp, cmp)					\
    (((tsp)->tv_sec == (usp)->tv_sec) ?                                 \
     ((tsp)->tv_nsec cmp (usp)->tv_nsec) :				\
     ((tsp)->tv_sec cmp (usp)->tv_sec))
#endif

#ifndef timespecadd
#define timespecadd(tsp, usp, vsp)					\
    do {								\
        (vsp)->tv_sec = (tsp)->tv_sec + (usp)->tv_sec;                  \
        (vsp)->tv_nsec = (tsp)->tv_nsec + (usp)->tv_nsec;               \
        if ((vsp)->tv_nsec >= 1000000000L) {                            \
            (vsp)->tv_sec++;                                            \
            (vsp)->tv_nsec -= 1000000000L;                              \
        }                                                               \
    } while (0)
#endif

#ifndef timespecsub
#define timespecsub(tsp, usp, vsp)					\
    do {								\
        (vsp)->tv_sec = (tsp)->tv_sec - (usp)->tv_sec;                  \
        (vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec;               \
        if ((vsp)->tv_nsec < 0) {                                       \
            (vsp)->tv_sec--;                                            \
            (vsp)->tv_nsec += 1000000000L;                              \
        }                                                               \
    } while (0)
#endif

#endif