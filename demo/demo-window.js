// @ts-ignore
import * as Comlink from "https://unpkg.com/comlink/dist/esm/comlink.mjs";

const TEST_DURATION_MS = 10_000;
const PROGRESS_UPDATE_MS = 100;

function syncSettings(key) {
  const element = /** @type {HTMLSelectElement|HTMLInputElement} */
    (document.getElementById(key));
  const storedValue = sessionStorage.getItem(`demo-${key}`);
  if (storedValue !== null) {
    element.value = storedValue;
  }
  element.addEventListener('change', () => {
    sessionStorage.setItem(`demo-${key}`, element.value);
  });
}
syncSettings('contexts');
syncSettings('locking');
syncSettings('retry');
syncSettings('work');

document.getElementById('start').addEventListener('click', async event => {
  const onFinally = [];
  try {
    // Debounce the start button.
    (/** @type {HTMLButtonElement} */ (event.target)).disabled = true;
    onFinally.push(() => {
      (/** @type {HTMLButtonElement} */ (event.target)).disabled = false;
    });

    // Clear OPFS storage.
    await navigator.storage.getDirectory().then(async dirHandle => {
      // @ts-ignore
      for await (const name of dirHandle.keys()) {
        dirHandle.removeEntry(name, { recursive: true });
      }
    });

    const nContexts = parseInt(/** @type {HTMLSelectElement} */ (document.getElementById('contexts')).value);

    // Launch workers.
    const proxies = [];
    for (let i = 0; i < nContexts; i++) {
      const worker = new Worker(`demo-worker.js?name=${i}`, { type: 'module' });
      onFinally.push(() => worker.terminate());

      const proxy = Comlink.wrap(worker);
      proxies.push(proxy);
      onFinally.push(() => proxy[Comlink.releaseProxy]());
    }

    const config = {
      locking: /** @type {HTMLSelectElement} */ (document.getElementById('locking')).value,
      retryDelay: parseInt(/** @type {HTMLSelectElement} */ (document.getElementById('retry')).value),
      txnDelay: parseInt(/** @type {HTMLSelectElement} */ (document.getElementById('work')).value)
    }
    await Promise.all(proxies.map((proxy, i) => proxy.prepare(config)));

    // Start test.
    const startTime = performance.now() + performance.timeOrigin;
    const endTime = startTime + TEST_DURATION_MS;
    new BroadcastChannel('start-test').postMessage({
      endTime,
    });

    // Update progress bar.
    const progress = /** @type {HTMLProgressElement} */ (document.querySelector('progress'));
    let progressInterval = setInterval(() => {
      const now = performance.now() + performance.timeOrigin;
      progress.value = (now - startTime) / TEST_DURATION_MS;
      if (now >= endTime) {
        clearInterval(progressInterval);
      }
    }, PROGRESS_UPDATE_MS);

    // Wait for all workers to finish.
    await Promise.all(proxies.map(proxy => proxy.complete()));

    // Get results from one worker.
    const results = await proxies[0].getResults();
    const container = document.getElementById('results');
    for (const result of results) {
      renderTable(container, result);
    }
  } finally {
    while (onFinally.length) {
      try {
        await onFinally.pop()();
      } catch (e) {
        console.error(e);
      }
    }
  }

  
});

/**
 * @param {HTMLElement} parent 
 * @param {{columns: string[], rows: any[][]}} data 
 */
function renderTable(parent, data) {
  const table = document.createElement('table');
  const thead = document.createElement('thead');
  const tbody = document.createElement('tbody');
  table.appendChild(thead);
  table.appendChild(tbody);
  parent.appendChild(table);

  const headerRow = document.createElement('tr');
  for (const column of data.columns) {
    const th = document.createElement('th');
    th.textContent = column;
    headerRow.appendChild(th);
  }
  thead.appendChild(headerRow);

  for (const row of data.rows) {
    const tr = document.createElement('tr');
    for (const value of row) {
      const td = document.createElement('td');
      td.textContent = `${value}`;
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
}