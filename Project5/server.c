/*
 * server.c - Concurrent TCP university event registration server
 * ==============================================================
 * Linux Programming Summative - Project 5
 *
 * Students connect with the client program, authenticate with their student
 * ID, list the university events and register for them. The server keeps the
 * event table consistent while many clients act at the same time.
 *
 * ---------------------------------------------------------------------------
 * Concurrency model: one POSIX thread per client
 * ---------------------------------------------------------------------------
 * The main thread only accept()s connections. Each accepted connection gets
 * its own detached thread running client_thread(), which handles that
 * student's whole session with ordinary blocking I/O. A slow or idle client
 * therefore never delays the others, and the session logic reads top to
 * bottom (WELCOME -> AUTH -> LIST/REGISTER/... -> QUIT).
 *
 * Shared resources and how they are protected
 *   events[] (registration counts + attendee lists)  -> events_lock
 *   clients[] (connected-client table), active count  -> clients_lock
 *                                                       (+ clients_cond)
 *   server log (stdout + server.log)                  -> log_lock
 *   students[] (authorised IDs) is loaded once before any thread starts and
 *   is read-only afterwards, so it needs no lock.
 *
 * The "last seat" race
 *   register_student() performs the duplicate check, the capacity check and
 *   the seat assignment inside ONE critical section on events_lock. If two
 *   students try to take the last place at the same moment, the second thread
 *   blocks on the mutex until the first has finished; when it gets the lock
 *   it sees registered == capacity and receives EVENT_FULL. A check followed
 *   by a separately locked update (check-then-act) would let both pass the
 *   check - the classic TOCTOU race - and over-book the event.
 *
 *   Network I/O is never performed while holding events_lock: data is copied
 *   under the lock and sent afterwards, so one slow client cannot block the
 *   registration of everybody else.
 *
 * Robustness
 *   - Unexpected disconnections: recv() returns 0 (peer closed) or fails
 *     with ECONNRESET; the thread logs it, frees the client slot and exits.
 *     Registrations already made are kept.
 *   - SIGPIPE is ignored and MSG_NOSIGNAL is used, so writing to a vanished
 *     client cannot kill the server.
 *   - Idle sessions are closed after --timeout seconds (SO_RCVTIMEO).
 *   - Graceful shutdown on SIGINT/SIGTERM: stop accepting, tell every client,
 *     wait for the session threads, print the final registration table.
 *   - Registrations are appended to a CSV file and reloaded on start-up.
 *
 * Build:  make            (gcc -Wall -Wextra -O2 -pthread server.c common.c)
 * Run:    ./server [-p PORT] [--fresh] [--timeout SEC] [--max-clients N]
 *                  [--students FILE] [--state FILE] [--log FILE]
 *                  [--race-delay-ms MS]
 */
#define _GNU_SOURCE

#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_STUDENTS       256
#define MAX_EVENT_CAPACITY 64
#define CLIENT_SLOTS       256          /* hard upper bound for --max-clients */
#define MAX_AUTH_ATTEMPTS  3

/* ---------------------------------------------------------------- data -- */
typedef struct {
    int  id;
    char name[48];
    int  capacity;
    int  registered;                                   /* seats taken      */
    char attendees[MAX_EVENT_CAPACITY][STUDENT_ID_MAX]; /* who holds them   */
} Event;

typedef struct {
    char id[STUDENT_ID_MAX];
    char name[48];
} Student;

typedef struct {
    bool active;
    int  fd;
    char ip[INET_ADDRSTRLEN];
    int  port;
    char student[STUDENT_ID_MAX];      /* "" until authenticated */
} ClientSlot;

typedef enum { REG_OK, REG_NO_EVENT, REG_DUPLICATE, REG_FULL } RegResult;

/* ------------------------------------------------------------- shared -- */
static Event events[] = {
    { .id = 101, .name = "AI Workshop",           .capacity = 5 },
    { .id = 102, .name = "Cybersecurity Seminar", .capacity = 4 },
    { .id = 103, .name = "Research Conference",   .capacity = 6 },
};
#define NUM_EVENTS ((int)(sizeof events / sizeof events[0]))
static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;

static ClientSlot      clients[CLIENT_SLOTS];
static int             active_clients;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  clients_cond = PTHREAD_COND_INITIALIZER; /* count dropped */

