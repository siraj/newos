/*
** Copyright 2003-2004, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#ifndef _TESTAPP_TESTS_H
#define _TESTAPP_TESTS_H

// thread tests
int sleep_test(int arg);
int thread_spawn_test(int arg);
int syscall_bench(int arg);
int sig_test(int arg);
int fpu_test(int arg);

#endif

