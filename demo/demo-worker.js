// @ts-ignore
import * as Comlink from "https://unpkg.com/comlink/dist/esm/comlink.mjs";
import SQLiteESMFactory from '../wa-sqlite/dist/wa-sqlite-jspi.mjs';
import * as SQLite from '../wa-sqlite/src/sqlite-api.js';
import { OPFSBaseUnsafeVFS } from "./OPFSBaseUnsafeVFS.js";
import { OPFSNoWriteHintVFS } from "./OPFSNoWriteHintVFS.js";

/** @type {SQLiteAPI} */ let sqlite3;
/** @type {number} */ let db;

class DemoWorker {
  name;

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
    this.name = config.name;

    const module = await SQLiteESMFactory();
    sqlite3 = SQLite.Factory(module);

    const vfs = new OPFSNoWriteHintVFS('opfs-unsafe', module);
    sqlite3.vfs_register(vfs, true);

    db = await sqlite3.open_v2('blocking-demo.db');

    await this.query(`
      CREATE TABLE IF NOT EXISTS
        test(name TEXT, ticks INTEGER, remaining INTEGER, retries INTEGER)
    `);

    // Pre-compile repeated statements.
    this.beginImmediate = await prepare(`BEGIN IMMEDIATE`);
    this.insert = await prepare(`
      INSERT INTO test(name, ticks, remaining, retries) VALUES(?, ?, ?, ?)
    `);
    this.commit = await prepare(`COMMIT`);

    new BroadcastChannel('start-test').onmessage = async ({ data }) => {
      const {
        endTime,
        txPadding,
        retryDelay,
      } = data;

      try {
        // Send transactions until past endTime.
        let txTime = 0;
        while (txTime < endTime) {
          // Repeat a transaction until it commits or time has expired.
          let retries = 0;
          while (true) {
            try {
              await sqlite3.step(this.beginImmediate);

              // We have the lock but are we still within the time window?
              // If time has expired, abandon this transaction.
              txTime = Date.now();
              if (txTime >= endTime) {
                await this.query('ROLLBACK');
                break;
              }

              if (txPadding) {
                // The chance of deadlock increases with time spent in the
                // RESERVED state, which could be from database reads,
                // database computations, or application code. Add a delay
                // here to simulate a more complex transaction.
                await new Promise(resolve => setTimeout(resolve, txPadding));
              }

              sqlite3.bind_collection(
                this.insert,
                [this.name, txTime, endTime - txTime, retries]);
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
                if (retryDelay) {
                  await new Promise(resolve => setTimeout(resolve, retryDelay));
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

        console.log(`Worker ${this.name} completed work at ${Date.now()}`);
        dispatchEvent(new CustomEvent('complete'));
      } catch (e) {
        dispatchEvent(new CustomEvent('complete', { detail: e }));
      }
    };
  }

  /**
   * @param {string} sql 
   * @returns {Promise<{columns: string[], rows: SQLiteCompatibleType[][]}>}
   */
  async query(sql) {
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

  async complete() {
    await this.isComplete;
    await finalize(this.beginImmediate);
    await finalize(this.insert);
    await finalize(this.commit);
  }

  async getResults() {
    const result = await this.query(`
      SELECT
        name,
        COUNT() AS transactions,
        SUM(retries) AS retries
      FROM test
      GROUP BY name
      ORDER BY name`);
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
