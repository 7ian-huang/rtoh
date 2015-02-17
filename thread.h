#include <rtoh.h>

void *rtmp_getter(void *arg);
void init_blocks(struct blocks *blocks);
void init_metas(struct metas *metas);
void free_piece(piece *piece);
int item_mm_init();
item *malloc_item();
void free_item(item *it);
