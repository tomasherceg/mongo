/*-
 * Public Domain 2014-2015 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Based on "Spinlocks and Read-Write Locks" by Dr. Steven Fuerst:
 *	http://locklessinc.com/articles/locks/
 *
 * Dr. Fuerst further credits:
 *	There exists a form of the ticket lock that is designed for read-write
 * locks. An example written in assembly was posted to the Linux kernel mailing
 * list in 2002 by David Howells from RedHat. This was a highly optimized
 * version of a read-write ticket lock developed at IBM in the early 90's by
 * Joseph Seigh. Note that a similar (but not identical) algorithm was published
 * by John Mellor-Crummey and Michael Scott in their landmark paper "Scalable
 * Reader-Writer Synchronization for Shared-Memory Multiprocessors".
 *
 * The following is an explanation of this code. First, the underlying lock
 * structure.
 *
 *	struct {
 *		uint16_t writers;	Now serving for writers
 *		uint16_t readers;	Now serving for readers
 *		uint16_t users;		Next available ticket number
 *		uint16_t overflow;	Overflow from users
 *	} s;
 *
 * First, imagine a store's 'take a number' ticket algorithm. A customer takes
 * a unique ticket number and customers are served in ticket order. In the data
 * structure, 'writers' is the next writer to be served, 'readers' is the next
 * reader to be served, and 'users' is the next available ticket number.
 *
 * Next, consider exclusive (write) locks. The 'now serving' number for writers
 * is 'writers'. To lock, 'take a number' and wait until that number is being
 * served; more specifically, atomically copy the current value of 'users' and
 * increment it, and then wait until 'writers' equals that copied number.
 *
 * Shared (read) locks are similar. Like writers, readers atomically get the
 * next number available. However, instead of waiting for 'writers' to equal
 * their number, they wait for 'readers' to equal their number.
 *
 * This has the effect of queueing lock requests in the order they arrive
 * (incidentally avoiding starvation).
 *
 * When readers unlock, they increment the value of 'writers'; when 'writers'
 * unlock, they increment the value of 'readers' and 'writers'. Specifically,
 * each lock/unlock pair requires incrementing both 'readers' and 'writers':
 * in the case of a reader, the 'readers' increment happens immediately and the
 * 'writers' increment happens when the reader unlocks. In the case of a writer,
 * the 'readers' and 'writers' increment both happen when the writer unlocks.
 *
 * For example, consider the following read and write lock requests (R, W):
 *
 *						writers	readers	users
 *						0	0	0
 *	R: ticket 0, readers match	OK	0	1	1
 *	R: ticket 1, readers match	OK	0	2	2
 *	R: ticket 2, readers match	OK	0	3	3
 *	W: ticket 3, writers no match	block	0	3	4
 *	R: ticket 2, unlock			1	3	4
 *	R: ticket 0, unlock			2	3	4
 *	R: ticket 1, unlock			3	3	4
 *	W: ticket 3, writers match	OK
 *
 * Note the writer blocks until 'writers' equals its ticket number and it does
 * not matter if readers unlock in order or not.
 *
 * Readers or writers entering the system after the write lock is queued block,
 * and the next ticket holder (reader or writer) will unblock when the writer
 * unlocks.
 *						writers	readers	users
 *	...
 *	W: ticket 3, writers no match	block	0	3	4
 *	R: ticket 0, unlock			1	3	4
 *	R: ticket 2, unlock			2	3	4
 *	R: ticket 1, unlock			3	3	4
 *	R: ticket 4, readers no match	block	3	3	5
 *	W: ticket 5, writers no match	block	3	3	6
 *	W: ticket 3, writers match	OK
 *	R: ticket 6, readers no match	block	3	3	7
 *	W: ticket 7, writers no match	block	3	3	8
 *	W: ticket 3, unlock			4	4	8
 *	R: ticket 4, readers match	OK	4	5	8
 *
 * The 'users' field is a 2B value so the available ticket number wraps at 64K
 * requests. If a thread's lock request is not granted until the 'users' field
 * cycles and the same ticket is taken by another thread, we could grant a lock
 * to two separate threads at the same time, and bad things happen: two writer
 * threads, or a reader thread and a writer thread, would run in parallel, and
 * lock waiters could be skipped if the unlocks race. This is unlikely, it only
 * happens if a lock request is blocked by 64K other requests. The fix would be
 * to grow the lock structure fields, but the largest atomic instruction we have
 * is 8B, the structure has no room to grow.
 */

#include "wt_internal.h"

/*
 * __wt_rwlock_alloc --
 *	Allocate and initialize a read/write lock.
 */
int
__wt_rwlock_alloc(
    WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp, const char *name)
{
	WT_RWLOCK *rwlock;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: alloc %s", name));

	WT_RET(__wt_calloc_one(session, &rwlock));

	rwlock->name = name;

	*rwlockp = rwlock;
	return (0);
}

/*
 * __wt_try_readlock --
 *	Try to get a shared lock, fail immediately if unavailable.
 */
