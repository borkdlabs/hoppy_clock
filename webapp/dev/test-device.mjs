// Offline check of the device layer against a fake CDC port. Node only.
// Node ships a read-only `navigator`; graft a fake `serial` onto it.
Object.defineProperty(globalThis.navigator, 'serial', {
  value: new EventTarget(), configurable: true,
});

const {HoppyClock} = await import('../js/device.js');
const {buildFrame, crc8, CMD, SOF} = await import('../js/protocol.js');
const {decodeAlarm, encodeAlarm} = await import('../js/alarms.js');
const {decodeLight, encodeLight, lightColor} = await import('../js/lights.js');
const {
  crc32,
  describeSound,
  encodePcm,
  soundSeconds,
  synthesizeTone
} = await import('../js/sounds.js');

// A stand-in for the clock: parses requests and answers like the firmware,
// deliberately dribbling responses out in small chunks the way USB can.
function fakePort({
                    dropCommand = null,
                    splitEvery = 3,
                    config = null,
                    commitDelayMs = 0,
                  } = {}) {
  let enqueue;
  const rtc = {yy: 20, mo: 1, dd: 1, wd: 3, hh: 0, mm: 0, ss: 0};

  // What the manifest currently holds, and the staging area a BEGIN opens.
  const stored = config ?? {
    alarms: [], lights: [], lampOn: 0, lampOff: 0, ledCount: 1, buttonSound: 0,
  };
  let staged = null;

  // Sound slots: an entry, or null for an empty slot. `played` records what
  // the host last asked for, so tests can assert on the payload.
  const sounds = [{
    format: 1, rate: 16000, length: 320000, crc: 0xdeadbeef
  }, null];
  const played = {id: null, fade: null, stopped: false};

  // What an in-flight SND_BEGIN/DATA/END stream has produced so far.
  const upload = {
    begin: null, chunks: [], bytes: [], crc: null, committed: false
  };

  const respond = (cmd, payload) => {
    const frame = buildFrame(cmd, payload);
    for (let i = 0; i < frame.length; i += splitEvery) {
      enqueue(frame.subarray(i, i + splitEvery));
    }
  };

  return {
    rtc, stored, sounds, played, upload, readable: new ReadableStream({
      start(controller) {
        enqueue = (chunk) => controller.enqueue(chunk);
      },
    }), writable: new WritableStream({
      write(frame) {
        const cmd = frame[1];
        const payload = frame.subarray(3, 3 + frame[2]);
        if (frame[0] !== SOF) throw new Error('bad SOF');
        if (frame[frame.length - 1] !== crc8(frame.subarray(1, -1))) {
          throw new Error('bad CRC from host');
        }
        if (cmd === dropCommand) return; // Simulate a lost command.

        if (cmd === CMD.PING) respond(cmd, [0]); else if (cmd === CMD.SET_TIME) {
          [rtc.yy, rtc.mo, rtc.dd, rtc.wd, rtc.hh, rtc.mm, rtc.ss] = payload;
          respond(cmd, [0]);
        } else if (cmd === CMD.GET_TIME) {
          respond(cmd, [0, ...Object.values(rtc)]);
        } else if (cmd === CMD.SND_BEGIN) {
          upload.begin = [...payload];
          upload.chunks = [];
          upload.bytes = [];
          upload.committed = false;
          respond(cmd, [payload.length === 8 ? 0 : 1]);
        } else if (cmd === CMD.SND_DATA) {
          upload.chunks.push(payload.length);
          upload.bytes.push(...payload);
          respond(cmd, [0]);
        } else if (cmd === CMD.SND_END) {
          upload.crc = (payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24)) >>> 0;
          // Commit only if the CRC matches what actually arrived, the way the
          // firmware verifies before publishing the entry.
          upload.committed = upload.crc === crc32(Uint8Array.from(upload.bytes));
          respond(cmd, [upload.committed ? 0 : 1]);
        } else if (cmd === CMD.SND_INFO) {
          const entry = sounds[payload[0]];
          respond(cmd, entry ? [0, entry.format, entry.rate & 0xff, entry.rate >> 8, entry.length & 0xff, (entry.length >> 8) & 0xff, (entry.length >> 16) & 0xff, (entry.length >>> 24) & 0xff, entry.crc & 0xff, (entry.crc >> 8) & 0xff, (entry.crc >> 16) & 0xff, (entry.crc >>> 24) & 0xff,] : [1],);
        } else if (cmd === CMD.SND_PLAY) {
          played.id = payload[0];
          played.fade = payload.length > 1 ? payload[1] : null;
          respond(cmd, [0]);
        } else if (cmd === CMD.SND_STOP) {
          played.stopped = true;
          respond(cmd, [0]);
        } else if (cmd === CMD.CFG_GET_COUNT) {
          respond(cmd, [0, stored.alarms.length, stored.lights.length, stored.lampOn, stored.lampOff, stored.ledCount, stored.buttonSound,]);
        } else if (cmd === CMD.CFG_GET_ALARM || cmd === CMD.CFG_GET_LIGHT) {
          const table = cmd === CMD.CFG_GET_ALARM ? stored.alarms : stored.lights;
          const record = table[payload[0]];
          respond(cmd, record ? [0, ...record] : [1]);
        } else if (cmd === CMD.CFG_BEGIN) {
          staged = {...stored, alarms: [], lights: []};
          respond(cmd, [0]);
        } else if (!staged && cmd >= CMD.CFG_SET_ALARM && cmd <= CMD.CFG_SET_BTN) {
          respond(cmd, [1]); // Staging edit outside a BEGIN/COMMIT pair.
        } else if (cmd === CMD.CFG_SET_ALARM) {
          staged.alarms[payload[0]] = [...payload.subarray(1)];
          respond(cmd, [0]);
        } else if (cmd === CMD.CFG_SET_LIGHT) {
          staged.lights[payload[0]] = [...payload.subarray(1)];
          respond(cmd, [0]);
        } else if (cmd === CMD.CFG_SET_LAMP) {
          [staged.lampOn, staged.lampOff] = payload;
          respond(cmd, [0]);
        } else if (cmd === CMD.CFG_SET_LEDS) {
          staged.ledCount = payload[0];
          respond(cmd, [0]);
        } else if (cmd === CMD.CFG_SET_BTN) {
          staged.buttonSound = payload[0];
          respond(cmd, [0]);
        } else if (cmd === CMD.CFG_COMMIT) {
          // The commit's counts are what actually lands, so honour them.
          Object.assign(stored, staged, {
            alarms: staged.alarms.slice(0, payload[0]),
            lights: staged.lights.slice(0, payload[1]),
          });
          staged = null;
          // Flash erase takes real time; let a test stretch it out.
          setTimeout(() => respond(cmd, [0]), commitDelayMs);
        } else respond(cmd, [1]);
      },
    }), open: async () => {
    }, close: async () => {
    },
  };
}

