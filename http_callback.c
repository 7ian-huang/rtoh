#include <stdio.h>
#include <http_callback.h>
#include <rtoh.h>

struct conn *get_conn_adrr(void *p) 
{
    struct conn tmp;
    struct conn *ret = (struct conn *)(p - ((char *)&(tmp.http_parser) - (char *)&tmp));
    return ret;
}
int on_message_begin(http_parser* _) {
  (void)_;
  return 0;
}

int on_headers_complete(http_parser* _) {
  (void)_;
  return 0;
}

int on_message_complete(http_parser* _) {
  (void)_;
  struct conn *conn = get_conn_adrr(_);
  conn->http_request_finished = 1;
  return 0;
}

int on_url(http_parser* _, const char* at, size_t length) {
  (void)_;
  struct conn *conn = get_conn_adrr(_);
  conn->url = (char *)at;
  conn->url_length = length;
  return 0;
}

int on_header_field(http_parser* _, const char* at, size_t length) {
  (void)_;
  return 0;
}

int on_header_value(http_parser* _, const char* at, size_t length) {
  (void)_;
  return 0;
}

int on_body(http_parser* _, const char* at, size_t length) {
  (void)_;
  return 0;
}
