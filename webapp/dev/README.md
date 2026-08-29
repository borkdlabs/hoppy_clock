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

Exercises the framing and transaction layers against a fake CDC port, with no
hardware attached. Covers the happy path, error statuses, command timeouts and
mid-command unplug.

```bash
cd webapp/dev
node test-device.mjs
```

## `inject-fake-port.js`

Paste into the browser console (or evaluate through DevTools) while the app is
open. It replaces `navigator.serial.requestPort` with a stub clock whose RTC
runs 47 s fast, so the connect flow, the drift readout and the sync button can
all be driven without a board.