let failures = 0;
const check = (name, pass, extra = '') => {
  console.log(`${pass ? 'ok  ' : 'FAIL'} ${name}${extra ? ` -- ${extra}` : ''}`);
  if (!pass) failures++;
};

// Happy path: ping, sync, read back.
{
  const port = fakePort();
  const clock = new HoppyClock();
  await clock.connect(port);
  check('connected', clock.connected);

  await clock.ping();
  check('ping resolves', true);

  const before = new Date();
  const readback = await clock.syncTime();
  const drift = Math.abs(readback - before) / 1000;
  check('syncTime round-trips', drift < 3, `drift ${drift.toFixed(1)}s`);
  check('ISO weekday sent (Sun=7, not 0)', port.rtc.wd >= 1 && port.rtc.wd <= 7, `wd=${port.rtc.wd}`,);
  check('two-digit year sent', port.rtc.yy === new Date().getFullYear() % 100);

  // Concurrent callers must not interleave frames on the wire.
  const results = await Promise.all([clock.getTime(), clock.getTime(), clock.getTime(),]);
  check('concurrent txns all resolve', results.every((d) => d instanceof Date));

  await clock.disconnect();
  check('disconnected cleanly', !clock.connected);
}

// A device error status must reject rather than resolve.
{
  const clock = new HoppyClock();
  await clock.connect(fakePort());
  const err = await clock.command(CMD.WIPE, [0]).catch((e) => e);
  check('non-OK status rejects', err?.name === 'DeviceError', err?.message);
  await clock.disconnect();
}

// A dropped command must time out and leave the link usable.
{
  const clock = new HoppyClock();
  await clock.connect(fakePort({dropCommand: CMD.PING}));
  const started = Date.now();
  const err = await clock.ping().catch((e) => e);
  check('lost command times out', /timed out/.test(err?.message ?? '') && Date.now() - started >= 900,);
  check('link still usable after a timeout', (await clock.getTime()) instanceof Date);
  await clock.disconnect();
}

