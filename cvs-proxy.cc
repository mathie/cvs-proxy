/* $Id: cvs-proxy.cc,v 1.6 2003/08/14 15:12:14 mathie Exp $
 *
 * $Log: cvs-proxy.cc,v $
 * Revision 1.6  2003/08/14 15:12:14  mathie
 * * Switch to using a dynamically-allocated connection list.  The code for
 *   coping with the head of the list being closed is fugly.
 * * reaping children is still broken.  I really need to rework that...
 *
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

#include <assert.h>
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

struct connection 
{
  struct connection *next;
  int tcp_fd;
  pid_t spawned_pid;
  int spawned_rfd;
  int spawned_wfd;
  struct sockaddr_in sa;
};

struct connection *cl_head = NULL;
struct connection fake_head;

void dump_connection_list(void);

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
    struct connection *cur = cl_head;

    dump_connection_list();
    
    FD_ZERO(&rfds);
    FD_SET(sockfd, &rfds);
    while(cur) {
      FD_SET(cur->tcp_fd, &rfds);
      FD_SET(cur->spawned_rfd, &rfds);
      cur = cur->next;
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

      {
        struct connection *newcon = calloc(1, sizeof(struct connection));
        newcon->tcp_fd = accept(sockfd,
                                (struct sockaddr *)&newcon->sa,
                                &client_len);
        if(newcon->tcp_fd < 0) {
          perror("accept()");
          exit(EXIT_FAILURE);
        }
        if(ioctl(newcon->tcp_fd, FIONBIO, &on) < 0) {
          perror("ioctl(FIONBIO)");
          exit(EXIT_FAILURE);
        }
        printf("Connection accepted from %s\n",
               inet_ntoa(newcon->sa.sin_addr));

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
          if ((newcon->spawned_pid = fork()) < 0) {
            perror("fork()");
          } else if (newcon->spawned_pid > 0) { /* Parent processes */
            close(pipe1[0]);
            close(pipe2[1]);
            newcon->spawned_wfd = pipe1[1];
            newcon->spawned_rfd = pipe2[0];
            if(ioctl(newcon->spawned_rfd, FIONBIO, &on) < 0) {
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
        newcon->next = cl_head;
        cl_head = newcon;
      }
      ret--;
    }
    for (cur = cl_head; cur; cur = cur->next) {
      if (ret == 0) {
        /* No more fds left to check */
        break;
      }
      if(FD_ISSET(cur->tcp_fd, &rfds)) { 
        while (1) {
          unsigned char buf[70];
          int n_bytes;

          n_bytes = read(cur->tcp_fd, buf, 70);
          if (n_bytes < 0) {
            if((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
              /* No data left to read. */
              break;
            } else {
              perror("read()");
              exit(EXIT_FAILURE);
            }
          } else if (n_bytes == 0) { /* EOF */
            int status, ret;
            close(cur->tcp_fd);
            close(cur->spawned_wfd);
            close(cur->spawned_rfd);
            FD_CLR(cur->spawned_rfd, &rfds);

            printf("Waiting on pid %d quitting.\n", cur->spawned_pid);
            if ((ret = waitpid(cur->spawned_pid, &status, WNOHANG)) < 0) {
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
            printf("Disconnection from %s.\n", inet_ntoa(cur->sa.sin_addr));

            if(cur == cl_head) {
              cl_head = cur->next;
              fake_head.next = cl_head;
              free(cur);
              cur = &fake_head;
            } else {
              struct connection *prev = cl_head;
              while (prev && prev->next != cur) {
                prev = prev->next;
              }
              assert(prev->next == cur);
              prev->next = cur->next;
              free(cur);
              cur = prev;
            }
            break;
          }

          if(write(cur->spawned_wfd, buf, n_bytes) < 0) {
            perror("write()");
            exit(EXIT_FAILURE);
          }
        }
        ret--;
      }
      if(ret && FD_ISSET(cur->spawned_rfd, &rfds)) { 
        while (1) {
          unsigned char buf[70];
          int n_bytes;

          n_bytes = read(cur->spawned_rfd, buf, 70);
          if (n_bytes < 0) {
            if((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
              /* No data left to read. */
              break;
            } else {
              perror("read()");
              exit(EXIT_FAILURE);
            }
          } else if (n_bytes == 0) { /* EOF */
            int status, ret;
            close(cur->tcp_fd);
            close(cur->spawned_wfd);
            close(cur->spawned_rfd);
            FD_CLR(cur->spawned_rfd, &rfds);

            printf("Waiting on pid %d quitting.\n", cur->spawned_pid);
            if ((ret = waitpid(cur->spawned_pid, &status, WNOHANG)) < 0) {
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
            printf("Disconnection from %s.\n", inet_ntoa(cur->sa.sin_addr));

            if(cur == cl_head) {
              cl_head = cur->next;
              fake_head.next = cl_head;
              free(cur);
              cur = &fake_head;
            } else {
              struct connection *prev = cl_head;
              while (prev && prev->next != cur) {
                prev = prev->next;
              }
              assert(prev->next == cur);
              prev->next = cur->next;
              free(cur);
              cur = prev;
            }
            break;
          }

          if(write(cur->tcp_fd, buf, n_bytes) < 0) {
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

void dump_connection_list() 
{
  struct connection *cur;
  for(cur = cl_head; cur; cur = cur->next) {
    printf("Connection pid = %d, tcp_fd = %d, rfd = %d, wfd = %d, s_port = %d.\n",
           cur->spawned_pid, cur->tcp_fd, cur->spawned_rfd, cur->spawned_wfd,
           ntohs(cur->sa.sin_port));
  }
}
