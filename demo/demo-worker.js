// @ts-ignore
import * as Comlink from "https://unpkg.com/comlink/dist/esm/comlink.mjs";
import SQLiteESMFactory from '../wa-sqlite/dist/wa-sqlite-jspi.mjs';
import * as SQLite from '../wa-sqlite/src/sqlite-api.js';

/** @type {SQLiteAPI} */ let sqlite3;
/** @type {number} */ let db;

class DemoWorker {
  name;
  
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
              await this.query(`BEGIN IMMEDIATE`);

              txTime = Date.now();
              if (txPadding) {
                // Simulate a longer transaction.
                await new Promise(resolve => setTimeout(resolve, txPadding));
              }

              await this.query(`
                INSERT INTO test(name, ticks, remaining, retries)
                  VALUES('${this.name}', ${txTime}, ${endTime - txTime}, ${retries})`);
              await this.query(`COMMIT`);
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

  async query(sql) {
    const results = [];
    while (true) {
      try {
        await sqlite3.exec(db, sql, (row, columns) => {
          if (columns != results.at(-1)?.columns) {
            results.push({ columns, rows: [] });
          }
          results.at(-1).rows.push(row);
        });
        return results;
      } catch (e) {
        if (e.code === SQLite.SQLITE_BUSY) {
          await new Promise(resolve => setTimeout(resolve));
          continue;
        }
        throw e;
      }
    }
  }

  async complete() {
    return this.isComplete
  }

  async getResults() {
    return this.query(`
      SELECT
        name,
        COUNT() AS transactions,
        SUM(retries) AS retries
      FROM test
      GROUP BY name
      ORDER BY name`);
  }
}

Comlink.expose(new DemoWorker());