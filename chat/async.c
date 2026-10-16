#pragma once

#include "basic.c"

#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#define CRASH(error) printf("%s:%d: %s\n", __FILE__, __LINE__, error); abort()

typedef struct {
 enum {
  R_READ,
  R_WRITE
 } type;

 void *user_data;

 int file;
 uint64_t size;
 uint64_t offset;
} AsyncRequest;

typedef struct {
 enum {
  E_READ,
  E_WRITE,
  E_ACCEPT,
  E_RECV,
  E_SEND,
  E_CLOSE
  // TODO
 } type;

 void *user_data;

 int error;

 union {
  struct {
   int file;
   uint64_t bytes_read;
   uint8_t *data;
  } read;

  struct {
   int file;
  } write;

  struct {
   int server_socket;
   int client_socket;
   struct sockaddr_in client_addr;
  } accept;

  struct {
   int sock;
   uint64_t bytes_read;
   uint8_t *data;
  } recv;

  struct {
   int sock;
   uint64_t bytes_sent;
  } send;
 };
} AsyncEvent;

typedef struct {
 pthread_t thread_handle;
 int message_pipe;

 int instructions_pipe;
 int output_pipe;

 uint32_t result_size;
 int result_error;
} AsyncThreadState;

#define ASYNC_MAX_TRANSFER_SIZE Kilobytes(16)
#define ASYNC_MAX_PACKET_SIZE (1 << 16)

#define ASYNC_MAX_EPOLL_EVENTS 16

struct epoll_event async_epoll_events[ASYNC_MAX_EPOLL_EVENTS];
static uint32_t async_epoll_events_count;

static uint8_t async_read_buffer[ASYNC_MAX_TRANSFER_SIZE];
static uint8_t async_recv_buffer[ASYNC_MAX_PACKET_SIZE];

#define ASYNC_MAX_EVENTS 16
static AsyncEvent async_events[ASYNC_MAX_EVENTS];

#define ASYNC_THREADS_COUNT 1

static AsyncThreadState async_threads_state[ASYNC_THREADS_COUNT];
static uint32_t async_target_thread_index;
static int async_receive_pipe;

static void *async_thread_main(void *threadid) {
 uint32_t id = (uint64_t)threadid;
 AsyncThreadState *state = &async_threads_state[id];

 AsyncRequest req;
 uint8_t transfer_buffer[sizeof (AsyncEvent) + ASYNC_MAX_TRANSFER_SIZE];

 int read_from_pipe;
 while (true) {
  read_from_pipe = read(state->instructions_pipe, &req, sizeof req);
  if (read_from_pipe != sizeof req) { CRASH("Failed to read from pipe"); }

  AsyncEvent event;
  memzero(&event);

  event.user_data = req.user_data;

  switch (req.type) {
  case R_READ: {
   assert(req.size + sizeof (AsyncEvent) <= sizeof(transfer_buffer));
   int64_t bytes_read = pread(req.file, transfer_buffer + sizeof (AsyncEvent), req.size, req.offset);

   event.type = E_READ;

   if (bytes_read < 0) {
    event.error = errno;
   } else {
    event.read.file = req.file;
    event.read.bytes_read = (uint64_t)bytes_read;
    event.read.data = async_read_buffer;
   }

   memcpy(transfer_buffer, &event, sizeof (AsyncEvent));
   int64_t bytes_written = write(state->output_pipe,
                                 transfer_buffer,
                                 sizeof (AsyncEvent) + event.read.bytes_read);

   if (bytes_written != (int64_t)(sizeof (AsyncEvent) + event.read.bytes_read)) {
    CRASH("Failed to write to pipe");
   }
  } break;
  case R_WRITE: {
   read_from_pipe = read(state->instructions_pipe, transfer_buffer, req.size);
   if (read_from_pipe != (int64_t)req.size) { CRASH("Failed to read from pipe"); }

   int64_t bytes_written = 0;

   do {
    int64_t res = pwrite(req.file, transfer_buffer, req.size, req.offset + bytes_written);
    if (res < 0) {
     bytes_written = res;
     break;
    } else {
     bytes_written += res;
    }
   } while(bytes_written < (int64_t)req.size);

   event.type = E_WRITE;

   if (bytes_written < 0) {
    event.error = errno;
   } else {
    event.write.file = req.file;
   }

   if (write(state->output_pipe, &event, sizeof event) != sizeof event) {
    CRASH("Failed to write to output pipe");
   }
  } break;
  default: CRASH("Wrong request!");
  }
 }
}

typedef struct {
 int sock;
 uint32_t data_size;
 uint8_t data[];
} AsyncMessage;

