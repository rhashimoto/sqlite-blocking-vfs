/* Originally based on ext/misc/vfsstat.c */
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

/* An instance of the VFS */
struct ShimVfs {
  sqlite3_vfs base;               /* VFS methods */
  sqlite3_vfs *pVfs;              /* Parent VFS */
};
typedef struct ShimVfs ShimVfs;

/* An open file */
struct ShimFile {
  sqlite3_file base;              /* IO methods */
  sqlite3_file *pReal;            /* Underlying file handle */
  int openFlags;
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

static int shimLock(sqlite3_file *pFile, int eLock){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xLock(p->pReal, eLock);
  return rc;
}

static int shimUnlock(sqlite3_file *pFile, int eLock){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xUnlock(p->pReal, eLock);
  return rc;
}

static int shimCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  int rc;
  ShimFile *p = (ShimFile *)pFile;
  rc = p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
  return rc;
}

static int shimFileControl(sqlite3_file *pFile, int op, void *pArg){
  ShimFile *p = (ShimFile *)pFile;
  int rc;
  rc = p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
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
  rc = REALVFS(pVfs)->xOpen(REALVFS(pVfs), zName, p->pReal, flags, pOutFlags);
  p->openFlags = flags;
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
  shim_vfs.base.szOsFile = sizeof(ShimFile) + shim_vfs.pVfs->szOsFile;
  rc = sqlite3_vfs_register(&shim_vfs.base, 1);
  if( rc==SQLITE_OK ) rc = SQLITE_OK_LOAD_PERMANENTLY;
  return rc;
}
