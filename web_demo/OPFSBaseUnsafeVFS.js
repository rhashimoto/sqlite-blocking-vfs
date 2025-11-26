import { FacadeVFS } from "./wa-sqlite/src/FacadeVFS.js";
import * as VFS from './wa-sqlite/src/VFS.js';

/**
 * @typedef FileEntry
 * @property {FileSystemSyncAccessHandle} accessHandle
 * @property {() => Promise<void>?} onClose
 * @property {any} [extra]
 */

/**
 * Cache the OPFS root directory handle.
 * @type {FileSystemDirectoryHandle}
 */
let dirHandle = null;

/**
 * This is a minimal OPFS VFS implementation that uses unsafe access
 * handles for concurrent read-write access (currently supported only
 * in Chromium browsers). It does not implement any locking, so it is
 * not safe to use with multiple database connections unless the
 * application implements appropriate locking at a higher level.
 */
export class OPFSBaseUnsafeVFS extends FacadeVFS  {
  lastError = null;
  // log = console.log;
  
  /** @type {Map<number, FileEntry>} */ mapFileIdToEntry = new Map();

  constructor(name, module) {
    super(name, module);
  }

 /**
   * @param {string?} filename 
   * @param {number} fileId 
   * @param {number} flags 
   * @param {DataView} pOutFlags 
   * @returns {Promise<number>}
   */
  async jOpen(filename, fileId, flags, pOutFlags) {
    try {
      // For simplicity, everything goes into the OPFS root directory.
      dirHandle = dirHandle ?? await navigator.storage.getDirectory();
      const fileHandle = await dirHandle.getFileHandle(
        filename,
        { create: (flags & VFS.SQLITE_OPEN_CREATE) === VFS.SQLITE_OPEN_CREATE });

      // Open a synchronous access handle with concurrent access.
      // @ts-ignore
      const accessHandle = await fileHandle.createSyncAccessHandle({
        mode: 'readwrite-unsafe'
      });
      this.mapFileIdToEntry.set(fileId, {
        accessHandle,
        onClose: (flags & VFS.SQLITE_OPEN_DELETEONCLOSE) ?
          () => dirHandle.removeEntry(filename, { recursive: false }) :
          null,
      });
      pOutFlags.setInt32(0, flags, true);
      return VFS.SQLITE_OK;
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_CANTOPEN;
    }
  }

  /**
   * @param {string} zName 
   * @param {number} syncDir 
   * @returns {Promise<number>}
   */
  async jDelete(zName, syncDir) {
    try {
      dirHandle = dirHandle ?? await navigator.storage.getDirectory();
      await dirHandle.removeEntry(zName, { recursive: false });
      return VFS.SQLITE_OK;
    } catch (e) {
      return VFS.SQLITE_IOERR_DELETE;
    }
  }

  /**
   * @param {string} zName 
   * @param {number} flags 
   * @param {DataView} pResOut 
   * @returns {Promise<number>}
   */
  async jAccess(zName, flags, pResOut) {
    try {
      dirHandle = dirHandle ?? await navigator.storage.getDirectory();
      const fileHandle = await dirHandle.getFileHandle(zName, { create: false });
      pResOut.setInt32(0, 1, true);
      return VFS.SQLITE_OK;
    } catch (e) {
      if (e.name === 'NotFoundError') {
        pResOut.setInt32(0, 0, true);
        return VFS.SQLITE_OK;
      }
      this.lastError = e;
      return VFS.SQLITE_IOERR_ACCESS;
    }
  }

  /**
   * @param {number} fileId 
   * @returns {Promise<number>}
   */
  async jClose(fileId) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);
      this.mapFileIdToEntry.delete(fileId);
      file?.accessHandle.close();
      file?.onClose?.();
      return VFS.SQLITE_OK;
    } catch (e) {
      return VFS.SQLITE_IOERR_CLOSE;
    }
  }

  /**
   * @param {number} fileId 
   * @param {Uint8Array} pData 
   * @param {number} iOffset
   * @returns {number}
   */
  jRead(fileId, pData, iOffset) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);

      // On Chrome (at least), passing pData to accessHandle.read() is
      // an error because pData is a Proxy of a Uint8Array. Calling
      // subarray() produces a real Uint8Array and that works.
      const bytesRead = file.accessHandle.read(pData.subarray(), { at: iOffset });
      if (bytesRead < pData.byteLength) {
        pData.fill(0, bytesRead);
        return VFS.SQLITE_IOERR_SHORT_READ;
      }
      return VFS.SQLITE_OK;
    } catch (e) {
      return VFS.SQLITE_IOERR_READ;
    }
  }

  /**
   * @param {number} fileId 
   * @param {Uint8Array} pData 
   * @param {number} iOffset
   * @returns {number}
   */
  jWrite(fileId, pData, iOffset) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);

      // On Chrome (at least), passing pData to accessHandle.write() is
      // an error because pData is a Proxy of a Uint8Array. Calling
      // subarray() produces a real Uint8Array and that works.
      file.accessHandle.write(pData.subarray(), { at: iOffset });
      return VFS.SQLITE_OK;
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_IOERR_WRITE;
    }
  }

  /**
   * @param {number} fileId 
   * @param {number} iSize 
   * @returns {number}
   */
  jTruncate(fileId, iSize) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);
      file.accessHandle.truncate(iSize);
      return VFS.SQLITE_OK;
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_IOERR_TRUNCATE;
    }
  }

  /**
   * @param {number} fileId 
   * @param {number} flags 
   * @returns {number}
   */
  jSync(fileId, flags) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);
      file.accessHandle.flush();
      return VFS.SQLITE_OK;
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_IOERR_FSYNC;
    }
  }

  /**
   * @param {number} fileId 
   * @param {DataView} pSize64 
   * @returns {number}
   */
  jFileSize(fileId, pSize64) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);
      const size = file.accessHandle.getSize();
      pSize64.setBigInt64(0, BigInt(size), true);
      return VFS.SQLITE_OK;
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_IOERR_FSTAT;
    }
  }

  jGetLastError(zBuf) {
    if (this.lastError) {
      console.error(this.lastError);
      const outputArray = zBuf.subarray(0, zBuf.byteLength - 1);
      const { written } = new TextEncoder().encodeInto(this.lastError.message, outputArray);
      zBuf[written] = 0;
    }
    return VFS.SQLITE_OK
  }
}