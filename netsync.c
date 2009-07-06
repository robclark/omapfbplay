/*
    Copyright (C) 2009 Mans Rullgard

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use, copy,
    modify, merge, publish, distribute, sublicense, and/or sell copies
    of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "timer.h"

/*
 * Protocol:
 *
 * protocol_version     u8
 * type                 u8
 * seq_no               u8
 * if (type == 0)
 *     hello()
 * else if (type == 1)
 *     ready()
 * else if (type == 2)
 *     go()
 * else if (type == 3)
 *     ping()
 * else if (type == 4)
 *     pong()
 *
 * go() {
 *     start_time       u32[2]
 * }
 *
 * ping() {
 *     local_time       u32[2]
 *     rtt              u32
 * }
 *
 * pong() {
 *     in_reply_to      u32[2]
 * }
 *
 */

#define MSG_SIZE 15

#define MSG_TYPE_HELLO 0
#define MSG_TYPE_READY 1
#define MSG_TYPE_GO    2
#define MSG_TYPE_PING  3
#define MSG_TYPE_PONG  4

#define MSG_TYPE_LAST  MSG_TYPE_PONG

#define PING_INTERVAL 1000

struct netsync_msg {
    uint8_t type;
    uint8_t seqno;
    struct timespec time;
    unsigned rtt;
};

static struct slave {
    struct sockaddr addr;
    socklen_t addrlen;
    unsigned rtt;
    unsigned seqno;
} *slaves;

static unsigned num_slaves;
static unsigned seen_slaves;
static unsigned ready_slaves;

static struct timespec start_time;
static struct timespec ping_time_local;
static struct timespec ping_time_master;
static unsigned rtt;
static unsigned ping_count;

static int sockfd;

static pthread_mutex_t ns_lock;
static pthread_cond_t ns_cond;
static sem_t sleep_sem;

static pthread_t ns_thread;

static int ns_stop;

static inline unsigned
get_be32(const void *p)
{
    const struct {
        uint32_t v;
    }  __attribute__((packed)) *pp = p;
    return ntohl(pp->v);
}

static inline void
put_be32(void *p, unsigned v)
{
    struct {
        uint32_t v;
    }  __attribute__((packed)) *pp = p;
    pp->v = htonl(v);
}

static int
unpack_msg(struct netsync_msg *msg, const uint8_t buf[MSG_SIZE])
{
    if (buf[0]) {
        fprintf(stderr, "netsync: bad protocol version %d\n", buf[0]);
        return -1;
    }

    msg->type = buf[1];
    if (msg->type > MSG_TYPE_LAST) {
        fprintf(stderr, "netsync: invalid message type %d\n", msg->type);
        return -1;
    }

    msg->seqno = buf[2];

    if (msg->type >= MSG_TYPE_GO) {
        msg->time.tv_sec  = get_be32(buf + 3);
        msg->time.tv_nsec = get_be32(buf + 7);
    }

    if (msg->type == MSG_TYPE_PING)
        msg->rtt = get_be32(buf + 11);

    return 0;
}

static unsigned
pack_msg(const struct netsync_msg *msg, uint8_t buf[MSG_SIZE])
{
    unsigned len = 3;

    buf[0] = 0;
    buf[1] = msg->type;
    buf[2] = msg->seqno;

    if (msg->type >= MSG_TYPE_GO) {
        put_be32(buf + 3, msg->time.tv_sec);
        put_be32(buf + 7, msg->time.tv_nsec);
        len += 8;
    }

    if (msg->type == MSG_TYPE_PING) {
        put_be32(buf + 11, msg->rtt);
        len += 4;
    }

    return len;
}

static int
cmp_addr(const struct sockaddr *addr1, const struct sockaddr *addr2)
{
    const struct sockaddr_in *in1, *in2;

    if (addr1->sa_family != addr2->sa_family)
        return 1;

    if (addr1->sa_family != AF_INET)
        return 1;

    in1 = (const struct sockaddr_in *)addr1;
    in2 = (const struct sockaddr_in *)addr2;

    return in1->sin_addr.s_addr != in2->sin_addr.s_addr ||
        in1->sin_port != in2->sin_port;
}

