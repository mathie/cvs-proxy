# CVS Proxy

I discovered an old Subversion repository containing code that I wrote many
years ago. This was one of the more complete programs sitting in there. For the
life of me, I can't recall why I wanted a CVS proxy, or what it actually does.

The Python version (which is just boilerplate, but actually has some
documentation!) suggests the goal was to improve performance interacting with a
remote CVS server. It would serve reads (looking at `CVS log`, presumably),
locally, while proxying writes back up to the remote. That doesn't sound like
the sort of thing which could ever go wrong, eh?

## License

Since some of the code in here already has the GPL boilerplate in the header,
let's go with the entire project having been released under the GPL v2.
