/* $Id: cvs-proxy.cc,v 1.14 2003/08/15 14:43:35 mathie Exp $
 *
 * $Log: cvs-proxy.cc,v $
 * Revision 1.14  2003/08/15 14:43:35  mathie
 * * Do the rcsid thing as suggested by ident(1)
 *
 * Revision 1.13  2003/08/15 14:34:44  mathie
 * * Tidy up fork_child()
 *
 * Revision 1.12  2003/08/15 14:20:39  mathie
 * * Optionally, run as a daemon.
 * * Catch sig(int|term) and cleanup before quitting.
 * * Log to syslog instead of stdout/stderr if running as a daemon.
 * * Capture stderr of spawned program and log it to syslog/stderr.
 *
 * Revision 1.11  2003/08/15 10:46:42  mathie
 * * Cache the 'next' entry when going through the connection lists as the
 *   current entry may be deleted if the connection is closed.
 *
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
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
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
  int spawned_stderr;
  struct sockaddr_in sa;
};
typedef struct connection *connection_list;

connection_list conn_list = NULL;
char *cvs_binary = NULL, *local_cvs_root = NULL, *remote_cvs_host = NULL,
  *remote_cvs_port = NULL, *remote_cvs_path = NULL;
int daemonize = 1, daemonized = 0;
int sockfd = -1;

static char const rcsid[] = "$Id: cvs-proxy.cc,v 1.14 2003/08/15 14:43:35 mathie Exp $";

void sighandler(int sig, siginfo_t *sip, void *scp);
void cleanup(int retcode);
int parse_args(int argc, char *argv[]);
void usage(void);
int init_socket(void);
struct connection *accept_connection(int sockfd);
int close_connection(struct connection *con);
int fork_child(struct connection *con);
int read_from_tcp(struct connection *con);
int read_from_child(struct connection *con);
int read_from_child_stderr(struct connection *con);
int fdcpy(int dst, int src);
void add_to_connection_list(connection_list *list, struct connection *item);
void del_from_connection_list(connection_list *list, struct connection *item);
void dump_connection_list(void);
void log(int priority, const char*message, ...);

int main (int argc, char *argv[]) 
{
  struct sigaction act;
  
  if (parse_args(argc, argv) < 0) {
    usage();
    cleanup(EXIT_FAILURE);
  }

  if(daemonize) {
    if (daemon(0, 0) < 0) {
      log(LOG_ERR, "Failed to fork as a daemon: %s.\n", strerror(errno));
      cleanup(EXIT_FAILURE);
    }
    openlog(argv[0], LOG_PID, LOG_DAEMON);
    daemonized = 1;
  }

  /* Handle sigint to cleanly kill the process */
  act.sa_handler = (void *)sighandler;
  act.sa_flags = SA_SIGINFO;
  act.sa_mask = 0; /* Set it to 0 since Apple didn't bother to document
                      its function at all! */
  if (sigaction(SIGINT, &act, NULL) < 0) {
    log(LOG_ERR, "Failed to register SIGINT signal handler: %s.\n",
        strerror(errno));
    cleanup(EXIT_FAILURE);
  }
  if (sigaction(SIGTERM, &act, NULL) < 0) {
    log(LOG_ERR, "Failed to register SIGTERM signal handler: %s.\n",
        strerror(errno));
    cleanup(EXIT_FAILURE);
  }
  
  if ((sockfd = init_socket()) < 0) {
    log(LOG_ERR, "Failed to created TCP listening socket: %s.\n",
        strerror(errno));
    cleanup(EXIT_FAILURE);
  }

  log(LOG_INFO, "Daemon started.\n");
  log(LOG_INFO, "%s\n", rcsid);
  
  while(1) {
    struct timeval timeout;
    struct fd_set rfds;
    int n_active_fds;
    struct connection *cur, *next;

    FD_ZERO(&rfds);
    FD_SET(sockfd, &rfds);
    for(cur = conn_list; cur; cur = cur->next) {
      FD_SET(cur->tcp_fd, &rfds);
      FD_SET(cur->spawned_rfd, &rfds);
      FD_SET(cur->spawned_stderr, &rfds);
    }
    
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    n_active_fds = select(FD_SETSIZE, &rfds, NULL, NULL, &timeout);
    if (n_active_fds < 0) {
      log(LOG_ERR, "Warning, select() failed: %s.\n", strerror(errno));
      continue;
    } else if (n_active_fds == 0) {
      log(LOG_DEBUG, "select() timed out.\n");
      continue;
    }

    /* See if it's action on the TCP listening socket which woke us up */
    if(FD_ISSET(sockfd, &rfds)) {
      struct connection *newcon = accept_connection(sockfd);
      if (newcon == NULL) {
        log(LOG_ERR, "Failed to accept new incoming connection: %s\n",
            strerror(errno));
      } else {
        add_to_connection_list(&conn_list, newcon);
      }
      n_active_fds--;
    }
    for(cur = conn_list; cur && n_active_fds; cur = next) {
      /* cur may be deleted somewhere in here, so we cache next right at
         the start. */
      next = cur->next;
      
      if(FD_ISSET(cur->tcp_fd, &rfds)) {
        read_from_tcp(cur);
        n_active_fds--;
      }
      if(n_active_fds && FD_ISSET(cur->spawned_rfd, &rfds)) { 
        read_from_child(cur);
        n_active_fds--;
      }
      if(n_active_fds && FD_ISSET(cur->spawned_stderr, &rfds)) {
        read_from_child_stderr(cur);
        n_active_fds--;
      }
    }
  }
  return 0;
}

