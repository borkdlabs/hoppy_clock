# Development helpers

Not part of the deployed site, the Pages workflow strips this directory before
publishing.

## Serve the app locally

Web Serial needs a secure context, and `http://localhost` counts as one, so no
HTTPS setup is required:

```bash
cd webapp
python -m http.server 8000
```

Then open <http://localhost:8000> in Chrome or Edge.

## `test-device.mjs`

Exercises the framing, transaction and manifest layers against a fake CDC port,
with no hardware attached. Covers the happy path, error statuses, command
timeouts, mid-command unplug, the packed alarm, light and sound records, and
editing alarms, light looks, the lamp ids, the LED count and the button song
without disturbing the rest of the manifest. Sound upload is covered too: PCM
encoding, a CRC-32 check against a known vector, the chunked stream arriving
byte for byte, and a cancel part-way leaving nothing committed.

```bash
cd webapp/dev
node test-device.mjs
```

## `inject-fake-port.js`

Paste into the browser console (or evaluate through DevTools) while the app is
open. It replaces `navigator.serial.requestPort` with a stub clock whose RTC
runs 47 s fast, whose manifest holds two alarms and two light looks, and whose
first sound slot holds a 10 s blob, so every tab can be driven without a board.
It accepts uploads as well: playback commands land on `window.__lastPlayback`
rather than making a noise, and a committed upload lands on
`window.__lastUpload`. The stub's manifest is left on
`window.__fakeConfig`, which is the quickest way to see what a save actually
wrote.
