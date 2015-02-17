#ifndef __RTOH_H
#define __RTOH_H
/* from system */
#include <pthread.h>

/* from ./ */
#include <event.h>
#include <http_parser.h>

#define INVALID_PARAM "HTTP/1.1 400 OK\r\nContent-Length: 16\r\n\r\ninvalid params\r\n"
#define NOT_FOUND "HTTP/1.1 404 Not Found\r\nContent-Length: 11\r\n\r\nNot found\r\n"
#define OK "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nOK\r\n"
#define INTERNAL_ERROR "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 23\r\n\r\nInternal Server Error\r\n"
#define CHUNKED "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nqwert\r\n4\r\n1234\r\n0\r\n\r\n"

#define METAINFO "mediainfo"
#define DATAINFO "datainfo"
#define STREAMID "streamid"
#define METAID   "metaid"
#define RTTPVER  "rutpver"
#define TN       "tn"
#define BLOCKID  "blockid"

#define BLOCK_LIST_MAX 12
#define FRAME_LIST_MAX (BLOCK_LIST_MAX * 4)
#define ITEM_KEY_MAX_LEN 128
#define PIECE_MAX_LEN (128 * 1024)

extern struct settings settings;
extern struct stats stats;
extern struct event_base *main_base;
void on_read(int fd, short int event, void *arg);
void on_write_piece(int fd, short event, void *arg);
#define ITEM_key(x) ((x)->key)
#define TODO do{}while(0);
#define BLOCK_NOT_COMPLETE 1
#pragma pack(push, 8)
#pragma pack(1)
struct settings {
    unsigned int maxconns;
    unsigned int daemonize;
    unsigned int tcpport;
    char *pidfile;
    unsigned int work_threads;
    unsigned int total_threads;
    char *username;
    unsigned int verbose;
    unsigned int maxgetters;
};
struct stats {
    pthread_mutex_t lock;
    unsigned int  curr_items;
    unsigned int  total_items;
    unsigned int  curr_conns;
    unsigned int  total_conns;
    uint64_t      get_cmds;
    uint64_t      get_hits;
    uint64_t      get_misses;
    uint64_t      get_stats;
    uint64_t      get_dengerous;
    time_t        started;          /* when the process was started */
    unsigned int  accepted;  /* whether we are currently accepting */
};
struct conn {
    int fd;
    char read_buf[4096];
    int read_buf_len;
    int read_buf_used;
    char write_buf[1024];
    int write_buf_len;
    int write_buf_to_send;
    int write_buf_sent;
    struct event read_ev;
    struct event write_ev;
    http_parser_settings http_parser_settings;
    http_parser http_parser;
    int http_request_finished;
    char *url;
    size_t url_length;
    char *streamid_val;
    char *metaid_val;
    char *rttpver_val;
    char *tn_val;
    char *blockid_val;
    struct conn *next;
    struct conn *mm_next;
    struct piece *piece_head;
    struct piece *piece;
    int piece_sent;
    struct item  *item;
    struct block *block;
};
typedef struct conn conn;

struct piece_core {
    uint8_t ver; //协议版本
    uint8_t type; //Piece类型（meta还是data）
    uint64_t index; //Piece编号
    uint32_t checksum; //校验值
    uint32_t length; //数据有效长度
    uint64_t timestamp; //piece时间戳
    uint8_t reserve0;
};
struct piece_tag {
    struct piece_core pc;
    int32_t offset; //往前的偏移位置，负值或0
    uint8_t flag; //是否关键帧
};
struct header_piece {
    struct piece_core pc;
    uint8_t ttl; //piece生存时间
    uint8_t media_type; //流类型
    uint8_t reserve;
};
struct data_piece {
    struct piece_core pc;
    uint8_t ttl; //piece生存时间
    uint8_t flag; //是否关键帧
    uint64_t metadata_index; //metadata编号;
    uint64_t videocodec_index;
    uint64_t audiocodec_index;
    uint8_t reserve;
};
enum piece_type {
    META_PIECE,
    DATA_PIECE,
    DEFAULT_PIECE,
    AUDIO_PIECE,
    VIDEO_PIECE,
    UNKNOWN_PIECE,
};
enum piece_state {
    PIECE_UNCOMPLETE,
    PIECE_COMPLETE,
};
struct piece {
    int ref;
    enum piece_type type;
    enum piece_state state;
    size_t len;
    size_t used;
    char *current;
    char *data;
    size_t data_len;
    char *start;
    size_t to_send;
    struct piece *next;
    struct piece *mm_next;
    int unparsed;
    char *unparsed_start;
    int is_tail;
    union {
        struct piece_core pc;
        struct header_piece hp;
        struct data_piece dp;
    } pu;
};
typedef struct piece piece;

struct block {
    int complete;
    uint64_t block_id;
    int piece_num;
    struct piece *head;
    struct piece *tail;
    conn *wait_list;
};
struct blocks {
    uint64_t piece_id_min;
    uint64_t  piece_id_max;
    uint64_t blockid_max;
    uint64_t blockid_min;
    struct block block_list[BLOCK_LIST_MAX];
};
struct metas {
    struct piece *meta_head;
    struct piece *audio_head;
    struct piece *video_head;
};
struct frame {
    uint64_t index;
    uint64_t timestamp;
};
struct frames {
    int start;
    int end;
    struct frame frame_list[FRAME_LIST_MAX];
};
struct item {
    char key[ITEM_KEY_MAX_LEN];
    size_t nkey;
    pthread_t thread_id;
    pthread_mutex_t lock;
    struct conn *wait_list;
    struct conn *meta_wait_list;
    struct item *h_next;
    struct item *mm_next;
    struct blocks blocks;
    struct metas metas;
    struct frames frs;
};
typedef struct item item;
struct source_info {
    uint64_t MIN; //RingBuffer最小piece编号
    uint32_t MAX; //最大piece编号（和MIN的偏移）
    uint32_t CUR; //当前位置（和MIN的偏移）
    uint16_t type; //ringbuffer内容描述体类型
    uint16_t piecenumofperblock;  // 每个block的piece数目
    uint32_t info_size; //ringbuffer内容描述体大小
};
struct flv_file_header {
    char c1;
    char c2;
    char c3;
    uint8_t ver;
    uint8_t bits;
    uint32_t len;
};
struct flv_tag_header {
    uint8_t type;
    uint8_t data_len_h;
    uint16_t data_len_l;
    uint32_t timestamp;
    uint8_t stream_id_h;
    uint16_t stream_id_l;
};
#pragma pack(pop)
void destroy_conn(struct conn *conn);
#endif
