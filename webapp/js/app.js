/**
 * UI for the Hoppy Clock USB configuration tool.
 *
 * Wires the connect button and the time card to a HoppyClock instance, polls
 * the device clock once a second and reports its drift against this computer.
 */

import {DeviceError, HoppyClock, isSupported, USB_FILTER} from './device.js';

/** How often to re-read the device clock while connected. */
const POLL_MS = 1000;

/** Age past which a device reading is too old to compare against. */
const STALE_MS = 4000;

/** Most log lines kept before the oldest are dropped. */
const LOG_LIMIT = 200;

const el = (id) => document.getElementById(id);

const ui = {
  app: el('app'),
  unsupported: el('unsupported'),
  status: el('status'),
  connect: el('connect'),
  sync: el('sync'),
  deviceTime: el('device-time'),
  deviceDate: el('device-date'),
  hostTime: el('host-time'),
  hostDate: el('host-date'),
  drift: el('drift'),
  driftNote: el('drift-note'),
  log: el('log'),
  clearLog: el('clear-log'),
};

const clock = new HoppyClock();

/** Latest device reading: {at: hostMsWhenRead, time: Date} or null. */
let lastReading = null;

/** Set while a user-initiated command owns the link, to pause polling. */
let busy = false;

const timeFmt = new Intl.DateTimeFormat(undefined, {
  hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
});
const dateFmt = new Intl.DateTimeFormat(undefined, {
  weekday: 'short', year: 'numeric', month: 'short', day: 'numeric',
});

/**
 * Append a line to the on-screen log.
 *
 * @param {string} message Text to show.
 * @param {'info'|'ok'|'err'} [kind] Styling hint.
 */
function log(message, kind = 'info') {
  const line = document.createElement('div');
  line.className = `log__line log__line--${kind}`;

  const stamp = document.createElement('span');
  stamp.className = 'log__time';
  stamp.textContent = `${timeFmt.format(new Date())}  `;

  line.append(stamp, document.createTextNode(message));
  ui.log.append(line);

  while (ui.log.childElementCount > LOG_LIMIT) {
    ui.log.firstElementChild.remove();
  }
  ui.log.scrollTop = ui.log.scrollHeight;
}

/**
 * Update the connection pill and button.
 *
 * @param {'idle'|'busy'|'live'|'err'} state Which look to show.
 * @param {string} text Pill label.
 */
function setStatus(state, text) {
  ui.status.className = `pill pill--${state}`;
  ui.status.textContent = text;
}

/** Reflect the current connection state across the whole UI. */
function refresh() {
  const on = clock.connected;
  document.body.classList.toggle('is-connected', on);
  ui.connect.textContent = on ? 'Disconnect' : 'Connect';
  ui.sync.disabled = !on || busy;

  if (!on) {
    lastReading = null;
    ui.deviceTime.textContent = '-';
    ui.deviceDate.textContent = '';
    ui.drift.textContent = '-';
    ui.drift.className = 'readout__value';
    ui.driftNote.textContent = '';
  }
}

/** Redraw the host clock and the drift, called on a display tick. */
function render() {
  const now = new Date();
  ui.hostTime.textContent = timeFmt.format(now);
  ui.hostDate.textContent = dateFmt.format(now);

  if (!lastReading) {
    return;
  }

  ui.deviceTime.textContent = timeFmt.format(lastReading.time);
  ui.deviceDate.textContent = dateFmt.format(lastReading.time);

  // A reading only stays meaningful for as long as the poll interval; past
  // that the link has stalled and any "drift" would just be our own staleness.
  if (Date.now() - lastReading.at > STALE_MS) {
    ui.drift.textContent = '-';
    ui.drift.className = 'readout__value';
    ui.driftNote.textContent = 'Waiting for a fresh reading.';
    return;
  }

  // The device reports whole seconds, so compare against the host truncated
  // the same way; a perfectly synced clock then reads exactly zero.
  const hostWholeSec = Math.floor(lastReading.at / 1000) * 1000;
  const seconds = Math.round((lastReading.time.getTime() - hostWholeSec) / 1000);
  const magnitude = Math.abs(seconds);

  if (seconds === 0) {
    ui.drift.textContent = 'In sync';
  } else {
    // The unit gets its own span so it is not padded out to a monospace cell.
    const unit = document.createElement('span');
    unit.className = 'readout__unit';
    unit.textContent = magnitude === 1 ? 'second' : 'seconds';
    ui.drift.replaceChildren(`${seconds > 0 ? '+' : '-'}${magnitude}`, ' ', unit,);
  }
  ui.drift.className = `readout__value ${magnitude <= 1 ? 'is-ok' : magnitude <= 60 ? 'is-warn' : 'is-err'}`;
  ui.driftNote.textContent = seconds === 0 ? 'Within the clock\'s one-second resolution.' : seconds > 0 ? 'The clock is ahead of this computer.' : 'The clock is behind this computer.';
}

