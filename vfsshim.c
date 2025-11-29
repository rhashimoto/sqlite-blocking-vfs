/* Originally based on ext/misc/vfsstat.c */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

/* Copied from os.h */
#define NO_LOCK         0
#define SHARED_LOCK     1
#define RESERVED_LOCK   2
#define PENDING_LOCK    3
#define EXCLUSIVE_LOCK  4

#define PENDING_BYTE      (0x40000000)
#define RESERVED_BYTE     (PENDING_BYTE+1)
#define SHARED_FIRST      (PENDING_BYTE+2)
#define SHARED_SIZE       510

// This additional lock will be exclusively acquired when the
// next SHARED lock is expected to eventually be upgraded to
// RESERVED/EXCLUSIVE.
#define HINT_BYTE         (SHARED_FIRST+SHARED_SIZE)

/* Copied from os_unix.c */
typedef struct unixFile unixFile;
struct unixFile {
  sqlite3_io_methods const *pMethod;  /* Always the first entry */
  sqlite3_vfs *pVfs;                  /* The VFS that created this unixFile */
  void *pInode;                       /* Info about locks on this inode */
  int h;                              /* The file descriptor */
  unsigned char eFileLock;            /* The type of lock held on this fd */

  /* additional member declarations removed */
};

/* An instance of the VFS */
struct ShimVfs {
  sqlite3_vfs base;               /* VFS methods */
  sqlite3_vfs *pVfs;              /* Parent VFS */
};
typedef struct ShimVfs ShimVfs;

/* An open file */
struct ShimFile {
  sqlite3_file base;              /* IO methods */
  sqlite3_file *pReal;            /* Underlying file handle (unixFile) */
  unsigned char writeHint;
};
typedef struct ShimFile ShimFile;

/* Get pointer to underlying VFS */
#define REALVFS(p) (((ShimVfs*)(p))->pVfs)

/* Methods for ShimFile */
static int shimClose(sqlite3_file*);
static int shimRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int shimWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64 iOfst);
static int shimTruncate(sqlite3_file*, sqlite3_int64 size);
static int shimSync(sqlite3_file*, int flags);
static int shimFileSize(sqlite3_file*, sqlite3_int64 *pSize);
static int shimLock(sqlite3_file*, int);
static int shimUnlock(sqlite3_file*, int);
static int shimCheckReservedLock(sqlite3_file*, int *pResOut);
static int shimFileControl(sqlite3_file*, int op, void *pArg);
static int shimSectorSize(sqlite3_file*);
static int shimDeviceCharacteristics(sqlite3_file*);
static int shimShmMap(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
static int shimShmLock(sqlite3_file*, int offset, int n, int flags);
static void shimShmBarrier(sqlite3_file*);
static int shimShmUnmap(sqlite3_file*, int deleteFlag);
static int shimFetch(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
static int shimUnfetch(sqlite3_file*, sqlite3_int64 iOfst, void *p);

/* Methods for ShimVfs */
static int shimOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);
static int shimDelete(sqlite3_vfs*, const char *zName, int syncDir);
static int shimAccess(sqlite3_vfs*, const char *zName, int flags, int *);
static int shimFullPathname(sqlite3_vfs*, const char *zName, int, char *zOut);
static void *shimDlOpen(sqlite3_vfs*, const char *zFilename);
static void shimDlError(sqlite3_vfs*, int nByte, char *zErrMsg);
static void (*shimDlSym(sqlite3_vfs *pVfs, void *p, const char*zSym))(void);
static void shimDlClose(sqlite3_vfs*, void*);
static int shimRandomness(sqlite3_vfs*, int nByte, char *zOut);
static int shimSleep(sqlite3_vfs*, int microseconds);
static int shimCurrentTime(sqlite3_vfs*, double*);
static int shimGetLastError(sqlite3_vfs*, int, char *);
static int shimCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);

