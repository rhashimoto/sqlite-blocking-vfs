import * as VFS from '../wa-sqlite/src/VFS.js';
import { OPFSBaseUnsafeVFS } from "./OPFSBaseUnsafeVFS.js";
import { Lock } from "./Lock.js";

/**
 * @typedef LockState
 * @property {number} lockState
 * @property {Lock} accessLock
 * @property {Lock} reservedLock
 * @property {Lock} pendingLock
 */

export class OPFSNoWriteHintVFS extends OPFSBaseUnsafeVFS  {
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
        accessLock: new Lock(`OPFSNoWriteHint-${filename}-access`),
        reservedLock: new Lock(`OPFSNoWriteHint-${filename}-reserved`),
        pendingLock: new Lock(`OPFSNoWriteHint-${filename}-pending`),
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
      const { accessLock, reservedLock, pendingLock } = /** @type {LockState} */ (file.extra);
      const timeout = -1; // TODO: Make configurable.
      switch (file.extra.lockState) {
        case VFS.SQLITE_LOCK_NONE:
          switch (lockType) {
            case VFS.SQLITE_LOCK_SHARED:
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
              if (!await reservedLock.acquire('exclusive', 0)) {
                // Deadlock.
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
      const { accessLock, reservedLock } = /** @type {LockState} */ (file.extra);
      switch (file.extra.lockState) {
        case VFS.SQLITE_LOCK_SHARED:
          switch (lockType) {
            case VFS.SQLITE_LOCK_NONE:
              accessLock.release();
              break;
            default:
              throw new Error(`Invalid unlock transition ${file.extra.lockState} to ${lockType}`);
          }
          break;
        case VFS.SQLITE_LOCK_RESERVED:
          switch (lockType) {
            case VFS.SQLITE_LOCK_SHARED:
              reservedLock.release();
              break;
            default:
              throw new Error(`Invalid unlock transition ${file.extra.lockState} to ${lockType}`);
          }
          break;
        case VFS.SQLITE_LOCK_EXCLUSIVE:
          switch (lockType) {
            case VFS.SQLITE_LOCK_NONE:
              reservedLock.release();
              accessLock.release();
              break;
            case VFS.SQLITE_LOCK_SHARED:
              reservedLock.release();
              break;
            default:
              throw new Error(`Invalid unlock transition ${file.extra.lockState} to ${lockType}`);
          }
          break;
      }
      file.extra.lockState = lockType;
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
}