/** Read the device clock once, tolerating a transient failure. */
async function poll() {
  if (!clock.connected || busy) {
    return;
  }
  try {
    const time = await clock.getTime();
    lastReading = {at: Date.now(), time};
    render();
  } catch (err) {
    if (clock.connected) {
      log(`could not read the clock: ${err.message}`, 'err');
    }
  }
}

/**
 * Run a user action that owns the link, with the UI locked meanwhile.
 *
 * @param {string} label Shown on the status pill while it runs.
 * @param {() => Promise<void>} action The work to do.
 */
async function withBusy(label, action) {
  busy = true;
  setStatus('busy', label);
  refresh();
  try {
    await action();
  } finally {
    busy = false;
    if (clock.connected) {
      setStatus('live', 'Connected');
    }
    refresh();
  }
}

/**
 * Open a port and greet the device.
 *
 * @param {SerialPort} [port] A previously granted port to reuse.
 */
async function connect(port) {
  await withBusy('Connecting...', async () => {
    await clock.connect(port);
    await clock.ping();
    log('connected, PING -> OK', 'ok');
    await poll();
  });
}

/** Handle the connect/disconnect button. */
async function toggleConnection() {
  if (clock.connected) {
    await clock.disconnect();
    return;
  }

  try {
    await connect();
  } catch (err) {
    // The picker throwing NotFoundError just means the user dismissed it.
    if (err instanceof DOMException && err.name === 'NotFoundError') {
      return;
    }
    log(`connect failed: ${err.message}`, 'err');
    setStatus('err', 'Failed');
    // A port that opened but did not answer is worse than no port at all.
    await clock.disconnect();
    refresh();
  }
}

/** Handle the sync button. */
async function syncNow() {
  await withBusy('Syncing...', async () => {
    try {
      const readback = await clock.syncTime();
      lastReading = {at: Date.now(), time: readback};
      render();
      log(`SET_TIME -> OK, clock now reads ${timeFmt.format(readback)}`, 'ok');
    } catch (err) {
      const detail = err instanceof DeviceError && err.status !== undefined ? ` (status ${err.status})` : '';
      log(`sync failed: ${err.message}${detail}`, 'err');
    }
  });
}

/** Reconnect silently to a port this site was already granted. */
async function tryPreviousPort() {
  const ports = await navigator.serial.getPorts();
  const match = ports.find((port) => {
    const info = port.getInfo?.() ?? {};
    return (info.usbVendorId === USB_FILTER.usbVendorId && info.usbProductId === USB_FILTER.usbProductId);
  });
  if (!match) {
    return;
  }
  try {
    await connect(match);
  } catch (err) {
    log(`could not reopen the last port: ${err.message}`, 'err');
    await clock.disconnect();
    refresh();
  }
}

function main() {
  if (!isSupported()) {
    ui.unsupported.hidden = false;
    ui.app.hidden = true;
    ui.connect.disabled = true;
    setStatus('err', 'Unsupported');
    return;
  }

  clock.addEventListener('open', () => {
    setStatus('live', 'Connected');
    refresh();
  });
  clock.addEventListener('close', (event) => {
    setStatus('idle', 'Disconnected');
    log(event.detail?.message ?? 'disconnected');
    refresh();
  });

  ui.connect.addEventListener('click', toggleConnection);
  ui.sync.addEventListener('click', syncNow);
  ui.clearLog.addEventListener('click', () => ui.log.replaceChildren());

  // The host clock is redrawn faster than it ticks so the seconds stay honest.
  setInterval(render, 250);
  setInterval(poll, POLL_MS);

  refresh();
  render();

  tryPreviousPort();
}

main();
