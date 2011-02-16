#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "st.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "st_ssl.h"
#include <zlib.h>

union address_u {
    struct sockaddr sa;
    struct sockaddr_in sa_in;
    struct sockaddr_in6 sa_in6;
    struct sockaddr_storage sa_stor;
};
typedef union address_u address_t;

#define ADDRESS_PORT(addr) ((addr).sa.sa_family == AF_INET ? (addr).sa_in.sin_port : (addr).sa_in6.sin6_port)
#define ADDRESS_STRING(addr, buf, size) \
    ((addr).sa.sa_family == AF_INET ? \
        inet_ntop((addr).sa.sa_family, &(addr).sa_in.sin_addr, buf, size) : \
        inet_ntop((addr).sa.sa_family, &(addr).sa_in6.sin6_addr, buf, size))

/* addr_s is compact version of sockaddr_in[6] for sending over network */
struct addr_s {
    u_int16_t family;
    u_int16_t port;
    union {
        struct in_addr in4;
        struct in6_addr in6;
    } addr;
} __attribute__((__packed__));
typedef struct addr_s addr_t;

#define ADDR_STRING(a, buf, size) \
    ((a).family == AF_INET ? \
        inet_ntop((a).family, &(a).addr.in4, buf, size) : \
        inet_ntop((a).family, &(a).addr.in6, buf, size))

static void address_to_addr(address_t *a, addr_t *b) {
    if (a->sa.sa_family == AF_INET) {
        b->family = a->sa_in.sin_family;
        b->port = a->sa_in.sin_port;
        b->addr.in4 = a->sa_in.sin_addr;
    } else if (a->sa.sa_family == AF_INET6) {
        b->family = a->sa_in6.sin6_family;
        b->port = a->sa_in6.sin6_port;
        b->addr.in6 = a->sa_in6.sin6_addr;
    }
}

static void addr_to_address(addr_t *a, address_t *b) {
    memset(b, 0, sizeof(address_t));
    if (a->family == AF_INET) {
        b->sa_in.sin_family = a->family;
        b->sa_in.sin_port = a->port;
        b->sa_in.sin_addr = a->addr.in4;
    } else if (a->family == AF_INET6) {
        b->sa_in6.sin6_family = a->family;
        b->sa_in6.sin6_port = a->port;
        b->sa_in6.sin6_addr = a->addr.in6;
    }
}

static gboolean addr_match(gconstpointer a_, gconstpointer b_) {
    /* not strict equal, will match IN[6]ADDR_ANY with any addr */
    addr_t *a = (addr_t *)a_;
    addr_t *b = (addr_t *)b_;
    if ((a->family == a->family) && (a->port == a->port)) {
        if (a->family == AF_INET) {
            if (a->addr.in4.s_addr == b->addr.in4.s_addr) return TRUE;
            if (a->addr.in4.s_addr == INADDR_ANY) return TRUE;
            if (b->addr.in4.s_addr == INADDR_ANY) return TRUE;
        } else if (a->family == AF_INET6) {
            if (memcmp(&a->addr.in6, &b->addr.in6, sizeof(struct in6_addr)) == 0) return TRUE;
            if (memcmp(&a->addr.in6, &in6addr_any, sizeof(struct in6_addr)) == 0) return TRUE;
            if (memcmp(&b->addr.in6, &in6addr_any, sizeof(struct in6_addr)) == 0) return TRUE;
        }
    }
    return FALSE;
}


enum packet_flag_e {
    TUN_FLAG_CLOSE = 1,
    TUN_FLAG_COMPRESSED = 2,
};

struct packet_header_s {
    /* local addr */
    addr_t laddr;
    /* remote addr */
    addr_t raddr;
    /* packet size and data */
    u_int32_t flags;
    u_int16_t size;
} __attribute__((__packed__));
#define PACKET_HEADER_SIZE sizeof(struct packet_header_s)
#define PACKET_DATA_SIZE (4*1024)

struct packet_s {
    struct packet_header_s hdr;
    char buf[PACKET_DATA_SIZE];
} __attribute__((__packed__));

static void packet_free(gpointer data) {
    g_slice_free(struct packet_s, data);
}

/* TODO: replace server_s */
struct server_s {
    st_netfd_t nfd;
    void *(*start)(void *arg);

    GHashTable *connections;
    GAsyncQueue *read_queue;
    GAsyncQueue *write_queue;
    int read_fd; /* used to notify when read_queue becomes not empty */
    int write_fd; /* used to notify when write queue becomes not empty */

    addr_t listen_addr;
    addr_t remote_addr; /* remote address to send packets to on the other side of tunnel */
    addr_t tunnel_addr; /* address of remote tunnel */
    int tunnel_secure; /* TODO: maybe this should be a mask of modes? */

    st_thread_t listen_sthread;
    st_thread_t write_sthread;
};
typedef struct server_s server_t;

server_t *server_new(void) {
    int sockets[2];
    int status;

    server_t *s = g_slice_new0(server_t);
    status = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    g_assert(status ==  0);
    s->write_fd = sockets[0];
    s->read_fd = sockets[1];
    s->read_queue = g_async_queue_new_full(packet_free);
    s->write_queue = g_async_queue_new_full(packet_free);
    s->connections = g_hash_table_new(g_int_hash, addr_match);
    return s;
}

