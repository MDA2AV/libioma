#include "clients/socket.h"
#include "io/conn.h"
#include "io/coro.h"
#include "io/internal.h"
#include "ioxd/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>

struct client_pipe {
    ioxd_pipe pipe;
    conn_t          *conn;
    char             gather[IOXD_PIPE_GATHER];
    char             slab[IOXD_PIPE_LEAD + IOXD_PIPE_CAP + IOXD_PIPE_SLACK];
};

static int make_socket(proactor_t *p, int domain)
{
    op_t op;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
    sqe->opcode   = IORING_OP_SOCKET;
    sqe->fd       = domain;
    sqe->off      = p->ring.fixed_files ? SOCK_STREAM : SOCK_STREAM | SOCK_CLOEXEC;
    sqe->len      = 0;
    sqe->rw_flags = 0;
    if (p->ring.fixed_files)
        sqe->file_index = IORING_FILE_INDEX_ALLOC;
    return ioxd__io_await(sqe, &op);
}

static int connect_socket(proactor_t *p, int fd, const struct sockaddr *sa, socklen_t len)
{
    op_t op;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
    sqe->opcode = IORING_OP_CONNECT;
    sqe->fd     = fd;
    sqe->flags  = p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
    sqe->addr   = (uintptr_t)sa;
    sqe->off    = len;
    return ioxd__io_await(sqe, &op);
}

ioxd_pipe *ioxd__socket_connect(proactor_t *p, const struct sockaddr *sa, socklen_t len, int *err)
{
    int fd = make_socket(p, sa->sa_family);
    if (fd < 0) {
        *err = -fd;
        return nullptr;
    }
    int rc = connect_socket(p, fd, sa, len);
    if (rc < 0) {
        ioxd__conn_close_socket(p, fd);
        *err = -rc;
        return nullptr;
    }
    struct client_pipe *cp = malloc(sizeof *cp);
    if (!cp) {
        ioxd__conn_close_socket(p, fd);
        *err = ENOMEM;
        return nullptr;
    }
    conn_t *c = ioxd__conn_new(p, nullptr, fd);
    int one = 1;
    ioxd__conn_setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ioxd__conn_arm_recv(p, c);
    cp->conn = c;
    ioxd__pipe_init(&cp->pipe, c, cp->gather, sizeof cp->gather, cp->slab, IOXD_PIPE_LEAD, IOXD_PIPE_CAP, IOXD_PIPE_SLACK);
    *err = 0;
    return &cp->pipe;
}

void ioxd__socket_close(ioxd_pipe *pipe)
{
    struct client_pipe *cp = (struct client_pipe *)pipe;
    ioxd__pipe_close(pipe);
    ioxd__conn_close(cp->conn);
    free(cp);
}

ioxd_pipe *ioxd_connect(const char *host, int port)
{
    proactor_t *p = ioxd__proactor_current();
    if (!p || !ioxd__coro_current() || !host || port < 1 || port > 65535) {
        errno = EINVAL;
        return nullptr;
    }
    struct sockaddr_in  v4 = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    struct sockaddr_in6 v6 = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port) };
    const struct sockaddr *sa;
    socklen_t              len;
    if (inet_pton(AF_INET, host, &v4.sin_addr) == 1) {
        sa  = (const struct sockaddr *)&v4;
        len = sizeof v4;
    } else if (inet_pton(AF_INET6, host, &v6.sin6_addr) == 1) {
        sa  = (const struct sockaddr *)&v6;
        len = sizeof v6;
    } else {
        errno = EINVAL;
        return nullptr;
    }
    int err;
    ioxd_pipe *pipe = ioxd__socket_connect(p, sa, len, &err);
    if (!pipe)
        errno = err;
    return pipe;
}

void ioxd_disconnect(ioxd_pipe *pipe)
{
    if (pipe)
        ioxd__socket_close(pipe);
}