#define ASYNC_SEND_QUEUE_SIZE Megabytes(16)

static struct {
 AsyncMessage *messages;
 uint64_t start;
 uint64_t end;
} async_send_queue;

static void async_send_queue_push(int sock, uint8_t *buffer, uint32_t size) {
 assert(size > 0);

 uint32_t message_size = sizeof (AsyncMessage) + size;

 if (async_send_queue.end + message_size > ASYNC_SEND_QUEUE_SIZE) {
  if (async_send_queue.end < async_send_queue.start &&
      async_send_queue.end + message_size >= async_send_queue.start)
  {
   CRASH("Send queue is full!");
  }

  memset((uint8_t*)async_send_queue.messages + async_send_queue.end, 0,
         ASYNC_SEND_QUEUE_SIZE - async_send_queue.end);
  async_send_queue.end = 0;

  if (async_send_queue.start == 0) {
   CRASH("Send queue is full");
  }
 }

 if (async_send_queue.end < async_send_queue.start &&
     async_send_queue.end + message_size >= async_send_queue.start)
 {
  CRASH("Send queue is full!");
 }

 AsyncMessage *message = (AsyncMessage*)((uint8_t*)async_send_queue.messages +
                                         async_send_queue.end);

 message->sock = sock;
 message->data_size = size;
 memcpy(message->data, buffer, size);

 async_send_queue.end += message_size;
}

static AsyncMessage *async_send_queue_pop(bool8_t do_it) {
 if (async_send_queue.start == async_send_queue.end) {
  return 0;
 }

 if (ASYNC_SEND_QUEUE_SIZE - async_send_queue.start < sizeof (AsyncMessage)) {
  async_send_queue.start = 0;
 }

 AsyncMessage *message = (AsyncMessage*)((uint8_t*)async_send_queue.messages + async_send_queue.start);
 if (message->data_size == 0) {
  async_send_queue.start = 0;
  message = (AsyncMessage*)((uint8_t*)async_send_queue.messages + async_send_queue.start);
 }

 if (async_send_queue.start == async_send_queue.end) {
  return 0;
 }

 uint32_t message_size = sizeof (AsyncMessage) + message->data_size;
 if (do_it) async_send_queue.start += message_size;
 return message;
}

static int async_epoll_fd;

static void async_init() {
 int err;

 async_send_queue.messages = allocate_pages(ASYNC_SEND_QUEUE_SIZE);
 async_send_queue.start = 0;
 async_send_queue.end = 0;

 int receive_pipe[2];
 err = pipe(receive_pipe);
 if (err < 0) { CRASH("Failed to create pipe"); }

 async_receive_pipe = receive_pipe[0];

 for (uint64_t thread_index = 0; thread_index < ASYNC_THREADS_COUNT; thread_index++) {
  int pipes[2];
  err = pipe(pipes);
  if (err < 0) { CRASH("Failed to create pipe"); }

  async_threads_state[thread_index].instructions_pipe = pipes[0];
  async_threads_state[thread_index].output_pipe = receive_pipe[1];
  async_threads_state[thread_index].message_pipe = pipes[1];

  err = pthread_create(&async_threads_state[thread_index].thread_handle, NULL,
                       async_thread_main, (void*)thread_index);

  if (err < 0) { CRASH("Failed to create thread"); }
 }

 async_epoll_fd = epoll_create1(0);
 if (async_epoll_fd < 0) { CRASH("Failed to create epoll file descriptor"); }

 struct epoll_event file_event;
 file_event.events = EPOLLIN;
 file_event.data.fd = async_receive_pipe;

 if (epoll_ctl(async_epoll_fd, EPOLL_CTL_ADD, async_receive_pipe, &file_event) < 0) {
  CRASH("Failed to add file descriptor!");
 }
}

static void async_write(int file, uint8_t *buffer, uint64_t size, uint64_t offset, void *user_data) {
 uint8_t data_for_thread[sizeof (AsyncRequest) + ASYNC_MAX_TRANSFER_SIZE];
 memset(data_for_thread, 0, sizeof data_for_thread);

 if (size > ASYNC_MAX_TRANSFER_SIZE) { CRASH("Couldn't write this much memory!"); }

 AsyncRequest *req = (AsyncRequest*)data_for_thread;

 req->type = R_WRITE;
 req->file = file;
 req->size = size;
 req->offset = offset;
 req->user_data = user_data;

 memcpy(data_for_thread + sizeof (AsyncRequest), buffer, size);

 if (write(async_threads_state[async_target_thread_index].message_pipe, data_for_thread,
           sizeof (AsyncRequest) + size) != (int64_t)(sizeof (AsyncRequest) + size))
 {
  CRASH("Failed to write to pipe");
 }

 async_target_thread_index = (async_target_thread_index + 1) % ASYNC_THREADS_COUNT;
}

