/*
 * library_reservation.c - Multithreaded library book reservation system
 * =====================================================================
 * Linux Programming Summative - Project 4
 *
 * A producer-consumer simulation with POSIX threads:
 *
 *   STUDENT thread (producer)  generates 15 reservation requests, each with a
 *                              unique incremental reservation ID and a book ID,
 *                              taking 1 s to submit each one, and places them
 *                              in a bounded queue (capacity 5). It waits while
 *                              the queue is full.
 *   STAFF thread (consumer)    removes requests from the queue, takes 3 s to
 *                              process each, and approves even-numbered books
 *                              (available) / rejects odd-numbered books
 *                              (unavailable). It waits while the queue is empty.
 *   MONITOR thread             every 4 s prints the totals submitted, processed,
 *                              approved, rejected and the current queue size.
 *
 * ---------------------------------------------------------------------------
 * Synchronisation design
 * ---------------------------------------------------------------------------
 * Shared memory: ONE structure (ReservationSystem `sys`) holds the circular
 * queue AND every counter. ONE mutex, sys.lock, protects ALL of it. The queue
 * and the counters change together (e.g. enqueue + submitted++), so a single
 * lock makes each change atomic as a whole: no thread - in particular the
 * monitor - can ever observe a half-applied update such as "submitted was
 * incremented but the item is not in the queue yet".
 *
 * Rule: a field of `sys` is only read or written between
 * pthread_mutex_lock(&sys.lock) and pthread_mutex_unlock(&sys.lock).
 *
 * The slow work (1 s submission, 3 s processing) is simulated OUTSIDE the
 * lock, so the lock is only ever held for microseconds and the threads really
 * run concurrently.
 *
 * Condition variables (all used with sys.lock):
 *   not_full   - the producer waits on it while count == QUEUE_CAPACITY;
 *                the consumer signals it after removing an item.
 *   not_empty  - the consumer waits on it while count == 0 (and the producer
 *                is not finished); the producer signals it after inserting.
 *   shutdown   - the monitor sleeps on it with a 4-second timeout; the
 *                consumer broadcasts it when the last request is processed so
 *                the monitor wakes immediately instead of finishing its sleep.
 *
 * Every wait is inside a `while (condition)` loop, never an `if`: wake-ups can
 * be spurious, and another thread may change the state between the signal and
 * the moment the woken thread re-acquires the mutex. pthread_cond_wait
 * atomically releases the mutex while sleeping and re-acquires it before
 * returning, so no signal can be lost between testing the condition and
 * going to sleep.
 *
 * Lock ordering: log messages are serialised by a second mutex, log_lock.
 * Threads may take log_lock while holding sys.lock, but code holding log_lock
 * never tries to take sys.lock, so a deadlock cycle is impossible.
 *
 * Termination: the producer sets producer_done after its 15th request and
 * broadcasts not_empty; the consumer exits once the queue is empty and
 * producer_done is set, after setting all_done and broadcasting shutdown;
 * the monitor exits when it sees all_done. main() joins all three threads,
 * checks the final invariants, then destroys every mutex and condition
 * variable.
 *
 * Build:  gcc -Wall -Wextra -O2 -pthread -o library_reservation library_reservation.c
 * Run:    ./library_reservation [--time-scale=F] [--seed=N]
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Simulation parameters (from the assignment) --------------------------- */
#define QUEUE_CAPACITY      5      /* maximum pending reservations            */
#define TOTAL_REQUESTS     15      /* reservations to submit and process      */
#define SUBMIT_SECONDS    1.0      /* time for the student to submit one      */
#define PROCESS_SECONDS   3.0      /* time for the staff to process one       */
#define MONITOR_SECONDS   4.0      /* interval between monitoring reports     */
#define FIRST_BOOK_ID     100      /* book IDs are drawn from 100..199        */
#define BOOK_ID_RANGE     100

