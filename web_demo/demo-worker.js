// @ts-ignore
import * as Comlink from "https://unpkg.com/comlink/dist/esm/comlink.mjs";
import SQLiteESMFactory from './wa-sqlite/dist/wa-sqlite-jspi.mjs';
import * as SQLite from './wa-sqlite/src/sqlite-api.js';
import { OPFSBaseUnsafeVFS } from "./OPFSBaseUnsafeVFS.js";
import { OPFSNoWriteHintVFS } from "./OPFSNoWriteHintVFS.js";
import { OPFSWriteHintVFS } from "./OPFSWriteHintVFS.js";

// Self-terminate on command.
new BroadcastChannel('terminate').onmessage = () => close();

/** @type {SQLiteAPI} */ let sqlite3;
/** @type {number} */ let db;

class DemoWorker {
  name = new URL(self.location.href).searchParams.get('name');

  beginImmediate = 0;
  insert = 0;
  commit = 0;

  isComplete = new Promise((resolve, reject) => {
    addEventListener('complete', event => {
      if (event.detail instanceof Error) {
        reject(event.detail);
      } else {
        resolve(event.detail);
      }
    });
  });
  
  async prepare(config) {
    const module = await SQLiteESMFactory();
    sqlite3 = SQLite.Factory(module);

    let vfs;
    switch (config.locking) {
      case 'none':
        vfs = new OPFSBaseUnsafeVFS('opfs-unsafe', module);
        break;
      case 'standard':
        vfs = new OPFSNoWriteHintVFS('opfs-unsafe', module);
        break;
      case 'write-hint':
        vfs = new OPFSWriteHintVFS('opfs-unsafe', module);
        break;
      default:
        throw new Error(`Unknown locking policy: ${config.locking}`);
    }
    sqlite3.vfs_register(vfs, true);

    db = await sqlite3.open_v2('blocking-demo.db');

    await query(`
      CREATE TABLE IF NOT EXISTS
        test(worker TEXT, ticks NUMERIC, wait NUMERIC, retries INTEGER)
    `);

    // Pre-compile repeated statements.
    this.beginImmediate = await prepare(`BEGIN IMMEDIATE`);
    this.insert = await prepare(`
      INSERT INTO test(worker, ticks, wait, retries) VALUES(?, ?, ?, ?)
    `);
    this.commit = await prepare(`COMMIT`);

    new BroadcastChannel('start-test').onmessage = async ({ data }) => {
      const { endTime } = data;

      try {
        // Send transactions until past endTime.
        let txTime = 0;
        while (txTime < endTime) {
          // Repeat a transaction until it commits or time has expired.
          let retries = 0;
          const requestTime = performance.now() + performance.timeOrigin;
          while (true) {
            try {
              await sqlite3.step(this.beginImmediate);

              // We have the lock but are we still within the time window?
              // If time has expired, abandon this transaction.
              txTime = performance.now() + performance.timeOrigin;
              if (txTime >= endTime) {
                await query('ROLLBACK');
                break;
              }

              if (config.txnDelay > 0) {
                // The chance of deadlock increases with time spent in the
                // RESERVED state, which could be from database reads,
                // database computations, or application code. Add a delay
                // here to simulate a more complex transaction.
                await new Promise(resolve => setTimeout(resolve, config.txnDelay));
              }

              sqlite3.bind_collection(
                this.insert,
                [this.name, txTime, txTime - requestTime, retries]);
              await sqlite3.step(this.insert);

              await sqlite3.step(this.commit);
              break;
            } catch (e) {
              if (e.code === SQLite.SQLITE_BUSY) {
                // Applications should rollback if within a multi-statement
                // transaction. Here we know that is not the case because
                // we used BEGIN IMMEDIATE, so rollback is not needed, just
                // retry. 
                retries++;
                if (config.retryDelay) {
                  await new Promise(resolve => setTimeout(resolve, config.retryDelay));
                }
              } else {
                // This is not an error that rollback and retry can fix.
                throw e;
              }
            } finally {
              await sqlite3.reset(this.beginImmediate).catch(() => {});
              await sqlite3.reset(this.insert).catch(() => {});
              await sqlite3.reset(this.commit).catch(() => {});
            }
          }
        }

        dispatchEvent(new CustomEvent('complete'));
      } catch (e) {
        dispatchEvent(new CustomEvent('complete', { detail: e }));
      }
    };
  }

  async complete() {
    await this.isComplete;
    await finalize(this.beginImmediate);
    await finalize(this.insert);
    await finalize(this.commit);
  }

  async getResults() {
    const result = await query(`
      WITH EnhancedEvents AS (
          SELECT 
              worker,
              wait,
              retries,
              -- Calculate the difference from the previous rowid immediately
              rowid - LAG(rowid) OVER (PARTITION BY worker ORDER BY rowid) AS slot_diff
          FROM test
      )
      SELECT 
          worker,
          COUNT(*) AS transactions,
          AVG(wait) AS "avg wait",
          MAX(wait) AS "max wait",
          SUM(retries) AS retries,
          MIN(slot_diff) AS "min slots",
          MAX(slot_diff) AS "max slots"
      FROM EnhancedEvents
      GROUP BY worker
      ORDER BY worker;
    `);
    return [result];
  }
}
Comlink.expose(new DemoWorker());

// Convenience functions for persistent prepared statements.
const mapStatementToFinalizer = new Map();

/**
 * @param {string} sql 
 * @return {Promise<number>}
 */
async function prepare(sql) {
    const iterator = sqlite3.statements(db, sql)[Symbol.asyncIterator]();
    const statement = await iterator.next();
    mapStatementToFinalizer.set(statement.value, () => iterator.return());
    return statement.value;
}

/**
 * @param {number} statement 
 */
async function finalize(statement) {
    await mapStatementToFinalizer.get(statement)?.();
    mapStatementToFinalizer.delete(statement);
}

  /**
   * @param {string} sql 
   * @returns {Promise<{columns: string[], rows: SQLiteCompatibleType[][]}>}
   */
  async function query(sql) {
    const prepared = await prepare(sql);
    try {
      // Retry query until success or fatal exception.
      while (true) {
        try {
          const rows = [];
          while (await sqlite3.step(prepared) === SQLite.SQLITE_ROW) {
            const row = sqlite3.row(prepared);
            rows.push(row);
          }
          return { columns: sqlite3.column_names(prepared), rows };
        } catch (e) {
          if (e.code === SQLite.SQLITE_BUSY) {
            await new Promise(resolve => setTimeout(resolve));
            continue;
          }
          throw e;
        }
      }
    } finally {
      await finalize(prepared);
    }
  }

/**
 * Query function for console debugging.
 * @param {string} sql 
 */  
self['q'] = async function(sql) {
  for await (const stmt of sqlite3.statements(db, sql)) {
    let columns = null;
    const objects = [];
    while (await sqlite3.step(stmt) === SQLite.SQLITE_ROW) {
      columns = columns ?? sqlite3.column_names(stmt);
      const row = sqlite3.row(stmt);
      const object = Object.fromEntries(columns.map((col, i) => [col, row[i]]));
      objects.push(object);
    }
    console.table(objects);
  }
}