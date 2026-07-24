# hoppy_clock

![arm_gcc_build](https://github.com/borkdlabs/hoppy_clock/actions/workflows/arm_gcc_build.yaml/badge.svg)
![kibot](https://github.com/borkdlabs/hoppy_clock/actions/workflows/kibot.yaml/badge.svg)

STM32L432KC alarm clock lamp with optional backup source.

---

<details markdown="1">
  <summary>Table of Contents</summary>

<!-- TOC -->
* [hoppy_clock](#hoppy_clock)
  * [1 Overview](#1-overview)
    * [1.1 Bill of Materials (BOM)](#11-bill-of-materials-bom)
    * [1.2 Block Diagram](#12-block-diagram)
    * [1.3 Pin Configurations](#13-pin-configurations)
    * [1.4 Clock Configurations](#14-clock-configurations)
  * [2 Board Specifications](#2-board-specifications)
    * [2.1 Connectors](#21-connectors)
    * [2.2 Switches & Jumpers](#22-switches--jumpers)
    * [2.3 LEDs](#23-leds)
    * [2.4 Test Pads](#24-test-pads)
    * [2.5 Power Supply](#25-power-supply)
    * [2.6 Speaker](#26-speaker)
  * [Third-Party Licenses](#third-party-licenses)
<!-- TOC -->

</details>

---

## 1 Overview

|                       Top                        |                         Bottom                         |
|:------------------------------------------------:|:------------------------------------------------------:|
| ![hoppy_clock-top.png](docs/hoppy_clock-top.png) | ![hoppy_clock-bottom.png](docs/hoppy_clock-bottom.png) |

### 1.1 Bill of Materials (BOM)

| Manufacturer Part Number | Manufacturer        | Description             | Quantity | Notes |
|--------------------------|---------------------|-------------------------|---------:|-------|
| STM32L432KC              | STMicroelectronics  | 32-bit MCU              |        1 |       |
| WS2812B                  | (Various)           | PWM Addressable RGB LED |        1 |       |
| PAM8302AAS               | Diodes Incorporated | Audio Amplifier         |        1 |       |
| W25Q128JVSIQ             | Winbond Electronics | 128 Mbit NOR Memory     |        1 |       |
| Generic Push Button      |                     |                         |        1 |       |

### 1.2 Block Diagram

![hoppy_clock.drawio.png](docs/hoppy_clock.drawio.png)

> Drawio file here: [hoppy_clock.drawio](docs/hoppy_clock.drawio).

### 1.3 Pin Configurations

<details markdown="1">
  <summary>CubeMX Pinout</summary>

![CubeMX Pinout.png](docs/CubeMX%20Pinout.png)

</details>

<details markdown="1">
  <summary>Pin & Peripherals Table</summary>

| STM32L432KC | Peripheral        | Config               | Connection                            | Notes                                                        |
|-------------|-------------------|----------------------|---------------------------------------|--------------------------------------------------------------|
| PA14        | `SYS_JTCK-SWCLK`  |                      | TC2050 SWD Pin 4: `SWCLK`             |                                                              |
| PA13        | `SYS_JTMS-SWDIO`  |                      | TC2050 SWD Pin 2: `SWDIO`             |                                                              |
| PB3         | `SYS_JTDO-SWO`    |                      | TC2050 SWD Pin 2: `SWO`               |                                                              |
|             | `TIM6`            | TRGO update event    |                                       | `DAC1_OUT1` TRGO.                                            |
|             | `ADC1` `VREFINT`  | Scan conversion mode | VDDA Sense                            | Configured in ADC1 rank 1.                                   |
|             | `ADC1_IN17`       | Scan conversion mode | Temperature Sensor Channel            | Configured in ADC1 rank 2.                                   |
| PA10        | `Reserved`        | 115200 bps           | GPIO Breakout: (ie Qwiic: `I2C1_SDA`) | Reserved GPIO breakout (PA10).                               |
| PA9         | `Reserved`        | 115200 bps           | GPIO Breakout: (ie Qwiic: `I2C1_SCL`) | Reserved GPIO breakout (PA9).                                |
| PA11        | `USB_DM`          | Device (FS)          | USB-C D-                              |                                                              |
| PA12        | `USB_DP`          | Device (FS)          | USB-C D+                              |                                                              |
| PA8         | `TIM1_CH1`        | PWM Generation CH1   | WS2812B-2020 Pin: `DIN`               | DIN pin number depends on IC variant.                        |
| PA4         | `DAC1_OUT1`       |                      | PAM8302AAS Input Circuit              |                                                              |
| PA1         | `GPIO_Output`     | Hardware pull-down   | PAM8302AAS Pin 1: `SD`                |                                                              |
| PA3         | `QUADSPI_CLK`     |                      | W25Q128JVSIQ Pin 6: `CLK`             |                                                              |
| PA2         | `QUADSPI_BK1_NCS` | Hardware pull-up     | W25Q128JVSIQ Pin 1: `CS`              |                                                              |
| PB1         | `QUADSPI_BK1_IO0` |                      | W25Q128JVSIQ Pin 5: `IO0`             |                                                              |
| PB0         | `QUADSPI_BK1_IO1` |                      | W25Q128JVSIQ Pin 2: `IO1`             |                                                              |
| PA7         | `QUADSPI_BK1_IO2` | Hardware pull-up     | W25Q128JVSIQ Pin 3: `IO2`             | Hardware pull-up for potential bringup from SPI single-line. |
| PA6         | `QUADSPI_BK1_IO3` | Hardware pull-up     | W25Q128JVSIQ Pin 7: `IO3`             | Hardware pull-up for potential bringup from SPI single-line. |

</details>

### 1.4 Clock Configurations

```
16 MHz High Speed Internal (HSI)
 -> Phase-Locked Loop Main (PLLM)
 -> 80 MHz SYSCLK
 -> 80 MHz HCLK
     -> 80 MHz APB1 (Maxed) -> 80 MHz APB1 Timer
     -> 80 MHz APB2 (Maxed) -> 80 MHz APB2 Timer
 -> 48 MHz PLLSAI1Q -> 48 MHz USB

32.768 kHz Low Speed External (LSE)
     -> 32.768 kHz RTC
```

## 2 Board Specifications

### 2.1 Connectors

Connectors fixed by hardware (PCB traces or the connector itself).

| Connector            | Ref | Description                                                     |
|----------------------|:---:|-----------------------------------------------------------------|
| `Tag-Connect TC2050` | J1  | SWD programming/debug connector                                 |
| `USB-C`              | J2  | USB-C 5 V power & data source                                   |
| `Backup supply`      | J3  | 1x2 JST XH (2.5 mm pitch), Pin 1: Backup 5 V, Pin 2: ground     |
| `Qwiic`              | J4  | 1x4 JST SH, Pin 1: ground, Pin 2: 3.3 V, Pin 3: SDA, Pin 4: SCL |
| `WS2812B breakout`   | J5  | 1x3 JST PH, Pin 1: ground, Pin 2: DOUT, Pin 3: 5 V              |
| `Speaker`            | J6  | 1x2 JST PH, Pin 1: OUT+, Pin 2: OUT-                            |

### 2.2 Switches & Jumpers

User controllable hardware and/or firmware driven inputs.

| Switch/Jumper  | Ref | Description                           |
|----------------|:---:|---------------------------------------|
| `BOOT0 button` | SW1 | Push to pull `BOOT0` high             |
| `User button`  | SW2 | Generic 6 mm TH button, push to reset |

### 2.3 LEDs

LEDs used to show board status and/or user controllable.

| LED           | Mark | Description         |
|---------------|------|---------------------|
| `WS2812B LED` | None | RGB addressable LED |

### 2.4 Test Pads

| Test Point   | Ref | Description                   |
|--------------|:---:|-------------------------------|
| `TPS2116 ST` | TP1 | `ST` pin from onboard TPS2116 |

### 2.5 Power Supply

By default, the board is powered from the `USB-C` 5 V source. An onboard
TPS2116 priority power mux allows a backup 5 V supply to be connected via
the `Backup supply` connector (for example, a regulated battery pack
output). If the USB-C supply drops below the mux threshold, the TPS2116
automatically switches the board over to the backup supply, and switches back
when USB-C power returns. The mux status pin (`ST`) is exposed on the
`TPS2116 ST` test pad and is pulled low whenever the backup supply is in use,
allowing a probe to detect the active source during development/testing.

### 2.6 Speaker

An 8 ohm, >= 1 W speaker can be connected via the `Speaker` connector.
The amplifier output is bridge-tied (BTL): both terminals are driven, so
neither may be connected to ground.

---

## Third-Party Licenses

This project uses the following open-source software components:

- **STM32Cube HAL**, STMicroelectronics.
    - Licensed under the `3-Clause BSD License`.
        - See
          [`LICENSE.txt`](firmware/Drivers/STM32L4xx_HAL_Driver/LICENSE.txt).

> STMicroelectronics are trademarks of their respective owners. Use of these
> names does **not** imply any endorsement by the trademark holders.