// Unplugging mid-command must reject it, not hang forever.
{
  const port = fakePort({dropCommand: CMD.PING});
  const clock = new HoppyClock();
  await clock.connect(port);
  let closed = false;
  clock.addEventListener('close', () => (closed = true));

  const pending = clock.ping().catch((e) => e);
  // Browsers set event.target to the SerialPort; shadow the read-only getter.
  const unplug = new Event('disconnect');
  Object.defineProperty(unplug, 'target', {value: port, configurable: true});
  navigator.serial.dispatchEvent(unplug);
  const err = await pending;
  check('unplug rejects the in-flight command', err?.name === 'DeviceError');
  check('close event fired', closed);
  check('reports disconnected', !clock.connected);
}

// --- Alarms ----------------------------------------------------------------

const WEEKDAYS = 0b0011111;

/** An alarm spec with every field set to something distinguishable. */
const sampleAlarm = (over = {}) => ({
  enabled: true,
  monthly: false,
  daySel: WEEKDAYS,
  hour: 7,
  minute: 5,
  second: 30,
  timeoutS: 300,
  soundId: 2,
  lightId: 3,
  fadeS: 20, ...over,
});

// The packed record has to survive a round trip byte for byte.
{
  const alarm = sampleAlarm();
  const record = encodeAlarm(alarm);
  check('alarm record is 12 B', record.length === 12);
  check('alarm record round-trips', JSON.stringify(decodeAlarm(record)) === JSON.stringify(alarm),);
  // 300 s = 0x012C, and the firmware reads it little-endian.
  check('timeout_s is little-endian', record[5] === 0x2c && record[6] === 0x01);
  check('enabled + weekly flags', record[0] === 0b01);
  check('monthly sets its flag', encodeAlarm(sampleAlarm({
    monthly: true, daySel: 14
  }))[0] === 0b11,);

  const bad = (over) => {
    try {
      encodeAlarm(sampleAlarm(over));
      return false;
    } catch (e) {
      return e instanceof RangeError;
    }
  };
  check('rejects hour 24', bad({hour: 24}));
  check('rejects a weekly alarm with no days', bad({daySel: 0}));
  check('rejects day-of-month 0', bad({monthly: true, daySel: 0}));
}

// Reading the manifest back off the device.
{
  const port = fakePort({
    config: {
      alarms: [[...encodeAlarm(sampleAlarm())], [...encodeAlarm(sampleAlarm({hour: 9}))],],
      lights: [[...new Uint8Array(12).fill(7)]],
      lampOn: 1,
      lampOff: 2,
      ledCount: 8,
      buttonSound: 3,
    },
  });
  const clock = new HoppyClock();
  await clock.connect(port);

  const config = await clock.readConfig();
  check('readConfig returns every alarm', config.alarms.length === 2);
  check('readConfig returns every light', config.lights.length === 1);
  check('readConfig carries the odds and ends', config.lampOn === 1 && config.lampOff === 2 && config.ledCount === 8 && config.buttonSound === 3,);
  check('alarm records survive the read', decodeAlarm(config.alarms[1]).hour === 9,);
  await clock.disconnect();
}

// Adding must preserve everything the page does not edit: the manifest is
// written whole, so a missed field would be silently wiped.
{
  const port = fakePort({
    config: {
      alarms: [[...encodeAlarm(sampleAlarm())]],
      lights: [[...new Uint8Array(12).fill(7)]],
      lampOn: 1,
      lampOff: 2,
      ledCount: 8,
      buttonSound: 3,
    },
  });
  const clock = new HoppyClock();
  await clock.connect(port);

  const after = await clock.addAlarm(encodeAlarm(sampleAlarm({hour: 6})));
  check('addAlarm appends', after.length === 2);
  check('the new alarm is stored', port.stored.alarms.length === 2);
  check('lights survive an alarm write', port.stored.lights.length === 1 && port.stored.lights[0][0] === 7,);
  check('lamp, LEDs and button survive an alarm write', port.stored.lampOn === 1 && port.stored.lampOff === 2 && port.stored.ledCount === 8 && port.stored.buttonSound === 3,);

  const left = await clock.removeAlarm(0);
  check('removeAlarm drops that index', left.length === 1);
  check('the right one was kept', decodeAlarm(left[0]).hour === 6);
  check('the device agrees', port.stored.alarms.length === 1);

  const err = await clock.removeAlarm(7).catch((e) => e);
  check('removeAlarm rejects a bad index', err?.name === 'DeviceError');
  await clock.disconnect();
}

// CFG_COMMIT erases a flash sector before answering, so it needs a longer
// budget than an ordinary command's 1 s.
{
  const port = fakePort({commitDelayMs: 1500});
  const clock = new HoppyClock();
  await clock.connect(port);
  const result = await clock
    .addAlarm(encodeAlarm(sampleAlarm()))
    .catch((e) => e);
  check('a slow commit is not timed out', Array.isArray(result));
  await clock.disconnect();
}

