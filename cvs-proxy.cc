/* $Id: cvs-proxy.cc,v 1.4 2003/08/14 07:12:17 mathie Exp $
 *
 * $Log: cvs-proxy.cc,v $
 * Revision 1.4  2003/08/14 07:12:17  mathie
 * * Spawn another process and connect the incoming TCP stream to its
 *   stdin; connect its stdout to the outgoing TCP stream.
 * * Decrement the number of fds that need checked every time one is
 *   checked, so that wading through the entire client list can be avoided
 *   for most select() calls.
 *
 * Revision 1.3  2003/08/13 11:40:21  mathie
 * * Use select() to multiplex handling of multiple connections at once.
 *
 * Revision 1.2  2003/08/13 10:15:46  mathie
 * * Added RCS keywords.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_CLIENTS 10
int client_fds[MAX_CLIENTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
int echo_rfds[MAX_CLIENTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
int echo_wfds[MAX_CLIENTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
int n_clients = 0;

int main (int argc, char *argv[]) 
{
  int sockfd, ret = 0;
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
    struct timeval timeout;
    struct fd_set rfds;
    int i;
    
    FD_ZERO(&rfds);
    FD_SET(sockfd, &rfds);
    for (i = 0; i < n_clients; i++) {
      FD_SET(client_fds[i], &rfds);
      FD_SET(echo_rfds[i], &rfds);
    }
    
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    ret = select(FD_SETSIZE, &rfds, NULL, NULL, &timeout);
    if (ret < 0) {
      perror("select()");
      exit(EXIT_FAILURE);
    } else if (ret == 0) {
      printf("select() timed out.\n");
      continue;
    }
    if(FD_ISSET(sockfd, &rfds)) {
      int con, client_len = sizeof(struct sockaddr_in);
      char on = 1;
      struct sockaddr_in client;

      memset (&client, 0, sizeof(client));
      con = accept(sockfd, (struct sockaddr *)&client, &client_len);
      if (con < 0) {
        perror("accept()");
        exit(EXIT_FAILURE);
      }
      if (n_clients >= MAX_CLIENTS) {
        printf("Maximum number of concurrent clients (b)reached!\n");
        close(con);
      } else {
        if(ioctl(con, FIONBIO, &on) < 0) {
          perror("ioctl(FIONBIO)");
          exit(EXIT_FAILURE);
        }
        printf("Connection accepted from %s\n", inet_ntoa(client.sin_addr));

        /* Fork a child with a pipe */
        {
          int childpid, pipe1[2], pipe2[2];
          if(pipe(pipe1) < 0) {
            perror("pipe(pipe1)");
            exit(EXIT_FAILURE);
          }
          if(pipe(pipe2) < 0) {
            perror("pipe(pipe2)");
            exit(EXIT_FAILURE);
          }
          if ((childpid = fork()) < 0) {
            perror("fork()");
          } else if (childpid > 0) { /* Parent processes */
            close(pipe1[0]);
            close(pipe2[1]);
            echo_wfds[n_clients] = pipe1[1];
            echo_rfds[n_clients] = pipe2[0];
            if(ioctl(echo_rfds[n_clients], FIONBIO, &on) < 0) {
              perror("ioctl(echo_fd, FIONBIO)");
              exit(EXIT_FAILURE);
            }
          } else { /* Child process */
            close(pipe1[1]);
            close(pipe2[0]);
            if(dup2(pipe1[0], STDIN_FILENO) < 0) {
              perror("dup2(STDIN)");
              exit(EXIT_FAILURE);
            }
            if(dup2(pipe2[1], STDOUT_FILENO) < 0) {
              perror("dup2(STDOUT)");
              exit(EXIT_FAILURE);
            }
            if (execl("/Users/mathie/src/cvs-proxy/echo-stdin", "echo-stdin", NULL) < 0) {
              perror("exec()");
              exit(EXIT_FAILURE);
            }
          }
        }
        client_fds[n_clients++] = con;
      }
      ret--;
    }
    for (i = 0; i < n_clients; i++) {
      if (ret == 0) {
        /* No more fds left to check */
        break;
      }
      if(FD_ISSET(client_fds[i], &rfds)) { 
        while (1) {
          unsigned char buf[70];
          int n_bytes;

          n_bytes = read(client_fds[i], buf, 70);
          if (n_bytes < 0) {
            if((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
              /* No data left to read. */
              break;
            } else {
              perror("read()");
              exit(EXIT_FAILURE);
            }
          } else if (n_bytes == 0) { /* EOF */
            int j;
            if(close(client_fds[i]) < 0) {
              perror("close()");
              exit(EXIT_FAILURE);
            }

            /* Jiggle the client_fds array to remove the client that
               just closed.  Eww. */
            n_clients--;
            for (j = i; j < n_clients; j++) {
              client_fds[j] = client_fds[j+1];
            }
            i--;
            
            break;
          }

          if(write(echo_wfds[i], buf, n_bytes) < 0) {
            perror("write()");
            exit(EXIT_FAILURE);
          }
        }
        ret--;
      }
      if(ret && FD_ISSET(echo_rfds[i], &rfds)) { 
        while (1) {
          unsigned char buf[70];
          int n_bytes;

          n_bytes = read(echo_rfds[i], buf, 70);
          if (n_bytes < 0) {
            if((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
              /* No data left to read. */
              break;
            } else {
              perror("read()");
              exit(EXIT_FAILURE);
            }
          } else if (n_bytes == 0) { /* EOF */
            /* FIXME: Closing the pipe is utterly broken and will not
               work. */
            break;
          }

          if(write(client_fds[i], buf, n_bytes) < 0) {
            perror("write()");
            exit(EXIT_FAILURE);
          }
        }
        ret--;
      }
    }
  }
  return 0;
}