/* globals */
static GHashTable *netmap;
static GHashTable *tunmap;
static server_t *tunnel_server;
static const gchar *tunnel_cert_path;
static const gchar *tunnel_pkey_path;

void queue_push_notify(int fd, GAsyncQueue *q, gpointer data) {
    g_async_queue_lock(q);
    int len = g_async_queue_length_unlocked(q);
    g_async_queue_push_unlocked(q, data);
    g_async_queue_unlock(q);
    if (len == 0) {
        int n = write(fd, (void*)"\x01", 1);
        if (n != 1) {
            perror("queue push notify write failed?!?!?!");
            abort();
        }
    }
}

static ssize_t packet_bio_write(z_streamp strm, BIO *bio, struct packet_s *p) {
    char buf[PACKET_DATA_SIZE*2];
    ssize_t nw = 0;
    //g_debug("bio write: %zu", p->hdr.size);
    if (p->hdr.size) {
        strm->next_in = (Bytef *)p->buf;
        strm->avail_in = p->hdr.size;
        strm->next_out = (Bytef *)buf;
        strm->avail_out = sizeof(buf);
        int status = deflate(strm, Z_FINISH);
        g_assert(status == Z_STREAM_END);
        if (strm->total_out < p->hdr.size) {
            g_debug("< deflate total out: %lu/%u saved %.2f%%",
                strm->total_out,
                p->hdr.size,
                (1.0 - (strm->total_out / (float)p->hdr.size)) * 100.0);
            p->hdr.flags |= TUN_FLAG_COMPRESSED;
            p->hdr.size = strm->total_out;
            memcpy(p->buf, buf, strm->total_out);
        }
        nw = BIO_write(bio, p, PACKET_HEADER_SIZE+p->hdr.size);
        deflateReset(strm);
    } else {
        nw = BIO_write(bio, p, PACKET_HEADER_SIZE+p->hdr.size);
    }
    return nw;
}

static ssize_t bio_read_fully(BIO *bio, void *buf, size_t size) {
    ssize_t pos = 0;
    size_t left = size;
    while ((size_t)pos != size) {
        ssize_t nr = BIO_read(bio, &((char *)buf)[pos], left);
        if (nr > 0) {
            pos += nr;
            left -= nr;
        } else {
            pos = nr;
            break;
        }
    }
    return pos;
}

static ssize_t packet_bio_read(z_streamp strm, BIO *bio, struct packet_s *p) {
    //g_debug("bio read");
    ssize_t nr = bio_read_fully(bio, p, PACKET_HEADER_SIZE);
    if (nr <= 0) return -1;
    if (p->hdr.size) {
        nr = bio_read_fully(bio, p->buf, p->hdr.size);
        if (nr != p->hdr.size) return -1;
    } else {
        // 0 bytes read because packet payload was empty
        nr = 0;
    }
    //g_debug("bio read hdr size: %zd", nr);
    if (p->hdr.flags & TUN_FLAG_COMPRESSED) {
        char buf[PACKET_DATA_SIZE];
        strm->next_in = (Bytef *)p->buf;
        strm->avail_in = p->hdr.size;
        strm->next_out = (Bytef *)buf;
        strm->avail_out = sizeof(buf);
        int status = inflate(strm, Z_FINISH);
        //g_debug("status: %d msg: %s", status, strm->msg);
        g_assert(status == Z_STREAM_END);
        g_debug("< inflate %lu/%zu", strm->total_out, p->hdr.size);
        p->hdr.flags &= TUN_FLAG_COMPRESSED;
        p->hdr.size = strm->total_out;
        memcpy(p->buf, buf, p->hdr.size);
        inflateReset(strm);
        return p->hdr.size;
    }
    return nr;
}

static ssize_t packet_write(z_streamp strm, st_netfd_t nfd, struct packet_s *p) {
    char buf[PACKET_DATA_SIZE*2];
    ssize_t nw = 0;
    if (p->hdr.size) {
        strm->next_in = (Bytef *)p->buf;
        strm->avail_in = p->hdr.size;
        strm->next_out = (Bytef *)buf;
        strm->avail_out = sizeof(buf);
        int status = deflate(strm, Z_FINISH);
        g_assert(status == Z_STREAM_END);
        if (strm->total_out < p->hdr.size) {
            g_debug("< deflate total out: %lu/%zu", strm->total_out, p->hdr.size);
            p->hdr.flags |= TUN_FLAG_COMPRESSED;
            p->hdr.size = strm->total_out;
            struct iovec iov[2];
            iov[0].iov_base = &p->hdr;
            iov[0].iov_len = PACKET_HEADER_SIZE;
            iov[1].iov_base = buf;
            iov[1].iov_len = strm->total_out;
            nw = st_writev(nfd, iov, 2, ST_UTIME_NO_TIMEOUT);
        } else {
            nw = st_write(nfd, p, PACKET_HEADER_SIZE+p->hdr.size, ST_UTIME_NO_TIMEOUT);
        }
        deflateReset(strm);
    } else {
        nw = st_write(nfd, p, PACKET_HEADER_SIZE+p->hdr.size, ST_UTIME_NO_TIMEOUT);
    }
    return nw;
}

