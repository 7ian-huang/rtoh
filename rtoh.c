/* from system */
#include <stdio.h>
#include <assert.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <sys/time.h>
#include <pwd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* from third */
#include <event.h>

/* from ./ */
#include <rtoh.h>
#include <sysv.h>
#include <info.h>
#include <http_callback.h>
#include <assoc.h>
#include <thread.h>

struct settings settings;
struct event_base *main_base;
struct stats stats;

void on_write_piece(int fd, short event, void *arg);
void on_write_header(int fd, short event, void *arg);
void on_read (int sock, short event, void *arg);
void on_write(int sock, short event, void *arg);
void destory_conn(struct conn *conn);
void init_conn(struct conn *conn, int fd);

/* init defaults */
static void stats_init()
{
    pthread_mutex_init(&stats.lock, NULL);
    stats.curr_items = 0;
    stats.total_items = settings.maxgetters;
    stats.curr_conns = 0;
    stats.total_conns = settings.maxconns;
    stats.get_cmds = 0;
    stats.get_hits = 0;
    stats.get_misses = 0;
    stats.get_stats = 0;
    stats.get_dengerous = 0;
    stats.started = time(0);
    stats.accepted = 0;
}
static void settings_init(void)
{
    settings.maxconns = 4096;
    settings.daemonize = 0;
    settings.tcpport = 1935;
    settings.pidfile = "/tmp/rtoh.pid";
    settings.work_threads = 4;
    settings.total_threads = settings.work_threads + 1; /* main thread is a dispatcher */
    settings.username = "nobody";
    settings.verbose = 0;
    settings.maxgetters = 1024;
}
static void dump_settings(void)
{
    printf("settings.maxconns     = %d\n", settings.maxconns);
    printf("settings.daemonize    = %d\n", settings.daemonize);
    printf("settings.tcpport      = %d\n", settings.tcpport);
    printf("settings.pidfile      = %s\n", settings.pidfile);
    printf("settings.work_threads = %d\n", settings.work_threads);
    printf("settings.total_threads= %d\n", settings.total_threads);
    printf("settings.username     = %s\n", settings.username);
    printf("settings.verbose      = %d\n", settings.verbose);
    printf("settings.maxgetters      = %d\n", settings.maxgetters);
}
static void sig_handler(int sig) 
{
    fprintf(stderr, "%d\n", sig);
    exit(EXIT_SUCCESS);
}

static void usage()
{
    printf("\n\n" PACKAGE " " VERSION "\n\n");
    printf("Usage:\n");
    printf("\t-c <num>   max connections, default is 4096\n"
           "\t-d         run as a daemon\n"
           "\t-p <num>   TCP port number to listen on (default: 1935)\n"
           "\t-P <file>  save PID in <file>, only used with -d option\n"
           "\t-r         maximize core file limit\n"
#ifdef ENABLE_THREADS
           "\t-t <num>   number of threads to use, default 4\n"
#endif
           "\t-u <user>  username to run as\n"
           "\t-v         verbose (print errors/warnings while in event loop)\n"
           "\t-h|?       dump usage\n"
    );
}
static void save_pid(const pid_t pid, const char *pid_file) {
    FILE *fp;
    if (pid_file == NULL)
        return;

    if ((fp = fopen(pid_file, "w")) == NULL) {
        fprintf(stderr, "Could not open the pid file %s for writing\n", pid_file);
        return;
    }

    fprintf(fp,"%ld\n", (long)pid);
    if (fclose(fp) == -1) {
        fprintf(stderr, "Could not close the pid file %s.\n", pid_file);
        return;
    }
}
static void remove_pidfile(const char *pid_file) {
    if (pid_file == NULL)
        return;

    if (unlink(pid_file) != 0) {
        fprintf(stderr, "Could not remove the pid file %s.\n", pid_file);
    }
}

