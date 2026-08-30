/**
 * UI for the Hoppy Clock USB configuration tool.
 *
 * Wires the connect button, the time card and the alarm card to a HoppyClock
 * instance, polls the device clock once a second and reports its drift against
 * this computer.
 */

import {DeviceError, HoppyClock, isSupported, USB_FILTER} from './device.js';
import {
  MAX_ALARMS, decodeAlarm, describeDays, encodeAlarm, formatAlarmTime,
} from './alarms.js';
import {
  BLANK_LIGHT,
  EFFECTS,
  MAX_LIGHTS,
  SPREAD_LABELS,
  decodeLight,
  describeLight,
  encodeLight,
  fromHex,
  lightColor,
  toHex,
} from './lights.js';
import {MAX_SOUNDS, describeSound} from './sounds.js';

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
  alarmList: el('alarm-list'),
  alarmSummary: el('alarm-summary'),
  refreshAlarms: el('refresh-alarms'),
  alarmAdder: el('alarm-adder'),
  alarmForm: el('alarm-form'),
  alarmTime: el('alarm-time'),
  alarmMode: el('alarm-mode'),
  alarmDays: el('alarm-days'),
  alarmDomField: el('alarm-dom-field'),
  alarmDom: el('alarm-dom'),
  alarmSound: el('alarm-sound'),
  alarmLight: el('alarm-light'),
  alarmTimeout: el('alarm-timeout'),
  alarmFade: el('alarm-fade'),
  alarmEnabled: el('alarm-enabled'),
  alarmAdd: el('alarm-add'),
  lightList: el('light-list'),
  lightSummary: el('light-summary'),
  refreshLights: el('refresh-lights'),
  lampOn: el('lamp-on'),
  lampOff: el('lamp-off'),
  lampSave: el('lamp-save'),
  ledCount: el('led-count'),
  ledSave: el('led-save'),
  tabs: document.querySelectorAll('.tab'),
  soundList: el('sound-list'),
  soundSummary: el('sound-summary'),
  refreshSounds: el('refresh-sounds'),
  soundFade: el('sound-fade'),
  soundStop: el('sound-stop'),
  buttonSound: el('button-sound'),
  buttonSoundSave: el('button-sound-save'),
  lightEditor: el('light-editor'),
  lightEditorSummary: el('light-editor-summary'),
  lightForm: el('light-form'),
  lightId: el('light-id'),
  lightEffect: el('light-effect'),
  lightColorField: el('light-color-field'),
  lightColor: el('light-color'),
  lightBrightness: el('light-brightness'),
  lightPeriodLabel: el('light-period-label'),
  lightPeriod: el('light-period'),
  lightCurveField: el('light-curve-field'),
  lightCurve: el('light-curve'),
  lightSpreadField: el('light-spread-field'),
  lightSpreadLabel: el('light-spread-label'),
  lightSpread: el('light-spread'),
  lightSave: el('light-save'),
  lightReset: el('light-reset'),
};

const clock = new HoppyClock();

/** Where the chosen tab is remembered between visits. */
const TAB_KEY = 'hoppy-clock.tab';

/**
 * Show one tab's panel and hide the rest.
 *
 * @param {string} name The tab key, e.g. 'clock'.
 * @param {boolean} [focus] Move focus to the tab, for keyboard use.
 */
function showTab(name, focus = false) {
  for (const tab of ui.tabs) {
    const selected = tab.id === `tab-${name}`;
    tab.setAttribute('aria-selected', String(selected));
    // Roving tabindex: only the selected tab is in the tab order, and the
    // arrow keys move between them from there.
    tab.tabIndex = selected ? 0 : -1;
    el(tab.getAttribute('aria-controls')).hidden = !selected;
    if (selected && focus) {
      tab.focus();
    }
  }
  try {
    localStorage.setItem(TAB_KEY, name);
  } catch {
    // Private mode, or storage turned off; the tab just will not persist.
  }
}

/** Move between tabs with the arrow keys, as a tablist should. */
function onTabKey(event) {
  const keys = {ArrowLeft: -1, ArrowRight: 1};
  const step = keys[event.key];
  if (!step) {
    return;
  }
  const tabs = [...ui.tabs];
  const at = tabs.findIndex((tab) => tab.getAttribute('aria-selected') === 'true');
  const next = tabs[(at + step + tabs.length) % tabs.length];
  showTab(next.id.replace('tab-', ''), true);
  event.preventDefault();
}

