# $Id: Makefile,v 1.1 2003/08/14 07:43:07 mathie Exp $
#
# $Log: Makefile,v $
# Revision 1.1  2003/08/14 07:43:07  mathie
# Initial revision
#

CFLAGS = -Wall -g

default: all

all: cvs-proxy echo-stdin

cvs-proxy: cvs-proxy.o

echo-stdin: echo-stdin.o

clean:
	rm -f cvs-proxy echo-stdin *.o *~