/* from system */
#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <malloc.h>
#include <string.h>

/* from ./ */
#include <rtoh.h>
/* from third */
#include <rtmp_sys.h>
#include <event.h>

#ifdef as_debug
struct stats stats;
struct settings settings;
void destroy_conn(conn *conn)
{
}
void mt_assoc_delete(char *key, int n)
{
}
int do_generate_response(item *it, conn *conn)
{
}
#endif

static piece *piece_head = NULL;
static pthread_mutex_t piece_mm_lock;
#define PER_PICEE_MALLOC 64
#define PIECE_MAX_LEN (128 * 1024)
int piece_mm_init()
{
    piece *base = NULL;
    piece *tmp = NULL;
    unsigned int index;

    base = (piece *)malloc(PIECE_MAX_LEN * PER_PICEE_MALLOC); // 10M 
    if (!base)
        return -1;
    for (index = 0; index < PER_PICEE_MALLOC; index ++) {
        tmp = (piece *)((char *)base + index * (PIECE_MAX_LEN));
        tmp->mm_next = piece_head;
        piece_head = tmp;
    }

    pthread_mutex_init(&piece_mm_lock, NULL);
    return 0;
}
piece *malloc_piece()
{
    piece *base = NULL;
    piece *tmp = NULL;
    unsigned int index;

    pthread_mutex_lock(&piece_mm_lock);
    if (piece_head == NULL) {
        base = (piece *)malloc(PIECE_MAX_LEN * PER_PICEE_MALLOC);
        if (!base) {
            pthread_mutex_unlock(&piece_mm_lock);
            return NULL;
        }
        for (index = 0; index < PER_PICEE_MALLOC; index ++) {
            tmp = (piece *)((char *)base + index * (PIECE_MAX_LEN));
            tmp->mm_next = piece_head;
            piece_head = tmp;
        }
    }
    
    tmp = piece_head;
    piece_head = piece_head->mm_next;

    tmp->mm_next = NULL;
    pthread_mutex_unlock(&piece_mm_lock);

    return tmp;
}
void free_piece(piece *piece)
{
    if (!piece)
        return;

    pthread_mutex_lock(&piece_mm_lock);
    piece->mm_next = piece_head;
    piece_head = piece;
    pthread_mutex_unlock(&piece_mm_lock);
    
    return;
}

static item *item_head = NULL;
static pthread_mutex_t item_mm_lock;
int item_mm_init()
{
    item *base = NULL;
    item *tmp = NULL;
    unsigned int index;

    pthread_mutex_lock(&stats.lock);
    stats.total_items = settings.maxgetters;
    stats.curr_items = 0;
    pthread_mutex_unlock(&stats.lock);

    base = (item *)malloc(sizeof(item) * stats.total_items);
    if (!base)
        return -1;
    for (index = 0; index < stats.total_items; index ++) {
        tmp = (item *)((char *)base + index * sizeof(item));
        tmp->mm_next = item_head;
        item_head = tmp;
    }
    
    pthread_mutex_init(&item_mm_lock, NULL);
    return 0;
}
item *malloc_item()
{
    item *tmp = NULL;

    pthread_mutex_lock(&item_mm_lock);
    if (item_head == NULL) {
        pthread_mutex_unlock(&item_mm_lock);
        return NULL;
    }

    tmp = item_head;
    item_head = item_head->mm_next;

    tmp->mm_next = NULL;
    pthread_mutex_lock(&stats.lock);
    stats.curr_items ++;
    pthread_mutex_unlock(&stats.lock);
    pthread_mutex_unlock(&item_mm_lock);

    return tmp;
}