// --- Lights ----------------------------------------------------------------

/** A look with every field set to something distinguishable. */
const sampleLight = (over = {}) => ({
  effect: 'sweep',
  r: 255,
  g: 200,
  b: 120,
  brightness: 160,
  periodMs: 1500,
  curve: 'ease',
  spread: 40, ...over,
});

// The packed look has to survive a round trip byte for byte.
{
  const light = sampleLight();
  const record = encodeLight(light);
  check('light record is 12 B', record.length === 12);
  check('light record round-trips', JSON.stringify(decodeLight(record)) === JSON.stringify(light),);
  check('effect is an index', record[0] === 2); // LIGHT_FX_SWEEP.
  check('curve is an index', record[7] === 1); // LIGHT_CURVE_EASE.
  // 1500 ms = 0x05DC, little-endian like the alarm's timeout.
  check('period_ms is little-endian', record[5] === 0xdc && record[6] === 0x05);
  check('the swatch scales by brightness', lightColor(sampleLight({
    effect: 'solid', brightness: 0
  })) === 'rgb(0, 0, 0)',);

  let threw = false;
  try {
    encodeLight(sampleLight({effect: 'strobe'}));
  } catch (e) {
    threw = e instanceof RangeError;
  }
  check('rejects an unknown effect', threw);
}

// Saving a look must leave the alarms alone, and grow the table when the id
// runs past the end.
{
  const port = fakePort({
    config: {
      alarms: [[...encodeAlarm(sampleAlarm())]],
      lights: [[...encodeLight(sampleLight())]],
      lampOn: 0,
      lampOff: 0,
      ledCount: 8,
      buttonSound: 1,
    },
  });
  const clock = new HoppyClock();
  await clock.connect(port);

  const lights = await clock.saveLight(1, encodeLight(sampleLight({r: 1})));
  check('saveLight appends at the next id', lights.length === 2);
  check('the look is stored', decodeLight(lights[1]).r === 1);
  check('alarms survive a light write', port.stored.alarms.length === 1 && decodeAlarm(Uint8Array.from(port.stored.alarms[0])).hour === 7,);

  // Id 4 with only two looks stored: ids are positions, so the gap fills with
  // blanks rather than the look landing at the wrong index.
  const grown = await clock.saveLight(4, encodeLight(sampleLight({b: 9})));
  check('saveLight grows the table with blanks', grown.length === 5);
  check('the gap is blank', decodeLight(grown[3]).brightness === 0);
  check('the look lands on its id', decodeLight(grown[4]).b === 9);

  const err = await clock.saveLight(16, encodeLight(sampleLight())).catch((e) => e);
  check('saveLight rejects an out-of-range id', err?.name === 'DeviceError');
  await clock.disconnect();
}

// The lamp ids are what a button press plays.
{
  const port = fakePort({
    config: {
      alarms: [[...encodeAlarm(sampleAlarm())]],
      lights: [[...encodeLight(sampleLight())], [...encodeLight(sampleLight())]],
      lampOn: 0,
      lampOff: 0,
      ledCount: 8,
      buttonSound: 1,
    },
  });
  const clock = new HoppyClock();
  await clock.connect(port);

  await clock.setLamp(1, 0);
  check('setLamp stores both ids', port.stored.lampOn === 1 && port.stored.lampOff === 0,);
  check('alarms and lights survive a lamp write', port.stored.alarms.length === 1 && port.stored.lights.length === 2,);
  check('the button sound is left alone', port.stored.buttonSound === 1 && port.stored.ledCount === 8,);

  const err = await clock.setLamp(0, 99).catch((e) => e);
  check('setLamp rejects an out-of-range id', err?.name === 'DeviceError');
  await clock.disconnect();
}

// --- LED chain -------------------------------------------------------------

{
  const port = fakePort({
    config: {
      alarms: [[...encodeAlarm(sampleAlarm())]],
      lights: [[...encodeLight(sampleLight())]],
      lampOn: 0,
      lampOff: 0,
      ledCount: 1,
      buttonSound: 3,
    },
  });
  const clock = new HoppyClock();
  await clock.connect(port);

  await clock.setLedCount(30);
  check('setLedCount stores the count', port.stored.ledCount === 30);
  check('the rest of the manifest survives', port.stored.alarms.length === 1 && port.stored.lights.length === 1 && port.stored.buttonSound === 3,);

  const zero = await clock.setLedCount(0).catch((e) => e);
  check('setLedCount rejects 0', zero?.name === 'DeviceError');
  const over = await clock.setLedCount(65).catch((e) => e);
  check('setLedCount rejects past LED_COUNT_MAX', over?.name === 'DeviceError');
  check('a rejected count is not written', port.stored.ledCount === 30);
  await clock.disconnect();
}

