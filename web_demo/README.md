# Experimental write hint test web app
This web app compares VFS locking implementations for SQLite, including use of the
[experimental write hint](https://sqlite.org/forum/forumpost/c4ca8e7f4a887aa4).
It uses both [WebAssembly JSPI](https://github.com/WebAssembly/js-promise-integration)
and [OPFS readwrite-unsafe mode](https://developer.mozilla.org/en-US/docs/Web/API/FileSystemFileHandle/createSyncAccessHandle#readwrite-unsafe),
which currently are only available in Chromium-based browsers. The web app can be
loaded from [here](https://rhashimoto.github.io/sqlite-blocking-vfs/demo/).

## Notable files
* *demo-worker.js* - runs SQLite code.
* *Lock.js* - convenience abstraction implemented with the Web Locks API.
* *OPFSBaseUnsafeVFS.js* - base class VFS using OPFS. Locking methods are no-ops.
* *OPFSNoWriteHintVFS.js* - VFS implementing the standard locking model.
* *OPFSWriteHintVFS.js* - VFS using the experiment write hint.

## Implementation
The authoritative documentation of the SQLite locking model is [here](https://sqlite.org/lockingv3.html).

In the browser, locking is best implemented with the [Web Locks API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Locks_API).

### OPFSNoWriteHintVFS
The standard model can be implemented with two web locks, an access lock and a reserved lock.
The access lock allows read access when held in shared mode, and write access when held
in exclusive mode. The reserved lock serves two purposes: it protects other connections
from upgrading their access lock while the current connection releases its shared mode
access lock and reacquires it in exclusive mode.

Here are the lock modes for each locking state:
|locking state|access lock|reserved lock|
|-|-|-|
|NONE|||
|SHARED|shared||
|RESERVED|shared (keep previous state)|exclusive*|
|EXCLUSIVE|exclusive|(keep previous state)|

\* The reserved lock must be polled and SQLITE_BUSY returned on failure. All other lock requests may be blocking.

This *is* a blocking VFS - it blocks on the transition to the SHARED and EXCLUSIVE states. However, it cannot block on the transition to the RESERVED state as this would leave the system in deadlock.

### OPFSWriteHintVFS
Using the write hint requires adding another lock, the write hint lock. When the write hint has been received, the lock modes are:
|locking state|write hint lock|access lock|reserved lock|
|-|-|-|-|
|NONE||||
|SHARED|exclusive|shared*||
|RESERVED|exclusive (keep previous state)|shared (keep previous state)|exclusive**|
|EXCLUSIVE|exclusive (keep previous state)|exclusive|(keep previous state)|

\* The shared lock must be requested *after* acquiring the write hint lock.\
\** Unlike the implementation without the write hint, *all* lock requests may be blocking.

When the write hint has not been received, the lock modes are:
|locking state|write hint lock|access lock|reserved lock|
|-|-|-|-|
|NONE||||
|SHARED||shared||
|RESERVED|exclusive*|shared (keep previous state)|exclusive|
|EXCLUSIVE|exclusive (keep previous state)|exclusive|(keep previous state)|

\* The write hint lock must be polled and SQLITE_BUSY returned on failure. All other lock requests may be blocking.

This is a fully blocking VFS, made possible with the write hint. It blocks on the transition to every state and will never return SQLITE_BUSY except on an application-requested timeout.

## Test description
The test creates one or more workers, with each worker executing as many write transactions as it can in a fixed amount of time. Each transaction inserts one row into a table with these columns:

* *worker* - index of the worker that inserted this row
* *ticks* - insertion timestamp in milliseconds
* *wait* - milliseconds from BEGIN IMMEDIATE to first real statement
* *retries* - SQLITE_BUSY count before successful insertion

## Test parameters
* *Number of workers*
* *Locking implementation* - Selects the VFS class to use. Note that selecting "No locking" will corrupt the database if used with more than one worker.
* *Retry delay* - After receiving SQLITE_BUSY, sleep this many milliseconds before retrying.
* *Simulated work* - Sleep this many milliseconds within each transaction.

## Test results
The results are returned in a table with one row per worker with the following columns:

* *worker* - worker index
* *transactions* - Number of transactions (i.e. rows) committed by this worker.
* *avg wait* - Mean time in milliseconds to get access to the database.
* *max wait* - Maximum time in milliseconds to get access to the database.
* *retries* - Total number of retries.
* *min slots* - Minimum transaction count between transactions by this worker. 1 means consecutive transactions.
* *max slots* - Maximum transaction count between transactions by this worker.

## DIY query
After a test finishes, you can use the [Chrome debugger console](https://developer.chrome.com/docs/devtools/console) to submit custom queries to the database. Use the Execution Context Selector to change the console context from "top" to one of the "demo-worker.js" contexts. Then pass a single SQL statement string to the global function `q()`, e.g.:

```javascript
q(`SELECT * FROM test LIMIT 10`);
```

The results will be printed to the console.