static Student students[MAX_STUDENTS];     /* read-only after start-up */
static int     num_students;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE           *log_file;
static FILE           *state_file;         /* registrations.csv, appended under events_lock */

/* Set by the SIGINT/SIGTERM handler (main thread) and read by the session
   threads. A lock-free C11 atomic is required here: `volatile sig_atomic_t`
   is only safe between a handler and the thread it interrupted, and reading
   it from other threads is a data race (ThreadSanitizer reported exactly that
   during testing). Lock-free atomics are async-signal-safe. */
static atomic_int stop_requested;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "atomic_int must be lock-free for use in a signal handler");

/* ------------------------------------------------------------ options -- */
static int         opt_port          = DEFAULT_PORT;
static int         opt_timeout       = 120;   /* idle seconds before disconnect */
static int         opt_max_clients   = 64;
static int         opt_race_delay_ms = 0;     /* simulated latency in the critical section */
static bool        opt_fresh         = false;
static const char *opt_students      = "students.txt";
static const char *opt_state         = "registrations.csv";
static const char *opt_log           = "server.log";

/* ================================================================ utils == */

/* Thread-safe log line: "[HH:MM:SS.mmm] message" to stdout and server.log. */
static void log_msg(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static void log_msg(const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    char stamp[32], text[768];
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_lock);
    printf("[%s] %s\n", stamp, text);
    fflush(stdout);
    if (log_file) {
        fprintf(log_file, "[%s] %s\n", stamp, text);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_lock);
}

static void sleep_ms(int ms)
{
    struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;
}

static const Student *find_student(const char *id)
{
    for (int i = 0; i < num_students; i++)
        if (strcmp(students[i].id, id) == 0)
            return &students[i];
    return NULL;
}

static Event *find_event(int id)                     /* caller holds events_lock */
{
    for (int i = 0; i < NUM_EVENTS; i++)
        if (events[i].id == id)
            return &events[i];
    return NULL;
}

static bool is_attendee(const Event *e, const char *student)  /* events_lock held */
{
    for (int i = 0; i < e->registered; i++)
        if (strcmp(e->attendees[i], student) == 0)
            return true;
    return false;
}

/* ============================================================ start-up == */

/* students.txt: one "ID,Full Name" per line; blank lines and '#' comments
   are ignored. */
static void load_students(void)
{
    FILE *f = fopen(opt_students, "r");
    if (!f) {
        perror(opt_students);
        exit(EXIT_FAILURE);
    }
    char line[256];
    while (fgets(line, sizeof line, f) && num_students < MAX_STUDENTS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#')
            continue;
        char *comma = strchr(line, ',');
        if (comma)
            *comma = '\0';
        if (!valid_student_id(line)) {
            fprintf(stderr, "%s: ignoring invalid student ID '%s'\n", opt_students, line);
            continue;
        }
        Student *s = &students[num_students++];
        snprintf(s->id, sizeof s->id, "%.15s", line);   /* validated: <= 15 chars */
        snprintf(s->name, sizeof s->name, "%s", comma ? comma + 1 : "");
    }
    fclose(f);
}

/* Replay registrations.csv ("event_id,student_id,unix_time") into events[],
   then keep the file open for appending new registrations. */
static int load_state(void)
{
    int loaded = 0;
    if (!opt_fresh) {
        FILE *f = fopen(opt_state, "r");
        if (f) {
            char line[128], sid[STUDENT_ID_MAX];
            int eid;
            while (fgets(line, sizeof line, f)) {
                if (sscanf(line, "%d,%15[A-Za-z0-9]", &eid, sid) != 2)
                    continue;                                  /* malformed */
                Event *e = find_event(eid);
                if (e && find_student(sid) && !is_attendee(e, sid) &&
                    e->registered < e->capacity) {
                    snprintf(e->attendees[e->registered++], STUDENT_ID_MAX, "%s", sid);
                    loaded++;
                }
            }
            fclose(f);
        }
    }
    state_file = fopen(opt_state, opt_fresh ? "w" : "a");
    if (!state_file) {
        perror(opt_state);
        exit(EXIT_FAILURE);
    }
    return loaded;
}

/* ======================================================== registration == */

/*
 * The critical section that guarantees capacity is never exceeded.
 * Check (event exists, not already registered, seat available) and act
 * (record the attendee, increment the count, persist) happen while holding
 * events_lock, so no other thread can observe or change the event in between.
 */
