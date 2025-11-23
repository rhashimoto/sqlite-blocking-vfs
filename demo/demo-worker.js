// @ts-ignore
import * as Comlink from "https://unpkg.com/comlink/dist/esm/comlink.mjs";
import SQLiteESMFactory from '../wa-sqlite/dist/wa-sqlite-jspi.mjs';
import * as SQLite from '../wa-sqlite/src/sqlite-api.js';

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

    // TODO: register VFS

    db = await sqlite3.open_v2(':memory:');

    await this.query(`
      CREATE TABLE IF NOT EXISTS
        test(name TEXT, ticks INTEGER, remaining INTEGER, retries INTEGER)
    `);

    // Pre-compile repeated statements.
    this.beginImmediate = await prepare(`BEGIN IMMEDIATE`);
    this.insert = await prepare(`
      INSERT INTO test(name, ticks, remaining, retries)
        VALUES(?, ?, ?, ?)
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
          let retries = 0;
          let isCommitted = false;
          while (!isCommitted) {
            try {
              await sqlite3.step(this.beginImmediate);

              txTime = Date.now();
              if (txPadding) {
                // Simulate a longer transaction.
                await new Promise(resolve => setTimeout(resolve, txPadding));
              }

              sqlite3.bind_collection(
                this.insert,
                [this.name, txTime, endTime - txTime, retries]);
              await sqlite3.step(this.insert);

              await sqlite3.step(this.commit);
              isCommitted = true;
            } catch (e) {
              if (e.code === SQLite.SQLITE_BUSY) {
                retries++;
                if (retryDelay) {
                  await new Promise(resolve => setTimeout(resolve, retryDelay));
                }
                continue;
              }
              throw e;
            } finally {
              sqlite3.reset(this.beginImmediate);
              sqlite3.reset(this.insert);
              sqlite3.reset(this.commit);
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