/* ---- Error checking for pthread calls (they return an error number) -------- */
#define CHECK(call)                                                             \
    do {                                                                        \
        int rc_ = (call);                                                       \
        if (rc_ != 0) {                                                         \
            fprintf(stderr, "fatal: %s failed: %s (%s:%d)\n",                   \
                    #call, strerror(rc_), __FILE__, __LINE__);                  \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

/* ---- Data types ------------------------------------------------------------ */
typedef struct {
    int    reservation_id;         /* unique, incremental: 1, 2, 3, ...        */
    int    book_id;                /* even = available, odd = unavailable      */
    double enqueued_at;            /* seconds since start (queue-wait stats)   */
} Reservation;

typedef struct {
    /* Circular buffer: items live in buf[head .. head+count-1] (mod CAPACITY).
       Insert at tail, remove at head; both indices wrap around, so no element
       is ever shifted and both operations are O(1). */
    Reservation buf[QUEUE_CAPACITY];
    int head;                      /* index of the oldest request              */
    int tail;                      /* index where the next request goes        */
    int count;                     /* current number of queued requests        */

    /* Statistics */
    int submitted, processed, approved, rejected;
    int max_queue_seen;            /* highest occupancy observed               */
    int producer_waits;            /* times the producer found the queue full  */
    int consumer_waits;            /* times the consumer found it empty        */
    double total_queue_wait;       /* sum of time requests spent queued        */

    /* Life-cycle flags */
    bool producer_done;            /* all requests have been submitted         */
    bool all_done;                 /* all requests have been processed         */

    /* Synchronisation */
    pthread_mutex_t lock;          /* protects EVERY field above               */
    pthread_cond_t  not_full;      /* producer waits here when queue is full   */
    pthread_cond_t  not_empty;     /* consumer waits here when queue is empty  */
    pthread_cond_t  shutdown;      /* monitor's timed sleep / early wake-up    */
} ReservationSystem;

/* A copy of the shared state taken under the lock, so that it can be printed
   after the lock has been released. */
typedef struct {
    int submitted, processed, approved, rejected, count;
    int queued_ids[QUEUE_CAPACITY];
} Snapshot;

/* ---- Globals --------------------------------------------------------------- */
static ReservationSystem sys;
static pthread_mutex_t   log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec   start_time;
static double            time_scale = 1.0;      /* 1.0 = real assignment timings */
static unsigned int      book_seed  = 2026;     /* fixed seed -> reproducible run */

/* ========================================================================= */
/*  Utilities                                                                */
/* ========================================================================= */

static double elapsed_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start_time.tv_sec) +
           (double)(now.tv_nsec - start_time.tv_nsec) / 1e9;
}

/* Thread-safe logging: one complete line per call, prefixed with the elapsed
   time and the thread's role. log_lock guarantees that lines printed by
   different threads never interleave character by character. */
static void log_msg(const char *who, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&log_lock);
    printf("[%6.2fs] %-8s | ", elapsed_seconds(), who);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* Sleep for `seconds` x time_scale, resuming correctly if interrupted. */
static void simulate_work(double seconds)
{
    double s = seconds * time_scale;
    struct timespec req = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;
}

/* Add `seconds` to an absolute timespec (used for the monitor's deadlines). */
static void timespec_add(struct timespec *t, double seconds)
{
    long long ns = t->tv_nsec + (long long)(seconds * 1e9);
    t->tv_sec  += (time_t)(ns / 1000000000LL);
    t->tv_nsec  = (long)(ns % 1000000000LL);
}

/* ---- Circular-buffer operations: caller MUST hold sys.lock ----------------- */

static void enqueue(Reservation r)
{
    assert(sys.count < QUEUE_CAPACITY);        /* never exceed capacity      */
    sys.buf[sys.tail] = r;
    sys.tail = (sys.tail + 1) % QUEUE_CAPACITY;
    sys.count++;
    if (sys.count > sys.max_queue_seen)
        sys.max_queue_seen = sys.count;
}

static Reservation dequeue(void)
{
    assert(sys.count > 0);                     /* never remove from empty    */
    Reservation r = sys.buf[sys.head];
    sys.head = (sys.head + 1) % QUEUE_CAPACITY;
    sys.count--;
    return r;
}

static Snapshot take_snapshot(void)            /* caller holds sys.lock      */
{
    Snapshot s = { sys.submitted, sys.processed, sys.approved, sys.rejected,
                   sys.count, {0} };
    for (int i = 0; i < sys.count; i++)
        s.queued_ids[i] = sys.buf[(sys.head + i) % QUEUE_CAPACITY].reservation_id;
    return s;
}

static void format_queue(const Snapshot *s, char *out, size_t size)
{
    size_t used = (size_t)snprintf(out, size, "[");
    for (int i = 0; i < s->count && used < size; i++)
        used += (size_t)snprintf(out + used, size - used, "%s#%d",
                                 i ? " " : "", s->queued_ids[i]);
    if (used < size)
        snprintf(out + used, size - used, "]");
}

/* ========================================================================= */
/*  Thread functions                                                         */
/* ========================================================================= */

/* ---- STUDENT: producer ----------------------------------------------------- */
static void *student_producer(void *arg)
{
    (void)arg;
    for (int id = 1; id <= TOTAL_REQUESTS; id++) {
        /* Prepare the request. rand_r with a private seed is thread-safe
           (rand() keeps hidden global state). Only this thread uses it. */
        Reservation r = { .reservation_id = id,
                          .book_id = FIRST_BOOK_ID + (int)(rand_r(&book_seed) % BOOK_ID_RANGE) };
        log_msg("STUDENT", "preparing reservation #%d for book %d ...",
                r.reservation_id, r.book_id);

        simulate_work(SUBMIT_SECONDS);         /* 1 s - outside the lock      */

        pthread_mutex_lock(&sys.lock);
        /* FULL QUEUE: sleep until the consumer frees a slot. The loop
           re-checks the condition after every wake-up. */
        while (sys.count == QUEUE_CAPACITY) {
            sys.producer_waits++;
            log_msg("STUDENT", "queue FULL (%d/%d) - waiting to submit #%d",
                    sys.count, QUEUE_CAPACITY, r.reservation_id);
            pthread_cond_wait(&sys.not_full, &sys.lock);
            /* here: lock re-acquired, condition must be tested again */
        }
        r.enqueued_at = elapsed_seconds();
        enqueue(r);
        sys.submitted++;
        log_msg("STUDENT", "SUBMITTED reservation #%d (book %d)  queue %d/%d",
                r.reservation_id, r.book_id, sys.count, QUEUE_CAPACITY);
        pthread_cond_signal(&sys.not_empty);   /* wake the consumer if asleep */
        pthread_mutex_unlock(&sys.lock);
    }

    /* Tell the consumer that no more requests will arrive. broadcast (not
       signal) so that every waiter re-checks its condition. */
    pthread_mutex_lock(&sys.lock);
    sys.producer_done = true;
    pthread_cond_broadcast(&sys.not_empty);
    pthread_mutex_unlock(&sys.lock);
    log_msg("STUDENT", "all %d reservations submitted - student thread exiting",
            TOTAL_REQUESTS);
    return NULL;
}

/* ---- LIBRARY STAFF: consumer ----------------------------------------------- */
static void *staff_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&sys.lock);
        /* EMPTY QUEUE: sleep until the producer adds a request or finishes. */
        while (sys.count == 0 && !sys.producer_done) {
            sys.consumer_waits++;
            log_msg("STAFF", "queue EMPTY - waiting for reservations");
            pthread_cond_wait(&sys.not_empty, &sys.lock);
        }
        if (sys.count == 0 && sys.producer_done) {     /* nothing left, ever */
            pthread_mutex_unlock(&sys.lock);
            break;
        }
        Reservation r = dequeue();
        double waited = elapsed_seconds() - r.enqueued_at;
        sys.total_queue_wait += waited;
        log_msg("STAFF", "took reservation #%d (book %d) from queue after %.1fs  queue %d/%d",
                r.reservation_id, r.book_id, waited, sys.count, QUEUE_CAPACITY);
        pthread_cond_signal(&sys.not_full);    /* a slot is free: wake producer */
        pthread_mutex_unlock(&sys.lock);

        /* Process without holding the lock (3 s). Availability rule:
           even book ID = available -> APPROVED, odd = unavailable -> REJECTED. */
        simulate_work(PROCESS_SECONDS);
        bool available = (r.book_id % 2 == 0);

        pthread_mutex_lock(&sys.lock);
        sys.processed++;
        if (available)
            sys.approved++;
        else
            sys.rejected++;
        log_msg("STAFF", "%s reservation #%d - book %d is %s  (processed %d/%d)",
                available ? "APPROVED" : "REJECTED", r.reservation_id, r.book_id,
                available ? "available (even ID)" : "unavailable (odd ID)",
                sys.processed, TOTAL_REQUESTS);
        pthread_mutex_unlock(&sys.lock);
    }

    /* Every request has been processed: let the monitor finish now. */
    pthread_mutex_lock(&sys.lock);
    sys.all_done = true;
    pthread_cond_broadcast(&sys.shutdown);
    pthread_mutex_unlock(&sys.lock);
    log_msg("STAFF", "queue empty and student finished - staff thread exiting");
    return NULL;
}