static RegResult register_student(int event_id, const char *student,
                                  int *seat, int *capacity, char *name, size_t name_size)
{
    RegResult rc;

    pthread_mutex_lock(&events_lock);              /* ---- critical section ---- */
    Event *e = find_event(event_id);
    if (e == NULL) {
        rc = REG_NO_EVENT;
    } else if (is_attendee(e, student)) {
        rc = REG_DUPLICATE;
    } else if (e->registered >= e->capacity) {
        rc = REG_FULL;
    } else {
        /* Optional simulated latency (e.g. writing to a database) between
           the capacity check and the update. It widens the window in which
           a race could happen; because the lock is still held, competing
           threads simply wait. Used by the last-seat stress test. */
        if (opt_race_delay_ms > 0)
            sleep_ms(opt_race_delay_ms);
        snprintf(e->attendees[e->registered], STUDENT_ID_MAX, "%s", student);
        e->registered++;
        if (state_file) {
            fprintf(state_file, "%d,%s,%ld\n", e->id, student, (long)time(NULL));
            fflush(state_file);
        }
        rc = REG_OK;
    }
    if (e) {
        *seat = e->registered;
        *capacity = e->capacity;
        snprintf(name, name_size, "%s", e->name);
    }
    pthread_mutex_unlock(&events_lock);            /* -------------------------- */
    return rc;
}

/* LIST: copy the table under the lock, send it after unlocking. */
static int send_event_list(int fd)
{
    Event copy[NUM_EVENTS];
    pthread_mutex_lock(&events_lock);
    memcpy(copy, events, sizeof copy);
    pthread_mutex_unlock(&events_lock);

    if (send_line(fd, "EVENTS %d", NUM_EVENTS) < 0)
        return -1;
    for (int i = 0; i < NUM_EVENTS; i++)
        if (send_line(fd, "EVENT %d|%s|%d|%d", copy[i].id, copy[i].name,
                      copy[i].registered, copy[i].capacity) < 0)
            return -1;
    return send_line(fd, "END");
}

/* MYEVENTS: the events this student is registered for. */
static int send_my_events(int fd, const char *student)
{
    int ids[NUM_EVENTS], n = 0;
    char names[NUM_EVENTS][48];
    pthread_mutex_lock(&events_lock);
    for (int i = 0; i < NUM_EVENTS; i++)
        if (is_attendee(&events[i], student)) {
            ids[n] = events[i].id;
            snprintf(names[n], sizeof names[n], "%s", events[i].name);
            n++;
        }
    pthread_mutex_unlock(&events_lock);

    if (send_line(fd, "MYEVENTS %d", n) < 0)
        return -1;
    for (int i = 0; i < n; i++)
        if (send_line(fd, "EVENT %d|%s", ids[i], names[i]) < 0)
            return -1;
    return send_line(fd, "END");
}

/* ======================================================= client thread == */

/* Mark the slot as authenticated as `student` unless that student already has
   an active session. Returns false if they are already logged in elsewhere. */
static bool claim_identity(int slot, const char *student)
{
    bool ok = true;
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < CLIENT_SLOTS; i++)
        if (i != slot && clients[i].active && strcmp(clients[i].student, student) == 0)
            ok = false;
    if (ok)
        snprintf(clients[slot].student, STUDENT_ID_MAX, "%s", student);
    pthread_mutex_unlock(&clients_lock);
    return ok;
}

