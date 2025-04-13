# POC VFS for SQLite file control write hint
I [proposed](https://sqlite.org/forum/forumpost/d588fbc72d97a7ed) a new
SQLite file control opcode for the library to send to a VFS on
`BEGIN IMMEDIATE` or any single statement write transaction. This would
make it possible to write a VFS that uses this signal to improve
locking efficiency and behavior.

This repo contains a [shim VFS](https://sqlite.org/vfs.html#vfs_shims)
for the "unix" VFS on Linux that takes advantage of a write hint. This
is only a proof of concept implementation, not for production use. It
probably works only on Linux, and it does not support timeouts, threads,
NFS, etc. It should interoperate with the standard unix VFS, i.e. you
should be able to run clients with both shimmed and un-shimmed VFS
concurrently.

The shim replaces xLock(), xUnlock(), and xFileControl() on the original
VFS. To simulate hinting, use `PRAGMA write_hint` before beginning a
write transaction (note that this workaround for a real file control op
won't work on the first transaction after opening the database).

Build a loadable Linux shared object in a shell with `make`. If your
local sqlite3ext.h include file is not in a standard location, then
use `CPATH=<path-to-include-dir> make`:

```
$ CPATH=./sqlite-autoconf-3490100/ make
cc -DSHIM_NAME=vfsshim -O3 -fPIC   -c -o vfsshim.o vfsshim.c
cc -shared -Wl,-soname=vfsshim.so -o vfsshim.so vfsshim.o
$ 
```

Load the shim with:
```
$ sqlite3
SQLite version 3.49.1 2025-02-18 13:38:58
Enter ".help" for usage hints.
Connected to a transient in-memory database.
Use ".open FILENAME" to reopen on a persistent database.
sqlite> .load ./vfsshim
sqlite> .open foo.db
sqlite> CREATE TABLE IF NOT EXISTS foo(x);
shimLock 0xaaaaea37c830 0 -> 1
shimUnlock 0xaaaaea37c830 1 -> 0
shimLock 0xaaaaea37c830 0 -> 1
shimLock 0xaaaaea37c830 1 -> 2
shimLock 0xaaaaea37c830 2 -> 4
shimUnlock 0xaaaaea37c830 4 -> 1
write_hint cleared
shimUnlock 0xaaaaea37c830 1 -> 0
sqlite> PRAGMA write_hint;
write_hint set
sqlite> INSERT INTO foo VALUES ('hello'), ('world');
shimLock 0xaaaaea37c830 0 -> 1
shimLock 0xaaaaea37c830 1 -> 2
shimLock 0xaaaaea37c830 2 -> 4
shimUnlock 0xaaaaea37c830 4 -> 1
write_hint cleared
shimUnlock 0xaaaaea37c830 1 -> 0
sqlite> 
```
