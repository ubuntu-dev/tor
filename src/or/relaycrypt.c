/* Copyright (c) 2012, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file relay.c
 * \brief Handle relay cell encryption in worker threads and related
 * job dispatching and signaling.
 **/

#include "or.h"
#include "relaycrypt.h"

#ifdef TOR_USES_THREADED_RELAYCRYPT

/* #error Threaded relaycrypt is not finished yet; turn it off. */

/*
 * Several of these structures have mutexes; observe these rules to avoid
 * deadlock:
 *
 * 1.) Never hold the mutexes for two relaycrypt_job_t or relaycrypt_thread_t
 *     structures simultaneously.
 *
 * 2.) If you hold more than one mutex for different types of structure at
 *     once, acquire them in this order:
 *
 *     [relaycrypt_dispatcher_t], relaycrypt_thread_t, relaycrypt_job_t
 *
 *     where [relaycrypt_dispatcher_t] could be jobs_lock, jobs_lock
 *     then threads_lock, or threads_lock, but not threads_lock then
 *     jobs_lock.
 */

/*
 * This is the master data structure tracking threaded relaycrypt status;
 * only one should exist per Tor process, and it gets created in
 * relaycrypt_init() and freed in relaycrypt_free_all().  It has two
 * lists of tracked objects: a list of active worker thread structures
 * of type relaycrypt_thread_t and a list of jobs of type
 * relaycrypt_job_t.
 */

struct relaycrypt_dispatcher_s {
  /*
   * Lock this for access to the threads list
   */
  tor_mutex_t *threads_lock;
  /*
   * How many worker threads do we want to have?  Use this in
   * relaycrypt_set_num_workers() to figure out how many to start
   * or stop.
   */
  int num_workers_wanted;
  /*
   * List of relaycrypt_thread_t instances; no lock needed since these
   * are always added and removed in the main thread.
   */
  smartlist_t *threads;

  /*
   * Lock this for access to the jobs list
   */
  tor_mutex_t *jobs_lock;
  /*
   * List of relaycrypt_job_t instances; jobs are added and may have their
   * status changed by the main thread if it tries to queue a cell to a
   * (circuit, direction) tuple which does not already have one, and
   * may have their status modified by a worker thread, or be removed
   * if the worker thread finishes the job and it has been marked dead
   * (circuit closed) by the main thread while the worker held it.
   * Main or worker threads should hold jobs_lock for access to this.
   * If locking both jobs_lock and the per-job lock in relaycrypt_job_t,
   * lock this one first so we know we can't deadlock.
   */
  smartlist_t *jobs;
};

/*
 * State of a relaycrypt job and cell queues
 */

struct relaycrypt_job_s {
  /* Mutex for state changes and queue access */
  tor_mutex_t *job_lock;

  /*
   * Circuit this job is for *outgoing* cells on, or NULL if the circuit
   * has been closed and this job should go away.  This should be constant
   * for the lifetime of the job except that the main thread may change it
   * to NULL once if a circuit dies; since writing a pointer should be atomic
   * on reasonable machines it'll be safe to check without locking if the
   * workers want to poll this to see if they should give up and exit early.
   */
  circuit_t *circ;
  /*
   * Direction on circ this job crypts
   */
  cell_direction_t dir;
  /*
   * State of this job object:
   *
   * RELAYCRYPT_JOB_IDLE:
   *
   *   No cells are queued to be crypted, but the job object sticks around
   *   for when some next show up and to hold any crypted cells the main
   *   thread hasn't seen yet.  The worker field should be NULL and the input
   *   queue should be empty.
   *
   * RELAYCRYPT_JOB_READY:
   *
   *   Cells are available on the input queue and this job is eligible for
   *   dispatch, but hasn't been dispatched yet.  The worker field should be
   *   NULL and the input queue should be non-empty.
   *
   * RELAYCRYPT_JOB_RUNNING:
   *
   *   A worker is processing cells on this job; the worker field should point
   *   to it.
   *
   * RELAYCRYPT_JOB_DEAD:
   *
   *   A worker finished this and found the circuit field had been set to
   *   NULL, indicating a dead circuit.  It should be freed at some point.
   *   The worker field should be NULL.
   */
  enum {
    RELAYCRYPT_JOB_IDLE,
    RELAYCRYPT_JOB_READY,
    RELAYCRYPT_JOB_RUNNING,
    RELAYCRYPT_JOB_DEAD
  } state;
  /* If this is in RELAYCRYPT_JOB_RUNNING, what worker has it? */
  relaycrypt_thread_t *worker;
  /*
   * TODO cell queues - simple smartlist at first, maybe, but we probably
   * want an efficient way for the worker thread to snarf a whole block of
   * cells at once from the input without having to repeatedly lock/unlock
   * or hold a lock that the main thread might want while doing CPU-intensive
   * crypto ops.
   */
};

