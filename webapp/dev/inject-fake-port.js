() => {
  // Stand in for the clock so the UI can be exercised without hardware.
  // The fake RTC starts 47 s fast so the drift readout has something to show.
  const rtc = {yy: 26, mo: 8, dd: 28, wd: 5, hh: 0, mm: 0, ss: 0};
  const seeded = new Date(Date.now() + 47000);
  rtc.hh = seeded.getHours();
  rtc.mm = seeded.getMinutes();
  rtc.ss = seeded.getSeconds();
  const startedAt = Date.now();
  const baseSec =
    seeded.getHours() * 3600 + seeded.getMinutes() * 60 + seeded.getSeconds();

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
          out = frame(cmd, [
            0,
            rtc.yy,
            rtc.mo,
            rtc.dd,
            rtc.wd,
            rtc.hh,
            rtc.mm,
            rtc.ss,
          ]);
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
  navigator.serial.requestPort = async () => port;
  return 'fake port installed';
}
