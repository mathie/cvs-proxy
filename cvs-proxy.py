#/usr/bin/python

# $Id: cvs-proxy.py,v 1.1 2003/08/10 08:23:54 mathie Exp $
#
# CVS Proxy server
# Copyright (c) 2003 Graeme Mathieson <mathie@wossname.org.uk>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 1, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
#
# $Log: cvs-proxy.py,v $
# Revision 1.1  2003/08/10 08:23:54  mathie
# Initial revision
#
#

"""cvs-proxy is a CVS proxy server which will, when possible, serve requests
   using a local copy of a repository, only resorting to using a remote master
   repository for writes.  The intention is to improve the performance of CVS
   whilst using a remote system.
   
   Note that keeping the local repository up to date is done out of band
   (perhaps with rsync kicked from a cron job or a post-commit script)."""

# Imports
import socket, SocketServer

class MyHandler(SocketServer.BaseRequestHandler):
  def handle(self):
    while 1:
      dataReceived = self.request.recv(1024)
      if not dataReceived:
	break
      self.request.send(dataReceived)
      print dataReceived
  def setup(self):
      print "Connection from: ", self.client_address
  def finish(self):
      print "Disconnected: ", self.client_address

try:
  myPort = socket.getservbyname('cvspserver', 'tcp')
except:
  myPort = 2401 # Default to port 2401, the well-known-port for CVS

myServer = SocketServer.TCPServer(('', myPort), MyHandler)
try:
  myServer.serve_forever()
except KeyboardInterrupt:
  print "Exiting."