static int setnonblocking(int sock)
{
    int opts;
    opts = fcntl(sock, F_GETFL);
    if(opts<0) {
        perror("fcntl(sock, GETFL)");
        return -1;
    }
    opts = opts|O_NONBLOCK;
    if(fcntl(sock, F_SETFL, opts) < 0) {
        perror("fcntl(sock, SETFL,opts)");
        return -1;
    }
    return 0;
}
static int create_listen_socket()
{
    struct linger ling = {0, 0};
    struct sockaddr_in my_addr;
    int sock = 0;
    int flags = 1;
    int error;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("failed to create socket");
        return -1;
    }

    if (setnonblocking(sock) == -1) {
        fprintf(stderr, "setnonblocking for listen socket!\n");
    }

    error = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&flags, sizeof(flags));
    if (error != 0)
        perror("setsockopt(sock, SOL_SOCKET, SO_REUSEADDR)");
    error = setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
    if (error != 0)
        perror("setsockopt(sock, SOL_SOCKET, SO_REUSEADDR)");
    error = setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
    if (error != 0)
        perror("setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE)");
    error = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
    if (error != 0)
        perror("setsockopt(sock, SOL_SOCKET, SO_LINGER)");

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(settings.tcpport);
    my_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr*)&my_addr, sizeof(struct sockaddr)) < 0) {
        if (errno == EADDRINUSE) {
            fprintf(stderr, "port %d is in use\n", settings.tcpport);
        } else {
            perror("failed to bind");
        }
        close(sock);
        return -1;
    }
    if (listen(sock, 256) == -1) {
        perror("failed to listen");
        close(sock);
        return -1;
    }

    return sock;
}
typedef unsigned int rel_time_t;
volatile rel_time_t current_time;
static struct event clockevent;

static void set_current_time(void) {
    struct timeval timer;

    gettimeofday(&timer, NULL);
    current_time = (rel_time_t) (timer.tv_sec - stats.started);
}

static void clock_handler(const int fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 1, .tv_usec = 0};
    static int initialized = 0;

    if (initialized) {
        /* only delete the event if it's actually there. */
        evtimer_del(&clockevent);
    } else {
        initialized = 1;
    }

    if (settings.verbose >= 3) {
        fprintf(stderr, "clock_handler: %x\n", current_time);
    }

    evtimer_set(&clockevent, clock_handler, 0);
    event_base_set(main_base, &clockevent);
    evtimer_add(&clockevent, &t);
    set_current_time();
}

