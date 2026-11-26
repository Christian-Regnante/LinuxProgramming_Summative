/*
 * client.c - TCP client for the university event registration server
 * ==================================================================
 * Linux Programming Summative - Project 5
 *
 *   1. connects to the server over TCP
 *   2. asks for the student ID and sends it for authentication (AUTH)
 *   3. displays the authentication result (up to 3 attempts)
 *   4. displays the available events (LIST)
 *   5. lets the student pick an event and sends REGISTER <id>
 *   6. displays the result: registered / already registered / event full /
 *      no such event
 *   7. on exit sends QUIT, closes the connection and prints
 *      "Registration session closed. Goodbye, <ID>."
 *
 * The client reads plain lines from standard input, so it works interactively
 * and can also be driven by a script (input piped in). When input does not
 * come from a terminal, each answer is echoed so transcripts stay readable.
 *
 * Usage: ./client [host] [port]         (defaults: 127.0.0.1 8080)
 */
#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static int        sock = -1;
static LineReader reader;
static bool       stdin_is_tty;
static char       student[STUDENT_ID_MAX];   /* "" until authenticated */

/* ------------------------------------------------------------ helpers -- */

static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res, *ai;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Cannot resolve %s:%s: %s\n", host, port, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {          /* try each address */
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        fprintf(stderr, "Cannot connect to %s:%s: %s\n", host, port, strerror(errno));
    return fd;
}

/* Read one line from the server; on failure explain why and exit. */
static void recv_or_die(char *line, size_t size)
{
    int n = read_line(&reader, line, size);
    if (n >= 0)
        return;
    if (n == -1)
        printf("\nConnection to the server was lost (server closed the connection).\n");
    else if (n == -2)
        printf("\nConnection error: %s\n", strerror(errno));
    else
        printf("\nProtocol error: response too long.\n");
    close(sock);
    exit(EXIT_FAILURE);
}

static void send_or_die(const char *fmt, const char *arg)
{
    if (send_line(sock, fmt, arg) < 0) {
        printf("\nConnection to the server was lost (%s).\n", strerror(errno));
        close(sock);
        exit(EXIT_FAILURE);
    }
}

/* Server sent "ERR <CODE> <text>" asynchronously (timeout or shutdown)? */
static void check_fatal_server_message(const char *line)
{
    if (strncmp(line, "ERR TIMEOUT", 11) == 0 || strncmp(line, "ERR SERVER_SHUTDOWN", 19) == 0 ||
        strncmp(line, "ERR SERVER_FULL", 15) == 0) {
        const char *text = strchr(line + 4, ' ');
        printf("\nServer: %s\n", text ? text + 1 : line);
        printf("Connection closed by the server.\n");
        close(sock);
        if (student[0])
            printf("Registration session closed. Goodbye, %s.\n", student);
        exit(EXIT_FAILURE);
    }
}

/* Prompt and read one line from stdin. Returns false on end of input.
   While waiting for the user, poll() watches the socket too, so a message the
   server sends on its own initiative (idle timeout, server shutdown) or a lost
   connection is reported immediately instead of after the next keystroke.
   stdin is unbuffered (see main), so poll() on fd 0 sees every pending byte. */
