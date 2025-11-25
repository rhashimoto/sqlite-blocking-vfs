import * as VFS from '../wa-sqlite/src/VFS.js';
import { OPFSBaseUnsafeVFS } from "./OPFSBaseUnsafeVFS.js";
import { Lock } from "./Lock.js";

/**
 * @typedef LockState
 * @property {number} lockState
 * @property {Lock} accessLock
 * @property {Lock} reservedLock
 * 
 * @property {string?} writeHint
 * @property {Lock} writeHintLock
 */

/**
 * This VFS extends OPFSBaseUnsafeVFS by adding locking with the
 * experimental write hint.
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
        writeHintLock: new Lock(`OPFSWriteHint-${filename}-writehint`)
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
      if (lockType === file.extra.lockState) return VFS.SQLITE_OK;

      const { accessLock, reservedLock } = /** @type {LockState} */ (file.extra);
      const timeout = -1; // TODO: Make configurable.
      switch (file.extra.lockState) {
        case VFS.SQLITE_LOCK_NONE:
          switch (lockType) {
            case VFS.SQLITE_LOCK_SHARED:
              if (file.extra.writeHint) {
                if (!await file.extra.writeHintLock.acquire('exclusive', timeout)) {
                  return VFS.SQLITE_BUSY; // reached only on timeout
                }
                await accessLock.acquire('shared');
              } else {
                if (!await accessLock.acquire('shared', timeout)) {
                  file.extra.writeHintLock.release();
                  return VFS.SQLITE_BUSY; // reached only on timeout
                }
              }
              break;
            default:
              throw new Error(`Invalid lock transition ${file.extra.lockState} to ${lockType}`);
          }
          break;
        case VFS.SQLITE_LOCK_SHARED:
          switch (lockType) {
            case VFS.SQLITE_LOCK_RESERVED:
              if (!file.extra.writeHint) {
                // This is a write transaction without a write hint, i.e.
                // a write statement within BEGIN DEFERRED. 
                if (!await file.extra.writeHintLock.acquire('exclusive', 0)) {
                  return VFS.SQLITE_BUSY;
                }
              }

              // The reserved lock is guaranteed to be available here.
              await reservedLock.acquire('exclusive');
              break;
            case VFS.SQLITE_LOCK_EXCLUSIVE:
              if (!await accessLock.acquire('exclusive', timeout)) {
                return VFS.SQLITE_BUSY; // reached only on timeout
              }
              break;
          }
          break;
        case VFS.SQLITE_LOCK_RESERVED:
          switch (lockType) {
            case VFS.SQLITE_LOCK_EXCLUSIVE:
              accessLock.release();
              if (!await accessLock.acquire('exclusive', timeout)) {
                return VFS.SQLITE_BUSY; // reached only on timeout
              }
              break;
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

      const { accessLock, reservedLock } = /** @type {LockState} */ (file.extra);
      switch (lockType) {
        case VFS.SQLITE_LOCK_NONE:
          reservedLock.release();
          accessLock.release();
          file.extra.writeHintLock.release();
          file.extra.writeHint = null;
          break;
        case VFS.SQLITE_LOCK_SHARED:
          // If the current lock state is EXCLUSIVE, technically we should
          // downgrade accessLock from exclusive to shared. However, this is
          // expensive because it requires releasing and reacquiring the lock,
          // plus that forces this method to be async.
          //
          // Most of the time the lock state will subsequently transition to
          // NONE, which releases accessLock anyway. The exceptions are when
          // dealing with unexpected journal file states. In those rare cases,
          // leaving accessLock as exclusive prevents read concurrency during
          // that transaction but is otherwise harmless.
          reservedLock.release();
          break;
        default:
          throw new Error(`Invalid unlock transition ${file.extra.lockState} to ${lockType}`);
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