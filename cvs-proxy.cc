/* $Id: cvs-proxy.cc,v 1.5 2003/08/14 13:44:01 mathie Exp $
 *
 * $Log: cvs-proxy.cc,v $
 * Revision 1.5  2003/08/14 13:44:01  mathie
 * * Collect all the data about a connection into a single structure.
 * * Close connections correctly.
 *
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
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_CLIENTS 10

struct connection 
{
  int tcp_fd;
  pid_t spawned_pid;
  int spawned_rfd;
  int spawned_wfd;
  struct sockaddr_in sa;
};

struct connection conn_list[MAX_CLIENTS];
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
      FD_SET(conn_list[i].tcp_fd, &rfds);
      FD_SET(conn_list[i].spawned_rfd, &rfds);
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
      int client_len = sizeof(struct sockaddr_in);
      char on = 1;

      if (n_clients >= MAX_CLIENTS) {
        struct sockaddr_in sa;
        int con;
        memset(&sa, 0, sizeof(struct sockaddr_in));
        con = accept(sockfd, (struct sockaddr *)&sa, &client_len);
        if (con < 0) {
          printf("accept failed, but it doesn't matter since we were going to bounce the connection anyway...\n");
        } else {
          printf("Maximum number of concurrent clients reached.  Refusing connection from %s\n", inet_ntoa(sa.sin_addr));
          close(con);
        }
      } else {
        memset (&conn_list[n_clients].sa, 0, sizeof(struct sockaddr_in));
        conn_list[n_clients].tcp_fd = accept(sockfd,
                                             (struct sockaddr *)&conn_list[n_clients].sa,
                                             &client_len);
        if (conn_list[n_clients].tcp_fd < 0) {
          perror("accept()");
          exit(EXIT_FAILURE);
        }
        if(ioctl(conn_list[n_clients].tcp_fd, FIONBIO, &on) < 0) {
          perror("ioctl(FIONBIO)");
          exit(EXIT_FAILURE);
        }
        printf("Connection accepted from %s\n",
               inet_ntoa(conn_list[n_clients].sa.sin_addr));

        /* Fork a child with a pipe */
        {
          int pipe1[2], pipe2[2];
          if(pipe(pipe1) < 0) {
            perror("pipe(pipe1)");
            exit(EXIT_FAILURE);
          }
          if(pipe(pipe2) < 0) {
            perror("pipe(pipe2)");
            exit(EXIT_FAILURE);
          }
          if ((conn_list[n_clients].spawned_pid = fork()) < 0) {
            perror("fork()");
          } else if (conn_list[n_clients].spawned_pid > 0) { /* Parent processes */
            close(pipe1[0]);
            close(pipe2[1]);
            conn_list[n_clients].spawned_wfd = pipe1[1];
            conn_list[n_clients].spawned_rfd = pipe2[0];
            if(ioctl(conn_list[n_clients].spawned_rfd, FIONBIO, &on) < 0) {
              perror("ioctl(spawned_rfd, FIONBIO)");
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
        n_clients++;
      }
      ret--;
    }
    for (i = 0; i < n_clients; i++) {
      if (ret == 0) {
        /* No more fds left to check */
        break;
      }
      if(FD_ISSET(conn_list[i].tcp_fd, &rfds)) { 
        while (1) {
          unsigned char buf[70];
          int n_bytes;

          n_bytes = read(conn_list[i].tcp_fd, buf, 70);
          if (n_bytes < 0) {
            if((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
              /* No data left to read. */
              break;
            } else {
              perror("read()");
              exit(EXIT_FAILURE);
            }
          } else if (n_bytes == 0) { /* EOF */
            int j, status, ret;
            close(conn_list[i].tcp_fd);
            close(conn_list[i].spawned_wfd);
            close(conn_list[i].spawned_rfd);
            FD_CLR(conn_list[i].spawned_rfd, &rfds);

            if ((ret = waitpid(conn_list[i].spawned_pid, &status, 0)) < 0) {
              perror("waitpid()");
              exit(EXIT_FAILURE);
            } else if (ret == 0) {
              printf("Bugger.  Nothing wanted to report its status.\n");
            } else if (WIFEXITED(status)) {
              printf("Child %d exited with status code %d.\n", ret, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
              printf("Child %d was killed by signal %d%s.\n", ret, WTERMSIG(status),
                     WCOREDUMP(status) ? " (core dumped)" : "");
            } else if (WIFSTOPPED(status)) {
              printf("Child %d is stopped by signal %d.\n", ret, WSTOPSIG(status));
            } else {
              printf("Child %d exited through some bizarre incantation %d.\n", ret, status);
            }
            printf("Disconnection from %s.\n", inet_ntoa(conn_list[i].sa.sin_addr));
            
            for (j = i; j < (n_clients - 1); j++) {
              memcpy(&conn_list[j], &conn_list[j+1], sizeof(struct connection));
            }

            n_clients--;
            i--;
            break;
          }

          if(write(conn_list[i].spawned_wfd, buf, n_bytes) < 0) {
            perror("write()");
            exit(EXIT_FAILURE);
          }
        }
        ret--;
      }
      if(ret && FD_ISSET(conn_list[i].spawned_rfd, &rfds)) { 
        while (1) {
          unsigned char buf[70];
          int n_bytes;

          n_bytes = read(conn_list[i].spawned_rfd, buf, 70);
          if (n_bytes < 0) {
            if((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
              /* No data left to read. */
              break;
            } else {
              perror("read()");
              exit(EXIT_FAILURE);
            }
          } else if (n_bytes == 0) { /* EOF */
            int j, status;
            close(conn_list[i].tcp_fd);
            close(conn_list[i].spawned_wfd);
            close(conn_list[i].spawned_rfd);
            FD_CLR(conn_list[i].spawned_rfd, &rfds);

            if (waitpid(conn_list[i].spawned_pid, &status, WNOHANG) < 0) {
              perror("waitpid()");
              exit(EXIT_FAILURE);
            }
            if (WEXITSTATUS(status)) {
              printf("Child exited with status code %d.\n", WEXITSTATUS(status));
            } else {
              printf("Child exited through some bizarre incantation %d.\n", status);
            }
            printf("Disconnection from %s.\n", inet_ntoa(conn_list[i].sa.sin_addr));
            
            for (j = i; j < (n_clients - 1); j++) {
              memcpy(&conn_list[j], &conn_list[j+1], sizeof(struct connection));
            }

            n_clients--;
            i--;
            break;
          }

          if(write(conn_list[i].tcp_fd, buf, n_bytes) < 0) {
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