static void async_read(int file, uint64_t size, uint64_t offset, void *user_data) {
 AsyncRequest req;
 memzero(&req);

 if (size > ASYNC_MAX_TRANSFER_SIZE) { CRASH("Couldn't read this much memory!"); }

 req.type = R_READ;
 req.file = file;
 req.size = size;
 req.offset = offset;
 req.user_data = user_data;

 if (write(async_threads_state[async_target_thread_index].message_pipe, &req, sizeof req) != sizeof req) {
  CRASH("Failed to write to pipe");
 }
 async_target_thread_index = (async_target_thread_index + 1) % ASYNC_THREADS_COUNT;
}

#define ASYNC_MAX_SOCKETS 100000

#define S_ACCEPT 0x1
#define S_RECV 0x2
#define S_SEND 0x4

typedef struct {
 void *user_data;
 int sock;
 uint8_t flags;
 uint64_t packets_to_send;
} AsyncSocketData;

static AsyncSocketData async_connections[ASYNC_MAX_SOCKETS];
static uint32_t async_connections_count;

// TODO: This function is retarded and does the job of two different functions. Fix it.
static void async_add_socket_event(int sock, void *user_data, uint32_t epoll_events, uint8_t flags) {
 for (uint32_t i = 0; i < async_connections_count; i++) {
  if (async_connections[i].sock == sock) {
   struct epoll_event event;
   event.events = epoll_events;
   event.data.fd = sock;

   async_connections[i].flags |= flags;
   if (user_data != 0) {
    async_connections[i].user_data = user_data;
   }

   if (flags & S_SEND) async_connections[i].packets_to_send++;

   if (epoll_ctl(async_epoll_fd, EPOLL_CTL_MOD, sock, &event) < 0) {
    CRASH("Failed to modify file descriptor!");
   }

   return;
  }
 }

 assert(!(flags & S_SEND));

 // If doesn't exist yet
 if (async_connections_count >= ASYNC_MAX_SOCKETS) {
  CRASH("Too many accepts!");
 }

 struct epoll_event event;
 event.events = epoll_events;
 event.data.fd = sock;

 if (epoll_ctl(async_epoll_fd, EPOLL_CTL_ADD, sock, &event) < 0) {
  CRASH("Failed to add file descriptor!");
 }

 AsyncSocketData *data = &async_connections[async_connections_count];
 memzero(data);

 data->flags = flags;
 data->sock = sock;
 data->user_data = user_data;

 async_connections_count++;
}

static void async_accept(int sock, void *user_data) {
 async_add_socket_event(sock, user_data, EPOLLIN, S_ACCEPT);
}