/* ---- MONITOR --------------------------------------------------------------- */
static void print_report(const char *title, int number, const Snapshot *s)
{
    char queue[96];
    format_queue(s, queue, sizeof queue);
    log_msg("MONITOR", "------ %s #%d ------", title, number);
    log_msg("MONITOR", "  submitted %2d | processed %2d | approved %2d | rejected %2d | queue %d/%d %s",
            s->submitted, s->processed, s->approved, s->rejected,
            s->count, QUEUE_CAPACITY, queue);
}

static void *monitor_thread(void *arg)
{
    (void)arg;
    struct timespec deadline;
    int report = 0;

    /* Absolute deadlines (start + 4 s, + 8 s, ...) on the monotonic clock:
       reports do not drift even though printing takes time, and changes to
       the wall-clock time cannot affect the interval. */
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    timespec_add(&deadline, MONITOR_SECONDS * time_scale);

    pthread_mutex_lock(&sys.lock);
    while (!sys.all_done) {
        int rc = pthread_cond_timedwait(&sys.shutdown, &sys.lock, &deadline);
        if (rc == ETIMEDOUT && !sys.all_done) {
            /* Copy the statistics while holding the lock (consistent view),
               then release it BEFORE printing, so terminal output never
               delays the producer or the consumer. */
            Snapshot s = take_snapshot();
            pthread_mutex_unlock(&sys.lock);
            print_report("System status report", ++report, &s);
            timespec_add(&deadline, MONITOR_SECONDS * time_scale);
            pthread_mutex_lock(&sys.lock);
        } else if (rc != 0 && rc != ETIMEDOUT) {
            fprintf(stderr, "monitor: pthread_cond_timedwait: %s\n", strerror(rc));
            break;
        }
        /* rc == 0: woken by the shutdown broadcast (or spuriously) - the
           loop condition decides whether to exit or keep waiting. */
    }
    Snapshot final = take_snapshot();
    pthread_mutex_unlock(&sys.lock);
    print_report("FINAL report", report + 1, &final);
    log_msg("MONITOR", "all reservations processed - monitor thread exiting");
    return NULL;
}