#define QUOTE(x) #x
static ShimVfs shim_vfs = {
  {
    2,                            /* iVersion */
    0,                            /* szOsFile (set by register_shim()) */
    1024,                         /* mxPathname */
    0,                            /* pNext */
    QUOTE(SHIM_NAME),             /* zName */
    0,                            /* pAppData */
    shimOpen,                     /* xOpen */
    shimDelete,                   /* xDelete */
    shimAccess,                   /* xAccess */
    shimFullPathname,             /* xFullPathname */
    shimDlOpen,                   /* xDlOpen */
    shimDlError,                  /* xDlError */
    shimDlSym,                    /* xDlSym */
    shimDlClose,                  /* xDlClose */
    shimRandomness,               /* xRandomness */
    shimSleep,                    /* xSleep */
    shimCurrentTime,              /* xCurrentTime */
    shimGetLastError,             /* xGetLastError */
    shimCurrentTimeInt64          /* xCurrentTimeInt64 */
  },
  0
};

static const sqlite3_io_methods shim_io_methods = {
  3,                              /* iVersion */
  shimClose,                      /* xClose */
  shimRead,                       /* xRead */
  shimWrite,                      /* xWrite */
  shimTruncate,                   /* xTruncate */
  shimSync,                       /* xSync */
  shimFileSize,                   /* xFileSize */
  shimLock,                       /* xLock */
  shimUnlock,                     /* xUnlock */
  shimCheckReservedLock,          /* xCheckReservedLock */
  shimFileControl,                /* xFileControl */
  shimSectorSize,                 /* xSectorSize */
  shimDeviceCharacteristics,      /* xDeviceCharacteristics */
  shimShmMap,                     /* xShmMap */
  shimShmLock,                    /* xShmLock */
  shimShmBarrier,                 /* xShmBarrier */
  shimShmUnmap,                   /* xShmUnmap */
  shimFetch,                      /* xFetch */
  shimUnfetch                     /* xUnfetch */
};



static int shimClose(sqlite3_file *pFile){
  ShimFile *p = (ShimFile *)pFile;
  int rc = SQLITE_OK;

  if( p->pReal->pMethods ){
    rc = p->pReal->pMethods->xClose(p->pReal);
  }
  return rc;
}


static int shimRead(
  sqlite3_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  int rc;
  ShimFile *p = (ShimFile *)pFile;

  rc = p->pReal->pMethods->xRead(p->pReal, zBuf, iAmt, iOfst);
  return rc;
}

static int shimWrite(
  sqlite3_file *pFile,
  const void *z,
  int iAmt,
  sqlite_int64 iOfst
){
  int rc;
  ShimFile *p = (ShimFile *)pFile;

  rc = p->pReal->pMethods->xWrite(p->pReal, z, iAmt, iOfst);
  return rc;
}

static int shimTruncate(sqlite3_file *pFile, sqlite_int64 size){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xTruncate(p->pReal, size);
  return rc;
}

static int shimSync(sqlite3_file *pFile, int flags){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xSync(p->pReal, flags);
  return rc;
}

static int shimFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xFileSize(p->pReal, pSize);
  return rc;
}

// Helper function for shimLock/shimUnlock to change the
// state of a POSIX lock.
static int doLock(int fd, int location, int state, int op) {
  assert(location == PENDING_BYTE ||
         location == SHARED_FIRST ||
         location == RESERVED_BYTE ||
         location == HINT_BYTE);
  assert(state == F_RDLCK ||
         state == F_WRLCK ||
         state == F_UNLCK);
  assert(op == F_SETLKW || op == F_SETLK);

  // RESERVED_BYTE and HINT_BYTE are always locked exclusively.
  assert(location != RESERVED_BYTE || state != F_RDLCK);
  assert(location != HINT_BYTE     || state != F_RDLCK);
  
  struct flock lock = {
    state,
    SEEK_SET,
    location,
    location == SHARED_FIRST ? SHARED_SIZE : 1
  };
  int rc = fcntl(fd, op, &lock);
  if (rc) {
    const char *locationName =
      location == PENDING_BYTE  ? "shim PENDING"  :
      location == SHARED_FIRST  ? "shim SHARED"   :
      location == RESERVED_BYTE ? "shim RESERVED" :
      location == HINT_BYTE     ? "shim HINT"     :
      "unreachable";
    
    if (state == F_UNLCK) {
      perror(locationName);
    } else {
      switch (errno) {
      case EACCES:  // normal polling failure
      case EAGAIN:  // normal polling failure
      case EINTR:   // blocking lock interrupted by signal
        break;
      default:
        perror(locationName);
        break;
      }
    }
  }
  return rc;
}

