/*
 * common.c - socket helpers shared by server and client (see common.h).
 */
#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

void line_reader_init(LineReader *r, int fd)
{
    r->fd = fd;
    r->len = 0;
}

int read_line(LineReader *r, char *out, size_t out_size)
{
    for (;;) {
        /* 1. Is a complete line already buffered? */
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl != NULL) {
            size_t line_len = (size_t)(nl - r->buf);
            size_t copy = line_len;
            if (copy > 0 && r->buf[copy - 1] == '\r')     /* accept CRLF too */
                copy--;
            if (copy >= out_size || copy > MAX_LINE)
                return -3;
            memcpy(out, r->buf, copy);
            out[copy] = '\0';
            /* drop the consumed line (and its '\n') from the buffer */
            r->len -= line_len + 1;
            memmove(r->buf, nl + 1, r->len);
            return (int)copy;
        }
        if (r->len >= sizeof r->buf)                      /* no '\n' in 1 KiB */
            return -3;

        /* 2. Need more bytes: one recv() may deliver part of a line, one
              line, or several lines - the loop handles all three cases. */
        ssize_t n = recv(r->fd, r->buf + r->len, sizeof r->buf - r->len, 0);
        if (n == 0)
            return -1;                                    /* orderly EOF  */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -2;                                    /* errno is set */
        }
        r->len += (size_t)n;
    }
}

int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        /* MSG_NOSIGNAL: if the peer has gone away, return EPIPE instead of
           killing the whole process with SIGPIPE. */
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int send_line(int fd, const char *fmt, ...)
{
    char line[MAX_LINE + 2];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, MAX_LINE, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if (n >= MAX_LINE)
        n = MAX_LINE - 1;                                 /* truncate */
    line[n++] = '\n';
    return send_all(fd, line, (size_t)n);
}

int valid_student_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= STUDENT_ID_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)id[i]))
            return 0;
    return 1;
}