static bool prompt(const char *text, char *buf, size_t size)
{
    printf("%s", text);
    fflush(stdout);
    for (;;) {
        struct pollfd fds[2] = { { .fd = STDIN_FILENO, .events = POLLIN },
                                 { .fd = sock,         .events = POLLIN } };
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {   /* server spoke first */
            char line[MAX_LINE + 1];
            recv_or_die(line, sizeof line);    /* exits on EOF / error */
            check_fatal_server_message(line);  /* exits on TIMEOUT / SHUTDOWN */
            printf("\nServer: %s\n%s", line, text);
            fflush(stdout);
            continue;
        }
        if (fds[0].revents & (POLLIN | POLLHUP))
            break;                             /* user input is ready */
    }
    if (!fgets(buf, (int)size, stdin)) {
        printf("\n");
        return false;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    if (!stdin_is_tty)
        printf("%s\n", buf);                     /* echo scripted input */
    return true;
}

/* ------------------------------------------------------------ actions -- */

static void show_events(void)
{
    char line[MAX_LINE + 1];
    send_or_die("LIST", NULL);
    recv_or_die(line, sizeof line);
    check_fatal_server_message(line);
    int count = 0;
    if (sscanf(line, "EVENTS %d", &count) != 1) {
        printf("Unexpected response: %s\n", line);
        return;
    }
    printf("\n  %-5s %-24s %-12s %s\n", "ID", "Event", "Registered", "Availability");
    printf("  %-5s %-24s %-12s %s\n", "-----", "------------------------", "----------", "------------");
    for (int i = 0; i < count; i++) {
        recv_or_die(line, sizeof line);
        int id, reg, cap;
        char name[64];
        if (sscanf(line, "EVENT %d|%63[^|]|%d|%d", &id, name, &reg, &cap) == 4) {
            char seats[16], avail[32];
            snprintf(seats, sizeof seats, "%d / %d", reg, cap);
            if (reg >= cap)
                snprintf(avail, sizeof avail, "FULL");
            else
                snprintf(avail, sizeof avail, "%d place%s left", cap - reg, cap - reg == 1 ? "" : "s");
            printf("  %-5d %-24s %-12s %s\n", id, name, seats, avail);
        }
    }
    recv_or_die(line, sizeof line);              /* END */
    printf("\n");
}

static void register_for_event(void)
{
    char input[64], line[MAX_LINE + 1];
    if (!prompt("Enter the ID of the event to register for: ", input, sizeof input))
        return;
    if (input[0] == '\0') {
        printf("No event ID entered.\n\n");
        return;
    }
    send_or_die("REGISTER %s", input);
    recv_or_die(line, sizeof line);
    check_fatal_server_message(line);

    int eid, seat, cap;
    char name[64], text[MAX_LINE + 1];
    if (sscanf(line, "OK REGISTERED %d %d %d %63[^\n]", &eid, &seat, &cap, name) == 4) {
        printf(">> SUCCESS: you are registered for %d %s (place %d of %d).\n\n", eid, name, seat, cap);
    } else if (sscanf(line, "ERR DUPLICATE %[^\n]", text) == 1) {
        printf(">> NOT REGISTERED: duplicate registration - %s.\n\n", text);
    } else if (sscanf(line, "ERR EVENT_FULL %[^\n]", text) == 1) {
        printf(">> NOT REGISTERED: the event is full - %s.\n\n", text);
    } else if (sscanf(line, "ERR NO_SUCH_EVENT %[^\n]", text) == 1) {
        printf(">> NOT REGISTERED: event unavailable - %s.\n\n", text);
    } else if (sscanf(line, "ERR BAD_REQUEST %[^\n]", text) == 1) {
        printf(">> Invalid input - %s.\n\n", text);
    } else {
        printf(">> Server response: %s\n\n", line);
    }
}

static void show_my_events(void)
{
    char line[MAX_LINE + 1];
    send_or_die("MYEVENTS", NULL);
    recv_or_die(line, sizeof line);
    check_fatal_server_message(line);
    int count = 0;
    sscanf(line, "MYEVENTS %d", &count);
    if (count == 0)
        printf("\nYou are not registered for any event yet.\n");
    else
        printf("\nYour registrations:\n");
    for (int i = 0; i < count; i++) {
        recv_or_die(line, sizeof line);
        int id;
        char name[64];
        if (sscanf(line, "EVENT %d|%63[^\n]", &id, name) == 2)
            printf("  - %d %s\n", id, name);
    }
    recv_or_die(line, sizeof line);              /* END */
    printf("\n");
}

/* ---------------------------------------------------------------- main -- */
int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : "8080";
    char line[MAX_LINE + 1], input[64];

    signal(SIGPIPE, SIG_IGN);
    stdin_is_tty = isatty(STDIN_FILENO);
    setvbuf(stdin, NULL, _IONBF, 0);             /* lets prompt() poll stdin reliably */

    sock = connect_to_server(host, port);
    if (sock < 0)
        return EXIT_FAILURE;
    line_reader_init(&reader, sock);

    recv_or_die(line, sizeof line);              /* WELCOME or ERR SERVER_FULL */
    check_fatal_server_message(line);
    printf("Connected to %s:%s\n%s\n\n", host, port,
           strncmp(line, "WELCOME ", 8) == 0 ? line + 8 : line);

    /* ---- authentication (server allows 3 attempts) ---- */
    bool authenticated = false;
    while (!authenticated) {
        if (!prompt("Enter your student ID: ", input, sizeof input)) {
            send_line(sock, "QUIT");
            close(sock);
            printf("Registration session closed.\n");
            return EXIT_SUCCESS;
        }
        send_or_die("AUTH %s", input);
        recv_or_die(line, sizeof line);
        check_fatal_server_message(line);

        char sid[STUDENT_ID_MAX], name[64] = "", text[MAX_LINE + 1];
        if (sscanf(line, "OK AUTH %15s %63[^\n]", sid, name) >= 1) {
            snprintf(student, sizeof student, "%s", sid);
            printf("Authentication successful. Welcome, %s (%s)!\n", name[0] ? name : sid, sid);
            authenticated = true;
        } else if (sscanf(line, "ERR AUTH_FAILED %[^\n]", text) == 1) {
            printf("Authentication FAILED: %s\n\n", text);
            if (strstr(text, "disconnecting")) {
                close(sock);
                printf("Registration session closed.\n");
                return EXIT_FAILURE;
            }
        } else if (sscanf(line, "ERR ALREADY_LOGGED_IN %[^\n]", text) == 1) {
            printf("Authentication FAILED: %s\n\n", text);
        } else {
            printf("Unexpected response: %s\n\n", line);
        }
    }

    /* ---- main menu ---- */
    show_events();
    for (;;) {
        printf("Menu:  1) List events   2) Register for an event   3) My registrations   0) Exit\n");
        if (!prompt("Choose an option: ", input, sizeof input))
            break;                               /* end of input = exit */
        if (strcmp(input, "1") == 0)
            show_events();
        else if (strcmp(input, "2") == 0)
            register_for_event();
        else if (strcmp(input, "3") == 0)
            show_my_events();
        else if (strcmp(input, "0") == 0 || strcasecmp(input, "q") == 0)
            break;
        else
            printf("Unknown option '%s'.\n\n", input);
    }

    /* ---- graceful close ---- */
    if (send_line(sock, "QUIT") == 0 && read_line(&reader, line, sizeof line) >= 0 &&
        strncmp(line, "BYE", 3) != 0)
        printf("Server: %s\n", line);
    close(sock);
    printf("Registration session closed. Goodbye, %s.\n", student);
    return EXIT_SUCCESS;
}