static void *client_thread(void *arg)
{
    int slot = (int)(intptr_t)arg;
    char who[64], line[MAX_LINE + 1], student[STUDENT_ID_MAX] = "";
    const char *reason = "unknown";
    bool authenticated = false;
    int failures = 0;

    pthread_mutex_lock(&clients_lock);
    int fd = clients[slot].fd;
    snprintf(who, sizeof who, "%s:%d", clients[slot].ip, clients[slot].port);
    pthread_mutex_unlock(&clients_lock);

    /* Idle-session timeout: recv() fails with EAGAIN after opt_timeout s. */
    struct timeval tv = { .tv_sec = opt_timeout, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    LineReader reader;
    line_reader_init(&reader, fd);
    if (send_line(fd, "WELCOME University Event Registration Server - please authenticate") < 0) {
        reason = "send failed";
        goto done;
    }

    for (;;) {
        int n = read_line(&reader, line, sizeof line);
        if (n == -1) {                                   /* peer closed */
            if (atomic_load(&stop_requested)) {
                send_line(fd, "ERR SERVER_SHUTDOWN The server is shutting down. Goodbye.");
                reason = "server shutdown";
            } else {
                reason = "DISCONNECTED UNEXPECTEDLY (connection closed without QUIT)";
            }
            break;
        }
        if (n == -2) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                send_line(fd, "ERR TIMEOUT Session idle for %d s - disconnecting", opt_timeout);
                reason = "idle timeout";
            } else if (errno == ECONNRESET) {
                reason = "DISCONNECTED UNEXPECTEDLY (connection reset by peer)";
            } else {
                reason = "receive error";
            }
            break;
        }
        if (n == -3) {
            send_line(fd, "ERR BAD_REQUEST Line too long");
            reason = "protocol violation (line too long)";
            break;
        }

        /* Parse "COMMAND [ARGUMENT]" */
        char *save = NULL;
        char *cmd = strtok_r(line, " \t", &save);
        char *argument = strtok_r(NULL, " \t", &save);
        char *extra = strtok_r(NULL, " \t", &save);
        int rc = 0;

        if (cmd == NULL) {
            rc = send_line(fd, "ERR BAD_COMMAND Empty command");
        } else if (strcasecmp(cmd, "QUIT") == 0) {
            send_line(fd, "BYE %s", authenticated ? student : "guest");
            reason = "client quit normally";
            break;
        } else if (strcasecmp(cmd, "AUTH") == 0) {
            const Student *s;
            if (authenticated) {
                rc = send_line(fd, "ERR BAD_REQUEST Already authenticated as %s", student);
            } else if (!argument || extra || !valid_student_id(argument) ||
                       (s = find_student(argument)) == NULL) {
                failures++;
                log_msg("client %s: AUTH FAILED for '%s' (attempt %d/%d)", who,
                        argument ? argument : "", failures, MAX_AUTH_ATTEMPTS);
                if (failures >= MAX_AUTH_ATTEMPTS) {
                    send_line(fd, "ERR AUTH_FAILED Unknown student ID - too many failed attempts, disconnecting");
                    reason = "too many failed authentication attempts";
                    break;
                }
                rc = send_line(fd, "ERR AUTH_FAILED Unknown student ID (attempt %d of %d)",
                               failures, MAX_AUTH_ATTEMPTS);
            } else if (!claim_identity(slot, s->id)) {
                log_msg("client %s: AUTH REJECTED for %s - already logged in elsewhere", who, s->id);
                rc = send_line(fd, "ERR ALREADY_LOGGED_IN %s already has an active session", s->id);
            } else {
                authenticated = true;
                snprintf(student, sizeof student, "%s", s->id);
                snprintf(who + strlen(who), sizeof who - strlen(who), " (%s)", student);
                log_msg("client %s: authenticated as %s", who, s->name);
                rc = send_line(fd, "OK AUTH %s %s", s->id, s->name);
            }
        } else if (!authenticated) {
            rc = send_line(fd, "ERR NOT_AUTHENTICATED Please log in with AUTH <student_id> first");
        } else if (strcasecmp(cmd, "LIST") == 0) {
            rc = send_event_list(fd);
        } else if (strcasecmp(cmd, "MYEVENTS") == 0) {
            rc = send_my_events(fd, student);
        } else if (strcasecmp(cmd, "REGISTER") == 0) {
            char *end = NULL;
            long eid = argument ? strtol(argument, &end, 10) : 0;
            if (!argument || extra || *end != '\0' || eid <= 0 || eid > 99999) {
                rc = send_line(fd, "ERR BAD_REQUEST Usage: REGISTER <event_id>");
            } else {
                int seat = 0, cap = 0;
                char ename[48] = "";
                RegResult r = register_student((int)eid, student, &seat, &cap, ename, sizeof ename);
                switch (r) {
                case REG_OK:
                    log_msg("client %s: REGISTERED for %ld %s - seat %d/%d", who, eid, ename, seat, cap);
                    rc = send_line(fd, "OK REGISTERED %ld %d %d %s", eid, seat, cap, ename);
                    break;
                case REG_DUPLICATE:
                    log_msg("client %s: duplicate registration for %ld rejected", who, eid);
                    rc = send_line(fd, "ERR DUPLICATE You are already registered for %ld %s", eid, ename);
                    break;
                case REG_FULL:
                    log_msg("client %s: event %ld FULL (%d/%d) - registration rejected", who, eid, seat, cap);
                    rc = send_line(fd, "ERR EVENT_FULL %ld %s has reached its capacity of %d", eid, ename, cap);
                    break;
                case REG_NO_EVENT:
                    rc = send_line(fd, "ERR NO_SUCH_EVENT There is no event with ID %ld", eid);
                    break;
                }
            }
        } else {
            rc = send_line(fd, "ERR BAD_COMMAND Unknown command '%s'", cmd);
        }

        if (rc < 0) {                          /* client vanished while we wrote */
            reason = "DISCONNECTED UNEXPECTEDLY (send failed)";
            break;
        }
    }

