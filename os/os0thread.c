/*****************************************************************************

Copyright (c) 1995, 2010, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/**************************************************//**
@file os/os0thread.c
The interface to the operating system thread control primitives

Created 9/8/1995 Heikki Tuuri
*******************************************************/

#include "os0thread.h"

#ifdef UNIV_NONINL
#include "os0thread.ic"
#endif

#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif /* HAVE_PTHREAD_H */

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif /* HAVE_SYS_SELECT_H */

#ifndef UNIV_HOTBACKUP

#include "srv0srv.h"
#include "os0sync.h"

/***************************************************************//**
Compares two thread ids for equality.
@return	TRUE if equal */
UNIV_INTERN
ibool
os_thread_eq(
/*=========*/
	os_thread_id_t	a,	/*!< in: OS thread or thread id */
	os_thread_id_t	b)	/*!< in: OS thread or thread id */
{
#ifdef __WIN__
	if (a == b) {
		return(TRUE);
	}

	return(FALSE);
#else
	if (pthread_equal(a, b)) {
		return(TRUE);
	}

	return(FALSE);
#endif
}

/****************************************************************//**
Converts an OS thread id to a ulint. It is NOT guaranteed that the ulint is
unique for the thread though!
@return	thread identifier as a number */
UNIV_INTERN
ulint
os_thread_pf(
/*=========*/
	os_thread_id_t	a)	/*!< in: OS thread identifier */
{
#ifdef UNIV_HPUX10
	/* In HP-UX-10.20 a pthread_t is a struct of 3 fields: field1, field2,
	field3. We do not know if field1 determines the thread uniquely. */

	return((ulint)(a.field1));
#else
	return((ulint)a);
#endif
}

/*****************************************************************//**
Returns the thread identifier of current thread. Currently the thread
identifier in Unix is the thread handle itself. Note that in HP-UX
pthread_t is a struct of 3 fields.
@return	current thread identifier */
UNIV_INTERN
os_thread_id_t
os_thread_get_curr_id(void)
/*=======================*/
{
#ifdef __WIN__
	return(GetCurrentThreadId());
#else
	return(pthread_self());
#endif
}

/****************************************************************//**
Creates a new thread of execution. The execution starts from
the function given. The start function takes a void* parameter
and returns an ulint.
@return	handle to the thread */
UNIV_INTERN
os_thread_t
os_thread_create(
/*=============*/
#ifndef __WIN__
	os_posix_f_t		start_f,
#else
	ulint (*start_f)(void*),		/*!< in: pointer to function
						from which to start */
#endif
	void*			arg,		/*!< in: argument to start
						function */
	os_thread_id_t*		thread_id)	/*!< out: id of the created
						thread, or NULL */
{
#ifdef __WIN__
	os_thread_t	thread;
	DWORD		win_thread_id;

	os_mutex_enter(os_sync_mutex);
	os_thread_count++;
	os_mutex_exit(os_sync_mutex);

	thread = CreateThread(NULL,	/* no security attributes */
			      0,	/* default size stack */
			      (LPTHREAD_START_ROUTINE)start_f,
			      arg,
			      0,	/* thread runs immediately */
			      &win_thread_id);

	if (srv_set_thread_priorities) {

		/* Set created thread priority the same as a normal query,
		we try to prevent starvation of threads by assigning same
		priority QUERY_PRIOR to all */

		ut_a(SetThreadPriority(thread, srv_query_thread_priority));
	}

	if (thread_id) {
		*thread_id = win_thread_id;
	}

	return(thread);
#else
	int		ret;
	os_thread_t	pthread;
	pthread_attr_t	attr;

#ifndef UNIV_HPUX10
	pthread_attr_init(&attr);
#endif

#ifdef UNIV_AIX
	/* We must make sure a thread stack is at least 32 kB, otherwise
	InnoDB might crash; we do not know if the default stack size on
	AIX is always big enough. An empirical test on AIX-4.3 suggested
	the size was 96 kB, though. */

	ret = pthread_attr_setstacksize(&attr,
					(size_t)(PTHREAD_STACK_MIN
						 + 32 * 1024));
	if (ret) {
		srv_panic(ret,
			"InnoDB: Error: pthread_attr_setstacksize"
			" returned %d\n", ret);
	}
#endif
#ifdef __NETWARE__
	ret = pthread_attr_setstacksize(&attr,
					(size_t) NW_THD_STACKSIZE);
	if (ret) {
		srv_panic(ret,
			"InnoDB: Error: pthread_attr_setstacksize"
			" returned %d\n", ret);
	}
#endif
	os_mutex_enter(os_sync_mutex);
	os_thread_count++;
	os_mutex_exit(os_sync_mutex);

#ifdef UNIV_HPUX10
	ret = pthread_create(&pthread, pthread_attr_default, start_f, arg);
#else
	ret = pthread_create(&pthread, &attr, start_f, arg);
#endif
	if (ret) {
		srv_panic(ret,
			"InnoDB: Error: pthread_create returned %d\n", ret);
	}

#ifndef UNIV_HPUX10
	pthread_attr_destroy(&attr);
#endif
	if (srv_set_thread_priorities) {

#ifdef HAVE_PTHREAD_SETPRIO 
		pthread_setprio(pthread, srv_query_thread_priority);
#endif
	}

	if (thread_id) {
		*thread_id = pthread;
	}

	return(pthread);
#endif
}

