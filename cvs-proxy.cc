/* $Id: cvs-proxy.cc,v 1.10 2003/08/15 10:38:41 mathie Exp $
 *
 * $Log: cvs-proxy.cc,v $
 * Revision 1.10  2003/08/15 10:38:41  mathie
 * * Fix option parsing for -d.
 * * exec() the CVS binary, passing in the appropriate arguments.
 *
 * Revision 1.9  2003/08/15 07:04:12  mathie
 * * Basic argument parsing and validation.
 *
 * Revision 1.8  2003/08/15 06:10:12  mathie
 * * Closing of connections now works reliably (though there is the
 *   assumption that the client will be well-behaved).  Still skipping a
 *   descriptor from the list in the case of a closed connection though.
 *
 * Revision 1.7  2003/08/14 16:59:10  mathie
 * * Organise code into functions.  This should have reduced the
 *   duplication of code somewhat.
 *
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
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>

#define DEFAULT_LISTEN_PORT 2401

struct connection 
{
  struct connection *next;
  int tcp_fd;
  pid_t spawned_pid;
  int spawned_rfd;
  int spawned_wfd;
  struct sockaddr_in sa;
};
typedef struct connection *connection_list;

connection_list conn_list = NULL;
char *cvs_binary = NULL, *local_cvs_root = NULL, *remote_cvs_host = NULL,
  *remote_cvs_port = NULL, *remote_cvs_path = NULL;

int parse_args(int argc, char *argv[]);
void usage(void);
int init_socket(void);
struct connection *accept_connection(int sockfd);
int close_connection(struct connection *con);
int fork_child(struct connection *con);
int read_from_tcp(struct connection *con);
int read_from_child(struct connection *con);
int fdcpy(int dst, int src);
void add_to_connection_list(connection_list *list, struct connection *item);
void del_from_connection_list(connection_list *list, struct connection *item);
void dump_connection_list(void);

int main (int argc, char *argv[]) 
{
  int sockfd;

  if (parse_args(argc, argv) < 0) {
    usage();
    exit(EXIT_FAILURE);
  }
  
  if ((sockfd = init_socket()) < 0) {
    perror("init_socket()");
    exit(EXIT_FAILURE);
  }
  
  while(1) {
    struct timeval timeout;
    struct fd_set rfds;
    int n_active_fds;
    struct connection *cur;

    FD_ZERO(&rfds);
    FD_SET(sockfd, &rfds);
    for(cur = conn_list; cur; cur = cur->next) {
      FD_SET(cur->tcp_fd, &rfds);
      FD_SET(cur->spawned_rfd, &rfds);
    }
    
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    n_active_fds = select(FD_SETSIZE, &rfds, NULL, NULL, &timeout);
    if (n_active_fds < 0) {
      perror("select()");
      exit(EXIT_FAILURE);
    } else if (n_active_fds == 0) {
      printf("select() timed out.\n");
      continue;
    }

    /* See if it's action on the TCP listening socket which woke us up */
    if(FD_ISSET(sockfd, &rfds)) {
      struct connection *newcon = accept_connection(sockfd);
      if (newcon == NULL) {
        perror("accept_connection()");
      } else {
        add_to_connection_list(&conn_list, newcon);
      }
      n_active_fds--;
    }
    for(cur = conn_list; cur && n_active_fds; cur = cur->next) {
      if(FD_ISSET(cur->tcp_fd, &rfds)) {
        read_from_tcp(cur);
        n_active_fds--;
      }
      if(n_active_fds && FD_ISSET(cur->spawned_rfd, &rfds)) { 
        read_from_child(cur);
        n_active_fds--;
      }
    }
  }
  return 0;
}

