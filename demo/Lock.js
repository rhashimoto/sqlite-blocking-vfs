
export class Lock {
  #name;
  #releaser = null;

  constructor(name) {
    this.#name = name;
  }

  /**
   * @param {'shared'|'exclusive'} mode 
   * @param {number} timeout 
   * @return {Promise<boolean>}
   */
  async acquire(mode, timeout = -1) {
    if (this.#releaser) {
      throw new Error(`Lock ${this.#name} is already acquired`);
    }
    return new Promise(async (resolve, reject) => {
      // Set up locking options. Include timeout signal if a timeout
      // is specified.
      const options = { mode, ifAvailable: timeout === 0 };
      let timeoutId;
      if (timeout > 0) {
        const abortController = new AbortController();
        timeoutId = setTimeout(() => {
          abortController.abort();
        }, timeout);
        options.signal = abortController.signal;
      }

      navigator.locks.request(this.#name, options, lock => {
        if (timeoutId) clearTimeout(timeoutId);
        if (lock === null) {
          // Polling (with timeout = 0) failed to acquire the lock.
          return resolve(false);
        }

        // Lock acquired.
        return new Promise(releaser => {
          this.#releaser = releaser;
          resolve(true);
        })
      }).catch(e => {
        if (e.name === 'AbortError') {
          // Timeout expired while waiting for the lock.
          return resolve(false);
        }
        return reject(e);
      });
    });
  }

  release() {
    this.#releaser?.();
    this.#releaser = null;
  }
}