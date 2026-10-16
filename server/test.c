#include "async.c"

int main() {
 async_init();

 int sock = create_server(STR_L("0.0.0.0"), 2222);
 async_accept(sock, 0);

 while (true) {
  AsyncEvent event;
  async_get_event(&event);

  switch (event.type) {
  case E_ACCEPT: {
   printf("New connection!\n");

   async_recv(event.accept.socket, 0);
  } break;
  case E_RECV: {
   printf("Received data from connection\n");

   async_send(event.recv.sock, async_recv_buffer, event.recv.bytes_read, 0);
  } break;
  case E_SEND: {
   printf("Sent data to connection\n");
  } break;
  }
 }
}
