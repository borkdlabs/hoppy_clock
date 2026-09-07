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

- Pre-release 4-layer board variant (hardware-focused release).
    - Short-term pre-release board bring-up/testing release.
- Order date: 2026/07/21.
    - Note: Minor DNP status changes are included (changes made after order
      placement and before official release). No Gerber files are affected.

Manual corrections:

1. The silkscreen labeling the pins on the `WS2812B breakout` connector is
   incorrect (**_5 V and ground are flipped!_**). The true pinout is: 1x3 JST
   PH, Pin 1: ground, Pin 2: DOUT, Pin 3: 5 V.
    - The silkscreen represents the intended design (matching most WS2812B LED
      strip pinouts), but the connector pinout was ordered incorrectly. To be
      corrected in the following release.
2. The backup supply via the `Backup supply` connector should not be used unless
   jumpered.
    - The TPS2116DRL is missing something to help pull VIN1 low below the 1 V
      threshold in time of a supply switchover, leading to power issues when
      connecting to `Backup supply`. Manual fixes from quickest to best:
        1. Add a resistor light pull-down to VIN1 allowing a reliable drop of
           VIN1 to below 1 V.
        2. Add a divider circuit for a custom threshold:
            - Rewire PR1 as the divider center tap.
            - MODE to VIN1 directly (currently tied to VIN1 through PR1 pad)
            - Tested with a 76.8k / 22.1k divider (4.48 V threshold, less ideal
              due to very little upper tolerance).
3. _Optional_: The `WS2812B breakout` is currently wired to supply external
   off-board WS2812B LEDs using the TPS2116DRL post-mux supply. Cut the 3
   thermal relief traces on the 5 V net of the `WS2812B breakout` and jumper
   wire to the USB (VBUS) pour to prevent excessive backup supply drain.

---

## [v0.1.0 (TBD)](https://github.com/borkdlabs/hoppy_clock/releases/tag/v0.1.0)

- 4-layer board variant.
    - Moderate confidence near production release.
- **Modifications:**
    - Fix `WS2812B breakout` connector pinout to match silkscreen and expected
      WS2812B LED strip pinouts.
        - Update `README.md` docs accordingly.
    - Minor silkscreen cleanup.
    - Fix power switching, add voltage divider on TPS2116DRL for an improved
      supply mux threshold.
        - Previously used the default 1 V threshold.
        - VIN1 to 680k / 220k to ground voltage divider for 4.1 V threshold.
            - Pin PR1 disconnected from VIN1, center-tapped to voltage divider.
            - Pin MODE disconnected from PR1/VIN1, retraced directly to VIN1.
    - Add copper clearance on the top layer below the USB-C connector.
    - Add 1 mm fillet on USB-C area neck.
    - Fix the bad release link in `CHANGELOG.md` for v0.1.0-alpha.
    - Swap THT `User button` to match onboard `BOOT0 button` SMD part.
    - Move SMD right angle connectors `WS2812B breakout` and `Speaker` closer to
      edge cut for better wiring.
    - Split the `WS2812B breakout` to supply external off-board WS2812B LEDs via
      USB (VBUS) only to prevent excessive backup supply drain.
- Order date: **_TBD_**.