void free_item(item *it)
{
    if (!it)
        return;

    pthread_mutex_lock(&item_mm_lock);
    it->mm_next = item_head;
    item_head = it;
    pthread_mutex_lock(&stats.lock);
    stats.curr_items --;
    pthread_mutex_unlock(&stats.lock);
    pthread_mutex_unlock(&item_mm_lock);

    return;
}
void destroy_item(item *it)
{
    int index;

    if (it->thread_id != pthread_self()) {
        fprintf(stderr, "it->thread_id != pthread_self()\n");
    }

    pthread_mutex_lock(&it->lock);
    while (it->meta_wait_list) {
        conn *next = it->meta_wait_list->next;
        destroy_conn(it->meta_wait_list);
        it->meta_wait_list = next;
    }
    while (it->wait_list) {
        conn *next = it->wait_list->next;
        destroy_conn(it->wait_list);
        it->wait_list = next;
    }
    for (index = 0; index < BLOCK_LIST_MAX; index ++) {
        while (it->blocks.block_list[index].head) {
            piece *next = it->blocks.block_list[index].head->next;
            if (it->blocks.block_list[index].head->ref == 0) {
                it->blocks.block_list[index].piece_num --;
                free_piece(it->blocks.block_list[index].head);
            }
            it->blocks.block_list[index].head = next;
        }
    }
    while (it->metas.meta_head) {
        piece *next = it->metas.meta_head->next;
        if (it->metas.meta_head->ref == 0) {
            free_piece(it->metas.meta_head);
        }
        it->metas.meta_head = next;
    }
    pthread_mutex_unlock(&it->lock);
    pthread_mutex_destroy(&it->lock);
    mt_assoc_delete(it->key, it->nkey);
    free_item(it);
}

