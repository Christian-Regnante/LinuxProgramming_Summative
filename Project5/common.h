/*
 * common.h - shared protocol definitions for the event-registration
 *            server (server.c) and client (client.c).
 * Linux Programming Summative - Project 5
 *
 * APPLICATION PROTOCOL (text, one message per line, '\n'-terminated)
 * -----------------------------------------------------------------
 *   server -> client on connect:  WELCOME <text>
 *
 *   client -> server              server -> client
 *   AUTH <student_id>             OK AUTH <student_id> <name...>
 *                                 ERR AUTH_FAILED <reason>
 *                                 ERR ALREADY_LOGGED_IN <reason>
 *   LIST                          EVENTS <n>
 *                                 EVENT <id>|<name>|<registered>|<capacity>   (n lines)
 *                                 END
 *   REGISTER <event_id>           OK REGISTERED <event_id> <seat> <capacity> <name...>
 *                                 ERR EVENT_FULL | ERR DUPLICATE | ERR NO_SUCH_EVENT
 *                                 ERR BAD_REQUEST
 *   MYEVENTS                      MYEVENTS <n>
 *                                 EVENT <id>|<name>                            (n lines)
 *                                 END
 *   QUIT                          BYE <student_id>
 *
 *   Any command except AUTH/QUIT before authentication -> ERR NOT_AUTHENTICATED
 *   Unknown command                                    -> ERR BAD_COMMAND
 *   Idle too long                                      -> ERR TIMEOUT (then close)
 *   Server shutting down                               -> ERR SERVER_SHUTDOWN (then close)
 *   Too many clients                                   -> ERR SERVER_FULL (then close)
 *
 * TCP is a byte STREAM: one send() on one side does not correspond to one
 * recv() on the other. Messages are therefore framed by '\n', and
 * read_line() buffers incoming bytes until a complete line is available.
 */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <sys/types.h>

#define DEFAULT_PORT     8080
#define MAX_LINE         512      /* longest accepted protocol line */
#define STUDENT_ID_MAX   16       /* including the terminating NUL   */

/* Buffered line reader for one socket. */
typedef struct {
    int    fd;
    size_t len;                   /* bytes currently buffered */
    char   buf[2 * MAX_LINE];
} LineReader;

void    line_reader_init(LineReader *r, int fd);

/* Read one line (without "\n" or "\r\n") into out.
 * Returns: >= 0  length of the line
 *          -1    connection closed by the peer (EOF) before a full line
 *          -2    error; errno is set (EAGAIN/EWOULDBLOCK = receive timeout)
 *          -3    line longer than MAX_LINE (protocol violation)            */
int     read_line(LineReader *r, char *out, size_t out_size);

/* Send all len bytes (loops over partial sends). Never raises SIGPIPE.
 * Returns 0 on success, -1 on error. */
int     send_all(int fd, const char *buf, size_t len);

/* printf-style: format one line, append '\n', send it. 0 / -1 like send_all. */
int     send_line(int fd, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));

/* Valid student ID: 1..15 characters, letters and digits only. */
int     valid_student_id(const char *id);

#endif /* COMMON_H */