static ssize_t packet_read(z_streamp strm, st_netfd_t nfd, struct packet_s *p) {
    ssize_t nr = st_read_fully(nfd, p, PACKET_HEADER_SIZE, ST_UTIME_NO_TIMEOUT);
    if (nr <= 0) return -1;
    nr = st_read_fully(nfd, p->buf, p->hdr.size, ST_UTIME_NO_TIMEOUT);
    if (nr != p->hdr.size) return -1;
    if (p->hdr.flags & TUN_FLAG_COMPRESSED) {
        char buf[PACKET_DATA_SIZE];
        strm->next_in = (Bytef *)p->buf;
        strm->avail_in = p->hdr.size;
        strm->next_out = (Bytef *)buf;
        strm->avail_out = sizeof(buf);
        int status = inflate(strm, Z_FINISH);
        //g_debug("status: %d msg: %s", status, strm->msg);
        g_assert(status == Z_STREAM_END);
        g_debug("< inflate %lu/%zu", strm->total_out, p->hdr.size);
        p->hdr.flags &= TUN_FLAG_COMPRESSED;
        p->hdr.size = strm->total_out;
        memcpy(p->buf, buf, p->hdr.size);
        inflateReset(strm);
        return p->hdr.size;
    }
    return nr;
}

/* pass state info to tunnel_out_read_sthread */
struct tun_out_s {
    server_t *s;
    addr_t laddr;
};

static void *tunnel_out_read_sthread(void *arg) {
    struct tun_out_s *to = (struct tun_out_s *)arg;
    server_t *s = to->s;
    addr_t *laddr = &to->laddr;
    address_t remote_addr;
    socklen_t slen;
    int status;

    st_netfd_t client_nfd = g_hash_table_lookup(s->connections, laddr);
    g_assert(client_nfd);

    slen = sizeof(remote_addr);
    status = getpeername(st_netfd_fileno(client_nfd), &remote_addr.sa, &slen);
    g_assert(status == 0);

    char addrbuf[INET6_ADDRSTRLEN];
    g_message("new out tunnel remote peer: %s:%u",
        ADDRESS_STRING(remote_addr, addrbuf, sizeof(addrbuf)),
        ntohs(ADDRESS_PORT(remote_addr)));
    g_message("local peer addr: %s:%u",
        ADDR_STRING(*laddr, addrbuf, sizeof(addrbuf)),
        ntohs(laddr->port));

    g_debug("client: %p (%d)",
        (void*)client_nfd, st_netfd_fileno(client_nfd));

    for (;;) {
        struct packet_s *p = g_slice_new0(struct packet_s);
        ssize_t nr = st_read(client_nfd, p->buf, sizeof(p->buf), ST_UTIME_NO_TIMEOUT);
        if (nr <= 0) { g_slice_free(struct packet_s, p); break; }
        if (!g_hash_table_lookup(s->connections, laddr)) {
            g_message("client not found in connection table");
            /* client has been removed from table. probably got close packet */
            g_slice_free(struct packet_s, p);
            break;
        }
        memcpy(&p->hdr.laddr, laddr, sizeof(addr_t));
        address_to_addr(&remote_addr, &p->hdr.raddr);
        p->hdr.size = nr;
        queue_push_notify(s->read_fd, s->read_queue, p);
    }

    g_message("closing out tunnel connection: %s:%u",
        ADDRESS_STRING(remote_addr, addrbuf, sizeof(addrbuf)),
        ntohs(ADDRESS_PORT(remote_addr)));
    /* if the connection isnt found, it was likely closed by the other side first */
    if (g_hash_table_remove(s->connections, laddr)) {
        /* push empty packet to notify remote end of close */
        struct packet_s *p = g_slice_new0(struct packet_s);
        memcpy(&p->hdr.laddr, laddr, sizeof(addr_t));
        address_to_addr(&remote_addr, &p->hdr.raddr);
        p->hdr.flags |= TUN_FLAG_CLOSE;
        queue_push_notify(s->read_fd, s->read_queue, p);
    }

    g_slice_free(struct tun_out_s, to);
    st_netfd_close(client_nfd);
    return NULL;
}

