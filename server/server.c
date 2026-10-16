#include "../async.c"

#include <sys/stat.h>

#define TEMPORARY_POOL_SIZE Megabytes(1)
Pool temporary_pool;

int create_tcp_server(String host, uint16_t port) {
 int sock = socket(AF_INET, SOCK_STREAM, 0);
 if (sock < 0) return -1;

 int yes = 1;
 if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
  close(sock);
  return -1;
 }

 struct sockaddr_in addr;
 addr.sin_family = AF_INET;
 addr.sin_port = le_to_be_u16(port);
 if (!parse_ipv4(host, (uint8_t*)&addr.sin_addr)) {
  close(sock);
  return -1;
 }

 if (bind(sock, (struct sockaddr*)&addr, sizeof addr) < 0) {
  close(sock);
  return -1;
 }

 if (listen(sock, 32) < 0) {
  close(sock);
  return -1;
 }

 return sock;
}

typedef struct {
 String key;
 String value;
} HTTPOption;

typedef struct {
 String path;

 HTTPOption *options;
 uint32_t options_count;

 // TODO: uint16_t version[2];

 enum {
  HTTP_GET,
  HTTP_POST
 } type;
} HTTPRequest;

#define HTTP_MAX_HEADERS 256

HTTPRequest *http_parse_request(Pool *pool, String request_str) {
 HTTPRequest *req = pool_alloc(pool, sizeof (HTTPRequest));
 memzero(req);

 {
  pool_temp_begin(&temporary_pool);

  uint32_t lines_count;
  String *lines = string_split(&temporary_pool, request_str, '\n', &lines_count);
  if (lines_count < 1) {
   pool_temp_end(&temporary_pool);
   return 0;
  }

  // Parse first line
  uint32_t request_description_count;
  String *request_description = string_split(&temporary_pool, lines[0], ' ', &request_description_count);
  if (request_description_count != 3) {
   pool_temp_end(&temporary_pool);
   return 0;
  }

  if (string_equal(request_description[0], STR_L("GET"))) {
   req->type = HTTP_GET;
  } else if (string_equal(request_description[0], STR_L("POST"))) {
   req->type = HTTP_POST;
  } else {
   pool_temp_end(&temporary_pool);
   return 0;
  }

  req->path = string_copy(pool, request_description[1]);

  req->options_count = 0;

  bool8_t valid_options = true;
  for (uint32_t i = 1; i < lines_count; i++) {
   if (lines[i].count == 0 || lines[i].data[0] == '\r') break;
   req->options_count++;
  }

  if (req->options_count > HTTP_MAX_HEADERS) {
   pool_temp_end(&temporary_pool);
   return 0;
  }

  req->options = pool_alloc(pool, sizeof (HTTPOption) * req->options_count);

  for (uint32_t i = 1; i < req->options_count + 1; i++) {
   uint32_t off = 0;
   for (off = 0; off < lines[i].count && lines[i].data[off] != ':'; off++);

   if (off == lines[i].count) {
    pool_temp_end(&temporary_pool);
    return 0;
   }

   String key = string_sub(lines[i], 0, off);
   String value = string_sub(lines[i], off + 2 , -2);

   if (key.data == 0 || value.data == 0) { // Error in string_sub
    pool_temp_end(&temporary_pool);
    return 0;
   }

   req->options[i - 1].key = string_copy(pool, key);
   req->options[i - 1].value = string_copy(pool, value);
  }

  pool_temp_end(&temporary_pool);
 }

 return req;
}

static String path_get_extension(Pool *pool, String path) {
 uint32_t parts_count;
 String *parts = string_split(pool, path, '.', &parts_count);

 if (parts_count == 0) return (String){0};

 return parts[parts_count - 1];
}

static String extension_to_mimetype(String extension) {
 if (string_equal(extension, STR_L("txt"))) {
  return STR_L("text/plain");
 } else if (string_equal(extension, STR_L("html"))) {
  return STR_L("text/html");
 } else if (string_equal(extension, STR_L("png"))) {
  return STR_L("image/png");
 } else {
  return STR_L("application/octet-stream");
 }
}

uint8_t http_server_buffer[Megabytes(1)];
Pool http_server_pool;

static char http_response_header[] =
 "HTTP/1.1 200 OK\r\n"
 "Content-Type: %s\r\n"
 "Content-Length: %ld\r\n"
 "\r\n";

static char http_not_found_header[] =
 "HTTP/1.1 404 NOT FOUND\r\n"
 "Content-Type: text/plain\r\n"
 "Content-Length: 9\r\n"
 "\r\n"
 "Not found";