// --- Sounds ----------------------------------------------------------------

{
  const port = fakePort({
    config: {
      alarms: [],
      lights: [],
      lampOn: 0,
      lampOff: 0,
      ledCount: 1,
      buttonSound: 0,
    },
  });
  const clock = new HoppyClock();
  await clock.connect(port);

  const filled = await clock.soundInfo(0);
  check('soundInfo decodes a stored slot', filled?.format === 's16');
  check('rate and length are little-endian', filled.rateHz === 16000 && filled.lengthBytes === 320000,);
  // 320000 B of 16-bit samples at 16 kHz is 10 s.
  check('duration is derived', Math.abs(soundSeconds(filled) - 10) < 0.01);
  check('describeSound reads sensibly', describeSound(filled).includes('16000 Hz'));

  const empty = await clock.soundInfo(1);
  check('an empty slot is null, not an error', empty === null);

  const bad = await clock.soundInfo(2).catch((e) => e);
  check('soundInfo rejects an id past the slots', bad?.name === 'DeviceError');

  await clock.playSound(0);
  check('play with no fade sends one byte', port.played.id === 0 && port.played.fade === null,);
  await clock.playSound(0, 15);
  check('play with a fade sends both', port.played.fade === 15);
  await clock.stopSound();
  check('stop reaches the device', port.played.stopped);

  await clock.setButtonSound(7);
  check('setButtonSound stores the id', port.stored.buttonSound === 7);
  const over = await clock.setButtonSound(256).catch((e) => e);
  check('setButtonSound rejects 256', over?.name === 'DeviceError');
  await clock.disconnect();
}

// --- Sound upload ----------------------------------------------------------

{
  const port = fakePort();
  const clock = new HoppyClock();
  await clock.connect(port);

  // A 100 ms tone is 1600 samples, so 3200 B of s16: 50 full 64 B chunks.
  const samples = synthesizeTone(440, 0.1, 16000);
  const data = encodePcm(samples, 's16', 1);
  check('encodePcm sizes s16 at two bytes a sample', data.length === 3200);
  check('u8 is half the size and centred on 128', encodePcm(new Float32Array(4), 'u8', 1).every((b) => b === 128),);

  const seen = [];
  await clock.uploadSound(0, {
    format: 's16',
    rateHz: 16000,
    data
  }, {onProgress: (sent, total) => seen.push([sent, total])},);

  check('the blob arrived whole', port.upload.bytes.length === data.length);
  check('byte for byte', Uint8Array.from(port.upload.bytes).every((b, i) => b === data[i]),);
  check('chunks stay within the frame limit', Math.max(...port.upload.chunks) === 64);
  check('the device verified the CRC', port.upload.committed);
  check('BEGIN carries id, format, rate and length', port.upload.begin[0] === 0 && port.upload.begin[1] === 1 && port.upload.begin[2] === 0x80 && port.upload.begin[3] === 0x3e && port.upload.begin[4] === 0x80 && port.upload.begin[5] === 0x0c,);
  check('progress ends at the total', seen.at(-1)[0] === data.length);

  // A known vector, so a broken CRC cannot pass unnoticed.
  check('crc32 matches zlib', crc32(new TextEncoder().encode('123456789')) === 0xcbf43926,);

  const short = await clock
    .uploadSound(0, {format: 's16', rateHz: 16000, data: new Uint8Array(0)})
    .catch((e) => e);
  check('an empty blob is refused', short?.name === 'DeviceError');
  const badFormat = await clock
    .uploadSound(0, {format: 'flac', rateHz: 16000, data})
    .catch((e) => e);
  check('an unknown format is refused', badFormat?.name === 'DeviceError');

  // Cancelling part-way must stop sending and leave nothing committed.
  const controller = new AbortController();
  const cancelled = await clock
    .uploadSound(1, {format: 's16', rateHz: 16000, data}, {
      signal: controller.signal, onProgress: (sent) => {
        if (sent > 0) controller.abort();
      },
    },)
    .catch((e) => e);
  check('cancelling rejects', cancelled?.name === 'DeviceError');
  check('and nothing was committed', !port.upload.committed);
  check('and it stopped early', port.upload.bytes.length < data.length, `${port.upload.bytes.length} of ${data.length} B`,);
  await clock.disconnect();
}

console.log(failures ? `\n${failures} failure(s)` : '\nall checks passed');
process.exit(failures ? 1 : 0);