static struct slave *
find_slave(struct sockaddr *addr, socklen_t addrlen)
{
    int i;

    for (i = 0; i < seen_slaves; i++)
        if (!cmp_addr(addr, &slaves[i].addr))
            break;

    if (i == seen_slaves && i < num_slaves) {
        fprintf(stderr, "netsync: new slave found\n");
        slaves[i].addr = *addr;
        slaves[i].addrlen = addrlen;
        seen_slaves++;
    }

    return i < seen_slaves? &slaves[i]: NULL;
}

static int
send_msg(const struct netsync_msg *msg, const struct sockaddr *addr,
         socklen_t addrlen)
{
    uint8_t buf[MSG_SIZE];
    unsigned len;

    len = pack_msg(msg, buf);
    return sendto(sockfd, buf, len, 0, addr, addrlen);
}

static int
send_slave_msg(struct netsync_msg *msg, struct slave *s)
{
    msg->seqno = s->seqno++;
    return send_msg(msg, &s->addr, s->addrlen);
}

static void
bcast_msg(const struct netsync_msg *msg)
{
    uint8_t buf[MSG_SIZE];
    unsigned len;
    int i;

    len = pack_msg(msg, buf);

    for (i = 0; i < num_slaves; i++) {
        buf[2] = slaves[i].seqno++;
        sendto(sockfd, buf, len, 0, &slaves[i].addr, slaves[i].addrlen);
    }
}

static int
netsync_recv(int fd, struct netsync_msg *msg, struct timespec *rtime,
             struct sockaddr *addr, socklen_t *addrlen)
{
    uint8_t buf[MSG_SIZE];
    int n;

    n = recvfrom(sockfd, buf, sizeof(buf), 0, addr, addrlen);
    if (n < 0 && errno != EAGAIN)
        return -1;
    if (n <= 0)
        return 0;

    clock_gettime(CLOCK_REALTIME, rtime);

    if (unpack_msg(msg, buf))
        return 0;

    return 1;
}

static void
master_recv(void)
{
    struct netsync_msg msg;
    struct sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    struct timespec rtime;

    while (netsync_recv(sockfd, &msg, &rtime, &addr, &addrlen) > 0) {
        struct slave *s = find_slave(&addr, addrlen);
        if (!s)
            continue;

        switch (msg.type) {
        case MSG_TYPE_HELLO:
            send_slave_msg(&msg, s);
            break;

        case MSG_TYPE_READY:
            pthread_mutex_lock(&ns_lock);
            ready_slaves++;
            pthread_cond_broadcast(&ns_cond);
            pthread_mutex_unlock(&ns_lock);
            break;

        case MSG_TYPE_PONG:
            s->rtt = ts_diff_ns(&rtime, &msg.time);
            break;
        }
    }
}

static void
ping_slave(struct slave *s)
{
    struct netsync_msg msg;

    msg.type = MSG_TYPE_PING;
    clock_gettime(CLOCK_REALTIME, &msg.time);
    msg.rtt = s->rtt;

    send_slave_msg(&msg, s);
}

static void *
netsync_master(void *p)
{
    struct pollfd pfd = { sockfd, POLLIN };
    int next_ping = 0;
    int n;

    fprintf(stderr, "netsync: master starting\n");

    while (!ns_stop && (n = poll(&pfd, 1, PING_INTERVAL)) >= 0) {
        if (n) {
            master_recv();
            continue;
        }

        ping_slave(&slaves[next_ping]);
        if (++next_ping == num_slaves)
            next_ping = 0;
    }

    return NULL;
}

static void *
netsync_slave(void *p)
{
    struct netsync_msg msg = {};
    struct pollfd pfd = { sockfd, POLLIN };

    fprintf(stderr, "netsync: slave starting\n");

    msg.type = MSG_TYPE_HELLO;
    send_msg(&msg, NULL, 0);

    while (poll(&pfd, 1, 1000) >= 0 && !ns_stop) {
        struct timespec rtime;

        if (netsync_recv(sockfd, &msg, &rtime, NULL, 0) <= 0)
            continue;


        switch (msg.type) {
        case MSG_TYPE_GO:
            pthread_mutex_lock(&ns_lock);
            start_time = msg.time;
            pthread_cond_broadcast(&ns_cond);
            pthread_mutex_unlock(&ns_lock);
            break;

        case MSG_TYPE_PING:
            ping_time_local  = rtime;
            ping_time_master = msg.time;
            rtt = msg.rtt;

            msg.type = MSG_TYPE_PONG;
            send_msg(&msg, NULL, 0);

            pthread_mutex_lock(&ns_lock);
            ping_count++;
            pthread_cond_broadcast(&ns_cond);
            pthread_mutex_unlock(&ns_lock);
            break;
        }
    }

    return NULL;
}

