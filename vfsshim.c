#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <string.h>
#include <stdlib.h>
#include <assert.h>

/*
** Forward declaration of objects used by this utility
*/
typedef struct ShimVfs ShimVfs;
typedef struct ShimFile ShimFile;

/* An instance of the VFS */
struct ShimVfs {
  sqlite3_vfs base;               /* VFS methods */
  sqlite3_vfs *pVfs;              /* Parent VFS */
};

/* An open file */
struct ShimFile {
  sqlite3_file base;              /* IO methods */
  sqlite3_file *pReal;            /* Underlying file handle */
  int openFlags;
};

#define REALVFS(p) (((ShimVfs*)(p))->pVfs)

/*
** Methods for ShimFile
*/
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

/*
** Methods for ShimVfs
*/
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

static ShimVfs shim_vfs = {
  {
    2,                            /* iVersion */
    0,                            /* szOsFile (set by register_shim()) */
    1024,                         /* mxPathname */
    0,                            /* pNext */
    "vfsshim",                    /* zName */
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



/*
** Close an shim-file.
*/
static int shimClose(sqlite3_file *pFile){
  ShimFile *p = (ShimFile *)pFile;
  int rc = SQLITE_OK;

  if( p->pReal->pMethods ){
    rc = p->pReal->pMethods->xClose(p->pReal);
  }
  return rc;
}


/*
** Read data from an shim-file.
*/
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

/*
** Write data to an shim-file.
*/
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

/*
** Truncate an shim-file.
*/
static int shimTruncate(sqlite3_file *pFile, sqlite_int64 size){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xTruncate(p->pReal, size);
  return rc;
}

/*
** Sync an shim-file.
*/
static int shimSync(sqlite3_file *pFile, int flags){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xSync(p->pReal, flags);
  return rc;
}

/*
** Return the current file-size of an shim-file.
*/
static int shimFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xFileSize(p->pReal, pSize);
  return rc;
}

/*
** Lock an shim-file.
*/
static int shimLock(sqlite3_file *pFile, int eLock){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xLock(p->pReal, eLock);
  return rc;
}

/*
** Unlock an shim-file.
*/
static int shimUnlock(sqlite3_file *pFile, int eLock){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xUnlock(p->pReal, eLock);
  return rc;
}

/*
** Check if another file-handle holds a RESERVED lock on an shim-file.
*/
static int shimCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
  return rc;
}

/*
** File control method. For custom operations on an shim-file.
*/
static int shimFileControl(sqlite3_file *pFile, int op, void *pArg){
  ShimFile *p = (ShimFile *)pFile;
  int rc;
  rc = p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
  return rc;
}

/*
** Return the sector-size in bytes for an shim-file.
*/
static int shimSectorSize(sqlite3_file *pFile){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xSectorSize(p->pReal);
  return rc;
}

/*
** Return the device characteristic flags supported by an shim-file.
*/
static int shimDeviceCharacteristics(sqlite3_file *pFile){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xDeviceCharacteristics(p->pReal);
  return rc;
}

/* Create a shared memory file mapping */
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

/* Perform locking on a shared-memory segment */
static int shimShmLock(sqlite3_file *pFile, int offset, int n, int flags){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xShmLock(p->pReal, offset, n, flags);
}

/* Memory barrier operation on shared memory */
static void shimShmBarrier(sqlite3_file *pFile){
  ShimFile *p = (ShimFile *)pFile;
  p->pReal->pMethods->xShmBarrier(p->pReal);
}

/* Unmap a shared memory segment */
static int shimShmUnmap(sqlite3_file *pFile, int deleteFlag){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xShmUnmap(p->pReal, deleteFlag);
}

/* Fetch a page of a memory-mapped file */
static int shimFetch(
  sqlite3_file *pFile,
  sqlite3_int64 iOfst,
  int iAmt,
  void **pp
){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xFetch(p->pReal, iOfst, iAmt, pp);
}

/* Release a memory-mapped page */
static int shimUnfetch(sqlite3_file *pFile, sqlite3_int64 iOfst, void *pPage){
  ShimFile *p = (ShimFile *)pFile;
  return p->pReal->pMethods->xUnfetch(p->pReal, iOfst, pPage);
}

/*
** Open an shim file handle.
*/
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
  rc = REALVFS(pVfs)->xOpen(REALVFS(pVfs), zName, p->pReal, flags, pOutFlags);
  p->openFlags = flags;
  pFile->pMethods = rc ? 0 : &shim_io_methods;
  return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int shimDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  int rc;
  rc = REALVFS(pVfs)->xDelete(REALVFS(pVfs), zPath, dirSync);
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
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

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (INST_MAX_PATHNAME+1) bytes.
*/
static int shimFullPathname(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int nOut, 
  char *zOut
){
  return REALVFS(pVfs)->xFullPathname(REALVFS(pVfs), zPath, nOut, zOut);
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *shimDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  return REALVFS(pVfs)->xDlOpen(REALVFS(pVfs), zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void shimDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  REALVFS(pVfs)->xDlError(REALVFS(pVfs), nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*shimDlSym(sqlite3_vfs *pVfs, void *p, const char *zSym))(void){
  return REALVFS(pVfs)->xDlSym(REALVFS(pVfs), p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void shimDlClose(sqlite3_vfs *pVfs, void *pHandle){
  REALVFS(pVfs)->xDlClose(REALVFS(pVfs), pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of 
** random data.
*/
static int shimRandomness(sqlite3_vfs *pVfs, int nByte, char *zBufOut){
  return REALVFS(pVfs)->xRandomness(REALVFS(pVfs), nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds 
** actually slept.
*/
static int shimSleep(sqlite3_vfs *pVfs, int nMicro){
  return REALVFS(pVfs)->xSleep(REALVFS(pVfs), nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
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
int sqlite3_vfsshim_init(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
  shim_vfs.pVfs = sqlite3_vfs_find(0);
  if( shim_vfs.pVfs==0 ) return SQLITE_ERROR;
  shim_vfs.base.szOsFile = sizeof(ShimFile) + shim_vfs.pVfs->szOsFile;
  rc = sqlite3_vfs_register(&shim_vfs.base, 1);
  if( rc==SQLITE_OK ) rc = SQLITE_OK_LOAD_PERMANENTLY;
  return rc;
}