static int shimLock(sqlite3_file *pFile, int eLock){
  int rc = SQLITE_OK;
  ShimFile *p = (ShimFile *)pFile;
  unixFile *up = (unixFile *)p->pReal;
  int fd = up->h;
#if SHIM_CHATTY  
  fprintf(stderr, "shimLock %p %d -> %d\n", pFile, up->eFileLock, eLock);
#endif
  switch (eLock) {
  case SHARED_LOCK:
    if (up->eFileLock == NO_LOCK) {
      if (p->writeHint) {
        // A file control op has indicated that this SHARED lock is
        // expected to be upgraded to RESERVED and EXCLUSIVE.
        // Acquire the HINT byte.
        if (doLock(fd, HINT_BYTE, F_WRLCK, F_SETLKW)) {
          p->writeHint = 0;
#if SHIM_CHATTY
          fprintf(stderr, "write_hint cleared\n");
#endif
          return SQLITE_BUSY;
        }
      }
      
      // Acquire the PENDING byte.
      if (doLock(fd, PENDING_BYTE, F_RDLCK, F_SETLKW)) {
        // Release the HINT byte on failure.
        if (p->writeHint) {
          doLock(fd, HINT_BYTE, F_UNLCK, F_SETLK);
          p->writeHint = 0;
#if SHIM_CHATTY
          fprintf(stderr, "write_hint cleared\n");
#endif
        }
        return SQLITE_BUSY;
      }

      // Acquire the SHARED range.
      if (doLock(fd, SHARED_FIRST, F_RDLCK, F_SETLKW)) {
        // Release the PENDING and HINT byte on failure.
        doLock(fd, PENDING_BYTE, F_UNLCK, F_SETLK);
        if (p->writeHint) {
          doLock(fd, HINT_BYTE, F_UNLCK, F_SETLK);
          p->writeHint = 0;
#if SHIM_CHATTY
          fprintf(stderr, "write_hint cleared\n");
#endif
        }
        return SQLITE_BUSY;
      }

      // Release the PENDING byte.
      doLock(fd, PENDING_BYTE, F_UNLCK, F_SETLK);
    }
    break;
  case RESERVED_LOCK:
    if (up->eFileLock == SHARED_LOCK) {
      // This is the only place that we poll for locks instead of
      // blocking.
      if (!p->writeHint) {
        // We are upgrading the SHARED lock but we don't already have the
        // HINT byte. This can happen if the write hint is not received
        // prior to a write transaction, e.g. if BEGIN IMMEDIATE is not
        // used. Poll for the HINT byte.
        if (doLock(fd, HINT_BYTE, F_WRLCK, F_SETLK)) {
          return SQLITE_BUSY;
        }
        p->writeHint = 1;
      }
      
      // Poll for the RESERVED byte. This should succeed if all clients
      // require an exclusive lock on the HINT byte.
      if (doLock(fd, RESERVED_BYTE, F_WRLCK, F_SETLK)) {
        // We get here if a legacy client is holding the RESERVED byte
        // without the HINT byte. Release the HINT byte on failure.
        if (p->writeHint) {
          doLock(fd, HINT_BYTE, F_UNLCK, F_SETLK);
          p->writeHint = 0;
#if SHIM_CHATTY
          fprintf(stderr, "write_hint cleared\n");
#endif
        }
        return SQLITE_BUSY;
      }
    }
    break;
  case EXCLUSIVE_LOCK:
    if (up->eFileLock < EXCLUSIVE_LOCK) {
      if (up->eFileLock == RESERVED_LOCK) {
        // Acquire the PENDING byte.
        if (doLock(fd, PENDING_BYTE, F_WRLCK, F_SETLKW)) {
          return SQLITE_BUSY;
        }
      }

      // Acquire the SHARED range.
      if (doLock(fd, SHARED_FIRST, F_WRLCK, F_SETLKW)) {
        // Release the PENDING byte on failure.
        doLock(fd, PENDING_BYTE, F_UNLCK, F_SETLK);
        return SQLITE_BUSY;
      }
    }
    break;
  }
  
  up->eFileLock = eLock;
  return SQLITE_OK;
}

