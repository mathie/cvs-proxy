#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main (int argc, char *argv[]) 
{
  int sockfd;
  struct sockaddr_in addr;
  struct servent *pserver_port;
  
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if(sockfd < 0) {
    perror("socket()");
    exit(EXIT_FAILURE);
  }

  memset(&addr, 0, sizeof(addr));

  pserver_port = getservbyname("cvspserver", "tcp");
  if (pserver_port == NULL) {
    addr.sin_port = htons(2401);
  } else {
    addr.sin_port = htons(pserver_port->s_port);
  }
  
  if(bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind()");
    exit(EXIT_FAILURE);
  }

  if(listen(sockfd, 32) != 0) {
    perror("listen()");
    exit(EXIT_FAILURE);
  }

  while(1) {
    int con, client_len = 0;
    struct sockaddr_in client;
    memset (&client, 0, sizeof(client));
    con = accept(sockfd, (struct sockaddr *)&client, &client_len);
    if (con < 0) {
      perror("accept()");
      exit(EXIT_FAILURE);
    }
    printf("Connection accepted from %s\n", inet_ntoa(client.sin_addr));
    while (1) {
      unsigned char buf[70];
      int n_bytes;

      n_bytes = read(con, buf, 70);
      if (n_bytes < 0) {
        perror("read()");
        exit(EXIT_FAILURE);
      } else if (n_bytes == 0) { /* EOF */
        if(close(con) < 0) {
          perror("close()");
          exit(EXIT_FAILURE);
        }
        break;
      }

      if(write(con, buf, n_bytes) < 0) {
        perror("write()");
        exit(EXIT_FAILURE);
      }
    }
    printf("%s disconnected.\n", inet_ntoa(client.sin_addr));
  }
  return 0;
}
