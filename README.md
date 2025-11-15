# POC VFS for SQLite file control write hint
SQLite has [created](https://sqlite.org/src/info/e2b3f1a9480a9be3)
an experimental mechanism to inform a VFS that a write transaction will
follow. This can be used by a VFS implementation to improve locking
efficiency and behavior.

This feature requires SQLite to be built with the compile-time define
`SQLITE_EXPERIMENTAL_PRAGMA_20251114`, e.g. when building from the
source tree:

```
make CFLAGS=-DSQLITE_EXPERIMENTAL_PRAGMA_20251114
```

This repo contains a [shim VFS](https://sqlite.org/vfs.html#vfs_shims)
for the "unix" VFS on Linux that takes advantage of this write hint
feature. When multiple database connections concurrently attempt to
open a write transaction with the default VFS, all but one of them
will return SQLITE_BUSY (aka "database is locked") and will have to
retry. If instead all connections to a database are using this VFS and
they use `BEGIN IMMEDIATE` or `BEGIN EXCLUSIVE` to open any
multi-statement write transaction, then each transaction will be
served in order and will block until then, which is both more
efficient and more fair.

This is only a proof of concept implementation, not for production
use. It probably works only on Linux, and it does not support
timeouts, threads, NFS, etc. It should interoperate with the standard
unix VFS, i.e. you should be able to run clients with both shimmed and
un-shimmed VFS at the same time, but the improved behavior is only
guaranteed when the all contending connections are using this VFS.

Build the loadable Linux shared object in a shell with `make`. If your
local sqlite3ext.h include file is not in a standard location, then
use `CPATH=<path-to-include-dir> make`:

```
$ CPATH=./sqlite-20251114172720-e2b3f1a948/ make SHIM_CHATTY=1
```

The `SHIM_CHATTY=1` setting is optional to add some logging. This will
build a loadable shared object vfsshim.so.

The shim can be loaded into the SQLite CLI with `.load ./vshshim`,
which replaces the default VFS. Here is a sample session (with
SHIM_CHATTY enabled):

```
$ sqlite3 
SQLite version 3.51.0 2025-11-14 17:27:20
Enter ".help" for usage hints.
Connected to a transient in-memory database.
Use ".open FILENAME" to reopen on a persistent database.
sqlite> .load ./vfsshim
sqlite> .open foo.db
sqlite> CREATE TABLE IF NOT EXISTS foo(x);
shimLock 0xaaaae1bd3370 0 -> 1
shimUnlock 0xaaaae1bd3370 1 -> 0
write_hint set
shimLock 0xaaaae1bd3370 0 -> 1
shimLock 0xaaaae1bd3370 1 -> 2
shimLock 0xaaaae1bd3370 2 -> 4
shimUnlock 0xaaaae1bd3370 4 -> 1
write_hint cleared
shimUnlock 0xaaaae1bd3370 1 -> 0
sqlite> INSERT INTO foo VALUES ('hello'), ('world');
write_hint set
shimLock 0xaaaae1bd3370 0 -> 1
shimLock 0xaaaae1bd3370 1 -> 2
shimLock 0xaaaae1bd3370 2 -> 4
shimUnlock 0xaaaae1bd3370 4 -> 1
write_hint cleared
shimUnlock 0xaaaae1bd3370 1 -> 0
sqlite> 
```
