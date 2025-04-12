#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include <string.h>
#include <stdlib.h>
#include <assert.h>

/*
** Forward declaration of objects used by this utility
*/
typedef struct VStatVfs VStatVfs;
typedef struct VStatFile VStatFile;

/* An instance of the VFS */
struct VStatVfs {
  sqlite3_vfs base;               /* VFS methods */
  sqlite3_vfs *pVfs;              /* Parent VFS */
};

/* An open file */
struct VStatFile {
  sqlite3_file base;              /* IO methods */
  sqlite3_file *pReal;            /* Underlying file handle */
  int openFlags;
};

#define REALVFS(p) (((VStatVfs*)(p))->pVfs)

/*
** Methods for VStatFile
*/
static int vstatClose(sqlite3_file*);
static int vstatRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int vstatWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64 iOfst);
static int vstatTruncate(sqlite3_file*, sqlite3_int64 size);
static int vstatSync(sqlite3_file*, int flags);
static int vstatFileSize(sqlite3_file*, sqlite3_int64 *pSize);
static int vstatLock(sqlite3_file*, int);
static int vstatUnlock(sqlite3_file*, int);
static int vstatCheckReservedLock(sqlite3_file*, int *pResOut);
static int vstatFileControl(sqlite3_file*, int op, void *pArg);
static int vstatSectorSize(sqlite3_file*);
static int vstatDeviceCharacteristics(sqlite3_file*);
static int vstatShmMap(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
static int vstatShmLock(sqlite3_file*, int offset, int n, int flags);
static void vstatShmBarrier(sqlite3_file*);
static int vstatShmUnmap(sqlite3_file*, int deleteFlag);
static int vstatFetch(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
static int vstatUnfetch(sqlite3_file*, sqlite3_int64 iOfst, void *p);

/*
** Methods for VStatVfs
*/
static int vstatOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);
static int vstatDelete(sqlite3_vfs*, const char *zName, int syncDir);
static int vstatAccess(sqlite3_vfs*, const char *zName, int flags, int *);
static int vstatFullPathname(sqlite3_vfs*, const char *zName, int, char *zOut);
static void *vstatDlOpen(sqlite3_vfs*, const char *zFilename);
static void vstatDlError(sqlite3_vfs*, int nByte, char *zErrMsg);
static void (*vstatDlSym(sqlite3_vfs *pVfs, void *p, const char*zSym))(void);
static void vstatDlClose(sqlite3_vfs*, void*);
static int vstatRandomness(sqlite3_vfs*, int nByte, char *zOut);
static int vstatSleep(sqlite3_vfs*, int microseconds);
static int vstatCurrentTime(sqlite3_vfs*, double*);
static int vstatGetLastError(sqlite3_vfs*, int, char *);
static int vstatCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);

static VStatVfs vstat_vfs = {
  {
    2,                            /* iVersion */
    0,                            /* szOsFile (set by register_vstat()) */
    1024,                         /* mxPathname */
    0,                            /* pNext */
    "vfslog",                     /* zName */
    0,                            /* pAppData */
    vstatOpen,                     /* xOpen */
    vstatDelete,                   /* xDelete */
    vstatAccess,                   /* xAccess */
    vstatFullPathname,             /* xFullPathname */
    vstatDlOpen,                   /* xDlOpen */
    vstatDlError,                  /* xDlError */
    vstatDlSym,                    /* xDlSym */
    vstatDlClose,                  /* xDlClose */
    vstatRandomness,               /* xRandomness */
    vstatSleep,                    /* xSleep */
    vstatCurrentTime,              /* xCurrentTime */
    vstatGetLastError,             /* xGetLastError */
    vstatCurrentTimeInt64          /* xCurrentTimeInt64 */
  },
  0
};

