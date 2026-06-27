#ifndef PLATFORM_IO_H
#define PLATFORM_IO_H

/* ── Linux ──────────────────────────────────────────────────────────────── */
#ifdef __linux__
#include <liburing.h>
#include <sys/epoll.h>

#define PLATFORM_EPOLL_FD int epoll_fd

/* ── macOS ──────────────────────────────────────────────────────────────── */
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>

/* epoll → kqueue shim */
#define EPOLLIN     0x001
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDHUP  0x2000
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
};

static inline int epoll_create1(int flags) {
    (void)flags;
    return kqueue();
}

static inline int epoll_ctl(int kq, int op, int fd, struct epoll_event *ev) {
    struct kevent kev;
    if (op == EPOLL_CTL_ADD) {
        EV_SET(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, ev ? ev->data.ptr : NULL);
        return kevent(kq, &kev, 1, NULL, 0, NULL);
    } else if (op == EPOLL_CTL_DEL) {
        EV_SET(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        return kevent(kq, &kev, 1, NULL, 0, NULL);
    }
    return 0;
}

static inline int epoll_wait(int kq, struct epoll_event *events, int max, int timeout) {
    struct kevent kev_list[256];
    int max_ev = max > 256 ? 256 : max;
    struct timespec ts = { .tv_sec = timeout / 1000, .tv_nsec = (timeout % 1000) * 1000000ll };
    struct timespec *tsp = timeout < 0 ? NULL : &ts;
    int n = kevent(kq, NULL, 0, kev_list, max_ev, tsp);
    for (int i = 0; i < n; i++) {
        events[i].events = 0;
        if (kev_list[i].filter == EVFILT_READ)
            events[i].events |= EPOLLIN | EPOLLRDHUP;
        if (kev_list[i].flags & EV_EOF)
            events[i].events |= EPOLLHUP;
        events[i].data.ptr = kev_list[i].udata;
    }
    return n;
}

/* io_uring stubs — always fail, causing graceful fallback to epoll/kqueue */
struct io_uring { int _dummy; };
struct io_uring_cqe { int _dummy; int res; void *user_data; };
struct io_uring_sqe { int _dummy; };
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x1000
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC  0x80000
#endif
#define io_uring_for_each_cqe(ring, head, cqe) for (int _i_ = 0; _i_ < 0; _i_++)

static inline int io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags) {
    (void)entries; (void)ring; (void)flags; return -1;
}
static inline void io_uring_queue_exit(struct io_uring *ring) { (void)ring; }
static inline void io_uring_submit(struct io_uring *ring) { (void)ring; }
static inline void io_uring_cq_advance(struct io_uring *ring, unsigned n) { (void)ring; (void)n; }
static inline struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring) { (void)ring; return NULL; }
static inline void io_uring_prep_accept(struct io_uring_sqe *sqe, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags) { (void)sqe; (void)fd; (void)addr; (void)addrlen; (void)flags; }
static inline void io_uring_prep_recv(struct io_uring_sqe *sqe, int fd, void *buf, unsigned len, int flags) { (void)sqe; (void)fd; (void)buf; (void)len; (void)flags; }
static inline void io_uring_sqe_set_data(struct io_uring_sqe *sqe, void *data) { (void)sqe; (void)data; }
static inline void *io_uring_cqe_get_data(const struct io_uring_cqe *cqe) { (void)cqe; return NULL; }

#define PLATFORM_EPOLL_FD int epoll_fd

/* ── Windows ─────────────────────────────────────────────────────────────── */
#elif defined(_WIN32)
#include <winsock2.h>
#include <windows.h>

#define EPOLLIN     0x001
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDHUP  0x2000
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
};

/* select()-based epoll shim. Maintains per-instance fd sets. */
#define MAX_EPOLL_FDS 1024
#define MAX_EPOLL_INSTANCES 8

typedef struct {
    SOCKET fds[MAX_EPOLL_FDS];
    int count;
} WinEpollSet;

static WinEpollSet g_win_epoll[MAX_EPOLL_INSTANCES];
static int g_win_epoll_count = 0;

static inline int epoll_create1(int flags) {
    (void)flags;
    if (g_win_epoll_count >= MAX_EPOLL_INSTANCES) return -1;
    int idx = g_win_epoll_count++;
    g_win_epoll[idx].count = 0;
    return idx + 1; /* positive fd, 0-based index + 1 */
}

static inline int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
    int idx = epfd - 1;
    if (idx < 0 || idx >= g_win_epoll_count) return -1;
    (void)ev;
    WinEpollSet *set = &g_win_epoll[idx];
    if (op == EPOLL_CTL_ADD) {
        if (set->count >= MAX_EPOLL_FDS) return -1;
        set->fds[set->count++] = (SOCKET)fd;
    } else if (op == EPOLL_CTL_DEL) {
        for (int i = 0; i < set->count; i++) {
            if (set->fds[i] == (SOCKET)fd) {
                set->fds[i] = set->fds[--set->count];
                break;
            }
        }
    }
    return 0;
}

static inline int epoll_wait(int epfd, struct epoll_event *events, int max, int timeout) {
    int idx = epfd - 1;
    if (idx < 0 || idx >= g_win_epoll_count) return -1;
    WinEpollSet *set = &g_win_epoll[idx];
    if (set->count == 0) return 0;

    fd_set readfds;
    FD_ZERO(&readfds);
    SOCKET max_fd = 0;
    for (int i = 0; i < set->count; i++) {
        FD_SET(set->fds[i], &readfds);
        if (set->fds[i] > max_fd) max_fd = set->fds[i];
    }

    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout >= 0) {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    } else if (timeout < 0) {
        ptv = NULL; /* infinite */
    }

    int n = select((int)(max_fd + 1), &readfds, NULL, NULL, ptv);
    if (n <= 0) return n;

    int found = 0;
    for (int i = 0; i < set->count && found < max; i++) {
        if (FD_ISSET(set->fds[i], &readfds)) {
            events[found].events = EPOLLIN;
            events[found].data.fd = (int)set->fds[i];
            found++;
        }
    }
    return found;
}

/* io_uring stubs — same as macOS, always fail */
struct io_uring { int _dummy; };
struct io_uring_cqe { int _dummy; int res; void *user_data; };
struct io_uring_sqe { int _dummy; };
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC  0
#endif
#define io_uring_for_each_cqe(ring, head, cqe) for (int _i_ = 0; _i_ < 0; _i_++)

static inline int io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags) {
    (void)entries; (void)ring; (void)flags; return -1;
}
static inline void io_uring_queue_exit(struct io_uring *ring) { (void)ring; }
static inline void io_uring_submit(struct io_uring *ring) { (void)ring; }
static inline void io_uring_cq_advance(struct io_uring *ring, unsigned n) { (void)ring; (void)n; }
static inline struct io_uring_sqe *io_uring_get_sqe(struct io_uring *ring) { (void)ring; return NULL; }
static inline void io_uring_prep_accept(struct io_uring_sqe *sqe, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags) { (void)sqe; (void)fd; (void)addr; (void)addrlen; (void)flags; }
static inline void io_uring_prep_recv(struct io_uring_sqe *sqe, int fd, void *buf, unsigned len, int flags) { (void)sqe; (void)fd; (void)buf; (void)len; (void)flags; }
static inline void io_uring_sqe_set_data(struct io_uring_sqe *sqe, void *data) { (void)sqe; (void)data; }
static inline void *io_uring_cqe_get_data(const struct io_uring_cqe *cqe) { (void)cqe; return NULL; }

#define PLATFORM_EPOLL_FD int epoll_fd

#else
#error "Unsupported platform"
#endif

#endif