static int
netsync_open(const char *arg)
{
    char *host = NULL;
    unsigned port = 0;
    const char *p = arg;
    char *q;
    int len;

    if (!arg)
        goto argerr;

    while ((len = strcspn(p, " ,;")) > 0) {
        int c = p[0];

        if (p[1] != '=')
            goto argerr;

        p += 2;
        len -= 2;

        switch (c) {
        case 's':
            num_slaves = strtol(p, NULL, 0);
            break;
        case 'm':
            host = malloc(len + 1);
            if (!host)
                goto err;
            q = memccpy(host, p, ':', len);
            host[len] = 0;
            if (q) {
                q[-1] = 0;
                port = strtol(p + (q - host), NULL, 0);
            }
            break;
        case 'p':
            port = strtol(p, NULL, 0);
            break;
        default:
            goto argerr;
        }

        p += len + !!p[len];
    }

    if (!port || (!num_slaves && !host))
        goto argerr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == -1)
        goto err;

    if (num_slaves) {
        struct sockaddr_in addr;

        slaves = calloc(num_slaves, sizeof(*slaves));
        if (!slaves)
            goto err;

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    } else {
        struct hostent *hent = gethostbyname(host);
        struct sockaddr_in addr;

        if (!hent)
            goto err;

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = *(uint32_t*)hent->h_addr_list[0];
        addr.sin_port = htons(port);

        if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)))
            goto err;
    }

    fcntl(sockfd, F_SETFL, (int)O_NONBLOCK);

    pthread_mutex_init(&ns_lock, NULL);
    pthread_cond_init(&ns_cond, NULL);
    sem_init(&sleep_sem, 0, 0);

    ns_stop = 0;

    pthread_create(&ns_thread, NULL,
                   slaves? netsync_master: netsync_slave, NULL);

    free(host);
    return 0;

argerr:
    fprintf(stderr, "netsync: params: s=slaves p=port | m=host:port\n");
err:
    free(slaves);
    free(host);
    return -1;
}

static int
netsync_start(struct timespec *ts)
{
    struct netsync_msg msg;

    if (slaves) {
        pthread_mutex_lock(&ns_lock);
        while (ready_slaves < num_slaves)
            pthread_cond_wait(&ns_cond, &ns_lock);
        pthread_mutex_unlock(&ns_lock);

        msg.type = MSG_TYPE_GO;
        clock_gettime(CLOCK_REALTIME, &msg.time);
        msg.time.tv_sec++;

        bcast_msg(&msg);
        *ts = msg.time;
    } else {
        pthread_mutex_lock(&ns_lock);
        while (ping_count < 10)
            pthread_cond_wait(&ns_cond, &ns_lock);
        pthread_mutex_unlock(&ns_lock);

        msg.type = MSG_TYPE_READY;
        send_msg(&msg, NULL, 0);

        pthread_mutex_lock(&ns_lock);
        while (!start_time.tv_sec)
            pthread_cond_wait(&ns_cond, &ns_lock);
        pthread_mutex_unlock(&ns_lock);

        *ts = start_time;
    }

    return 0;
}

static int
netsync_read(struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);

    if (!slaves) {
        ts_sub(ts, &ping_time_local);
        ts->tv_nsec += rtt / 2;
        ts_add(ts, &ping_time_master);
    }

    return 0;
}

static int
netsync_wait(struct timespec *ts)
{
    struct timespec nt = *ts;

    if (!slaves) {
        ts_sub(&nt, &ping_time_master);
        nt.tv_nsec += rtt / 2;
        ts_add(&nt, &ping_time_local);
    }

    sem_timedwait(&sleep_sem, &nt);
    return 0;
}

static int
netsync_close(void)
{
    ns_stop = 1;
    pthread_join(ns_thread, NULL);

    close(sockfd);
    free(slaves);

    pthread_mutex_destroy(&ns_lock);
    pthread_cond_destroy(&ns_cond);
    sem_destroy(&sleep_sem);

    return 0;
}

TIMER(netsync) = {
    .name  = "netsync",
    .open  = netsync_open,
    .start = netsync_start,
    .read  = netsync_read,
    .wait  = netsync_wait,
    .close = netsync_close,
};