static void *tunnel_out_thread(void *arg) {
    server_t *s = (server_t *)arg;
    st_init();

    struct pollfd pds[1];
    pds[0].fd = s->read_fd;
    pds[0].events = POLLIN;
    for (;;) {
        pds[0].revents = 0;
        if (st_poll(pds, 1, ST_UTIME_NO_TIMEOUT) <= 0) break;

        if (pds[0].revents & POLLIN) {
            char tmp[1];
            int n = read(s->read_fd, tmp, 1);
            g_assert(n == 1);
            struct packet_s *p;
            //char laddrbuf[INET6_ADDRSTRLEN];
            //char raddrbuf[INET6_ADDRSTRLEN];
            while ((p = g_async_queue_try_pop(s->write_queue))) {
                //g_debug("packet out write queue local: %s:%u remote: %s:%u size: %u",
                //    ADDR_STRING(p->hdr.laddr, laddrbuf, sizeof(laddrbuf)), ntohs(p->hdr.laddr.port),
                //    ADDR_STRING(p->hdr.raddr, raddrbuf, sizeof(raddrbuf)), ntohs(p->hdr.raddr.port),
                //    p->hdr.size);
                st_netfd_t client_nfd = g_hash_table_lookup(s->connections, &p->hdr.laddr);
                if ((p->hdr.flags & TUN_FLAG_CLOSE) && client_nfd) {
                    g_message("got close flag packet. removing tunnel out client: %p (%d)",
                        (void*)client_nfd, st_netfd_fileno(client_nfd));
                    g_hash_table_remove(s->connections, &p->hdr.laddr);
                } else if (client_nfd) {
                    ssize_t nw = st_write(client_nfd, p->buf, p->hdr.size, ST_UTIME_NO_TIMEOUT);
                    if (nw <= 0) { g_warning("write failed"); }
                } else if (!(p->hdr.flags & TUN_FLAG_CLOSE)) {
                    g_message("tunnel out client not found, creating one");
                    address_t rmt_addr;
                    int sock;
                    st_netfd_t rmt_nfd;

                    addr_to_address(&p->hdr.raddr, &rmt_addr);
                    /* Connect to remote host */
                    if ((sock = socket(rmt_addr.sa.sa_family, SOCK_STREAM, 0)) < 0) {
                        goto done;
                    }
                    if ((rmt_nfd = st_netfd_open_socket(sock)) == NULL) {
                        close(sock);
                        goto done;
                    }
                    if (st_connect(rmt_nfd, (struct sockaddr *)&rmt_addr,
                          sizeof(rmt_addr), ST_UTIME_NO_TIMEOUT) == 0) {
                        g_message("connected to remote host!");
                        struct tun_out_s *to = g_slice_new0(struct tun_out_s);
                        to->s = s;
                        memcpy(&to->laddr, &p->hdr.laddr, sizeof(addr_t));
                        g_hash_table_insert(s->connections, &to->laddr, rmt_nfd);

                        ssize_t nw = st_write(rmt_nfd, p->buf, p->hdr.size, ST_UTIME_NO_TIMEOUT);
                        g_debug("%zd bytes written to tunnel out client", nw);
                        st_thread_t t = st_thread_create(tunnel_out_read_sthread, to, 0, 4*1024);
                        g_assert(t);
                    } else {
                        g_message("connection to remote host failed. notify client through tunnel.");
                        struct packet_s *rp = g_slice_new0(struct packet_s);
                        memcpy(&rp->hdr.laddr, &p->hdr.laddr, sizeof(addr_t));
                        memcpy(&rp->hdr.raddr, &p->hdr.raddr, sizeof(addr_t));
                        rp->hdr.flags |= TUN_FLAG_CLOSE;
                        queue_push_notify(s->read_fd, s->read_queue, rp);
                    }
                } else {
                    g_message("no client found, dropping packet");
                }
                g_slice_free(struct packet_s, p);
            }
        }
    }
done:
    g_message("exiting tunnel out thread!!");
    st_thread_exit(NULL);
    g_warn_if_reached();
    return NULL;
}