typedef struct {
 int sock;
 int file;
 uint64_t file_offset;
 uint64_t file_size;
 bool8_t is_over;
 String mimetype;
} HTTPFileTransfer;

int main() {
 pool_init(&temporary_pool, allocate_pages(TEMPORARY_POOL_SIZE), TEMPORARY_POOL_SIZE);

 async_init();

 pool_init(&http_server_pool, http_server_buffer, sizeof http_server_buffer);

 int server_socket = create_tcp_server(STR_L("0.0.0.0"), 5000);
 async_accept(server_socket, 0);

 printf("Waiting for connections...\n");
 while (true) {
  uint32_t events_count = async_wait_for_events();
  pool_clear(&http_server_pool);

  for (uint32_t i = 0; i < events_count; i++) {
   AsyncEvent event = async_events[i];

   if (event.error != 0) {
    printf("Event type %u\n", event.type);
    printf("Errno: %s\n", strerror(event.error));
    CRASH("Received an error!");
   }

   if (event.type == E_ACCEPT) {
    printf("New connection!\n");
    async_add_connection(event.accept.client_socket, 0);
   } else if (event.type == E_RECV) {
    printf("----------------------- %ld BYTES ----------------------\n", event.recv.bytes_read);
    String request_string = {
     .data = (char*)event.recv.data,
     .count = event.recv.bytes_read
    };

    HTTPRequest *req = http_parse_request(&http_server_pool, request_string);
    if (req != 0) {
     printf("HTTP request, type %d, with url ", req->type);
     string_fprint(stdout, req->path);
     printf(" and options:\n");

     for (uint32_t i = 0; i < req->options_count; i++) {
      string_fprint(stdout, req->options[i].key);
      printf(": ");
      string_fprint(stdout, req->options[i].value);
      printf("\n");
     }

     int file = open(string_to_cstr(&http_server_pool, string_sub(req->path, 1, -1)), O_RDONLY);
     struct stat file_info;
     fstat(file, &file_info);

     if (file < 0) {
      async_send(event.recv.sock, (uint8_t*)http_not_found_header, strlen(http_not_found_header), 0);
     } else {
      HTTPFileTransfer *transfer = malloc(sizeof (HTTPFileTransfer));
      memzero(transfer);
      printf("Allocated: %p\n", transfer);

      transfer->sock = event.recv.sock;
      transfer->file = file;
      transfer->file_size = file_info.st_size;

      // TODO: WTF bro?
      transfer->mimetype = extension_to_mimetype(path_get_extension(&http_server_pool, req->path));
      printf("Sending file of size: %ld\n", file_info.st_size);

      async_read(file, ASYNC_MAX_TRANSFER_SIZE, 0, transfer);
     }
    }
   } else if (event.type == E_SEND) {
    HTTPFileTransfer *transfer = event.user_data;
    if (transfer != 0) {
     if (!transfer->is_over) {
      async_read(transfer->file, ASYNC_MAX_TRANSFER_SIZE, transfer->file_offset, transfer);
     }
    }
   } else if (event.type == E_CLOSE) {
    printf("Connection closed\n");

    HTTPFileTransfer *transfer = event.user_data;
    if (transfer) {
     transfer->is_over = true;
    }
   } else if (event.type == E_READ) {
    HTTPFileTransfer *transfer = event.user_data;
    if (!transfer->is_over) {
     if (transfer->file_offset == 0) {
      char *response_buffer = pool_alloc(
       &http_server_pool,
       128 + sizeof http_response_header + event.read.bytes_read
       );

      int offset = snprintf(
       response_buffer,
       128 + sizeof http_response_header,
       http_response_header,
       string_to_cstr(&http_server_pool, transfer->mimetype),
       transfer->file_size
       );

      memcpy(response_buffer + offset, event.read.data, event.read.bytes_read);

      async_send(transfer->sock, (uint8_t*)response_buffer, offset + event.read.bytes_read, transfer);

      transfer->file_offset += event.read.bytes_read;
     } else {
      if (event.read.bytes_read > 0) {
       async_send(transfer->sock, event.read.data, event.read.bytes_read, transfer);
      }

      transfer->file_offset += event.read.bytes_read;
      if (transfer->file_offset == transfer->file_size || event.read.bytes_read == 0) {
       transfer->is_over = true;
       close(event.read.file);
      }
     }
    }
   } else {
    printf("%d\n", event.type);
    CRASH("Erroneous response");
   }
  }
 }

 return 0;
}
