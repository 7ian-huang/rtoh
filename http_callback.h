#include <http_parser.h>
#include <rtoh.h>

int on_message_begin(http_parser* _);

int on_headers_complete(http_parser* _);

int on_message_complete(http_parser* _);

int on_url(http_parser* _, const char* at, size_t length);

int on_header_field(http_parser* _, const char* at, size_t length);

int on_header_value(http_parser* _, const char* at, size_t length);

int on_body(http_parser* _, const char* at, size_t length);