done:
    close(fd);
    pthread_mutex_lock(&clients_lock);
    clients[slot].active = false;
    clients[slot].student[0] = '\0';
    /* Log BEFORE the count drops: during shutdown main() waits for the count
       to reach 0 and then closes the log file, so no session thread may still
       be writing to it at that point. (Lock order clients_lock -> log_lock.) */
    log_msg("client %s: session ended - %s | active clients: %d",
            who, reason, active_clients - 1);
    active_clients--;
    pthread_cond_broadcast(&clients_cond);     /* shutdown waits for 0 */
    pthread_mutex_unlock(&clients_lock);
    return NULL;
}

/* ================================================================ main == */

static void on_signal(int sig)
{
    int saved_errno = errno;     /* a handler must not change errno for the  */
    (void)sig;                   /* code it interrupted                       */
    atomic_store(&stop_requested, 1);   /* async-signal-safe (lock-free)      */
    errno = saved_errno;
}

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  -p, --port N           TCP port to listen on (default %d)\n"
           "      --timeout SEC      disconnect idle sessions after SEC seconds (default 120)\n"
           "      --max-clients N    maximum simultaneous clients (default 64, max %d)\n"
           "      --students FILE    authorised student list (default students.txt)\n"
           "      --state FILE       persistent registrations (default registrations.csv)\n"
           "      --fresh            start with no registrations (truncate the state file)\n"
           "      --log FILE         server log file (default server.log)\n"
           "      --race-delay-ms MS simulated latency inside the registration critical\n"
           "                         section (testing aid for the last-seat race)\n",
           prog, DEFAULT_PORT, CLIENT_SLOTS);
}

static int parse_int(const char *s, int lo, int hi, const char *what)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "invalid %s '%s' (expected %d..%d)\n", what, s, lo, hi);
        exit(EXIT_FAILURE);
    }
    return (int)v;
}