void sighandler(int sig, siginfo_t *sip, void *scp) 
{
  switch(sig) {
  case SIGINT:
  case SIGTERM:
    cleanup(EXIT_SUCCESS);
    break;
  default:
    log(LOG_ERR, "Unexpected signal %d\n", sig);
  }
}

void cleanup(int retcode)
{
  struct connection *cur, *next;
  
  if(sockfd != -1) {
    if(close(sockfd) < 0) {
      log(LOG_WARNING, "Failed to close listening TCP socket.\n");
    }
  }
  next = conn_list;
  while((cur = next) != NULL) {
    next = cur->next;
    close_connection(cur);
    close_connection(cur);
  }

  log(LOG_INFO, "Daemon exiting, status %d.\n", retcode);
  exit(retcode);
}

int parse_args(int argc, char *argv[]) 
{
  struct stat sb;
  int ch;
  while((ch = getopt(argc, argv, "fb:l:h:p:d:")) != -1) {
    switch(ch) {
    case 'f':
      daemonize = 0;
      break;
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
    log(LOG_ERR, "Cannot access CVS binary: %s.\n", strerror(errno));
    return -1;
  }
  
  if(local_cvs_root == NULL) {
    log(LOG_ERR, "%s: Local CVS root directory required.\n", argv[0]);
    return -1;
  }
  if(stat(local_cvs_root, &sb) < 0) {
    log(LOG_ERR, "Cannot stat local CVS root: %s.\n", strerror(errno));
    return -1;
  }
  if(!(sb.st_mode & S_IFDIR)) {
    log(LOG_ERR, "CVS root %s is not a directory.\n", local_cvs_root);
    return -1;
  }
  
  if(remote_cvs_host == NULL) {
    log(LOG_ERR, "%s: Remote CVS host required.\n", argv[0]);
    return -1;
  }
  if(remote_cvs_port == NULL) {
    remote_cvs_port = strdup("2401");
  }
  if(remote_cvs_path == NULL) {
    log(LOG_ERR, "%s: Remote CVS path required.\n", argv[0]);
    return -1;
  }
  
  return 0;
}

void usage(void)
{
  log(LOG_INFO, "Usage:\n");
  log(LOG_INFO,
      "  -f             Do not fork\n"
      "  -b <filename>  Path to CVS binary\n"
      "  -l <path>      Local CVS root\n"
      "  -h <hostname>  Remote CVS host\n"
      "  -p <port>      Remote CVS port\n"
      "  -d <path>      Remote CVS root\n");
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
  int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2], on = 1;
  if(pipe(stdin_pipe) < 0) {
    return -1;
  }
  if(pipe(stdout_pipe) < 0) {
    int old_errno = errno;
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    errno = old_errno;
    return -1;
  }
  if(pipe(stderr_pipe) < 0) {
    int old_errno = errno;
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    errno = old_errno;
    return -1;
  }

  if ((con->spawned_pid = fork()) < 0) {
    int old_errno = errno;
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    errno = old_errno;
    return -1;
  } else if(con->spawned_pid == 0) { /* Child process */
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    if(dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
      log(LOG_ERR, "Failed to attach pipe to child stdin: %s.\n",
          strerror(errno));
      exit(EXIT_FAILURE);
    }
    if(dup2(stdout_pipe[1], STDOUT_FILENO) < 0) {
      log(LOG_ERR, "Failed to attach pipe to child stdout: %s.\n",
          strerror(errno));
      exit(EXIT_FAILURE);
    }
    if(dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
      log(LOG_ERR, "Failed to attach pipe to child stderr: %s.\n",
          strerror(errno));
      exit(EXIT_FAILURE);
    }
    /* Close all other open descriptors */
    {
      int i;
      for(i = 3; i < getdtablesize(); i++) close(i); 
    }
    
    if (execl(cvs_binary, cvs_binary, "--allow-root", local_cvs_root,
              "pserver", NULL) < 0) {
      log(LOG_ERR, "Failed to exec() CVS server: %s.\n", strerror(errno));
      exit(EXIT_FAILURE);
    }
  }
  
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  con->spawned_wfd = stdin_pipe[1];
  con->spawned_rfd = stdout_pipe[0];
  con->spawned_stderr = stderr_pipe[0];
  if(ioctl(con->spawned_rfd, FIONBIO, &on) < 0) {
    int old_errno = errno;
    close(con->spawned_wfd);
    close(con->spawned_rfd);
    close(con->spawned_stderr);
    errno = old_errno;
    return -1;
  }
  if(ioctl(con->spawned_stderr, FIONBIO, &on) < 0) {
    int old_errno = errno;
    close(con->spawned_wfd);
    close(con->spawned_rfd);
    close(con->spawned_stderr);
    errno = old_errno;
    return -1;
  }
  log(LOG_INFO, "Connection accepted from %s:%d.\n",
      inet_ntoa(con->sa.sin_addr), ntohs(con->sa.sin_port));
  return 0;
}