static void *tunnel_handler(void *arg) {
    st_netfd_t client_nfd = (st_netfd_t)arg;
    BIO *bio_nfd = BIO_new_netfd2(client_nfd, BIO_CLOSE);
    address_t local_addr;
    int status;
    struct pollfd pds[2];
    socklen_t slen;

    server_t *s = NULL;
    SSL_CTX *ctx = NULL;
    BIO *bio_ssl = NULL;
    BIO *bio = bio_nfd;

    if (tunnel_cert_path && tunnel_pkey_path) {
        ctx = SSL_CTX_new(SSLv23_server_method());

        // http://h71000.www7.hp.com/doc/83final/ba554_90007/ch04s03.html
        if (!SSL_CTX_use_certificate_file(ctx, tunnel_cert_path, SSL_FILETYPE_PEM)) {
            g_warning("error loading cert file");
            ERR_print_errors_fp(stderr);
            goto done;
        }
        if (!SSL_CTX_use_PrivateKey_file(ctx, tunnel_pkey_path, SSL_FILETYPE_PEM)) {
            g_warning("error loading private key file");
            ERR_print_errors_fp(stderr);
            goto done;
        }

        bio_ssl = BIO_new_ssl(ctx, 0);
        bio = BIO_push(bio_ssl, bio_nfd);

        if (BIO_do_handshake(bio_ssl) <= 0) {
            g_warning("Error establishing SSL with accept socket");
            ERR_print_errors_fp(stderr);
            goto done;
        }

        g_message("SSL handshake with client done");
    }

    z_stream zso;
    memset(&zso, 0, sizeof(zso));
    status = deflateInit2(&zso, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
    g_assert(status == Z_OK);
    g_debug("default bound: %lu", deflateBound(&zso, PACKET_DATA_SIZE));

    z_stream zsi;
    memset(&zsi, 0, sizeof(zsi));
    status = inflateInit2(&zsi, -15);
    g_assert(status == Z_OK);

    slen = sizeof(local_addr);
    status = getpeername(st_netfd_fileno(client_nfd), &local_addr.sa, &slen);
    g_assert(status == 0);

    s = server_new();
    /* use remote_addr to store the peer address since it is unused */
    address_to_addr(&local_addr, &s->remote_addr);
    g_thread_create(tunnel_out_thread, s, TRUE, NULL);
    g_hash_table_insert(tunmap, &s->remote_addr, s);

    pds[0].fd = st_netfd_fileno(client_nfd);
    pds[0].events = POLLIN;
    pds[1].fd = s->write_fd;
    pds[1].events = POLLIN;

    for (;;) {
        pds[0].revents = 0;
        pds[1].revents = 0;
        if (st_poll(pds, 2, ST_UTIME_NO_TIMEOUT) <= 0) break;

        if (pds[0].revents & POLLIN) {
            do {
                //g_debug("tunnel server read");
                struct packet_s *p = g_slice_new(struct packet_s);
                //ssize_t nr = packet_read(&zsi, client_nfd, p);
                ssize_t nr = packet_bio_read(&zsi, bio, p);
                if (nr < 0) { g_slice_free(struct packet_s, p); break; }
                //g_debug("tunnel slave read %zd out of %d", nr, p->hdr.size);
                //g_debug("bio pending: %zd", BIO_ctrl_pending(bio));
                queue_push_notify(s->write_fd, s->write_queue, p);
                /* BIO ssl seems to buffer data, so the loop with 
                 * BIO_ctrl_pending will help avoid
                 * getting stuck in st_poll while data is buffered
                 */
            } while (BIO_ctrl_pending(bio));
        }

        if (pds[1].revents & POLLIN) {
            char tmp[1];
            read(s->write_fd, tmp, 1);
            struct packet_s *p;
            //char laddrbuf[INET6_ADDRSTRLEN];
            //char raddrbuf[INET6_ADDRSTRLEN];
            while ((p = g_async_queue_try_pop(s->read_queue))) {
                //g_debug("tunnel packet local: %s:%u remote: %s:%u size: %u",
                //    ADDR_STRING(p->hdr.laddr, laddrbuf, sizeof(laddrbuf)), ntohs(p->hdr.laddr.port),
                //    ADDR_STRING(p->hdr.raddr, raddrbuf, sizeof(raddrbuf)), ntohs(p->hdr.raddr.port),
                //    p->hdr.size);

                //ssize_t nw = packet_write(&zso, client_nfd, p);
                ssize_t nw = packet_bio_write(&zso, bio, p);
                g_slice_free(struct packet_s, p);
                if (nw <= 0) goto done;
            }
        }
    }
done:
    g_message("exiting tunnel handler!!");
    if (ctx) SSL_CTX_free(ctx);
    BIO_free_all(bio);
    deflateEnd(&zso);
    inflateEnd(&zsi);
    if (s) {
        g_hash_table_remove(tunmap, &s->remote_addr);
        g_slice_free(server_t, s);
    }
    st_netfd_close(client_nfd);
    return NULL;
}

static void *handle_connection(void *arg) {
    st_netfd_t client_nfd = (st_netfd_t)arg;
    address_t listening_addr;
    address_t local_addr;
    socklen_t slen;
    int status;
    gpointer hkey;
    addr_t laddr;

    slen = sizeof(listening_addr);
    status = getsockname(st_netfd_fileno(client_nfd), &listening_addr.sa, &slen);
    g_assert(status == 0);

    slen = sizeof(local_addr);
    status = getpeername(st_netfd_fileno(client_nfd), &local_addr.sa, &slen);
    g_assert(status == 0);

    address_to_addr(&listening_addr, &laddr);
    server_t *s = g_hash_table_lookup(netmap, &laddr);
    g_assert(s);

    hkey = (gpointer)ADDRESS_PORT(local_addr);
    g_hash_table_insert(s->connections, hkey, client_nfd);

    char addrbuf[INET6_ADDRSTRLEN];
    g_message("new peer: %s:%u",
        ADDRESS_STRING(local_addr, addrbuf, sizeof(addrbuf)),
        ntohs(ADDRESS_PORT(local_addr)));

    /* begin by sending a 0 byte pay load packet across tunnel that will cause remote end to open connection */
    struct packet_s *p = g_slice_new0(struct packet_s);
    address_to_addr(&local_addr, &p->hdr.laddr);
    memcpy(&p->hdr.raddr, &s->remote_addr, sizeof(addr_t));
    p->hdr.size = 0;
    queue_push_notify(s->write_fd, s->write_queue, p);

    for (;;) {
        struct packet_s *p = g_slice_new0(struct packet_s);
        ssize_t nr = st_read(client_nfd, p->buf, sizeof(p->buf), ST_UTIME_NO_TIMEOUT);
        if (nr <= 0) { g_slice_free(struct packet_s, p); break; }
        /* TODO: maybe don't do think  translation every time through the loop. could just be a memcpy */
        if (!g_hash_table_lookup(s->connections, hkey)) {
            /* connection missing from hash, probably got close packet from tunnel */
            g_slice_free(struct packet_s, p);
            break;
        }
        address_to_addr(&local_addr, &p->hdr.laddr);
        memcpy(&p->hdr.raddr, &s->remote_addr, sizeof(addr_t));
        p->hdr.size = nr;
        queue_push_notify(s->write_fd, s->write_queue, p);
    }
    g_message("closing peer");
    if (g_hash_table_remove(s->connections, hkey)) {
        /* push empty packet to notify remote end of close */
        struct packet_s *p = g_slice_new0(struct packet_s);
        address_to_addr(&local_addr, &p->hdr.laddr);
        memcpy(&p->hdr.raddr, &s->remote_addr, sizeof(addr_t));
        p->hdr.flags |= TUN_FLAG_CLOSE;
        queue_push_notify(s->write_fd, s->write_queue, p);
    } else {
        g_message("peer connection not found. must have been closed already.");
    }
    st_netfd_close(client_nfd);
    return NULL;
}

static void *tunnel_thread(void *arg) {
    /* connect to remote side of tunnel */
    int status;
    server_t *s = (server_t *)arg;
    st_init();

    z_stream zso;
    memset(&zso, 0, sizeof(zso));
    status = deflateInit2(&zso, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
    g_assert(status == Z_OK);
    g_debug("default bound: %lu", deflateBound(&zso, PACKET_DATA_SIZE));

    z_stream zsi;
    memset(&zsi, 0, sizeof(zsi));
    status = inflateInit2(&zsi, -15);
    g_assert(status == Z_OK);

    address_t rmt_addr;
    int sock;
    //st_netfd_t rmt_nfd;
    BIO *bio_nfd = NULL;

    addr_to_address(&s->tunnel_addr, &rmt_addr);

    SSL_CTX *ctx = NULL;
    BIO *bio = NULL;
    BIO *bio_ssl = NULL;

    /* Connect to remote host */
    if ((sock = socket(rmt_addr.sa.sa_family, SOCK_STREAM, 0)) < 0) {
        close(sock);
        goto done;
    }

    st_netfd_t rmt_nfd;
    bio_nfd = BIO_new_netfd(sock, BIO_CLOSE);
    bio = bio_nfd;
    BIO_get_fp(bio_nfd, &rmt_nfd);

    for (;;) {
        if (st_connect(rmt_nfd, (struct sockaddr *)&rmt_addr,
              sizeof(rmt_addr), ST_UTIME_NO_TIMEOUT) == 0) {
            break;
        }
        g_message("sleeping before reconnecting tunnel");
        st_sleep(1);
    }
    g_message("connected to tunnel!");

    if (s->tunnel_secure) {
        ctx = SSL_CTX_new(SSLv23_client_method());

        bio_ssl = BIO_new_ssl(ctx, 1);
        bio = BIO_push(bio_ssl, bio_nfd);

        if (BIO_do_handshake(bio_ssl) <= 0) {
            g_warning("Error establishing SSL connection");
            ERR_print_errors_fp(stderr);
            goto done;
        }

        g_message("SSL handshake with tunnel done");
    }

    struct pollfd pds[2];
    pds[0].fd = sock;
    pds[0].events = POLLIN;
    pds[1].fd = s->read_fd;
    pds[1].events = POLLIN;
    for (;;) {
        pds[0].revents = 0;
        pds[1].revents = 0;
        if (st_poll(pds, 2, ST_UTIME_NO_TIMEOUT) <= 0) break;

        if (pds[0].revents & POLLIN) {
            do {
                struct packet_s *p = g_slice_new(struct packet_s);
                //ssize_t nr = packet_read(&zsi, rmt_nfd, p);
                ssize_t nr = packet_bio_read(&zsi, bio, p);
                if (nr < 0) { g_slice_free(struct packet_s, p); break; }
                queue_push_notify(s->read_fd, s->read_queue, p);
            /* BIO ssl seems to buffer data, so the loop with 
             * BIO_ctrl_pending will help avoid
             * getting stuck in st_poll while data is buffered
             */
            } while (BIO_ctrl_pending(bio));
        }

        if (pds[1].revents & POLLIN) {
            char tmp[1];
            read(s->read_fd, tmp, 1);
            struct packet_s *p;
            while ((p = g_async_queue_try_pop(s->write_queue))) {
                //char laddrbuf[INET6_ADDRSTRLEN];
                //char raddrbuf[INET6_ADDRSTRLEN];
                //g_debug("packet local: %s:%u remote: %s:%u size: %u",
                //    ADDR_STRING(p->hdr.laddr, laddrbuf, sizeof(laddrbuf)), ntohs(p->hdr.laddr.port),
                //    ADDR_STRING(p->hdr.raddr, raddrbuf, sizeof(raddrbuf)), ntohs(p->hdr.raddr.port),
                //    p->hdr.size);

                //ssize_t nw = packet_write(&zso, rmt_nfd, p);
                ssize_t nw = packet_bio_write(&zso, bio, p);
                g_slice_free(struct packet_s, p);
                if (nw <= 0) goto done;
            }
        }
    }
done:
    g_message("exiting tunnel thread!!");
    if (ctx) SSL_CTX_free(ctx);
    BIO_free_all(bio);
    deflateEnd(&zso);
    inflateEnd(&zsi);
    st_thread_exit(NULL);
    g_warn_if_reached();
    return NULL;
}

static void *accept_loop(void *arg) {
    struct server_s *s = (struct server_s *)arg;
    st_netfd_t client_nfd;
    struct sockaddr_in from;
    int fromlen = sizeof(from);

    for (;;) {
        client_nfd = st_accept(s->nfd,
          (struct sockaddr *)&from, &fromlen, ST_UTIME_NO_TIMEOUT);
        g_message("accepted new connection");
        if (st_thread_create(s->start,
          (void *)client_nfd, 0, 1024 * 1024) == NULL)
        {
            g_critical("st_thread_create error");
        }
    }
    g_warn_if_reached();
    return NULL;
}

static st_thread_t listen_server(server_t *s, void *(*start)(void *arg)) {
    int sock;
    int n;

    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
    }

    n = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0) {
        perror("setsockopt SO_REUSEADDR");
    }

    address_t serv_addr;
    addr_to_address(&s->listen_addr, &serv_addr);
    char addrbuf[INET6_ADDRSTRLEN];
    g_message("binding listening socket to: %s:%u",
        ADDRESS_STRING(serv_addr, addrbuf, sizeof(addrbuf)),
        ntohs(ADDRESS_PORT(serv_addr)));

    if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
    }

    if (listen(sock, 10) < 0) {
        perror("listen");
    }

    s->nfd = st_netfd_open_socket(sock);
    s->start = start;
    return st_thread_create(accept_loop, (void *)s, 0, 4 * 1024);
}