static int init_item(item *it, struct conn *conn)
{
    bzero(it, sizeof(struct item));
    it->nkey = strlen(conn->streamid_val);
    it->nkey = it->nkey > ITEM_KEY_MAX_LEN ? ITEM_KEY_MAX_LEN : it->nkey;
    memcpy(it->key, conn->streamid_val, it->nkey);
    if (pthread_create(&it->thread_id, NULL, rtmp_getter, (void *)it))
        return -1;

    pthread_mutex_init(&it->lock, NULL);

    return 0;
}
static void rescpy(struct conn *conn, const char *src, size_t len)
{
    if (conn == NULL || src == NULL)
        return;

    conn->write_buf_to_send = conn->write_buf_sent = 0;
    if (conn->write_buf_len < len)
        len = conn->write_buf_len;

    memcpy(conn->write_buf, src, len);
    conn->write_buf_to_send += len;
}
int invalid_param(struct conn *conn) {
    static char *res = INVALID_PARAM;
    int res_len = sizeof(INVALID_PARAM) - 1;
    rescpy(conn, res, res_len);
    return 0;
}
int notfound(struct conn *conn)
{
    static char *res = NOT_FOUND;
    int res_len = sizeof(NOT_FOUND) - 1;
    rescpy(conn, res, res_len);
    return 0;
}
int ok(struct conn *conn)
{
    static char *res = OK;
    int res_len = sizeof(OK) - 1;
    rescpy(conn, res, res_len);
    return 0;
}
int internal_error(struct conn *conn)
{
    static char *res = INTERNAL_ERROR;
    int res_len = sizeof(INTERNAL_ERROR) - 1;
    rescpy(conn, res, res_len);
    return 0;
}
int do_generate_response(item *it, struct conn *conn)
{
/*
 *    http://ip/metainfo/streamid/metaId?rttpVer=
 *    http://ip/metainfo/tn?streamid=&rttpVer=
 *    http://ip/datainfo/streamid/blockId?rttpVer=
 *    wiki: 
 *         http://wiki.letv.cn/pages/viewpage.action?pageId=34153301
 *
 */
    struct piece *p;

    if (!it || !conn)
	    return -1;

    pthread_mutex_lock(&it->lock);
    if (conn->tn_val) {
        if (it->metas.video_head && it->metas.audio_head) {
            int length = 1;
            uint8_t num = 0;
            p = it->metas.video_head;
            while (p) {
                length += p->data_len + sizeof(struct header_piece);
                num += 1;
                p = p->next;
            }
            p = it->metas.audio_head;
            while (p) {
                length += p->data_len + sizeof(struct header_piece);
                num += 1;
                p = p->next;
            }

            int start = it->frs.start;
            int end = it->frs.end;
            struct source_info si;
            si.MIN = htobe64(it->blocks.piece_id_min);
            si.MAX = htonl(it->blocks.piece_id_max - it->blocks.piece_id_min);
            si.CUR = 0;
            si.type = 0;
            si.piecenumofperblock = htons(4);
            si.info_size = 0;
            for ( ; start % (FRAME_LIST_MAX) != end; start ++) {
                if (it->frs.frame_list[start % (FRAME_LIST_MAX)].index != 0) {
                    si.info_size += sizeof(struct frame);
                }
            }
            length += sizeof(struct source_info) + si.info_size;
            si.info_size = htonl(si.info_size);
            conn->write_buf_to_send = sprintf(conn->write_buf, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", length);
            conn->write_buf[conn->write_buf_to_send] = num;
            conn->write_buf_to_send += 1;
            p = it->metas.video_head;
            while (p) {
                memcpy(conn->write_buf + conn->write_buf_to_send, &p->pu, sizeof(struct header_piece));
                conn->write_buf_to_send += sizeof(struct header_piece);
                memcpy(conn->write_buf + conn->write_buf_to_send, p->data, p->data_len);
                conn->write_buf_to_send += p->data_len;
                p = p->next;
            }
            p = it->metas.audio_head;
            while (p) {
                memcpy(conn->write_buf + conn->write_buf_to_send, &p->pu, sizeof(struct header_piece));
                conn->write_buf_to_send += sizeof(struct header_piece);
                memcpy(conn->write_buf + conn->write_buf_to_send, p->data, p->data_len);
                conn->write_buf_to_send += p->data_len;
                p = p->next;
            }
            
            memcpy(conn->write_buf + conn->write_buf_to_send, &si, sizeof(struct source_info));
            conn->write_buf_to_send += sizeof(struct source_info);
            start = it->frs.start;
            end = it->frs.end;
            for ( ; start % (FRAME_LIST_MAX) != end; start ++) {
                if (it->frs.frame_list[start % (FRAME_LIST_MAX)].index != 0) {
                    memcpy(conn->write_buf + conn->write_buf_to_send, &it->frs.frame_list[start % (FRAME_LIST_MAX)], sizeof(struct frame));
                    conn->write_buf_to_send += sizeof(struct frame);
                }
            }

		    pthread_mutex_unlock(&it->lock);
            on_write(conn->fd, 1, conn);
		    return 0;
	    }
        /* Response will be generated in rtmp_getter, DONT return 0 */
        /* May TIMEOUT */
        conn->next = it->meta_wait_list;
        it->meta_wait_list = conn;
        pthread_mutex_unlock(&it->lock);
        return 1;
    }
    if (conn->blockid_val) {
	    int block_id = atoi(conn->blockid_val);
        if (block_id > it->blocks.blockid_max + 100 || block_id < it->blocks.blockid_min) {
	        pthread_mutex_unlock(&it->lock);
            return notfound(conn);
        }
	    int i;
	    for (i = 0; i< BLOCK_LIST_MAX; i++) {
            /*if (it->blocks.block_list[i].used == 0)
                continue;*/
	        if (it->blocks.block_list[i].block_id != block_id)
		        continue;
            /* find */
	        p = it->blocks.block_list[i].head;
            conn->write_buf_to_send = sprintf(conn->write_buf, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
            conn->piece_head = p;
            conn->piece_sent = 0;
            conn->piece = p;
            p->ref ++;
            conn->item = it;
            conn->block = &it->blocks.block_list[i];
	        pthread_mutex_unlock(&it->lock);
            on_write_header(conn->fd, 1, conn);
	        return 1;
	    }
        conn->item = it;
        conn->piece = NULL;
        conn->write_buf_to_send = sprintf(conn->write_buf, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        conn->next = it->wait_list;
        it->wait_list = conn;
        pthread_mutex_unlock(&it->lock);
        on_write_header(conn->fd, 1, conn);
        return 1;
    }
    pthread_mutex_unlock(&it->lock);
    return 1;
}

int generate_response(struct conn *conn)
{
/*
 *    wiki: 
 *         http://wiki.letv.cn/pages/viewpage.action?pageId=34153301
 *    http://ip/mediainfo/tn?streamid=&rutpVer=
 *    http://ip/mediainfo/streamid/metaId?rutpVer=
 *    http://ip/mediainfo/streamid/acodecId?rutpVer=
 *    http://ip/mediainfo/streamid/vcodecId?rutpVer=
 *    http://ip/datainfo/streamid/blockId?rutpVer=
 */
    item *it;
    size_t len = conn->url_length;
    char *url = conn->url;
    char *str;

    const char *mediainfo = METAINFO;
    size_t mediainfo_len  = sizeof(METAINFO) - 1;
    const char *datainfo = DATAINFO;
    size_t datainfo_len  = sizeof(DATAINFO) - 1;

    const char *streamid = STREAMID;
    size_t streamid_len  = sizeof(STREAMID) - 1;
    const char *metaid   = METAID;
    size_t metaid_len    = sizeof(METAID)   - 1;
    const char *rttpver  = RTTPVER;
    size_t rttpver_len   = sizeof(RTTPVER)  - 1;
    const char *tn       = TN;
    size_t tn_len        = sizeof(TN)       - 1;
    const char *blockid  = BLOCKID;
    size_t blockid_len   = sizeof(BLOCKID)  - 1;

    conn->streamid_val = NULL;
    conn->metaid_val   = NULL;
    conn->rttpver_val  = NULL;
    conn->tn_val       = NULL;
    conn->blockid_val  = NULL;

    *(url + len) = '\0';
    url ++; /* ignore '/' */

    if (strncasecmp(url, mediainfo, mediainfo_len) == 0) {
        url += mediainfo_len + 1;
        str = strchr(url, '/');
        if (str) {
            conn->streamid_val = url;
            *str = '\0';
            url = str + 1;
            str = strchr(url, '?');
            if (str) {
                conn->metaid_val = url;
                *str = '\0';
                url = str + 1;
                while (*url) {
                    if (strncasecmp(url, rttpver, rttpver_len) ==0) {
                        conn->rttpver_val = url + rttpver_len + 1;
                    } else {
                        return invalid_param(conn);
                    }
                    url = strchr(url, '&');
                    if (url == NULL)
                        break;
                    *url = '\0';
                    url ++;
                }
            } else {
                return invalid_param(conn);
            }
        } else {
            conn->tn_val = url;
            str = strchr(url, '?');
            if (str) {
                *str = '\0';
                url = str + 1;
            } else {
                return invalid_param(conn);
            }
            while (*url) {
                if (strncasecmp(url, streamid, streamid_len) == 0) {
                    conn->streamid_val = url + streamid_len + 1;
                } else if (strncasecmp(url, rttpver, rttpver_len) ==0) {
                    conn->rttpver_val = url + rttpver_len + 1;
                } else {
                    return invalid_param(conn);
                }
                url = strchr(url, '&');
                if (url == NULL)
                    break;
                *url = '\0';
                url ++;
            }
        }
    } else if (strncasecmp(url, datainfo, datainfo_len) == 0) {
        url += datainfo_len + 1;
        str = strchr(url, '/');
        if (str) {
            conn->streamid_val = url;
            *str = '\0';
            url = str + 1;
            str = strchr(url, '?');
            if (str) {
                conn->blockid_val = url;
                *str = '\0';
                url = str + 1;
                while (*url) {
                    if (strncasecmp(url, rttpver, rttpver_len) ==0) {
                        conn->rttpver_val = url + rttpver_len + 1;
                    } else {
                        return invalid_param(conn);
                    }
                    url = strchr(url, '&');
                    if (url == NULL)
                        break;
                    *url = '\0';
                    url ++;
                }
            } else {
                return invalid_param(conn);
            }
        } else {
            return invalid_param(conn);
        }
    } else {
        return notfound(conn);
    }
    
    if (conn->streamid_val == NULL) {
        return invalid_param(conn);
    } else {
        if (conn->blockid_val) {
            if (conn->metaid_val || conn->tn_val) {
                return invalid_param(conn);
            }
        } else {
            if (conn->metaid_val && conn->tn_val) {
                return invalid_param(conn);
            }
            if (!conn->metaid_val && !conn->tn_val) {
                return invalid_param(conn);
            }
        }
    }

    fprintf(stderr, "%s %s %s %s %s\n", conn->streamid_val, conn->metaid_val, conn->blockid_val, conn->tn_val, conn->rttpver_val);

    int nkey = strlen(conn->streamid_val) < ITEM_KEY_MAX_LEN ? strlen(conn->streamid_val) : ITEM_KEY_MAX_LEN;
    if ( (it = mt_assoc_find(conn->streamid_val, nkey)) == NULL) {
        if (!conn->tn_val)
            return notfound(conn);
        it = malloc_item();
        if (it == NULL)
            return internal_error(conn);
        if (init_item(it, conn) < 0) {
            free_item(it);
            return internal_error(conn);
        }
        mt_assoc_insert(it);
        /* ATTENTION ATTENTION ATTENTION*/
        return do_generate_response(it, conn);
    } else {
        return do_generate_response(it, conn);
    }

    return -1;
}
void on_write_piece(int fd, short event, void *arg)
{
    int n;
    conn *conn = arg;
    piece *p = conn->piece;
    item *it = conn->item;

    while (conn->piece_sent != p->to_send) {
        n = write(fd, p->start + conn->piece_sent, p->to_send - conn->piece_sent);
        if (n < 0) {
            if (errno == EAGAIN) {
                break;
            } else {
                /*conn->piece_head->ref --;
                if (conn->piece_head->ref == 0) {
                    while (conn->piece_head) {
                        free_piece(conn->piece_head);
                        conn->piece_head = conn->piece_head->next;
                    }
                }*/
                destroy_conn(conn);
                return;
            }
        }
        conn->piece_sent += n;
    }

    if (conn->piece_sent == p->to_send) {
        if (p->next) {
            conn->piece = p->next;
            conn->piece_sent = 0;
            on_write_piece(conn->fd, 1, conn);
            return;
        } else if (p->is_tail) {
            conn->piece_head->ref --;
            if (conn->piece_head->ref == 0) {
                while (conn->piece_head) {
                    free_piece(conn->piece_head);
                    conn->piece_head = conn->piece_head->next;
                }
            }
            event_set(&conn->write_ev, fd, EV_WRITE, on_read, conn);
            event_base_set(main_base, &conn->read_ev);
            event_add(&conn->read_ev, NULL);
            fprintf(stderr, "ok\n");
            return;
        } else {
            pthread_mutex_lock(&conn->item->lock);
            conn->next = conn->block->wait_list;
            conn->block->wait_list = conn;
            pthread_mutex_unlock(&conn->item->lock);
            return;
        }
    } else {
        event_set(&conn->write_ev, fd, EV_WRITE, on_write_piece, conn);
        event_base_set(main_base, &conn->write_ev);
        event_add(&conn->write_ev, NULL);
    }

    return;
}
void on_write_header(int sock, short event, void *arg)
{
    struct conn *conn = (struct conn *)arg;
    int n;

    while (conn->write_buf_to_send != conn->write_buf_sent) {
        n = write(sock, conn->write_buf + conn->write_buf_sent, conn->write_buf_to_send - conn->write_buf_sent);
        if (n < 0)  {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (settings.verbose >= 2)
                    fprintf(stderr, "on_write: write return EAGAIN\n");
                break;
            } else {
                /* This conn will go die, so -- the ref */
                pthread_mutex_lock(&conn->item->lock);
                conn->piece->ref --;
                if (conn->piece->ref == 0) free_piece(conn->piece);
                pthread_mutex_unlock(&conn->item->lock);
                destroy_conn(conn);
                perror("on_write: write\n");
                return;
            }
        }
        conn->write_buf_sent += n;
    }
    
    if (conn->write_buf_sent == conn->write_buf_to_send) {
        conn->write_buf_sent = conn->write_buf_to_send = 0;
        if (conn->piece == NULL) return;
        on_write_piece(conn->fd, 1, conn);
    } else {
        event_set(&conn->write_ev, sock, EV_WRITE, on_write_header, conn);
        event_base_set(main_base, &conn->write_ev);
        event_add(&conn->write_ev, NULL);
    }
    
    return;
}
void on_write(int sock, short event, void *arg)
{
    struct conn *conn = (struct conn *)arg;
    int n;

    while (conn->write_buf_to_send != conn->write_buf_sent) {
        n = write(sock, conn->write_buf + conn->write_buf_sent, conn->write_buf_to_send - conn->write_buf_sent);
        if (n < 0)  {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (settings.verbose >= 2)
                    fprintf(stderr, "on_write: write return EAGAIN\n");
                break;
            } else {
                destroy_conn(conn);
                perror("on_write: write\n");
                return;
            }
        }
        conn->write_buf_sent += n;
    }
    
    if (conn->write_buf_sent == conn->write_buf_to_send) {
        conn->write_buf_sent = conn->write_buf_to_send = 0;
        event_set(&conn->read_ev, sock, EV_READ|EV_PERSIST, on_read, conn);
        event_base_set(main_base, &conn->read_ev);
        event_add(&conn->read_ev, NULL);
    } else {
        event_set(&conn->write_ev, sock, EV_WRITE, on_write, conn);
        event_base_set(main_base, &conn->write_ev);
        event_add(&conn->write_ev, NULL);
    }
    
    return;
}
void on_read(int sock, short event, void *arg)
{
    struct conn *conn = (struct conn *)arg;
    int n;
    size_t nparsed;

    while (conn->read_buf_len - conn->read_buf_used > 0) {
        n = read(sock, conn->read_buf + conn->read_buf_used, conn->read_buf_len - conn->read_buf_used);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                perror("on_read: read");
                destroy_conn(conn);
                return;
            }
        } else if (n == 0) {
            fprintf(stderr, "on_read: close()\n");
            destroy_conn(conn);
            return;
        }
        if (settings.verbose >= 3)
            fprintf(stderr, "on_read: read %d\n", n);
        conn->read_buf_used += n;
        if (conn->read_buf_len != conn->read_buf_used) {
            break;
        }
    }
    
    if (conn->read_buf_used == conn->read_buf_len && settings.verbose) {
        fprintf(stderr, "Warning: conn->read_buf_used == read_buf_len\n");
    }
    conn->read_buf[conn->read_buf_used] = '\0';
    nparsed = http_parser_execute(&(conn->http_parser), &(conn->http_parser_settings), \
                                   conn->read_buf + conn->http_parser.nread, \
                                   conn->read_buf_used - conn->http_parser.nread);
    if (conn->http_parser.http_errno != 0) {
        fprintf(stderr, "free conn because http_parser_execute error\n");
        destroy_conn(conn);
        return;
    }

    if (!conn->http_request_finished) {
        if (conn->read_buf_used == conn->read_buf_len) {
            fprintf(stderr, "free conn: request is too large\n");
            destroy_conn(conn);
        }
        return;
    }

    /* prepare for the next http request */
    conn->http_request_finished = 0;
    conn->read_buf_used = 0;
    http_parser_init(&(conn->http_parser), HTTP_REQUEST);
    event_del(&conn->read_ev);

    if (generate_response(conn) == 0) {
        /* notfound ok internal_error etc.. */
        on_write(sock, 0, conn);
    }
    //init_conn(conn, conn->fd);

    return;
}