void *rtmp_getter(void *arg)
{
    item *it = (item *) arg;
    piece *bp;
    char url[256];
    RTMP rtmp;
    int lost_tag = 0;
    int flag = 0;

    snprintf(url, 256, "rtmp://115.182.63.94/live/%s", it->key);
    //snprintf(url, 256, "rtmp://115.182.63.94/live/cctv5_800");
    RTMP_Init(&rtmp);
    RTMP_SetupURL(&rtmp, url);
    RTMP_SetBufferMS(&rtmp, 1024 * 1024);
    rtmp.Link.lFlags |= RTMP_LF_LIVE;
    if (!RTMP_Connect(&rtmp, NULL)) {
        fprintf(stderr, "RTMP_Connect return zero\n");
        destroy_item(it);
        return;
    }
    if (RTMP_ConnectStream(&rtmp, 0) == 0) {
        fprintf(stderr, "RTMP_ConnectStream(&rtmp, 0) == 0\n");
        destroy_item(it);
        return;
    }

    int is_first = 1;
    bp = NULL;
    while (1) {
        int need_break = 0;
        int len;
        piece *piece;
        if (!bp) {
            piece = malloc_piece();
            if (!piece){
                destroy_item(it);
                return;
            }
            piece->ref = 0;
            piece->type = UNKNOWN_PIECE;
            piece->state = PIECE_UNCOMPLETE;
            piece->len = PIECE_MAX_LEN;
            piece->used = sizeof(struct piece) * 2 + 128;
            piece->current = (char *)piece + piece->used;
            piece->data = piece->current;
            piece->data_len = 0;
            piece->next = NULL;
            piece->unparsed = 0;
            piece->unparsed_start = NULL;
        } else {
            piece = bp;
        }

        //len = 0;
        while (piece->unparsed > 0 || (len = RTMP_Read(&rtmp, piece->current, piece->len - piece->used)) > 0) {
            char *start;
            struct flv_tag_header *fth;
            if (piece->unparsed) {
                piece->unparsed = 0;
                start = piece->unparsed_start;
            } else {
                if (is_first) {
                    is_first = 0;
                    struct flv_file_header *ffh = (struct flv_file_header *)piece->current;
                    start = piece->current + ntohl(ffh->len) + 4;
                    piece->data = start;
                } else {
                    start = piece->current;
                }
                piece->used += len;
                piece->current += len;
                piece->data_len = piece->current - piece->data;
                if (len < sizeof(struct flv_tag_header)) {
                    destroy_item(it);
                    fprintf(stderr, "RTMP_Read return < sizeof(flv_tag_header)\n");
                }
            }
            //fprintf(stderr, "\n\n\npiece = %x\n len = %d\n", piece, len);
            while (start < piece->current) {
                int tag_data_len;
                int tag_header_len;
                int tag_len;
                int tag_total_len;
                fth = (struct flv_tag_header *)(start);
                tag_header_len = sizeof(struct flv_tag_header);
                tag_data_len = (fth->data_len_h << 16) + ntohs(fth->data_len_l);
                tag_len = tag_header_len + tag_data_len;
                tag_total_len = tag_len + 4;
                if (start + tag_total_len > piece->current) {
                    fprintf(stderr, "tag_total len \n");
                    destroy_item(it);
                    return;
                }
                //fprintf(stderr, "taglen = %d type = %d\n", tag_total_len, fth->type);
                if (fth->type == 0x1e) {
                    struct piece_tag *pt = (struct piece_tag *)(start + tag_header_len);
                    piece->pu.pc.ver       = pt->pc.ver;
                    piece->pu.pc.type      = pt->pc.type;
                    piece->pu.pc.index     = pt->pc.index;
                    piece->pu.pc.checksum  = pt->pc.checksum;
                    piece->pu.pc.length    = 0;//pt->pc.length;
                    piece->pu.pc.timestamp = pt->pc.timestamp; 
                    piece->pu.pc.reserve0  = pt->pc.reserve0;
                    if (pt->pc.type == META_PIECE
                            || pt->pc.type == VIDEO_PIECE
                            || pt->pc.type == AUDIO_PIECE) { // meta piece
                        piece->pu.hp.ttl = 0;
                        piece->pu.hp.media_type = 0;
                        piece->pu.hp.reserve = 0;
                    } else if (pt->pc.type == DATA_PIECE) { //data piece
                        piece->pu.dp.ttl = 0;
                        piece->pu.dp.flag = pt->flag;
                        piece->pu.dp.metadata_index = 0; // attention
                        piece->pu.dp.reserve = 0;
                        if (piece->pu.dp.flag) {
                            lost_tag = 0;
                        }
                        if (it->metas.meta_head)
                            piece->pu.dp.metadata_index = it->metas.meta_head->pu.hp.pc.index;
                        else
                            piece->pu.dp.metadata_index = 0;
                        if (it->metas.video_head)
                            piece->pu.dp.videocodec_index = it->metas.video_head->pu.hp.pc.index;
                        else
                            piece->pu.dp.videocodec_index = 0;
                        if (it->metas.audio_head)
                            piece->pu.dp.audiocodec_index = it->metas.audio_head->pu.hp.pc.index;
                        else
                            piece->pu.dp.audiocodec_index = 0;
                    }
                    piece->type = pt->pc.type;
                    piece->state = PIECE_COMPLETE;
                    int32_t offset = ntohl(pt->offset);
                    if (offset > 0) {
                        fprintf(stderr, "offset > 0\n");
                        destroy_item(it);
                        return;
                    }
                    offset = abs(offset);
                    //fprintf(stderr, "piecelen = %d, offset = %d\n", htonl(pt->pc.length), offset);
                    if (offset > 0 || start + tag_total_len < piece->current) {
                        //fprintf(stderr, "need copy\n");
                        bp = malloc_piece();
                        if (!bp) {
                            destroy_item(it);
                            return;
                        }
                        bp->ref = 0;
                        bp->type = UNKNOWN_PIECE;
                        bp->state = PIECE_UNCOMPLETE;
                        bp->len = PIECE_MAX_LEN;
                        bp->used = sizeof(struct piece) * 2 + 128;
                        bp->current = (char *)bp + bp->used;
                        bp->data = bp->current;
                        bp->data_len = 0;
                        bp->next = NULL;
                        bp->unparsed = 0;
                        bp->unparsed_start = NULL;
                        if (offset > 0) {
                            uint32_t *plen = (uint32_t *)(start - 4);
                            char *start_cpy = NULL;
                            int to_cpy = 0;
                            uint32_t lengh = 0;
                            while ((char *)plen > piece->data) {
                                uint32_t ptlen = ntohl(*plen);
                                uint32_t pdlen = ptlen - sizeof(struct flv_tag_header);
                                if ((char *)plen - pdlen < piece->data) {
                                    pdlen = (char *)plen - piece->data;
                                }
                                if (offset > pdlen) {
                                    offset -= pdlen;
                                } else {
                                    if (start_cpy == NULL) {
                                        start_cpy = (char *)plen - offset;
                                        to_cpy = start - start_cpy;
                                        lengh += (pdlen - offset);
                                        //fprintf(stderr, "off tocpy = %d\n", to_cpy);
                                        offset = 0;
                                    } else {
                                        /*if ((char *)plen - pdlen < piece->data) {
                                            lengh += (char *)plen - piece->data;
                                            break;
                                        } else {
                                            lengh += pdlen;
                                        }*/
                                        lengh += pdlen;
                                    }
                                }
                                
                                plen = (uint32_t *)((char *)plen - ptlen - 4);
                            }
                            if (lengh != ntohl(pt->pc.length)) {
                                lost_tag = 1;
                                //return;
                            } else if (to_cpy) {
                                piece->pu.pc.length = htonl(start_cpy - piece->data);
                                piece->data_len = start_cpy - piece->data;
                            }

                            memcpy(bp->current, start_cpy, to_cpy);
                            bp->used += to_cpy;
                            bp->current += to_cpy;
                            bp->data_len += to_cpy;
                        }
                        if (start + tag_total_len < piece->current) {
                            int to_cpy = piece->current - (start + tag_total_len);
                            //fprintf(stderr, "> tocpy = %d\n", to_cpy);
                            memcpy(bp->current, start + tag_total_len, to_cpy);
                            bp->unparsed = to_cpy;
                            bp->unparsed_start = bp->current;
                            bp->used += to_cpy;
                            bp->current += to_cpy;
                            bp->data_len += to_cpy;
                        }
                    } else {
                        bp = NULL;
                    }
                    if (lost_tag) {
                        piece->pu.pc.length = 0;
                        piece->data_len = 0;
                    } else if (piece->pu.pc.length == 0) {
                        piece->pu.pc.length = htonl(start - piece->data);
                        piece->data_len = start - piece->data;
                    }
                    //fprintf(stderr, "length = %d %d\n", ntohl(piece->pu.pc.length), piece->data_len);
                    pthread_mutex_lock(&it->lock);
                    struct block *block = NULL;
                    if (piece->type == META_PIECE) {
                        piece->next = it->metas.meta_head;
                        it->metas.meta_head = piece;
                        //fprintf(stderr, "meta head\n");
                    } else if (piece->type == VIDEO_PIECE) {
                        piece->next = it->metas.video_head;
                        it->metas.video_head = piece;
                        //fprintf(stderr, "video head\n");
                    } else if (piece->type == AUDIO_PIECE) {
                        piece->next = it->metas.audio_head;
                        it->metas.audio_head = piece;
                        //fprintf(stderr, "audio head\n");
                    } else if (piece->type == DATA_PIECE) {
                        uint64_t piece_num = be64toh(piece->pu.dp.pc.index);
                        uint64_t blockid = (piece_num % 4) + ((piece_num / 4 / 4) * 4);
                        uint32_t block_list_index = blockid % BLOCK_LIST_MAX;
                        if (it->blocks.blockid_max < blockid) {
                            it->blocks.blockid_max = blockid;
                        }
                        if (it->blocks.blockid_min == 0) it->blocks.blockid_min = blockid;
                        it->blocks.piece_id_max = piece_num;
                        if (it->blocks.piece_id_min == 0) it->blocks.piece_id_min = piece_num;
                        block = &(it->blocks.block_list[block_list_index]);
                        if ((piece_num - piece_num % 4 + 4) % 16 == 0)
                            piece->is_tail = 1;
                        else
                            piece->is_tail = 0;
                        if (block->block_id != blockid) {
                            block->piece_num = 1;
                            struct piece *next = block->head;
                            while (next) {
                                if (next->pu.dp.flag) {
                                    int start = it->frs.start;
                                    int end = it->frs.end;
                                    for ( ; start % (FRAME_LIST_MAX) != end; start ++) {
                                        if (it->frs.frame_list[start % (FRAME_LIST_MAX)].index == next->pu.dp.pc.index) {
                                            it->frs.frame_list[start % (FRAME_LIST_MAX)].index = 0;
                                            it->frs.frame_list[start % (FRAME_LIST_MAX)].timestamp = 0;
                                            if (start == it->frs.start) {
                                                it->frs.start = (it->frs.start + 1) % (FRAME_LIST_MAX);
                                            }
                                            break;
                                        }
                                    }
                                }
                                next = next->next;
                            }
                            if (block->head && block->head->ref == 0) {
                                while (block->head) {
                                    struct piece *next = block->head->next;
                                    free_piece(block->head);
                                    block->head = next;
                                }
                            } else {
                                block->head = block->tail = NULL;
                            }
                            piece->next = NULL;
                            block->head = block->tail = piece;
                            if (block->block_id > 0) {
                                struct block *bl = &(it->blocks.block_list[(block_list_index + 1) % BLOCK_LIST_MAX]);
                                it->blocks.piece_id_min = be64toh(bl->head->pu.dp.pc.index);
                                it->blocks.blockid_min = bl->block_id;
                            }
                            block->block_id = blockid;
                            conn **pconn = &(it->wait_list);
                            while (*pconn) {
                                conn *conn = *pconn;
                                if (atoi(conn->blockid_val) == blockid) {
                                    *pconn = conn->next;
                                    conn->next = block->wait_list;
                                    block->wait_list = conn;
                                } else {
                                    pconn = &(conn->next);
                                }
                            }
                        } else {
                            block->piece_num += 1;
                            piece->next = NULL;
                            if (blockid == 0 && block->head == NULL) {
                                block->head = block->tail = piece;
                            } else {
                                block->tail->next = piece;
                                block->tail = piece;
                            }
                        }
                        if (piece->pu.dp.flag) {
                            it->frs.frame_list[it->frs.end].index = piece->pu.dp.pc.index;
                            it->frs.frame_list[it->frs.end].timestamp = piece->pu.dp.pc.timestamp;
                            it->frs.end = (it->frs.end + 1) % (FRAME_LIST_MAX);
                            if (it->frs.end == it->frs.start)
                                it->frs.start = (it->frs.start + 1) % (FRAME_LIST_MAX);
                        }
                        memcpy(piece->data - sizeof(struct data_piece), &piece->pu, sizeof(struct data_piece));
                        piece->start = piece->data - sizeof(struct data_piece);
                        char buf[24];
                        int n = sprintf(buf, "%x\r\n", piece->data_len + sizeof(struct data_piece));
                        memcpy(piece->start - n, buf, n);
                        piece->start -= n;
                        if (piece->is_tail) {
                            sprintf(piece->data + piece->data_len, "\r\n0\r\n\r\n");
                            piece->to_send = piece->data_len + (piece->data - piece->start) + 7;
                        } else {
                            sprintf(piece->data + piece->data_len, "\r\n");
                            piece->to_send = piece->data_len + (piece->data - piece->start) + 2;
                        }

                        printf("piece = %lld, piece_max = %lld, piece_min = %lld, blockmax = %lld, blockidmin = %lld, com = %d\n", piece_num, it->blocks.piece_id_max, it->blocks.piece_id_min, it->blocks.blockid_max, it->blocks.blockid_min, block->complete);
                    }
                    conn *wait_list = NULL;
                    if (block) {
                        wait_list = block->wait_list;
                        block->wait_list = NULL;
                    }
                    conn *meta_conn_head = NULL;
                    if (it->meta_wait_list && it->metas.video_head && it->metas.audio_head && it->frs.start != it->frs.end) {
                        meta_conn_head = it->meta_wait_list;
                        it->meta_wait_list = NULL;
                    }
                    pthread_mutex_unlock(&it->lock);
                    while (meta_conn_head) {
                        do_generate_response(it, meta_conn_head);
                        meta_conn_head = meta_conn_head->next;
                    }
                    while (wait_list) {
                        wait_list->piece = piece;
                        wait_list->piece_sent = 0;
                        wait_list->block = block;
                        wait_list->piece_head = block->head;
                        on_write_piece(wait_list->fd, 1, wait_list);
                        wait_list = wait_list->next;
                    }
                    need_break = 1;
                    break;
                }
                start += tag_total_len;
            }
            if (need_break) break;
        }
        
        if (len <= 0) {
            fprintf(stderr, "RTMP_read return -1\n");
            fprintf(stderr, "........%d\n", piece->len - piece->used);
            destroy_item(it);
            return NULL;
        }
    }
}
#ifdef as_debug
int main()
{
    item * it= malloc(sizeof(item));
    bzero(it, sizeof(struct item));
    piece_mm_init();
    memcpy(it->key, "cctv5_800", 9);
    it->nkey = 9;
    it->key[it->nkey] = 0;
    rtmp_getter(it);
}
#endif