/* ========================================================================= */
/*  Set-up, command line, main                                               */
/* ========================================================================= */

static void init_system(void)
{
    memset(&sys, 0, sizeof sys);
    CHECK(pthread_mutex_init(&sys.lock, NULL));
    CHECK(pthread_cond_init(&sys.not_full, NULL));
    CHECK(pthread_cond_init(&sys.not_empty, NULL));

    /* The monitor's timed wait uses CLOCK_MONOTONIC deadlines, so its
       condition variable must be bound to that clock. */
    pthread_condattr_t attr;
    CHECK(pthread_condattr_init(&attr));
    CHECK(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    CHECK(pthread_cond_init(&sys.shutdown, &attr));
    CHECK(pthread_condattr_destroy(&attr));
}

static void destroy_system(void)
{
    CHECK(pthread_cond_destroy(&sys.shutdown));
    CHECK(pthread_cond_destroy(&sys.not_empty));
    CHECK(pthread_cond_destroy(&sys.not_full));
    CHECK(pthread_mutex_destroy(&sys.lock));
    CHECK(pthread_mutex_destroy(&log_lock));
}

static void usage(const char *prog)
{
    printf("Usage: %s [--time-scale=F] [--seed=N]\n"
           "  --time-scale=F  multiply every delay by F (default 1.0 = 1 s submit,\n"
           "                  3 s processing, 4 s monitoring; e.g. 0.1 for a fast run)\n"
           "  --seed=N        seed for the generated book IDs (default 2026)\n", prog);
}

static void parse_args(int argc, char **argv)
{
    static const struct option opts[] = {
        {"time-scale", required_argument, NULL, 't'},
        {"seed",       required_argument, NULL, 's'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int c;
    char *end;
    while ((c = getopt_long(argc, argv, "t:s:h", opts, NULL)) != -1) {
        switch (c) {
        case 't':
            time_scale = strtod(optarg, &end);
            if (*end != '\0' || time_scale <= 0.0 || time_scale > 100.0) {
                fprintf(stderr, "invalid --time-scale '%s' (must be > 0 and <= 100)\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 's':
            book_seed = (unsigned int)strtoul(optarg, &end, 10);
            if (*end != '\0') {
                fprintf(stderr, "invalid --seed '%s'\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    init_system();

    log_msg("MAIN", "Library reservation system starting: %d requests, queue capacity %d",
            TOTAL_REQUESTS, QUEUE_CAPACITY);
    log_msg("MAIN", "timings: submit %.2fs, process %.2fs, monitor every %.2fs (time scale %.2f)",
            SUBMIT_SECONDS * time_scale, PROCESS_SECONDS * time_scale,
            MONITOR_SECONDS * time_scale, time_scale);

    pthread_t student, staff, monitor;
    CHECK(pthread_create(&monitor, NULL, monitor_thread, NULL));
    CHECK(pthread_create(&staff,   NULL, staff_consumer, NULL));
    CHECK(pthread_create(&student, NULL, student_producer, NULL));

    /* Wait for all three threads to finish (graceful termination). */
    CHECK(pthread_join(student, NULL));
    CHECK(pthread_join(staff,   NULL));
    CHECK(pthread_join(monitor, NULL));
    log_msg("MAIN", "all threads joined");

    /* Only one thread is left, but the lock is still taken for consistency. */
    pthread_mutex_lock(&sys.lock);
    ReservationSystem s = sys;                 /* copy of the final state */
    pthread_mutex_unlock(&sys.lock);

    double runtime = elapsed_seconds();
    printf("\n=================== FINAL SUMMARY ===================\n");
    printf(" Reservations submitted      : %d\n", s.submitted);
    printf(" Reservations processed      : %d\n", s.processed);
    printf(" Approved (even book ID)     : %d\n", s.approved);
    printf(" Rejected (odd book ID)      : %d\n", s.rejected);
    printf(" Queue size at exit          : %d\n", s.count);
    printf(" Maximum queue occupancy     : %d / %d\n", s.max_queue_seen, QUEUE_CAPACITY);
    printf(" Producer waits (queue full) : %d\n", s.producer_waits);
    printf(" Consumer waits (queue empty): %d\n", s.consumer_waits);
    printf(" Average time in queue       : %.2f s\n",
           s.processed ? s.total_queue_wait / s.processed : 0.0);
    printf(" Total running time          : %.2f s\n", runtime);

    /* Invariants that must hold if synchronisation was correct. */
    struct { const char *what; bool ok; } checks[] = {
        { "all 15 requests submitted",            s.submitted == TOTAL_REQUESTS },
        { "all 15 requests processed",            s.processed == TOTAL_REQUESTS },
        { "approved + rejected == processed",     s.approved + s.rejected == s.processed },
        { "queue empty at exit",                  s.count == 0 },
        { "queue never exceeded capacity 5",      s.max_queue_seen <= QUEUE_CAPACITY },
    };
    bool all_ok = true;
    printf("-----------------------------------------------------\n");
    for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        printf(" [%s] %s\n", checks[i].ok ? "PASS" : "FAIL", checks[i].what);
        all_ok = all_ok && checks[i].ok;
    }
    printf("=====================================================\n");

    destroy_system();
    printf("All mutexes and condition variables destroyed. Program terminated %s.\n",
           all_ok ? "successfully" : "WITH ERRORS");
    return all_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
