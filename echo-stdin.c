/* $Id: echo-stdin.c,v 1.2 2003/08/13 11:37:30 mathie Exp $
 *
 * $Log: echo-stdin.c,v $
 * Revision 1.2  2003/08/13 11:37:30  mathie
 * * Noddy app to echo stdin to stdout.
 *
 * Revision 1.1  2003/08/13 11:28:09  mathie
 * Initial revision
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main (int argc, char* argv[]) 
{
  int stdin_fd, stdout_fd;

  stdin_fd = fcntl(STDIN_FILENO, F_DUPFD, 0);
  stdout_fd = fcntl(STDOUT_FILENO, F_DUPFD, 0);
  while (1) {
    char buf[1024];
    int n_bytes;
    
    n_bytes = read(stdin_fd, buf, 1024);
    if (n_bytes < 0) {
      perror("read()");
      exit(EXIT_FAILURE);
    } else if (n_bytes == 0) {
      return 0;
    }
    if(write(stdout_fd, buf, n_bytes) < 0) {
      perror("write()");
      exit(EXIT_FAILURE);
    }
  }
  return 0;
}

    