static const sqlite3_io_methods vstat_io_methods = {
  3,                              /* iVersion */
  vstatClose,                      /* xClose */
  vstatRead,                       /* xRead */
  vstatWrite,                      /* xWrite */
  vstatTruncate,                   /* xTruncate */
  vstatSync,                       /* xSync */
  vstatFileSize,                   /* xFileSize */
  vstatLock,                       /* xLock */
  vstatUnlock,                     /* xUnlock */
  vstatCheckReservedLock,          /* xCheckReservedLock */
  vstatFileControl,                /* xFileControl */
  vstatSectorSize,                 /* xSectorSize */
  vstatDeviceCharacteristics,      /* xDeviceCharacteristics */
  vstatShmMap,                     /* xShmMap */
  vstatShmLock,                    /* xShmLock */
  vstatShmBarrier,                 /* xShmBarrier */
  vstatShmUnmap,                   /* xShmUnmap */
  vstatFetch,                      /* xFetch */
  vstatUnfetch                     /* xUnfetch */
};



/*
** Close an vstat-file.
*/
static int vstatClose(sqlite3_file *pFile){
  VStatFile *p = (VStatFile *)pFile;
  int rc = SQLITE_OK;

  if( p->pReal->pMethods ){
    rc = p->pReal->pMethods->xClose(p->pReal);
  }
  return rc;
}


/*
** Read data from an vstat-file.
*/
static int vstatRead(
  sqlite3_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  int rc;
  VStatFile *p = (VStatFile *)pFile;

  rc = p->pReal->pMethods->xRead(p->pReal, zBuf, iAmt, iOfst);
  return rc;
}

/*
** Write data to an vstat-file.
*/
static int vstatWrite(
  sqlite3_file *pFile,
  const void *z,
  int iAmt,
  sqlite_int64 iOfst
){
  int rc;
  VStatFile *p = (VStatFile *)pFile;

  rc = p->pReal->pMethods->xWrite(p->pReal, z, iAmt, iOfst);
  return rc;
}

/*
** Truncate an vstat-file.
*/
static int vstatTruncate(sqlite3_file *pFile, sqlite_int64 size){
  int rc;
  VStatFile *p = (VStatFile *)pFile;
  rc = p->pReal->pMethods->xTruncate(p->pReal, size);
  return rc;
}

/*
** Sync an vstat-file.
*/
static int vstatSync(sqlite3_file *pFile, int flags){
  int rc;
  VStatFile *p = (VStatFile *)pFile;
  rc = p->pReal->pMethods->xSync(p->pReal, flags);
  return rc;
}

/*
** Return the current file-size of an vstat-file.
*/
static int vstatFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  int rc;
  VStatFile *p = (VStatFile *)pFile;
  rc = p->pReal->pMethods->xFileSize(p->pReal, pSize);
  return rc;
}

/*
** Lock an vstat-file.
*/
static int vstatLock(sqlite3_file *pFile, int eLock){
  int rc;
  VStatFile *p = (VStatFile *)pFile;
  rc = p->pReal->pMethods->xLock(p->pReal, eLock);
  return rc;
}

/*
** Unlock an vstat-file.
*/
static int vstatUnlock(sqlite3_file *pFile, int eLock){
  int rc;
  VStatFile *p = (VStatFile *)pFile;
  rc = p->pReal->pMethods->xUnlock(p->pReal, eLock);
  return rc;
}

/*
** Check if another file-handle holds a RESERVED lock on an vstat-file.
*/
static int vstatCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  int rc;
  VStatFile *p = (VStatFile *)pFile;
  rc = p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
  return rc;
}

/*
** File control method. For custom operations on an vstat-file.
*/
static int vstatFileControl(sqlite3_file *pFile, int op, void *pArg){
  VStatFile *p = (VStatFile *)pFile;
  int rc;
  rc = p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
  return rc;
}

/*
** Return the sector-size in bytes for an vstat-file.
*/
static int vstatSectorSize(sqlite3_file *pFile){
  int rc;
  VStatFile *p = (VStatFile *)pFile;
  rc = p->pReal->pMethods->xSectorSize(p->pReal);
  return rc;
}

/*
** Return the device characteristic flags supported by an vstat-file.
*/
static int vstatDeviceCharacteristics(sqlite3_file *pFile){
  int rc;
  VStatFile *p = (VStatFile *)pFile;
  rc = p->pReal->pMethods->xDeviceCharacteristics(p->pReal);
  return rc;
}