int
__wt_try_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t new, old, overflow, users, writers;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: try_readlock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	l = &rwlock->rwlock;

	/*
	 * Note the overflow field: we are updating the entire 8B of the lock
	 * atomically, so we have handle the eventual overflow of the 'users'
	 * field as part of our calculation.
	 */
	writers = l->s.writers;
	users = l->s.users;
	overflow = l->s.overflow;

	/*
	 * The old and new values for the lock.
	 *
	 * We use "users" twice in the calculation of the old value: this read
	 * lock can only be granted if the lock was last granted to a reader and
	 * there are no writers blocked on the lock, that is, if the ticket for
	 * this thread would be the next ticket granted. This might not be the
	 * lock's current value, rather it's the value the lock must have if we
	 * are to grant this read lock.
	 *
	 * Note the masking of "users + 1" in the calculation of the new value:
	 * we want to set the readers field to the next readers value, not the
	 * next users value, so it has to wrap, not overflow.
	 */
	old = (overflow << 48) + (users << 32) + (users << 16) + writers;
	new = (overflow << 48) +
	    ((users + 1) << 32) + (((users + 1) & 0xffff) << 16) + writers;
	return (WT_ATOMIC_CAS8(l->u, old, new) ? 0 : EBUSY);
}

/*
 * __wt_readlock --
 *	Get a shared lock.
 */
int
__wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t me;
	uint16_t val;
	int pause_cnt;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: readlock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	l = &rwlock->rwlock;

	/*
	 * Possibly wrap: if we have more than 64K lockers waiting, the ticket
	 * value will wrap and two lockers will simultaneously be granted the
	 * lock.
	 */
	me = WT_ATOMIC_FETCH_ADD8(l->u, (uint64_t)1 << 32);
	val = (uint16_t)(me >> 32);
	for (pause_cnt = 0; val != l->s.readers;) {
		/*
		 * We failed to get the lock; pause before retrying and if we've
		 * paused enough, sleep so we don't burn CPU to no purpose. This
		 * situation happens if there are more threads than cores in the
		 * system and we're thrashing on shared resources. Regardless,
		 * don't sleep long, all we need is to schedule the other reader
		 * threads to complete a few more instructions and increment the
		 * reader count.
		 */
		if (++pause_cnt < 1000)
			WT_PAUSE();
		else
			__wt_sleep(0, 10);
	}

	++l->s.readers;

	return (0);
}

/*
 * __wt_readunlock --
 *	Release a shared lock.
 */
int
__wt_readunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: read unlock %s", rwlock->name));

	l = &rwlock->rwlock;
	WT_ATOMIC_ADD2(l->s.writers, 1);

	return (0);
}

/*
 * __wt_try_writelock --
 *	Try to get an exclusive lock, fail immediately if unavailable.
 */
int
__wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t new, old, overflow, readers, users;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: try_writelock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	l = &rwlock->rwlock;

	/*
	 * Note the overflow field: we are updating the entire 8B of the lock
	 * atomically, so we have handle the eventual overflow of the 'users'
	 * field as part of our calculation.
	 */
	readers = l->s.readers;
	users = l->s.users;
	overflow = l->s.overflow;

	/*
	 * The old and new values for the lock.
	 *
	 * We use "users" twice in the calculation of the old value: this write
	 * lock can only be granted if the lock was last granted to a writer and
	 * there are no readers or writers blocked on the lock, that is, if the
	 * ticket for this thread would be the next ticket granted. This might
	 * not be the lock's current value, rather it's the value the lock must
	 * have if we are to grant this write lock.
	 */
	old = (overflow << 48) + (users << 32) + (readers << 16) + users;
	new = (overflow << 48) + ((users + 1) << 32) + (readers << 16) + users;
	return (WT_ATOMIC_CAS8(l->u, old, new) ? 0 : EBUSY);
}

/*
 * __wt_writelock --
 *	Wait to get an exclusive lock.
 */
int
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint64_t me;
	uint16_t val;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: writelock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	l = &rwlock->rwlock;

	/*
	 * Possibly wrap: if we have more than 64K lockers waiting, the ticket
	 * value will wrap and two lockers will simultaneously be granted the
	 * lock.
	 */
	me = WT_ATOMIC_FETCH_ADD8(l->u, (uint64_t)1 << 32);
	val = (uint16_t)(me >> 32);
	while (val != l->s.writers)
		WT_PAUSE();

	return (0);
}

/*
 * __wt_writeunlock --
 *	Release an exclusive lock.
 */
int
__wt_writeunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l, copy;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: writeunlock %s", rwlock->name));

	l = &rwlock->rwlock;

	copy = *l;

	WT_BARRIER();

	/*
	 * The try-lock functions are reading the lock fields separately as they
	 * create old/new values, we need to update atomically to avoid races.
	 */
	++copy.s.writers;
	++copy.s.readers;

	l->i.us = copy.i.us;

	return (0);
}

/*
 * __wt_rwlock_destroy --
 *	Destroy a read/write lock.
 */
int
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp)
{
	WT_RWLOCK *rwlock;

	rwlock = *rwlockp;		/* Clear our caller's reference. */
	if (rwlock == NULL)
		return (0);
	*rwlockp = NULL;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: destroy %s", rwlock->name));

	__wt_free(session, rwlock);
	return (0);
}