int parse_args(int argc, char *argv[]) 
{
  struct stat sb;
  int ch;
  while((ch = getopt(argc, argv, "b:l:h:p:d:")) != -1) {
    printf("arg %c, value = %s.\n", ch, (optarg ? optarg : "(none)"));
    
    switch(ch) {
    case 'b':
      cvs_binary = strdup(optarg);
      break;
    case 'l':
      local_cvs_root = strdup(optarg);
      break;
    case 'h':
      remote_cvs_host = strdup(optarg);
      break;
    case 'p':
      remote_cvs_port = strdup(optarg);
      break;
    case 'd':
      remote_cvs_path = strdup(optarg);
      break;
    default:
      return -1;
    }
  }
  if(cvs_binary == NULL) {
    cvs_binary = strdup("/usr/bin/cvs");
  }
  if(access(cvs_binary, X_OK) < 0) {
    perror("Cannot access cvs binary");
    return -1;
  }
  
  if(local_cvs_root == NULL) {
    printf("%s: Local CVS root directory required.\n", argv[0]);
    return -1;
  }
  if(stat(local_cvs_root, &sb) < 0) {
    perror("Cannot stat local CVS root");
    return -1;
  }
  if(!(sb.st_mode & S_IFDIR)) {
    printf("CVS root %s is not a directory.\n", local_cvs_root);
    return -1;
  }
  
  if(remote_cvs_host == NULL) {
    printf("%s: Remote CVS host required.\n", argv[0]);
    return -1;
  }
  if(remote_cvs_port == NULL) {
    remote_cvs_port = strdup("2401");
  }
  if(remote_cvs_path == NULL) {
    printf("%s: Remote CVS path required.\n", argv[0]);
    return -1;
  }
  
  return 0;
}

void usage(void)
{
  printf("Usage:\n");
  printf("  -b <filename>\tPath to CVS binary\n"
         "  -l <path>\tLocal CVS root\n"
         "  -h <hostname>\tRemote CVS host\n"
         "  -p <port>\tRemote CVS port\n"
         "  -d <path>\tRemote CVS root\n");
}

/* Initialise and bind a TCP socket to listen on.  Returns the bound
   socket on success or -1 on error (with errno set appropriately). */
int init_socket(void) 
{
  int sockfd;
  struct sockaddr_in addr;
  struct servent *pserver_port;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if(sockfd < 0) {
    return sockfd;
  }

  memset(&addr, 0, sizeof(addr));

  pserver_port = getservbyname("cvspserver", "tcp");
  if (pserver_port == NULL) {
    addr.sin_port = htons(DEFAULT_LISTEN_PORT);
  } else {
    addr.sin_port = htons(pserver_port->s_port);
  }

  if(bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    int old_errno = errno;
    close(sockfd);
    errno = old_errno;
    return -1;
  }

  if(listen(sockfd, 32) != 0) {
    int old_errno = errno;
    close(sockfd);
    errno = old_errno;
    return -1;
  }

  return sockfd;
}

struct connection *accept_connection(int sockfd) 
{
  int client_len = sizeof(struct sockaddr_in);
  char on = 1;
  
  struct connection *newcon = calloc(1, sizeof(struct connection));
  if(newcon == NULL) {
    return NULL;
  }
  
  newcon->tcp_fd = accept(sockfd, (struct sockaddr *)&newcon->sa, &client_len);
  if(newcon->tcp_fd < 0) {
    int old_errno = errno;
    free(newcon);
    errno = old_errno;
    return NULL;
  }
  if(ioctl(newcon->tcp_fd, FIONBIO, &on) < 0) {
    int old_errno = errno;
    close(newcon->tcp_fd);
    free(newcon);
    errno = old_errno;
    return NULL;
  }

  if(fork_child(newcon) < 0) {
    int old_errno = errno;
    close(newcon->tcp_fd);
    free(newcon);
    errno = old_errno;
    return NULL;
  }
  return newcon;
}

int fork_child(struct connection *con)
{
  int pipe1[2], pipe2[2], on = 1;
  if(pipe(pipe1) < 0) {
    return -1;
  }
  if(pipe(pipe2) < 0) {
    int old_errno = errno;
    close(pipe1[0]);
    close(pipe1[1]);
    errno = old_errno;
    return -1;
  }

  if ((con->spawned_pid = fork()) < 0) {
    int old_errno = errno;
    close(pipe1[0]);
    close(pipe1[1]);
    close(pipe2[0]);
    close(pipe2[1]);
    errno = old_errno;
    return -1;
  } else if(con->spawned_pid == 0) { /* Child process */
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

    /* Close all other open descriptors */
    {
      int i;
      for(i = 3; i < getdtablesize(); i++) close(i); 
    }
    
    if (execl(cvs_binary, cvs_binary, "--allow-root", local_cvs_root,
              "pserver", NULL) < 0) {
      perror("exec()");
      exit(EXIT_FAILURE);
    }
  }
  
  close(pipe1[0]);
  close(pipe2[1]);
  con->spawned_wfd = pipe1[1];
  con->spawned_rfd = pipe2[0];
  if(ioctl(con->spawned_rfd, FIONBIO, &on) < 0) {
    int old_errno = errno;
    close(con->spawned_wfd);
    close(con->spawned_rfd);
    errno = old_errno;
    return -1;
  }
  return 0;
}