static void *write_in_sthread(void *arg) {
    /* write data coming across the tunnel to the client which initiated the connection */
    server_t *s = (server_t *)arg;
    struct pollfd pds[1];
    pds[0].fd = s->write_fd;
    pds[0].events = POLLIN;
    for (;;) {
        pds[0].revents = 0;
        if (st_poll(pds, 1, ST_UTIME_NO_TIMEOUT) <= 0) break;
        /* TODO: this seems to be breaking, causing the queue size to grow large */

        if (pds[0].revents & POLLIN) {
            //g_debug("read queue notified");
            char tmp[1];
            read(s->write_fd, tmp, 1);
            struct packet_s *p;
            //char laddrbuf[INET6_ADDRSTRLEN];
            //char raddrbuf[INET6_ADDRSTRLEN];
            while ((p = g_async_queue_try_pop(s->read_queue))) {
                //g_debug("packet read queue local: %s:%u remote: %s:%u size: %u",
                //    ADDR_STRING(p->hdr.laddr, laddrbuf, sizeof(laddrbuf)), ntohs(p->hdr.laddr.port),
                //    ADDR_STRING(p->hdr.raddr, raddrbuf, sizeof(raddrbuf)), ntohs(p->hdr.raddr.port),
                //    p->hdr.size);
                uintptr_t port = p->hdr.laddr.port;
                st_netfd_t client_nfd = g_hash_table_lookup(s->connections, (gpointer)port);
                if (p->hdr.flags & TUN_FLAG_CLOSE && client_nfd) {
                    g_message("found peer client, disconnecting");
                    g_hash_table_remove(s->connections, (gpointer)port);
                } else if (client_nfd) {
                    //g_debug("found peer client!");
                    ssize_t nw = st_write(client_nfd, p->buf, p->hdr.size, ST_UTIME_NO_TIMEOUT);
                    //g_debug("%zd bytes written to client", nw);
                    if (nw <= 0) { g_warning("write failed"); }
                } else {
                    g_message("peer client not found");
                }
                g_slice_free(struct packet_s, p);
            }
        }
    }
    return NULL;
}

