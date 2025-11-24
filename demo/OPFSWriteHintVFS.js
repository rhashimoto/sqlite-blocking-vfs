import * as VFS from '../wa-sqlite/src/VFS.js';
import { OPFSBaseUnsafeVFS } from "./OPFSBaseUnsafeVFS.js";
import { Lock } from "./Lock.js";

/**
 * @typedef LockState
 * @property {number} lockState
 * @property {Lock} accessLock
 * @property {Lock} reservedLock
 * @property {Lock} pendingLock
 * @property {Lock} writeHintLock
 * @property {string} [writeHint]
 */

export class OPFSWriteHintVFS extends OPFSBaseUnsafeVFS  {
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
    const rc = await super.jOpen(filename, fileId, flags, pOutFlags);
    if (rc === VFS.SQLITE_OK) {
      const file = this.mapFileIdToEntry.get(fileId);
      file.extra = /** @type {LockState} */ {
        lockState: VFS.SQLITE_LOCK_NONE,
        accessLock: new Lock(`OPFSWriteHint-${filename}-access`),
        reservedLock: new Lock(`OPFSWriteHint-${filename}-reserved`),
        pendingLock: new Lock(`OPFSWriteHint-${filename}-pending`),
        writeHintLock: new Lock(`OPFSWriteHint-${filename}-writehint`),
      }
    }
    return rc;
  }

  /**
   * @param {number} fileId 
   * @param {number} lockType 
   * @returns {Promise<number>}
   */
  async jLock(fileId, lockType) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);
      const { accessLock, reservedLock, pendingLock, writeHintLock } =
        /** @type {LockState} */ (file.extra);
      const timeout = -1; // TODO: Make configurable.
      switch (file.extra.lockState) {
        case VFS.SQLITE_LOCK_NONE:
          switch (lockType) {
            case VFS.SQLITE_LOCK_SHARED:
              if (file.extra.writeHint) {
                // The write hint tells us this will be a write transaction.
                // One writer at a time will be admitted here.
                if (!await writeHintLock.acquire('exclusive', timeout)) {
                  return VFS.SQLITE_BUSY; // Timeout
                }
              }

              if (!await pendingLock.acquire('shared', timeout)) {
                return VFS.SQLITE_BUSY; // Timeout
              };
              if (!await accessLock.acquire('shared', timeout)) {
                return VFS.SQLITE_BUSY; // Timeout
              }
              pendingLock.release();             
              break;
            default:
              throw new Error(`Invalid lock transition ${file.extra.lockState} to ${lockType}`);
          }
          break;
        case VFS.SQLITE_LOCK_SHARED:
          switch (lockType) {
            case VFS.SQLITE_LOCK_SHARED:
              break;
            case VFS.SQLITE_LOCK_RESERVED:
              if (!file.extra.writeHint) {
                // We didn't get a write hint because a BEGIN DEFERRED
                // transaction was used. Poll for it now.
                if (!await writeHintLock.acquire('exclusive', 0)) {
                  // Deadlock.
                  return VFS.SQLITE_BUSY;
                }
              }

              // This poll should always succeed.
              if (!await reservedLock.acquire('exclusive', 0)) {
                // Deadlock.
                writeHintLock.release();
                return VFS.SQLITE_BUSY;
              }
              break;
            case VFS.SQLITE_LOCK_EXCLUSIVE:
              if (!await pendingLock.acquire('exclusive', timeout)) {
                return VFS.SQLITE_BUSY; // Timeout
              }
              if (!await accessLock.acquire('exclusive', timeout)) {
                return VFS.SQLITE_BUSY; // Timeout
              }
              break;
            default:
              throw new Error(`Invalid lock transition ${file.extra.lockState} to ${lockType}`);
          }
          break;
        case VFS.SQLITE_LOCK_RESERVED:
          switch (lockType) {
            case VFS.SQLITE_LOCK_RESERVED:
              break;
            case VFS.SQLITE_LOCK_EXCLUSIVE:
              if (!await pendingLock.acquire('exclusive', timeout)) {
                return VFS.SQLITE_BUSY; // Timeout
              }
              accessLock.release();
              if (!await accessLock.acquire('exclusive', timeout)) {
                return VFS.SQLITE_BUSY; // Timeout
              }
              pendingLock.release();
              break;
            default:
              throw new Error(`Invalid lock transition ${file.extra.lockState} to ${lockType}`);
          }
          break;
      }
      file.extra.lockState = lockType;
      return VFS.SQLITE_OK;
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_IOERR_LOCK;
    }
  }

  /**
   * @param {number} fileId 
   * @param {number} lockType 
   * @returns {number}
   */
  jUnlock(fileId, lockType) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);
      if (lockType === file.extra.lockState) return VFS.SQLITE_OK;
      const { accessLock, reservedLock, writeHintLock } = /** @type {LockState} */ (file.extra);
      switch (lockType) {
        case VFS.SQLITE_LOCK_NONE:
          reservedLock.release();
          writeHintLock.release();
          accessLock.release();
          break;
        case VFS.SQLITE_LOCK_SHARED:
          reservedLock.release();
          writeHintLock.release();
          break;
        default:
          throw new Error(`Invalid unlock transition ${file.extra.lockState} to ${lockType}`);
      }
      file.extra.lockState = lockType;
      file.extra.writeHint = null;
      return VFS.SQLITE_OK;
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_IOERR_UNLOCK;
    }
  }

  /**
   * @param {number} fileId 
   * @param {DataView} pResOut 
   * @returns {Promise<number>}
   */
  async jCheckReservedLock(fileId, pResOut) {
    try {
      const file = this.mapFileIdToEntry.get(fileId);
      const { reservedLock } = /** @type {LockState} */ (file.extra);

      // Try to acquire the reserved lock ourselves. Request shared mode
      // so we don't interfere with other jCheckReservedLock callers.
      if (reservedLock.acquire('shared', 0)) {
        // We got the lock so there is no exclusive holder.
        reservedLock.release();
        pResOut.setInt32(0, 0, true);
      } else {
        pResOut.setInt32(0, 1, true);
      }
      return VFS.SQLITE_OK;
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_IOERR_CHECKRESERVEDLOCK;
    }
  }

  /**
   * @param {number} pFile
   * @param {number} op
   * @param {DataView} pArg
   * @returns {number|Promise<number>}
   */
  jFileControl(pFile, op, pArg) {
    try {
      const file = this.mapFileIdToEntry.get(pFile);
      switch (op) {
        case VFS.SQLITE_FCNTL_PRAGMA:
          const key = extractString(pArg, 4);
          const value = extractString(pArg, 8);
          switch (key.toLowerCase()) {
            case 'experimental_pragma_20251114':
              file.extra.writeHint = value;
              break;
          }
      }
    } catch (e) {
      this.lastError = e;
      return VFS.SQLITE_IOERR;
    }
    return VFS.SQLITE_NOTFOUND;
  }
}

function extractString(dataView, offset) {
  const p = dataView.getUint32(offset, true);
  if (p) {
    const chars = new Uint8Array(dataView.buffer, p);
    return new TextDecoder().decode(chars.subarray(0, chars.indexOf(0)));
  }
  return null;
}