int close_connection(struct connection *con) 
{
  int status;
  pid_t pid;

  if (con->spawned_wfd != -1) {
    printf("Closing spawned_wfd %d.\n", con->spawned_wfd);
    if (close(con->spawned_wfd) < 0) {
      printf("Warning: failed to close spawned_wfd %d\n", con->spawned_wfd);
    }
    con->spawned_wfd = -1;
    return 0;
  }

  printf("Closing tcp %d and rfd %d.\n", con->tcp_fd, con->spawned_rfd);
  if(close(con->tcp_fd) < 0) {
    printf("Warning: Failed to close TCP descriptor %d\n", con->tcp_fd);
  }
  if(close(con->spawned_rfd) < 0) {
    printf("Warning: Failed to close spawned_rfd %d\n", con->spawned_rfd);
  }
  

  printf("Waiting on pid %d quitting.\n", con->spawned_pid);
  if ((pid = waitpid(con->spawned_pid, &status, 0)) < 0) {
    return -1;
  } else if (pid == 0) {
    printf("Bugger.  Nothing wanted to report its status.\n");
  } else if (WIFEXITED(status)) {
    printf("Child %d exited with status code %d.\n", pid, WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    printf("Child %d was killed by signal %d%s.\n", pid, WTERMSIG(status),
           WCOREDUMP(status) ? " (core dumped)" : "");
  } else if (WIFSTOPPED(status)) {
    printf("Child %d is stopped by signal %d.\n", pid, WSTOPSIG(status));
  } else {
    printf("Child %d exited through some bizarre incantation %d.\n",
           pid, status);
  }
  printf("Disconnection from %s.\n", inet_ntoa(con->sa.sin_addr));

  del_from_connection_list(&conn_list, con);
  free(con);
  return 0;
}

int read_from_tcp(struct connection *con)
{
  int ret = fdcpy(con->spawned_wfd, con->tcp_fd);
  if (ret == 1) {
    return close_connection(con);
  }
  return ret;
}

int read_from_child(struct connection *con)
{
  int ret = fdcpy(con->tcp_fd, con->spawned_rfd);
  if (ret == 1) {
    return close_connection(con);
  }
  return ret;
}

int fdcpy(int dst, int src) 
{
  while (1) {
    unsigned char buf[70];
    int n_bytes;

    n_bytes = read(src, buf, 70);
    if (n_bytes < 0) {
      if((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
        /* No data left to read. */
        return 0;
      } else {
        return -1;
      }
    } else if (n_bytes == 0) { /* EOF */
      return 1; /* FIXME: Ewww, magic numbers. */
    }

    if(write(dst, buf, n_bytes) < 0) {
      return -1;
    }
  }
}

void add_to_connection_list(connection_list *list, struct connection *item) 
{
  item->next = *list;
  (*list) = item;
}

void del_from_connection_list(connection_list *list, struct connection *item)
{
  struct connection *cur;
  
  if(*list == item) {
    *list = item->next;
    return;
  }
  for(cur = *list; cur; cur = cur->next) {
    if(cur->next == item) {
      cur->next = item->next;
      return;
    }
  }
  assert(0); /* We don't deal with items not being on lists very well,
                do we? */
}

void dump_connection_list() 
{
  struct connection *cur;
  for(cur = conn_list; cur; cur = cur->next) {
    printf("Connection pid = %d, tcp_fd = %d, rfd = %d, wfd = %d, s_port = %d.\n",
           cur->spawned_pid, cur->tcp_fd, cur->spawned_rfd, cur->spawned_wfd,
           ntohs(cur->sa.sin_port));
  }
}