/* Create a shared memory file mapping */
static int vstatShmMap(
  sqlite3_file *pFile,
  int iPg,
  int pgsz,
  int bExtend,
  void volatile **pp
){
  VStatFile *p = (VStatFile *)pFile;
  return p->pReal->pMethods->xShmMap(p->pReal, iPg, pgsz, bExtend, pp);
}

/* Perform locking on a shared-memory segment */
static int vstatShmLock(sqlite3_file *pFile, int offset, int n, int flags){
  VStatFile *p = (VStatFile *)pFile;
  return p->pReal->pMethods->xShmLock(p->pReal, offset, n, flags);
}

/* Memory barrier operation on shared memory */
static void vstatShmBarrier(sqlite3_file *pFile){
  VStatFile *p = (VStatFile *)pFile;
  p->pReal->pMethods->xShmBarrier(p->pReal);
}

/* Unmap a shared memory segment */
static int vstatShmUnmap(sqlite3_file *pFile, int deleteFlag){
  VStatFile *p = (VStatFile *)pFile;
  return p->pReal->pMethods->xShmUnmap(p->pReal, deleteFlag);
}

/* Fetch a page of a memory-mapped file */
static int vstatFetch(
  sqlite3_file *pFile,
  sqlite3_int64 iOfst,
  int iAmt,
  void **pp
){
  VStatFile *p = (VStatFile *)pFile;
  return p->pReal->pMethods->xFetch(p->pReal, iOfst, iAmt, pp);
}

/* Release a memory-mapped page */
static int vstatUnfetch(sqlite3_file *pFile, sqlite3_int64 iOfst, void *pPage){
  VStatFile *p = (VStatFile *)pFile;
  return p->pReal->pMethods->xUnfetch(p->pReal, iOfst, pPage);
}

/*
** Open an vstat file handle.
*/
static int vstatOpen(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
  int rc;
  VStatFile *p = (VStatFile*)pFile;

  p->pReal = (sqlite3_file*)&p[1];
  rc = REALVFS(pVfs)->xOpen(REALVFS(pVfs), zName, p->pReal, flags, pOutFlags);
  p->openFlags = flags;
  pFile->pMethods = rc ? 0 : &vstat_io_methods;
  return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int vstatDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  int rc;
  rc = REALVFS(pVfs)->xDelete(REALVFS(pVfs), zPath, dirSync);
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int vstatAccess(
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
static int vstatFullPathname(
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
static void *vstatDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  return REALVFS(pVfs)->xDlOpen(REALVFS(pVfs), zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated 
** with dynamic libraries.
*/
static void vstatDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  REALVFS(pVfs)->xDlError(REALVFS(pVfs), nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*vstatDlSym(sqlite3_vfs *pVfs, void *p, const char *zSym))(void){
  return REALVFS(pVfs)->xDlSym(REALVFS(pVfs), p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void vstatDlClose(sqlite3_vfs *pVfs, void *pHandle){
  REALVFS(pVfs)->xDlClose(REALVFS(pVfs), pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of 
** random data.
*/
static int vstatRandomness(sqlite3_vfs *pVfs, int nByte, char *zBufOut){
  return REALVFS(pVfs)->xRandomness(REALVFS(pVfs), nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds 
** actually slept.
*/
static int vstatSleep(sqlite3_vfs *pVfs, int nMicro){
  return REALVFS(pVfs)->xSleep(REALVFS(pVfs), nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int vstatCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut){
  return REALVFS(pVfs)->xCurrentTime(REALVFS(pVfs), pTimeOut);
}

static int vstatGetLastError(sqlite3_vfs *pVfs, int a, char *b){
  return REALVFS(pVfs)->xGetLastError(REALVFS(pVfs), a, b);
}
static int vstatCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *p){
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
  vstat_vfs.pVfs = sqlite3_vfs_find(0);
  if( vstat_vfs.pVfs==0 ) return SQLITE_ERROR;
  vstat_vfs.base.szOsFile = sizeof(VStatFile) + vstat_vfs.pVfs->szOsFile;
  rc = sqlite3_vfs_register(&vstat_vfs.base, 1);
  if( rc==SQLITE_OK ) rc = SQLITE_OK_LOAD_PERMANENTLY;
  return rc;
}