static int shimUnlock(sqlite3_file *pFile, int eLock){
  int rc = SQLITE_OK;
  ShimFile *p = (ShimFile *)pFile;
  unixFile *up = (unixFile *)p->pReal;
  int fd = up->h;
#if SHIM_CHATTY
  fprintf(stderr, "shimUnlock %p %d -> %d\n", pFile, up->eFileLock, eLock);
#endif
  if (eLock == up->eFileLock) return SQLITE_OK;
  switch (eLock) {
  case SHARED_LOCK:
    if (up->eFileLock > eLock) {
      // Downgrade the SHARED range to a read lock.
      doLock(fd, SHARED_FIRST, F_RDLCK, F_SETLK);
      doLock(fd, PENDING_BYTE, F_UNLCK, F_SETLK);
    }
    break;
  case NO_LOCK:
    if (up->eFileLock > eLock) {
      // Release all locks.
      doLock(fd, SHARED_FIRST, F_UNLCK, F_SETLK);
      doLock(fd, RESERVED_BYTE, F_UNLCK, F_SETLK);
      doLock(fd, PENDING_BYTE, F_UNLCK, F_SETLK);
      if (p->writeHint) {
        doLock(fd, HINT_BYTE, F_UNLCK, F_SETLK);
        p->writeHint = 0;
#if SHIM_CHATTY
        fprintf(stderr, "write_hint cleared\n");
#endif
      }
    }
    break;
  }
  
  up->eFileLock = eLock;
  return SQLITE_OK;
}

static int shimCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
  return rc;
}

static int shimFileControl(sqlite3_file *pFile, int op, void *pArg){
  ShimFile *p = (ShimFile *)pFile;
  unixFile *up = (unixFile *)p->pReal;
  
  switch (op) {
  // SQLite sends
  //  PRAGMA experimental_pragma_20251114 = 1|2
  // prior BEGIN IMMEDIATE or a standalone write statement.
  // https://sqlite.org/src/info/e2b3f1a948
  case SQLITE_FCNTL_PRAGMA:
    if (!strcasecmp(((char **)pArg)[1], "experimental_pragma_20251114")) {
#if SHIM_CHATTY
      fprintf(stderr, "write_hint set\n");
#endif
      p->writeHint = 1;
    }
    break;

  // Hopefully SQLite will eventually have a file control opcode.
/*   case SQLITE_FUTURE_WRITE_HINT_OPCODE: // TODO: replace with real opcode */
/*     if (up->eFileLock == NO_LOCK) { */
/* #if SHIM_CHATTY */
/*       fprintf(stderr, "write_hint set\n"); */
/* #endif */
/*       p->writeHint = 1; */
/*     } */
/*     break; */
  }
  
  int rc = p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
  return rc;
}

static int shimSectorSize(sqlite3_file *pFile){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xSectorSize(p->pReal);
  return rc;
}

static int shimDeviceCharacteristics(sqlite3_file *pFile){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xDeviceCharacteristics(p->pReal);
  return rc;
}

static int shimShmMap(
  sqlite3_file *pFile,
  int iPg,
  int pgsz,
  int bExtend,
  void volatile **pp
){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xShmMap(p->pReal, iPg, pgsz, bExtend, pp);
}

static int shimShmLock(sqlite3_file *pFile, int offset, int n, int flags){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xShmLock(p->pReal, offset, n, flags);
}

static void shimShmBarrier(sqlite3_file *pFile){
  ShimFile *p = (ShimFile *)pFile;
  p->pReal->pMethods->xShmBarrier(p->pReal);
}