int close_connection(struct connection *con) 
{
  int status;
  pid_t pid;

  if (con->spawned_wfd != -1) {
    log(LOG_DEBUG, "Closing spawned_wfd %d.\n", con->spawned_wfd);
    if (close(con->spawned_wfd) < 0) {
      log(LOG_DEBUG, "Warning: failed to close spawned_wfd %d\n",
          con->spawned_wfd);
    }
    con->spawned_wfd = -1;
    return 0;
  }

  log(LOG_DEBUG, "Closing tcp %d, rfd %d and sterr %d.\n", con->tcp_fd,
      con->spawned_rfd, con->spawned_stderr);
  if(close(con->tcp_fd) < 0) {
    log(LOG_DEBUG, "Warning: Failed to close TCP descriptor %d: %s.\n",
        con->tcp_fd, strerror(errno));
  }
  if(close(con->spawned_rfd) < 0) {
    log(LOG_DEBUG, "Warning: Failed to close spawned_rfd %d: %s.\n",
        con->spawned_rfd, strerror(errno));
  }
  if(close(con->spawned_stderr) < 0) {
    log(LOG_DEBUG, "Warning: Failed to close spawned_stderr %d: %s.\n",
        con->spawned_rfd, strerror(errno));
  }
  

  log(LOG_DEBUG, "Waiting on pid %d quitting.\n", con->spawned_pid);
  if ((pid = waitpid(con->spawned_pid, &status, 0)) < 0) {
    return -1;
  } else if (pid == 0) {
    log(LOG_DEBUG, "Bugger.  Nothing wanted to report its status.\n");
  } else if (WIFEXITED(status)) {
    log(LOG_DEBUG, "Child %d exited with status code %d.\n", pid,
        WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    log(LOG_DEBUG, "Child %d was killed by signal %d%s.\n", pid,
        WTERMSIG(status), WCOREDUMP(status) ? " (core dumped)" : "");
  } else if (WIFSTOPPED(status)) {
    log(LOG_DEBUG, "Child %d is stopped by signal %d.\n", pid,
        WSTOPSIG(status));
  } else {
    log(LOG_DEBUG, "Child %d exited through some bizarre incantation %d.\n",
        pid, status);
  }
  log(LOG_INFO, "Connection from %s:%d terminated.\n",
      inet_ntoa(con->sa.sin_addr), ntohs(con->sa.sin_port));

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

int read_from_child_stderr(struct connection *con)
{
  while (1) {
    unsigned char buf[1024];
    int n_bytes;

    n_bytes = read(con->spawned_stderr, buf, 1023);
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
    buf[n_bytes] = '\0';
    log(LOG_WARNING, "%s", buf);
  }
}

int fdcpy(int dst, int src) 
{
  while (1) {
    unsigned char buf[1024];
    int n_bytes;

    n_bytes = read(src, buf, 1024);
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
    log(LOG_DEBUG, "Connection pid = %d, tcp_fd = %d, rfd = %d, wfd = %d, s_port = %d.\n",
           cur->spawned_pid, cur->tcp_fd, cur->spawned_rfd, cur->spawned_wfd,
           ntohs(cur->sa.sin_port));
  }
}

void log(int priority, const char *message, ...) 
{
  va_list args;
  va_start(args, message);
  if(daemonized) {
    vsyslog(priority, message, args);
  } else {
    vfprintf(stderr, message, args);
  }
  va_end(args);
}