/*****************************************************************//**
Exits the current thread. */
UNIV_INTERN
void
os_thread_exit(
/*===========*/
	void*	exit_value)	/*!< in: exit value; in Windows this void*
				is cast as a DWORD */
{
#ifndef __WIN__
	int	ret;
#endif /* __WIN__ */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib_logger(ib_stream, "Thread exits, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	os_mutex_enter(os_sync_mutex);
	os_thread_count--;
	os_mutex_exit(os_sync_mutex);

#ifdef __WIN__
	ExitThread((DWORD)exit_value);
#else
	ret = pthread_detach(pthread_self());
	ut_a(ret == 0);

	pthread_exit(exit_value);
#endif /* __WIN__ */
}

/*****************************************************************//**
Returns handle to the current thread.
@return	current thread handle */
UNIV_INTERN
os_thread_t
os_thread_get_curr(void)
/*====================*/
{
#ifdef __WIN__
	return(GetCurrentThread());
#else
	return(pthread_self());
#endif
}

/*****************************************************************//**
Advises the os to give up remainder of the thread's time slice. */
UNIV_INTERN
void
os_thread_yield(void)
/*=================*/
{
#if defined(__WIN__)
	Sleep(0);
#elif (defined(HAVE_SCHED_YIELD) && defined(HAVE_SCHED_H))
	sched_yield();
#elif defined(HAVE_PTHREAD_YIELD_ZERO_ARG)
	pthread_yield();
#elif defined(HAVE_PTHREAD_YIELD_ONE_ARG)
	pthread_yield(0);
#else
	os_thread_sleep(0);
#endif
}
#endif /* !UNIV_HOTBACKUP */

/*****************************************************************//**
The thread sleeps at least the time given in microseconds. */
UNIV_INTERN
void
os_thread_sleep(
/*============*/
	ulint	tm)	/*!< in: time in microseconds */
{
#ifdef __WIN__
	Sleep((DWORD) tm / 1000);
#elif defined(__NETWARE__)
	delay(tm / 1000);
#else
	struct timeval	t;

	t.tv_sec = tm / 1000000;
	t.tv_usec = tm % 1000000;

	select(0, NULL, NULL, NULL, &t);
#endif
}

#ifndef UNIV_HOTBACKUP
/******************************************************************//**
Sets a thread priority. */
UNIV_INTERN
void
os_thread_set_priority(
/*===================*/
	os_thread_t	handle,	/*!< in: OS handle to the thread */
	ulint		pri)	/*!< in: priority */
{
#ifdef __WIN__
	int	os_pri;

	if (pri == OS_THREAD_PRIORITY_BACKGROUND) {
		os_pri = THREAD_PRIORITY_BELOW_NORMAL;
	} else if (pri == OS_THREAD_PRIORITY_NORMAL) {
		os_pri = THREAD_PRIORITY_NORMAL;
	} else if (pri == OS_THREAD_PRIORITY_ABOVE_NORMAL) {
		os_pri = THREAD_PRIORITY_HIGHEST;
	} else {
		ut_error;
	}

	ut_a(SetThreadPriority(handle, os_pri));
#else
	UT_NOT_USED(handle);
	UT_NOT_USED(pri);
#endif
}

/******************************************************************//**
Gets a thread priority.
@return	priority */
UNIV_INTERN
ulint
os_thread_get_priority(
/*===================*/
	os_thread_t	handle __attribute__((unused)))
				/*!< in: OS handle to the thread */
{
#ifdef __WIN__
	int	os_pri;
	ulint	pri;

	os_pri = GetThreadPriority(handle);

	if (os_pri == THREAD_PRIORITY_BELOW_NORMAL) {
		pri = OS_THREAD_PRIORITY_BACKGROUND;
	} else if (os_pri == THREAD_PRIORITY_NORMAL) {
		pri = OS_THREAD_PRIORITY_NORMAL;
	} else if (os_pri == THREAD_PRIORITY_HIGHEST) {
		pri = OS_THREAD_PRIORITY_ABOVE_NORMAL;
	} else {
		ut_error;
	}

	return(pri);
#else
	return(0);
#endif
}

/******************************************************************//**
Gets the last operating system error code for the calling thread.
@return	last error on Windows, 0 otherwise */
UNIV_INTERN
ulint
os_thread_get_last_error(void)
/*==========================*/
{
#ifdef __WIN__
	return(GetLastError());
#else
	return(0);
#endif
}
#endif /* !UNIV_HOTBACKUP */
