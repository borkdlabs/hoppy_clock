() => {
  // Stand in for the clock so the UI can be exercised without hardware.
  // The fake RTC starts 47 s fast so the drift readout has something to show.
  const rtc = {yy: 26, mo: 8, dd: 28, wd: 5, hh: 0, mm: 0, ss: 0};
  const seeded = new Date(Date.now() + 47000);
  rtc.hh = seeded.getHours();
  rtc.mm = seeded.getMinutes();
  rtc.ss = seeded.getSeconds();
  const startedAt = Date.now();
  const baseSec = seeded.getHours() * 3600 + seeded.getMinutes() * 60 + seeded.getSeconds();

  const crc8 = (data) => {
    let crc = 0;
    for (const b of data) {
      crc ^= b;
      for (let i = 0; i < 8; i++) {
        crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 0xff : (crc << 1) & 0xff;
      }
    }
    return crc;
  };
  const frame = (cmd, payload) => {
    const body = [cmd, payload.length, ...payload];
    return Uint8Array.from([0xa5, ...body, crc8(body)]);
  };

  let enqueue;
  let synced = false;

  // A manifest to edit, in the packed layout the firmware stores (manifest.h).
  // Two alarms and two light looks, so the alarm, light and lamp cards all
  // have something to show. Both 16-bit fields are little-endian.
  //
  //   alarm: flags, day_sel, h, m, s, timeout(2), sound, light, fade, rsv(2)
  //   light: effect, r, g, b, brightness, period(2), curve, spread, rsv(3)
  const cfg = {
    alarms: [[0x01, 0b0011111, 7, 30, 0, 60, 0, 1, 2, 20, 0, 0], [0x01, 0b1100000, 9, 0, 0, 0x2c, 0x01, 0, 0, 0, 0, 0],],
    lights: [[0, 255, 200, 120, 180, 0xe8, 0x03, 1, 0, 0, 0, 0], [0, 255, 140, 0, 20, 0xd0, 0x07, 0, 0, 0, 0, 0],],
    lampOn: 0,
    lampOff: 1,
    ledCount: 8,
    buttonSound: 1,
  };
  let staged = null;

  // Sound slot 0 holds a 10 s stereo-ish blob; slot 1 is empty.
  const sounds = [{
    format: 1, rate: 16000, length: 320000, crc: 0x1234abcd
  }, null];
  let incoming = null;

  const tick = () => {
    // Free-run the fake RTC off its own offset until the host sets it.
    const elapsed = Math.floor((Date.now() - startedAt) / 1000);
    const total = (baseSec + elapsed) % 86400;
    if (!synced) {
      rtc.hh = Math.floor(total / 3600);
      rtc.mm = Math.floor((total % 3600) / 60);
      rtc.ss = total % 60;
    } else {
      const now = new Date();
      rtc.hh = now.getHours();
      rtc.mm = now.getMinutes();
      rtc.ss = now.getSeconds();
    }
  };

  const port = {
    getInfo: () => ({usbVendorId: 0x0483, usbProductId: 0x5740}),
    open: async () => {
    },
    close: async () => {
    },
    readable: new ReadableStream({
      start(controller) {
        enqueue = (chunk) => controller.enqueue(chunk);
      },
    }),
    writable: new WritableStream({
      write(bytes) {
        const cmd = bytes[1];
        const payload = bytes.subarray(3, 3 + bytes[2]);
        let out;
        if (cmd === 0x01) {
          out = frame(cmd, [0]);
        } else if (cmd === 0x10) {
          [rtc.yy, rtc.mo, rtc.dd, rtc.wd, rtc.hh, rtc.mm, rtc.ss] = payload;
          synced = true;
          window.__lastSetTime = [...payload];
          out = frame(cmd, [0]);
        } else if (cmd === 0x11) {
          tick();
          out = frame(cmd, [0, rtc.yy, rtc.mo, rtc.dd, rtc.wd, rtc.hh, rtc.mm, rtc.ss,]);
        } else if (cmd === 0x40) {
          incoming = {
            id: payload[0],
            format: payload[1],
            rate: payload[2] | (payload[3] << 8),
            length: (payload[4] | (payload[5] << 8) | (payload[6] << 16) | (payload[7] << 24)) >>> 0,
            got: 0,
          };
          out = frame(cmd, [0]);
        } else if (cmd === 0x41) {
          if (incoming) incoming.got += payload.length;
          out = frame(cmd, [incoming ? 0 : 1]);
        } else if (cmd === 0x42) {
          // Publish the slot the way the firmware does on a good commit.
          if (incoming) {
            sounds[incoming.id] = {
              format: incoming.format,
              rate: incoming.rate,
              length: incoming.got,
              crc: (payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24)) >>> 0,
            };
          }
          window.__lastUpload = incoming;
          incoming = null;
          out = frame(cmd, [0]);
        } else if (cmd === 0x43) {
          const entry = sounds[payload[0]];
          out = frame(cmd, entry ? [0, entry.format, entry.rate & 0xff, entry.rate >> 8, entry.length & 0xff, (entry.length >> 8) & 0xff, (entry.length >> 16) & 0xff, (entry.length >>> 24) & 0xff, entry.crc & 0xff, (entry.crc >> 8) & 0xff, (entry.crc >> 16) & 0xff, (entry.crc >>> 24) & 0xff,] : [1],);
        } else if (cmd === 0x44 || cmd === 0x45) {
          window.__lastPlayback = {cmd, payload: [...payload]};
          out = frame(cmd, [0]);
        } else if (cmd === 0x33) {
          out = frame(cmd, [0, cfg.alarms.length, cfg.lights.length, cfg.lampOn, cfg.lampOff, cfg.ledCount, cfg.buttonSound,]);
        } else if (cmd === 0x34 || cmd === 0x37) {
          const table = cmd === 0x34 ? cfg.alarms : cfg.lights;
          const record = table[payload[0]];
          out = frame(cmd, record ? [0, ...record] : [1]);
        } else if (cmd === 0x30) {
          staged = {...cfg, alarms: [], lights: []};
          out = frame(cmd, [0]);
        } else if (!staged) {
          out = frame(cmd, [1]); // A staging edit outside BEGIN/COMMIT.
        } else if (cmd === 0x31) {
          staged.alarms[payload[0]] = [...payload.subarray(1)];
          out = frame(cmd, [0]);
        } else if (cmd === 0x35) {
          staged.lights[payload[0]] = [...payload.subarray(1)];
          out = frame(cmd, [0]);
        } else if (cmd === 0x36) {
          [staged.lampOn, staged.lampOff] = payload;
          out = frame(cmd, [0]);
        } else if (cmd === 0x38) {
          staged.ledCount = payload[0];
          out = frame(cmd, [0]);
        } else if (cmd === 0x39) {
          staged.buttonSound = payload[0];
          out = frame(cmd, [0]);
        } else if (cmd === 0x32) {
          Object.assign(cfg, staged, {
            alarms: staged.alarms.slice(0, payload[0]),
            lights: staged.lights.slice(0, payload[1]),
          });
          staged = null;
          out = frame(cmd, [0]);
        } else {
          out = frame(cmd, [1]);
        }
        // Dribble it out in small chunks, the way USB actually delivers.
        for (let i = 0; i < out.length; i += 3) {
          enqueue(out.subarray(i, i + 3));
        }
      },
    }),
  };

  window.__fakePort = port;
  window.__fakeConfig = cfg;
  navigator.serial.requestPort = async () => port;
  return 'fake port installed';
}