static void async_add_connection(int sock, void *user_data) {
 int one = 1;
 if (setsockopt(sock, SOL_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
  CRASH("Couldn't set TCP_NODELAY");
 }

 async_add_socket_event(sock, user_data, EPOLLIN, S_RECV);
}

static void async_send(int sock, uint8_t *buffer, uint32_t size, void *user_data) {
 if (size > ASYNC_MAX_PACKET_SIZE) { CRASH("Too much data sent"); }

 async_send_queue_push(sock, buffer, size);
 async_add_socket_event(sock, user_data, EPOLLIN | EPOLLOUT, S_SEND);
}

static void async_close(int sock) {
 bool8_t found = false;

 uint64_t i;
 for (i = 0; i < async_connections_count; i++) {
  if (async_connections[i].sock == sock) {
   found = true;
   break;
  }
 }

 if (!found) return;

 if (epoll_ctl(async_epoll_fd, EPOLL_CTL_DEL, async_connections[i].sock, 0) < 0) {
  CRASH("Failed to remove file descriptor!");
 }

 close(async_connections[i].sock);

 async_connections[i] = async_connections[--async_connections_count];
}

static uint32_t async_wait_for_events() {
 uint32_t events_count = 0;

 if (async_epoll_events_count == 0) {
  int nfds;
  while (true) {
   nfds = epoll_wait(async_epoll_fd, async_epoll_events, ASYNC_MAX_EPOLL_EVENTS, -1);
   if (nfds < 0) {
    if (errno == EINTR) {
     printf("Warning: Interrupted\n");
     continue;
    }

    printf("Error: %s\n", strerror(errno));
    CRASH("Couldn't wait on file descriptors");
   } else {
    break;
   }
  }

  async_epoll_events_count = nfds;
 }

 assert(async_epoll_events_count > 0);

 struct epoll_event *current_epoll_event = &async_epoll_events[--async_epoll_events_count]; // Should probably be a queue...

 if (current_epoll_event->data.fd == async_receive_pipe) { // File events
  AsyncEvent *event = &async_events[events_count++];
  memzero(event);

  int64_t bytes_read = read(async_receive_pipe, event, sizeof (AsyncEvent));
  if (bytes_read != sizeof (AsyncEvent)) { CRASH("Failed to read from pipe"); }

  if (event->error == 0 && event->type == E_READ) {
   int64_t read_from_pipe = 0;
   do {
    read_from_pipe += read(async_receive_pipe,
                           async_read_buffer + read_from_pipe,
                           event->read.bytes_read - read_from_pipe);
   } while (read_from_pipe < (int64_t)event->read.bytes_read);
  }
 }

 // Network events
 int32_t i = 0;

 while (i < (int32_t)async_connections_count) {
  if (async_connections[i].sock == current_epoll_event->data.fd) {
   if (current_epoll_event->events & EPOLLIN) {
    AsyncEvent *event = &async_events[events_count++];
    memzero(event);

    if (async_connections[i].flags & S_ACCEPT) {
     event->type = E_ACCEPT;
     event->user_data = async_connections[i].user_data;

     socklen_t addrlen = sizeof (struct sockaddr_in);

     event->accept.server_socket = async_connections[i].sock;
     if ((event->accept.client_socket = accept(event->accept.server_socket,
                                               (struct sockaddr*)&event->accept.client_addr, &addrlen)) < 0)
     {
      event->error = errno;
     }
    } else if (async_connections[i].flags & S_RECV) {
     event->type = E_RECV;
     event->user_data = async_connections[i].user_data;

     int64_t bytes_read = recv(async_connections[i].sock, async_recv_buffer, ASYNC_MAX_PACKET_SIZE, 0);
     if (bytes_read == 0 || (bytes_read < 0 && errno == ECONNRESET)) {
      event->type = E_CLOSE;
      printf("Recv close\n");
      // Remove RECV
      if (epoll_ctl(async_epoll_fd, EPOLL_CTL_DEL, async_connections[i].sock, 0) < 0) {
       CRASH("Failed to remove file descriptor!");
      }

      close(async_connections[i].sock);

      async_connections[i] = async_connections[--async_connections_count];
      continue;
     } else if (bytes_read < 0) {
      event->error = errno;
      printf("Recv error\n");
     }  else {
      event->recv.sock = async_connections[i].sock;
      event->recv.bytes_read = bytes_read;
      event->recv.data = async_recv_buffer;
     }
    }
   }

   if (async_connections[i].flags & S_SEND && current_epoll_event->events & EPOLLOUT) {
    AsyncMessage *message = async_send_queue_pop(false);
    if (message == 0) { CRASH("The send queue is empty!"); }

    int sock = message->sock;

    if (sock == current_epoll_event->data.fd) {
     AsyncEvent *event = &async_events[events_count++];
     memzero(event);

     event->user_data = async_connections[i].user_data;
     event->type = E_SEND;
     event->send.sock = sock;

     int64_t bytes_sent = send(sock, message->data, message->data_size, MSG_NOSIGNAL);
     if (bytes_sent < 0) {
      if (errno == EPIPE || errno == ECONNRESET) {
       event->type = E_CLOSE;
       printf("send close\n");
       if (epoll_ctl(async_epoll_fd, EPOLL_CTL_DEL, sock, 0) < 0) {
        CRASH("Failed to remove file descriptor!");
       }

       close(sock);

       async_connections[i] = async_connections[--async_connections_count];
       continue;
      } else {
       event->error = errno;
      }
     } else {
      async_send_queue_pop(true);

      assert(async_connections[i].packets_to_send > 0);
      async_connections[i].packets_to_send--;

      assert(bytes_sent == message->data_size);

      event->send.bytes_sent = message->data_size;

      if (async_connections[i].packets_to_send == 0) {
       struct epoll_event epoll_event;
       epoll_event.events = EPOLLIN;
       epoll_event.data.fd = sock;

       if (epoll_ctl(async_epoll_fd, EPOLL_CTL_MOD, sock, &epoll_event) < 0) {
        CRASH("Failed to modify file descriptor!");
       }

       async_connections[i].flags &= ~S_SEND;
      }
     }
    }
   }

   break;
  }

  i++;
 }

 assert(events_count < ASYNC_MAX_EVENTS);

 return events_count;
}
