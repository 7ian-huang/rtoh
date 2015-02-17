#include <rtoh.h>

void mt_assoc_delete(const char *key, const size_t nkey);
int mt_assoc_insert(item *it);
item *mt_assoc_find(const char *key, const size_t nkey);
void mt_assoc_init(void);