static int strtoaddr(const char *s, addr_t *a) {
    if (!s) return 0;
    int success = 0;

    gchar *port_str = strrchr(s, ':');
    if (!port_str) return 0;

    *port_str = 0;
    ++port_str;
    a->port = htons((u_int16_t)strtol(port_str, NULL, 0));
    if (a->port == 0) goto done;
    if (inet_pton(AF_INET, s, &a->addr.in4) > 0) {
        a->family = AF_INET;
    } else if (inet_pton(AF_INET6, s, &a->addr.in6) > 0) {
        a->family = AF_INET6;
    } else {
        goto done;
    }

    success=1;
done:
    --port_str;
    *port_str = ':';
    return success;
}

static void parse_config(const gchar *conffile) {
    GKeyFile *kf = g_key_file_new();

    if (!g_key_file_load_from_file(kf, conffile, G_KEY_FILE_NONE, NULL)) {
        g_error("error loading config: [%s]", conffile);
        goto free_key_file;
    }

    gchar *start_group = g_key_file_get_start_group(kf);
    g_debug("start group: %s", start_group);

    /* tunnel listening address */
    gchar *tun_listen_address_str = g_key_file_get_value(kf, "tunnel", "listen_address", NULL);
    g_assert(tun_listen_address_str);
    if (strtoaddr(tun_listen_address_str, &tunnel_server->listen_addr) != 1) {
        g_error("invalid address: %s", tun_listen_address_str);
    }
    g_free(tun_listen_address_str);

    /* tunnel cert */
    tunnel_cert_path = g_key_file_get_value(kf, "tunnel", "cert_file", NULL);
    tunnel_pkey_path = g_key_file_get_value(kf, "tunnel", "private_key_file", NULL);

    gchar **groups = g_key_file_get_groups(kf, NULL);
    gchar *group = NULL;
    for (int i = 0; (group = groups[i]); i++) {
        g_debug("group: %s", group);
        /* if group name starts with route, setup route */
        if (g_strstr_len(group, -1, "route") == group) {
            g_debug("route config found: %s", group);
            gchar *listen_address_str = g_key_file_get_value(kf, group, "listen_address", NULL);
            gchar *remote_address_str = g_key_file_get_value(kf, group, "remote_address", NULL);
            gchar *tunnel_address_str = g_key_file_get_value(kf, group, "tunnel_address", NULL);
            if (!listen_address_str || !remote_address_str || !tunnel_address_str) {
                goto free_address_strings;
            }
            server_t *s = g_slice_new0(server_t);
            s->tunnel_secure = g_key_file_get_boolean(kf, group, "tunnel_secure", NULL);
            if (strtoaddr(listen_address_str, &s->listen_addr) != 1) {
                g_error("invalid address: %s", listen_address_str);
                goto free_server;
            }
            if (strtoaddr(remote_address_str, &s->remote_addr) != 1) {
                g_error("invalid address: %s", remote_address_str);
                goto free_server;
            }
            if (strtoaddr(tunnel_address_str, &s->tunnel_addr) != 1) {
                g_error("invalid address: %s", tunnel_address_str);
                goto free_server;
            }
            char addrbuf[INET6_ADDRSTRLEN];
            g_debug("listening address: %s:%u",
                ADDR_STRING(s->listen_addr, addrbuf, sizeof(addrbuf)),
                ntohs(s->listen_addr.port));
            g_debug("remote address: %s:%u",
                ADDR_STRING(s->remote_addr, addrbuf, sizeof(addrbuf)),
                ntohs(s->remote_addr.port));
            g_hash_table_insert(netmap, &s->listen_addr, s);
            if (0) {
                /* only go here when there is an error */
free_server:
                g_slice_free(server_t, s);
            }
free_address_strings:
            if (listen_address_str) g_free(listen_address_str);
            if (remote_address_str) g_free(remote_address_str);
            if (tunnel_address_str) g_free(tunnel_address_str);
        }
    }
    g_free(start_group);
free_groups:
    g_strfreev(groups);

free_key_file:
    g_key_file_free(kf);
}