static conn *conn_head = NULL;
static pthread_mutex_t conn_mm_lock;

static int conn_mm_init()
{
    conn *base = NULL;
    conn *tmp = NULL;
    unsigned int index;

    stats.total_conns = settings.maxconns;
    stats.curr_conns  = 0;

    base = (conn *)malloc(sizeof(conn) * settings.maxconns);
    if (!base)
        return -1;
    for (index = 0; index < settings.maxconns; index ++) {
        tmp = (conn *)((char *)base + index * sizeof(conn));
        tmp->mm_next = conn_head;
        conn_head = tmp;
    }

    pthread_mutex_init(&conn_mm_lock, NULL);
    return 0;
}
conn *malloc_conn()
{
    conn *tmp = NULL;
    conn *base = NULL;
    unsigned int index;

    pthread_mutex_lock(&conn_mm_lock);
    if (conn_head == NULL) {
        base = (conn *)malloc(sizeof(conn) * settings.maxconns);
        if (!base) {
            pthread_mutex_unlock(&conn_mm_lock);
            return NULL;
        }
        for (index = 0; index < settings.maxconns; index ++) {
            tmp = (conn *)((char *)base + index * sizeof(conn));
            tmp->mm_next = conn_head;
            conn_head = tmp;
        }
        stats.total_conns += settings.maxconns;
    }
    
    tmp = conn_head;
    conn_head = conn_head->mm_next;

    tmp->mm_next = NULL;
    pthread_mutex_lock(&stats.lock);
    stats.curr_conns ++;
    pthread_mutex_unlock(&stats.lock);
    pthread_mutex_unlock(&conn_mm_lock);

    return tmp;
}
void free_conn(conn *conn)
{
    if (!conn)
        return;

    pthread_mutex_lock(&conn_mm_lock);
    conn->mm_next = conn_head;
    conn_head = conn;
    /*pthread_mutex_lock(&stats.lock);
    stats.curr_conns --;
    pthread_mutex_unlock(&stats.lock);*/
    pthread_mutex_unlock(&conn_mm_lock);
    
    return;
}
void destroy_conn(struct conn *conn)
{
    if (!conn)
        return;

    if (settings.verbose >= 2)
        fprintf(stderr, "destroy_conn: %x\n", conn);
    close(conn->fd);
    event_del(&conn->read_ev);
    event_del(&conn->write_ev);
    free_conn(conn);
}
void init_conn(struct conn *conn, int fd)
{
    
    conn->fd = fd;
    conn->read_buf_len = 4095;
    conn->read_buf_used = 0;
    conn->write_buf_len = 1023;
    conn->write_buf_to_send = 0;
    conn->write_buf_sent = 0;

    conn->http_parser_settings.on_message_begin = on_message_begin;
    conn->http_parser_settings.on_url = on_url;
    conn->http_parser_settings.on_header_field = on_header_field;
    conn->http_parser_settings.on_header_value = on_header_value;
    conn->http_parser_settings.on_headers_complete = on_headers_complete;
    conn->http_parser_settings.on_body = on_body;
    conn->http_parser_settings.on_message_complete = on_message_complete;
    http_parser_init(&(conn->http_parser), HTTP_REQUEST);
    conn->http_request_finished = 0;
    conn->url = NULL;
    conn->url_length = 0;
    conn->next = NULL;
    conn->mm_next = NULL;
    conn->piece = NULL;
    conn->piece_sent = 0;
    conn->item = NULL;
    
    return;
}
void on_accept(int sock, short event, void *arg)
{
    struct sockaddr_in addr;
    int fd, sin_size;
    struct conn *conn = NULL;

    if (settings.verbose >= 2)
        fprintf(stderr, "on_accept: %d %d %d\n", sock, event, arg);

    sin_size = sizeof(struct sockaddr_in);
    fd = accept(sock, (struct sockaddr *)&addr, &sin_size);
    if (fd < 0) {
        perror("accept");
        return;
    }

    /* a block socket may raise hang */
    if (setnonblocking(fd) == -1) {
        fprintf(stderr, "setnonblocking return -1\n");
        close(fd);
        return;
    }

    //if ((conn = (struct conn *)malloc(sizeof(struct conn))) == NULL) {
    if ((conn = malloc_conn()) == NULL) {
        fprintf(stderr, "on_accept: malloc conn\n");
        close(fd);
        return;
    }
    
    init_conn(conn, fd);

    event_set(&conn->read_ev, fd, EV_READ|EV_PERSIST, on_read, conn);
    event_base_set(main_base, &conn->read_ev);
    event_add(&conn->read_ev, NULL);
}
int main(int argc, char **argv) 
{
    int c;
    int maxcore = 0;
    struct rlimit rlim;
    struct passwd *pw = NULL;
    struct sigaction sa;
    int sock;
    struct event listen_ev;
    
    signal(SIGINT, sig_handler);
  
    settings_init();

    setbuf(stderr, NULL);

    while ((c = getopt(argc, argv, "c:dp:P:rt:u:vh?")) != -1) {
        switch (c) {
            case 'c':
                settings.maxconns = atoi(optarg);
                break;
            case 'd':
                settings.daemonize = 1;
                break;
            case 'p':
                settings.tcpport = atoi(optarg);
                break;
            case 'P':
                if (settings.daemonize == 0) {
                    usage();
                    _exit(EXIT_SUCCESS);
                }
                settings.pidfile = optarg;
                break;
            case 'r':
                maxcore = 1;;
                break;
            case 't':
                settings.work_threads = atoi(optarg);
                settings.total_threads = settings.work_threads + 1;
                break;
            case 'u':
                settings.username = optarg;
                break;
            case 'v':
                settings.verbose += 1;
                break;
            case '?':
            case 'h':
                usage();
                _exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "Illegal argument \"%c\"\n", c);
                return 1;
        }
    }
  
    if (maxcore != 0) {
        struct rlimit rlim_new;
        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
            rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
            if (setrlimit(RLIMIT_CORE, &rlim_new)!= 0) {
                rlim_new.rlim_cur = rlim_new.rlim_max = rlim.rlim_max;
                (void)setrlimit(RLIMIT_CORE, &rlim_new);
            }
        }

        if ((getrlimit(RLIMIT_CORE, &rlim) != 0) || rlim.rlim_cur == 0) {
            fprintf(stderr, "failed to ensure corefile creation\n");
            exit(EXIT_FAILURE);
        }
    }

    /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fprintf(stderr, "failed to getrlimit number of files\n");
        exit(EXIT_FAILURE);
    } else {
        int maxfiles = settings.maxconns;
        if (rlim.rlim_cur < maxfiles)
            rlim.rlim_cur = maxfiles + 3; /* 3 = stdin + stdout + stderr */
        if (rlim.rlim_max < rlim.rlim_cur)
            rlim.rlim_max = rlim.rlim_cur;
        rlim.rlim_cur = rlim.rlim_max = 65535;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            fprintf(stderr, "failed to set rlimit for open files. Try running as root or requesting smaller maxconns value.\n");
            exit(EXIT_FAILURE);
        }
    }

    
    //dump_settings();
    
    if (getuid() == 0 || geteuid() == 0) {
        char *username = settings.username;
        if (username == 0 || *username == '\0') {
            fprintf(stderr, "can't run as root without the -u switch\n");
            return 1;
        }
        if ((pw = getpwnam(username)) == NULL) {
            fprintf(stderr, "can't find the user %s to switch to\n", username);
            return 1;
        }
        if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "failed to assume identity of user %s\n", username);
            return 1;
        }
    }

    main_base = event_base_new();
    /* inits */
    stats_init();
    mt_assoc_init();
    if (-1 == conn_mm_init()) {
        fprintf(stderr, "conn_mm_init()\n");
        exit(EXIT_FAILURE);
    }
    if (-1 == item_mm_init()) {
        fprintf(stderr, "item_mm_init()\n");
        exit(EXIT_FAILURE);
    }
    if (-1 == piece_mm_init()) {
        fprintf(stderr, "piece_mm_init()\n");
        exit(EXIT_FAILURE);
    }

    /* SIGPIPE may happen when write a broken pipe */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigemptyset(&sa.sa_mask) == -1 ||
        sigaction(SIGPIPE, &sa, 0) == -1) {
        perror("failed to ignore SIGPIPE; sigaction");
        exit(EXIT_FAILURE);
    }

#ifdef ENABLE_HTTP_MULTI_THREAD
    thread_init(settings.work_threads, main_base);
#endif

    clock_handler(0, 0, 0);
    sock = create_listen_socket();
    if (sock < 0) {
        fprintf(stderr, "create_listen_socket return %d\n", sock);
        exit(EXIT_FAILURE);
    }

    if (settings.daemonize) {
        int res;
        res = daemon(maxcore, settings.verbose);
        if (res == -1) {
            fprintf(stderr, "failed to daemon() in order to daemonize\n");
            return 1;
        }
    }

    if (settings.daemonize)
        save_pid(getpid(), settings.pidfile);


    event_set(&listen_ev, sock, EV_READ|EV_PERSIST, on_accept, NULL);
    event_base_set(main_base, &listen_ev);
    event_add(&listen_ev, NULL);

    event_base_dispatch(main_base);

    if (settings.daemonize)
        remove_pidfile(settings.pidfile);

    return EXIT_SUCCESS;
}