/** Latest device reading: {at: hostMsWhenRead, time: Date} or null. */
let lastReading = null;

/** Set while a user-initiated command owns the link, to pause polling. */
let busy = false;

/** The clock's alarm table as last read, still in packed record form. */
let alarmRecords = [];

/** The clock's light table as last read, still in packed record form. */
let lightRecords = [];

/** The lamp's two idle light ids as last read: {on, off}. */
let lamp = {on: 0, off: 0};

/** How many LEDs the chain drives, as last read. */
let ledCount = 1;

/** The sound id a long press plays, as last read. */
let buttonSound = 0;

/** What each sound slot holds, as last read: a Sound, or null if empty. */
let soundSlots = [];

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

  updateAlarmControls();
  updateLightControls();
  updateSoundControls();

  if (!on) {
    lastReading = null;
    alarmRecords = [];
    lightRecords = [];
    soundSlots = [];
    renderAlarms();
    renderLights();
    renderSounds();
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

/** Enable or disable the alarm controls for the current state. */
function updateAlarmControls() {
  const live = clock.connected && !busy;
  ui.refreshAlarms.disabled = !live;
  ui.alarmAdd.disabled = !live || alarmRecords.length >= MAX_ALARMS;
  for (const button of ui.alarmList.querySelectorAll('button')) {
    button.disabled = !live;
  }
}

/** Draw the alarm table from the records last read off the clock. */
function renderAlarms() {
  const rows = alarmRecords.map((record, index) => {
    const alarm = decodeAlarm(record);
    const row = document.createElement('li');
    row.className = `alarm${alarm.enabled ? '' : ' alarm--off'}`;

    const time = document.createElement('span');
    time.className = 'alarm__time';
    time.textContent = formatAlarmTime(alarm);

    const when = document.createElement('span');
    when.className = 'alarm__when';
    when.textContent = describeDays(alarm);

    const meta = document.createElement('span');
    meta.className = 'alarm__meta';
    meta.textContent = [`sound ${alarm.soundId}`, `light ${alarm.lightId}`, alarm.timeoutS ? `quiets after ${alarm.timeoutS} s` : 'rings until stopped', alarm.fadeS ? `${alarm.fadeS} s fade-in` : null, alarm.enabled ? null : 'disabled',]
      .filter(Boolean)
      .join(' - ');

    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'btn btn--ghost';
    remove.dataset.index = String(index);
    remove.textContent = 'Delete';

    row.append(time, when, meta, remove);
    return row;
  });

  ui.alarmList.replaceChildren(...rows);
  ui.alarmSummary.textContent = !clock.connected ? 'Not read yet.' : rows.length === 0 ? 'No alarms stored.' : `${rows.length} of ${MAX_ALARMS} alarm slots used.`;
  updateAlarmControls();
}

/** Read the whole config once and refresh both cards from it. */
async function readAll() {
  try {
    const config = await clock.readConfig();
    alarmRecords = config.alarms;
    lightRecords = config.lights;
    lamp = {on: config.lampOn, off: config.lampOff};
    ledCount = config.ledCount;
    ui.ledCount.value = String(ledCount);
    buttonSound = config.buttonSound;
    ui.buttonSound.value = String(buttonSound);
    renderAlarms();
    renderLights();
    log(`read ${alarmRecords.length} alarm(s) and ` + `${lightRecords.length} light look(s)`, 'ok',);
  } catch (err) {
    log(`could not read the config: ${err.message}`, 'err');
  }
}

/** Show the day picker that matches the chosen repeat mode. */
function syncAlarmMode() {
  const monthly = ui.alarmMode.value === 'monthly';
  ui.alarmDays.hidden = monthly;
  ui.alarmDomField.hidden = !monthly;
}

/** Collect the add-alarm form into an Alarm, unvalidated. */
function readAlarmForm() {
  const [hour, minute, second = 0] = ui.alarmTime.value.split(':').map(Number);
  const monthly = ui.alarmMode.value === 'monthly';
  const checked = ui.alarmDays.querySelectorAll('input:checked');
  return {
    enabled: ui.alarmEnabled.checked,
    monthly,
    daySel: monthly ? Number(ui.alarmDom.value) : [...checked].reduce((mask, box) => mask | (1 << Number(box.value)), 0),
    hour,
    minute,
    second,
    timeoutS: Number(ui.alarmTimeout.value),
    soundId: Number(ui.alarmSound.value),
    lightId: Number(ui.alarmLight.value),
    fadeS: Number(ui.alarmFade.value),
  };
}

/** Handle the add-alarm form. */
async function submitAlarm(event) {
  event.preventDefault();

  let record;
  try {
    // encodeAlarm is the last word on what a record can hold, so let it reject
    // the spec rather than duplicating its limits here.
    record = encodeAlarm(readAlarmForm());
  } catch (err) {
    log(`cannot add that alarm: ${err.message}`, 'err');
    return;
  }

  await withBusy('Saving...', async () => {
    try {
      alarmRecords = await clock.addAlarm(record);
      renderAlarms();
      log(`alarm added, ${alarmRecords.length} stored`, 'ok');
      ui.alarmAdder.open = false;
    } catch (err) {
      log(`could not add the alarm: ${err.message}`, 'err');
    }
  });
}

/** Handle a Delete button in the alarm list. */
async function deleteAlarm(index) {
  const alarm = decodeAlarm(alarmRecords[index]);
  const label = `${formatAlarmTime(alarm)} (${describeDays(alarm)})`;
  if (!confirm(`Delete
  the
  ${label}
  alarm
  from
  the
  clock
  ?`)) {
    return;
  }

  await withBusy('Saving...', async () => {
    try {
      alarmRecords = await clock.removeAlarm(index);
      renderAlarms();
      log(`alarm deleted, ${alarmRecords.length} left`, 'ok');
    } catch (err) {
      log(`could not delete the alarm: ${err.message}`, 'err');
    }
  });
}

/** Enable or disable the light controls for the current state. */
function updateLightControls() {
  const live = clock.connected && !busy;
  ui.refreshLights.disabled = !live;
  ui.lightSave.disabled = !live;
  ui.lampSave.disabled = !live || lightRecords.length === 0;
  ui.ledSave.disabled = !live;
  for (const button of ui.lightList.querySelectorAll('button')) {
    button.disabled = !live;
  }
}

/** Draw the light table and the lamp pickers from the last read. */
function renderLights() {
  const rows = lightRecords.map((record, id) => {
    const light = decodeLight(record);
    const row = document.createElement('li');
    row.className = 'light';

    const swatch = document.createElement('span');
    swatch.className = 'light__swatch';
    // Rainbow has no single colour, so lightColor hands back a gradient.
    swatch.style.background = lightColor(light);

    const name = document.createElement('span');
    name.className = 'light__name';
    name.textContent = `${id}: ${light.effect}`;

    const meta = document.createElement('span');
    meta.className = 'light__meta';
    meta.textContent = describeLight(light);

    const edit = document.createElement('button');
    edit.type = 'button';
    edit.className = 'btn btn--ghost';
    edit.dataset.id = String(id);
    edit.textContent = 'Edit';

    row.append(swatch, name, meta, edit);
    return row;
  });

  ui.lightList.replaceChildren(...rows);
  ui.lightSummary.textContent = !clock.connected ? 'Not read yet.' : rows.length === 0 ? 'No light looks stored. Alarms and the lamp need at least one.' : `${rows.length} of ${MAX_LIGHTS} light looks stored.`;

  renderLampPickers();
  updateLightControls();
}

/** Fill the lamp on/off pickers with the ids that now exist. */
function renderLampPickers() {
  for (const [select, current] of [[ui.lampOn, lamp.on], [ui.lampOff, lamp.off],]) {
    const options = lightRecords.map((record, id) => {
      const option = document.createElement('option');
      option.value = String(id);
      option.textContent = `${id}: ${decodeLight(record).effect}`;
      return option;
    });
    // Keep a stored id that points nowhere, rather than silently retargeting
    // the lamp at another look the next time this is saved.
    if (current >= lightRecords.length) {
      const missing = document.createElement('option');
      missing.value = String(current);
      missing.textContent = `${current}: missing`;
      options.push(missing);
    }
    select.replaceChildren(...options);
    select.value = String(current);
  }
}

/** Relabel and hide the fields the chosen effect does not use. */
function syncLightEffect() {
  const effect = ui.lightEffect.value;
  const solid = effect === 'solid';
  const spreadLabel = SPREAD_LABELS[effect];

  ui.lightCurveField.hidden = !solid;
  ui.lightColorField.hidden = effect === 'rainbow';
  ui.lightSpreadField.hidden = !spreadLabel;
  if (spreadLabel) {
    ui.lightSpreadLabel.textContent = spreadLabel;
  }
  ui.lightPeriodLabel.textContent = solid ? 'Fade time (ms)' : 'Cycle time (ms)';
}

/** Load a look into the editor, or the defaults when given none. */
function fillLightForm(id, light) {
  ui.lightId.value = String(id);
  ui.lightEffect.value = EFFECTS.includes(light.effect) ? light.effect : 'solid';
  ui.lightColor.value = toHex(light);
  ui.lightBrightness.value = String(light.brightness);
  ui.lightPeriod.value = String(light.periodMs);
  ui.lightCurve.value = light.curve;
  ui.lightSpread.value = String(light.spread);
  ui.lightEditorSummary.textContent = id < lightRecords.length ? `Editing light ${id}` : 'Add a light look';
  syncLightEffect();
}

/** Collect the light editor into a Light, unvalidated. */
function readLightForm() {
  return {
    ...fromHex(ui.lightColor.value),
    effect: ui.lightEffect.value,
    brightness: Number(ui.lightBrightness.value),
    periodMs: Number(ui.lightPeriod.value),
    curve: ui.lightCurve.value,
    spread: Number(ui.lightSpread.value),
  };
}

/** Handle the light editor form. */
async function submitLight(event) {
  event.preventDefault();

  const id = Number(ui.lightId.value);
  let record;
  try {
    record = encodeLight(readLightForm());
  } catch (err) {
    log(`cannot save that look: ${err.message}`, 'err');
    return;
  }

  await withBusy('Saving...', async () => {
    try {
      lightRecords = await clock.saveLight(id, record);
      renderLights();
      log(`light ${id} saved, ${lightRecords.length} stored`, 'ok');
      ui.lightEditor.open = false;
    } catch (err) {
      log(`could not save the look: ${err.message}`, 'err');
    }
  });
}

/** Handle the lamp on/off save. */
async function saveLamp() {
  const on = Number(ui.lampOn.value);
  const off = Number(ui.lampOff.value);

  await withBusy('Saving...', async () => {
    try {
      await clock.setLamp(on, off);
      lamp = {on, off};
      log(`button lights saved, on=${on} off=${off}`, 'ok');
    } catch (err) {
      log(`could not save the button lights: ${err.message}`, 'err');
    }
  });
}

/** Handle the LED count save. */
async function saveLedCount() {
  const count = Number(ui.ledCount.value);

  await withBusy('Saving...', async () => {
    try {
      await clock.setLedCount(count);
      ledCount = count;
      log(`LED count saved, ${count} in the chain`, 'ok');
    } catch (err) {
      // Put the stored value back so the field cannot sit on a rejected one.
      ui.ledCount.value = String(ledCount);
      log(`could not save the LED count: ${err.message}`, 'err');
    }
  });
}

/** Enable or disable the sound controls for the current state. */
function updateSoundControls() {
  const live = clock.connected && !busy;
  ui.refreshSounds.disabled = !live;
  ui.soundStop.disabled = !live;
  ui.buttonSoundSave.disabled = !live;
  for (const button of ui.soundList.querySelectorAll('button')) {
    // An empty slot has nothing to play.
    button.disabled = !live || !soundSlots[Number(button.dataset.id)];
  }
}

/** Draw the sound slots from the last read. */
function renderSounds() {
  const rows = soundSlots.map((sound, id) => {
    const row = document.createElement('li');
    row.className = `sound${sound ? '' : ' sound--empty'}`;

    const label = document.createElement('span');
    label.className = 'sound__id';
    label.textContent = String(id);

    const name = document.createElement('span');
    name.className = 'sound__name';
    name.textContent = sound ? `Slot ${id}` : `Slot ${id}, empty`;

    const meta = document.createElement('span');
    meta.className = 'sound__meta';
    meta.textContent = sound ? describeSound(sound) : 'Nothing stored here yet.';

    const play = document.createElement('button');
    play.type = 'button';
    play.className = 'btn btn--ghost';
    play.dataset.id = String(id);
    play.textContent = 'Play';

    row.append(label, name, meta, play);
    return row;
  });

  ui.soundList.replaceChildren(...rows);
  const stored = soundSlots.filter(Boolean).length;
  ui.soundSummary.textContent = !clock.connected ? 'Not read yet.' : `${stored} of ${MAX_SOUNDS} slots hold a sound.`;
  updateSoundControls();
}

/** Ask each slot what it holds. */
async function readSounds() {
  try {
    const slots = [];
    for (let id = 0; id < MAX_SOUNDS; id++) {
      slots.push(await clock.soundInfo(id));
    }
    soundSlots = slots;
    renderSounds();
    log(`read ${slots.filter(Boolean).length} stored sound(s)`, 'ok');
  } catch (err) {
    log(`could not read the sounds: ${err.message}`, 'err');
  }
}

/** Handle a Play button in the sound list. */
async function playSound(id) {
  await withBusy('Playing...', async () => {
    try {
      const fade = Number(ui.soundFade.value);
      await clock.playSound(id, fade);
      log(`playing sound ${id}${fade ? `, ${fade} s fade-in` : ''}`, 'ok');
    } catch (err) {
      log(`could not play sound ${id}: ${err.message}`, 'err');
    }
  });
}

/** Handle the stop button. */
async function stopSound() {
  await withBusy('Stopping...', async () => {
    try {
      await clock.stopSound();
      log('playback stopped', 'ok');
    } catch (err) {
      log(`could not stop playback: ${err.message}`, 'err');
    }
  });
}

/** Handle the button song save. */
async function saveButtonSound() {
  const id = Number(ui.buttonSound.value);

  await withBusy('Saving...', async () => {
    try {
      await clock.setButtonSound(id);
      buttonSound = id;
      log(`button song saved, long press plays sound ${id}`, 'ok');
    } catch (err) {
      ui.buttonSound.value = String(buttonSound);
      log(`could not save the button song: ${err.message}`, 'err');
    }
  });
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
    await readAll();
    await readSounds();
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
  ui.refreshAlarms.addEventListener('click', () => withBusy('Reading...', readAll),);
  ui.alarmForm.addEventListener('submit', submitAlarm);
  ui.alarmMode.addEventListener('change', syncAlarmMode);
  // Delegated: rows come and go with every read.
  ui.alarmList.addEventListener('click', (event) => {
    const button = event.target.closest('button[data-index]');
    if (button) {
      deleteAlarm(Number(button.dataset.index));
    }
  });
  syncAlarmMode();
  ui.refreshLights.addEventListener('click', () => withBusy('Reading...', readAll),);
  ui.lightForm.addEventListener('submit', submitLight);
  ui.lightEffect.addEventListener('change', syncLightEffect);
  ui.lampSave.addEventListener('click', saveLamp);
  ui.ledSave.addEventListener('click', saveLedCount);
  ui.lightReset.addEventListener('click', () => fillLightForm(lightRecords.length, BLANK_LIGHT),);
  ui.lightList.addEventListener('click', (event) => {
    const button = event.target.closest('button[data-id]');
    if (button) {
      const id = Number(button.dataset.id);
      fillLightForm(id, decodeLight(lightRecords[id]));
      ui.lightEditor.open = true;
      ui.lightEditor.scrollIntoView({block: 'nearest'});
    }
  });
  syncLightEffect();
  ui.refreshSounds.addEventListener('click', () => withBusy('Reading...', readSounds),);
  ui.soundStop.addEventListener('click', stopSound);
  ui.buttonSoundSave.addEventListener('click', saveButtonSound);
  ui.soundList.addEventListener('click', (event) => {
    const button = event.target.closest('button[data-id]');
    if (button) {
      playSound(Number(button.dataset.id));
    }
  });

  for (const tab of ui.tabs) {
    tab.addEventListener('click', () => showTab(tab.id.replace('tab-', '')));
    tab.addEventListener('keydown', onTabKey);
  }
  let startTab = 'clock';
  try {
    startTab = localStorage.getItem(TAB_KEY) ?? 'clock';
  } catch {
    // Storage unavailable; start where we always did.
  }
  showTab(el(`tab-${startTab}`) ? startTab : 'clock');

  // The host clock is redrawn faster than it ticks so the seconds stay honest.
  setInterval(render, 250);
  setInterval(poll, POLL_MS);

  refresh();
  render();
  renderAlarms();
  renderLights();
  renderSounds();

  tryPreviousPort();
}

main();