static const gchar *conffile = "leytunnel.conf";

static GOptionEntry entires[] = {
    {"config", 'c', 0, G_OPTION_ARG_FILENAME, &conffile, "config file path", NULL},
    {NULL, 0, 0, 0, 0 , NULL, NULL}
};

int main(int argc, char *argv[]) {
    g_thread_init(NULL);

    SSL_load_error_strings();
    SSL_library_init();

    if (st_init() < 0) {
        perror("st_init");
        exit(1);
    }

#if 0
    g_debug("sizeof(addr_t) = %zu", sizeof(addr_t));
    g_debug("sizeof(struct sockaddr) = %zu", sizeof(struct sockaddr));
    g_debug("sizeof(struct sockaddr_in) = %zu", sizeof(struct sockaddr_in));
    g_debug("sizeof(struct sockaddr_in6) = %zu", sizeof(struct sockaddr_in6));
    g_debug("sizeof(struct sockaddr_storage) = %zu", sizeof(struct sockaddr_storage));
    g_debug("sizeof(address_t) = %zu", sizeof(address_t));
#endif

    int sockets[2];
    int status;

    netmap = g_hash_table_new(g_int_hash, addr_match);
    tunmap = g_hash_table_new(g_int_hash, addr_match);

    /* start tunnel listener */
    tunnel_server = g_slice_new0(server_t);

    GError *error = NULL;
    GOptionContext *optctx = g_option_context_new("leytunnel");

    g_option_context_add_main_entries(optctx, entires, NULL);
    if (!g_option_context_parse(optctx, &argc, &argv, &error)) {
        g_error("option parsing failed: %s", error->message);
        exit(1);
    }

    parse_config(conffile);

    /* TODO: should require a mode and either be a tunnel listener or connector */
    tunnel_server->listen_sthread = listen_server(tunnel_server, tunnel_handler);

    /* start port listeners */
    GHashTableIter iter;
    addr_t *listen_addr;
    server_t *s;
    g_hash_table_iter_init(&iter, netmap);
    while (g_hash_table_iter_next(&iter, (gpointer *)&listen_addr, (gpointer *)&s)) {
        status = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
        g_assert(status ==  0);
        s->write_fd = sockets[0];
        s->read_fd = sockets[1];
        s->read_queue = g_async_queue_new_full(packet_free);
        s->write_queue = g_async_queue_new_full(packet_free);
        s->connections = g_hash_table_new(g_direct_hash, g_direct_equal);
        s->listen_sthread = listen_server(s, handle_connection);
        s->write_sthread = st_thread_create(write_in_sthread, s, 0, 4*1024);
        g_thread_create(tunnel_thread, s, TRUE, NULL);
    }

    st_thread_join(tunnel_server->listen_sthread, NULL);

    st_thread_exit(NULL);
    g_warn_if_reached();
    return EXIT_FAILURE;
}
