# Changelog

---

<details markdown="1">
  <summary>Table of Contents</summary>

<!-- TOC -->
* [Changelog](#changelog)
  * [v0.1.0-alpha (2026-07-25)](#v010-alpha--2026-07-25-)
  * [v0.1.0 (TBD)](#v010--tbd-)
<!-- TOC -->

</details>

---

## [v0.1.0-alpha (2026-07-25)](https://github.com/borkdlabs/hoppy_clock/releases/tag/v0.1.0-alpha)

- Pre-release 4-layer board variant (hardware focused release).
    - Short-term pre-release board bring-up/testing release.
- Order date: 2026/07/21.
    - Note: Minor DNP status changes included (changes made after order
      placement and before official release). No Gerber files affected.

Manual corrections:

1. The silkscreen labelling the pins on the `WS2812B breakout` connector is
   incorrect (**_5 V and ground are flipped!_**). The true pinout is: 1x3 JST
   PH, Pin 1: ground, Pin 2: DOUT, Pin 3: 5 V.
    - The silkscreen represents the intended design (matching most WS2812B LED
      strip pinouts), but the connector pinout was ordered incorrectly. To be
      corrected in the following release.
2. The backup supply via the `Backup supply` connector should not be used.
    - The TPS2116DRL is missing something to help pull VIN1 low below the 1 V
      threshold in time of a supply switchover, leading to power issues when
      connecting to `Backup supply`.

---

## [v0.1.0 (TBD)](https://github.com/borkdlabs/hoppy_clock/releases/tag/v0.1.0)

- 4-layer board variant.
    - Moderate confidence near production release.
- **Modifications:**
    - Fix `WS2812B breakout` connector pinout to match silkscreen and expected
      WS2812B LED strip pinouts.
        - Update `README.md` docs accordingly.
    - Minor silkscreen cleanup.
    - Fix power switching, add voltage divider on TPS2116DRL for improved supply
      mux threshold.
        - Previously used default 1 V threshold.
    - Add copper clearance on top layer below USB-C connector.
    - Add 1 mm fillet on USB-C area neck.
    - Fix bad release link in `CHANGELOG.md` for v0.1.0-alpha.
    - Swap THT `User button` to match onboard `BOOT0 button` SMD part.
- Order date: **_TBD_**.