static int shimShmUnmap(sqlite3_file *pFile, int deleteFlag){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xShmUnmap(p->pReal, deleteFlag);
}

static int shimFetch(
  sqlite3_file *pFile,
  sqlite3_int64 iOfst,
  int iAmt,
  void **pp
){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xFetch(p->pReal, iOfst, iAmt, pp);
}

static int shimUnfetch(sqlite3_file *pFile, sqlite3_int64 iOfst, void *pPage){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xUnfetch(p->pReal, iOfst, pPage);
}

static int shimOpen(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
  int rc;
  ShimFile *p = (ShimFile*)pFile;

  p->pReal = (sqlite3_file*)&p[1];
  p->writeHint = 0;
  rc = REALVFS(pVfs)->xOpen(REALVFS(pVfs), zName, p->pReal, flags, pOutFlags);
  pFile->pMethods = rc ? 0 : &shim_io_methods;
  return rc;
}

static int shimDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  int rc;
  rc = REALVFS(pVfs)->xDelete(REALVFS(pVfs), zPath, dirSync);
  return rc;
}

static int shimAccess(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  int rc;
  rc = REALVFS(pVfs)->xAccess(REALVFS(pVfs), zPath, flags, pResOut);
  return rc;
}

static int shimFullPathname(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int nOut, 
  char *zOut
){
  return REALVFS(pVfs)->xFullPathname(REALVFS(pVfs), zPath, nOut, zOut);
}

static void *shimDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  return REALVFS(pVfs)->xDlOpen(REALVFS(pVfs), zPath);
}

static void shimDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  REALVFS(pVfs)->xDlError(REALVFS(pVfs), nByte, zErrMsg);
}

static void (*shimDlSym(sqlite3_vfs *pVfs, void *p, const char *zSym))(void){
  return REALVFS(pVfs)->xDlSym(REALVFS(pVfs), p, zSym);
}

static void shimDlClose(sqlite3_vfs *pVfs, void *pHandle){
  REALVFS(pVfs)->xDlClose(REALVFS(pVfs), pHandle);
}

static int shimRandomness(sqlite3_vfs *pVfs, int nByte, char *zBufOut){
  return REALVFS(pVfs)->xRandomness(REALVFS(pVfs), nByte, zBufOut);
}

static int shimSleep(sqlite3_vfs *pVfs, int nMicro){
  return REALVFS(pVfs)->xSleep(REALVFS(pVfs), nMicro);
}

static int shimCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut){
  return REALVFS(pVfs)->xCurrentTime(REALVFS(pVfs), pTimeOut);
}

static int shimGetLastError(sqlite3_vfs *pVfs, int a, char *b){
  return REALVFS(pVfs)->xGetLastError(REALVFS(pVfs), a, b);
}
static int shimCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *p){
  return REALVFS(pVfs)->xCurrentTimeInt64(REALVFS(pVfs), p);
}

/* 
** This routine is called when the extension is loaded.
**
** Register the new VFS.  Make arrangement to register the virtual table
** for each new database connection.
*/
#define DECLARE_INNER(x) sqlite3_ ## x ## _init
#define DECLARE(x) DECLARE_INNER(x)
#ifdef _WIN32
__declspec(dllexport)
#endif

int DECLARE(SHIM_NAME)(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
  shim_vfs.pVfs = sqlite3_vfs_find(0);
  if( shim_vfs.pVfs==0 ) return SQLITE_ERROR;
  if (strcmp(shim_vfs.pVfs->zName, "unix")) {
    fprintf(stderr, "%s: only wraps \"unix\"", QUOTE(SHIM_NAME));
    return SQLITE_ERROR;
  }
  shim_vfs.base.szOsFile = sizeof(ShimFile) + shim_vfs.pVfs->szOsFile;
  rc = sqlite3_vfs_register(&shim_vfs.base, 1);
  if( rc==SQLITE_OK ) rc = SQLITE_OK_LOAD_PERMANENTLY;
  return rc;
}