static void parse_args(int argc, char **argv)
{
    static const struct option opts[] = {
        {"port", required_argument, NULL, 'p'},     {"timeout", required_argument, NULL, 't'},
        {"max-clients", required_argument, NULL, 'm'}, {"students", required_argument, NULL, 's'},
        {"state", required_argument, NULL, 'S'},    {"fresh", no_argument, NULL, 'f'},
        {"log", required_argument, NULL, 'l'},      {"race-delay-ms", required_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},           {NULL, 0, NULL, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:h", opts, NULL)) != -1) {
        switch (c) {
        case 'p': opt_port = parse_int(optarg, 1, 65535, "port"); break;
        case 't': opt_timeout = parse_int(optarg, 1, 86400, "timeout"); break;
        case 'm': opt_max_clients = parse_int(optarg, 1, CLIENT_SLOTS, "max-clients"); break;
        case 's': opt_students = optarg; break;
        case 'S': opt_state = optarg; break;
        case 'f': opt_fresh = true; break;
        case 'l': opt_log = optarg; break;
        case 'r': opt_race_delay_ms = parse_int(optarg, 0, 10000, "race delay"); break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default:  usage(argv[0]); exit(EXIT_FAILURE);
        }
    }
}

static void print_registration_table(const char *title)
{
    pthread_mutex_lock(&events_lock);
    log_msg("%s", title);
    for (int i = 0; i < NUM_EVENTS; i++) {
        char list[MAX_EVENT_CAPACITY * STUDENT_ID_MAX + 8] = "";
        for (int j = 0; j < events[i].registered; j++) {
            strcat(list, j ? ", " : "");
            strcat(list, events[i].attendees[j]);
        }
        log_msg("  %d %-22s %d/%d  [%s]", events[i].id, events[i].name,
                events[i].registered, events[i].capacity, list);
    }
    pthread_mutex_unlock(&events_lock);
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    load_students();
    int restored = load_state();
    log_file = fopen(opt_log, "a");
    if (!log_file)
        perror(opt_log);                       /* logging to stdout still works */

    /* Signals: SIGPIPE ignored; SIGINT/SIGTERM set a flag. No SA_RESTART, so
       a blocked accept() returns EINTR and the loop can notice the flag. */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigset_t stop_signals, old_mask;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);

    /* Create the listening TCP socket. */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return EXIT_FAILURE; }
    int yes = 1;   /* allow immediate restart while old connections are in TIME_WAIT */
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)opt_port),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return EXIT_FAILURE;
    }
    if (listen(listen_fd, 64) < 0) { perror("listen"); return EXIT_FAILURE; }

    log_msg("server listening on port %d | %d authorised students | idle timeout %d s | max clients %d%s",
            opt_port, num_students, opt_timeout, opt_max_clients,
            opt_race_delay_ms ? " | race-test delay enabled" : "");
    log_msg("%d registration(s) restored from %s%s", restored, opt_state,
            opt_fresh ? " (fresh start)" : "");
    print_registration_table("events:");

    /* ---- accept loop ---- */
    while (!atomic_load(&stop_requested)) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            if (errno == EINTR)
                continue;                               /* re-check stop flag */
            log_msg("accept failed: %s", strerror(errno));
            if (errno == EMFILE || errno == ENFILE)
                sleep_ms(100);                          /* out of descriptors */
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        int port = ntohs(peer.sin_port);

        /* Register the client in the connected-client table. */
        int slot = -1, count;
        pthread_mutex_lock(&clients_lock);
        if (active_clients < opt_max_clients)
            for (int i = 0; i < CLIENT_SLOTS; i++)
                if (!clients[i].active) { slot = i; break; }
        if (slot >= 0) {
            clients[slot] = (ClientSlot){ .active = true, .fd = fd, .port = port };
            snprintf(clients[slot].ip, sizeof clients[slot].ip, "%s", ip);
            active_clients++;
        }
        count = active_clients;
        pthread_mutex_unlock(&clients_lock);

        if (slot < 0) {
            send_line(fd, "ERR SERVER_FULL Too many clients connected - try again later");
            close(fd);
            log_msg("connection from %s:%d refused - server full (%d clients)", ip, port, count);
            continue;
        }
        log_msg("client %s:%d connected | active clients: %d", ip, port, count);

        /* Session threads inherit the signal mask: block SIGINT/SIGTERM while
           creating them so that those signals are always delivered to the
           main thread (whose accept() must be interrupted). */
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_sigmask(SIG_BLOCK, &stop_signals, &old_mask);
        int err = pthread_create(&tid, &attr, client_thread, (void *)(intptr_t)slot);
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        pthread_attr_destroy(&attr);
        if (err != 0) {
            log_msg("pthread_create failed: %s", strerror(err));
            close(fd);
            pthread_mutex_lock(&clients_lock);
            clients[slot].active = false;
            active_clients--;
            pthread_mutex_unlock(&clients_lock);
        }
    }

    /* ---- graceful shutdown ---- */
    close(listen_fd);
    pthread_mutex_lock(&clients_lock);
    log_msg("shutdown requested - closing %d active session(s)", active_clients);
    for (int i = 0; i < CLIENT_SLOTS; i++)
        if (clients[i].active)
            shutdown(clients[i].fd, SHUT_RD);   /* their recv() returns 0 */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    while (active_clients > 0 &&
           pthread_cond_timedwait(&clients_cond, &clients_lock, &deadline) != ETIMEDOUT)
        ;
    int left = active_clients;
    pthread_mutex_unlock(&clients_lock);
    if (left > 0)
        log_msg("%d session(s) did not finish within 5 s", left);

    print_registration_table("final registrations:");
    if (state_file)
        fclose(state_file);
    log_msg("server stopped");
    if (log_file)
        fclose(log_file);
    return EXIT_SUCCESS;
}
