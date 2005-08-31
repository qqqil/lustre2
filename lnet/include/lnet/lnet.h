/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
#ifndef __PORTALS_P30_H__
#define __PORTALS_P30_H__

#include "build_check.h"

/*
 * p30.h
 *
 * User application interface file
 */
#if defined(__linux__)
#include <lnet/linux/p30.h>
#elif defined(__APPLE__)
#include <lnet/darwin/p30.h>
#else
#error Unsupported Operating System
#endif

#include <lnet/types.h>
#include <lnet/api.h>

#endif