/*
 * State of a relaycrypt worker
 */

struct relaycrypt_thread_s {
  /*
   * Lock this for worker state access
   */
  tor_mutex_t *thread_lock;

  /*
   * State of this worker:
   *
   * RELAYCRYPT_WORKER_STARTING:
   *
   *   The worker was just created and hasn't set its state to IDLE yet;
   *   the job field should be NULL.
   *
   * RELAYCRYPT_WORKER_IDLE:
   *
   *   The worker is waiting to be dispatched; the job field should be NULL
   *
   * RELAYCRYPT_WORKER_WORKING:
   *
   *   The worker is working; the job field should be the relaycrypt_job_t
   *   it is working on.
   *
   * RELAYCRYPT_WORKER_DEAD:
   *
   *   The worker has been told to exit and either has or is about to; the
   *   main thread should join and clean up dead workers at some point.
   */
  enum {
    RELAYCRYPT_WORKER_STARTING,
    RELAYCRYPT_WORKER_IDLE,
    RELAYCRYPT_WORKER_WORKING,
    RELAYCRYPT_WORKER_DEAD
  } state;
  /*
   * Flag to indicate the worker should be told to exit next time it asks
   * for more work; this is initially 0 and may be set to 1 once by the main
   * thread.
   */
  unsigned int exit_flag:1;
  /*
   * Job the worker is currently working on, if in RELAYCRYPT_WORKER_WORKING
   */
  relaycrypt_job_t *working_on;
  /*
   * The thread for this worker
   */
  tor_thread_t *thread;
};

/*
 * Static relaycrypt function declarations and descriptive comments
 *
 * Main thread functions:
 */

/**
 * Join all workers in the RELAYCRYPT_WORKER_DEAD state or, if the block
 * flag is true, also with the exit_flag set, and when they have exited
 * remove them from the worker list.
 */

static void relaycrypt_join_workers(int block);

/*
 * Worker thread functions:
 */

/**
 * Main loop for relaycrypt worker threads; takes the thread structure
 * as an argument and returns when the thread exits.
 */

static void relaycrypt_worker_main(relaycrypt_thread_t *thr);

/**
 * Get a relaycrypt_job_t for this thread to work on, or block until one is
 * available.  This returns NULL to signal that this worker should exit.
 */

static relaycrypt_job_t * relaycrypt_worker_get_job(relaycrypt_thread_t *thr);

/**
 * Release a relaycrypt job and become idle from a worker thread
 */

static void
relaycrypt_worker_release_job(relaycrypt_thread_t *thr,
                              relaycrypt_job_t *job);

/*
 * Global variables
 */

static relaycrypt_dispatcher_t *rc_dispatch = NULL;

/*
 * Function implementations (main thread functions)
 */

/**
 * Call this at startup to initialize relaycrypt; note that this does not
 * start any worker threads, so you should use relaycrypt_set_num_workers()
 * after this.
 */

void
relaycrypt_init(void)
{
  tor_assert(!rc_dispatch);

  rc_dispatch = tor_malloc_zero(sizeof(*rc_dispatch));

  /*
   * We do not create any threads here - that happens in
   * relaycrypt_set_num_workers() later on.
   */
}

/**
 * Call this to shut down all active workers, join them and then free
 * all relaycrypt data.
 */

void
relaycrypt_free_all(void)
{
  if (rc_dispatch) {
    /* First, tell all active workers to shut down */
    relaycrypt_set_num_workers(0);
    /* Wait for them to exit and join them */
    relaycrypt_join_workers(1);
    /* TODO free job/worker lists */
    tor_free(rc_dispatch);
    rc_dispatch = NULL;
  }
}

/*
 * Function implementations (worker thread functions)
 */

/**
 * Main loop for relaycrypt worker threads; takes the thread structure
 * as an argument and returns when the thread exits.
 */

static void
relaycrypt_worker_main(relaycrypt_thread_t *thr)
{
  relaycrypt_job_t *job = NULL;

  tor_assert(rc_dispatch);
  tor_assert(thr);

  while ((job = relaycrypt_worker_get_job(thr))) {
    /* Done with this job, return it to the dispatcher */
    relaycrypt_worker_release_job(thr, job);
  }

  /* If relaycrypt_worker_get_job(), time to exit */
}

#endif

