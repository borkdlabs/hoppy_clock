// Offline check of the device layer against a fake CDC port. Node only.
// Node ships a read-only `navigator`; graft a fake `serial` onto it.
Object.defineProperty(globalThis.navigator, 'serial', {
  value: new EventTarget(),
  configurable: true,
});

const {HoppyClock} = await import('../js/device.js');
const {buildFrame, crc8, CMD, SOF} = await import('../js/protocol.js');

// A stand-in for the clock: parses requests and answers like the firmware,
// deliberately dribbling responses out in small chunks the way USB can.
function fakePort({dropCommand = null, splitEvery = 3} = {}) {
  let enqueue;
  const rtc = {yy: 20, mo: 1, dd: 1, wd: 3, hh: 0, mm: 0, ss: 0};

  const respond = (cmd, payload) => {
    const frame = buildFrame(cmd, payload);
    for (let i = 0; i < frame.length; i += splitEvery) {
      enqueue(frame.subarray(i, i + splitEvery));
    }
  };

  return {
    rtc,
    readable: new ReadableStream({
      start(controller) {
        enqueue = (chunk) => controller.enqueue(chunk);
      },
    }),
    writable: new WritableStream({
      write(frame) {
        const cmd = frame[1];
        const payload = frame.subarray(3, 3 + frame[2]);
        if (frame[0] !== SOF) throw new Error('bad SOF');
        if (frame[frame.length - 1] !== crc8(frame.subarray(1, -1))) {
          throw new Error('bad CRC from host');
        }
        if (cmd === dropCommand) return; // Simulate a lost command.

        if (cmd === CMD.PING) respond(cmd, [0]);
        else if (cmd === CMD.SET_TIME) {
          [rtc.yy, rtc.mo, rtc.dd, rtc.wd, rtc.hh, rtc.mm, rtc.ss] = payload;
          respond(cmd, [0]);
        } else if (cmd === CMD.GET_TIME) {
          respond(cmd, [0, ...Object.values(rtc)]);
        } else respond(cmd, [1]);
      },
    }),
    open: async () => {
    },
    close: async () => {
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
  check(
    'ISO weekday sent (Sun=7, not 0)',
    port.rtc.wd >= 1 && port.rtc.wd <= 7,
    `wd=${port.rtc.wd}`,
  );
  check('two-digit year sent', port.rtc.yy === new Date().getFullYear() % 100);

  // Concurrent callers must not interleave frames on the wire.
  const results = await Promise.all([
    clock.getTime(),
    clock.getTime(),
    clock.getTime(),
  ]);
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
  check(
    'lost command times out',
    /timed out/.test(err?.message ?? '') && Date.now() - started >= 900,
  );
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

console.log(failures ? `\n${failures} failure(s)` : '\nall checks passed');
process.exit(failures ? 1 : 0);
