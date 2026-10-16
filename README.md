# ZeDART GNSS NTP Server

A small standalone **Stratum 1 NTP server** based on an ESP32 and a GNSS receiver with PPS output.

The device provides a precise local UTC time reference without relying on an Internet NTP server. Time is obtained directly from GNSS satellites, while the PPS signal is used to accurately align the beginning of each UTC second.

It can operate in two network modes:

- **Access Point mode**: the ESP32 creates its own isolated Wi-Fi network (`ZeDART-Time`).
- **Router mode**: the ESP32 joins an existing Wi-Fi network and exposes the NTP service on the local LAN, including through `zedart-time.local` using mDNS.

The operating mode is selected at boot using a single GPIO pin, so no screen, buttons or configuration interface are required.

---

## How it works

The GNSS receiver provides two complementary sources of timing information:

- **NMEA data over UART** provides the absolute UTC date and time.
- **PPS (Pulse Per Second)** provides a very accurate electrical pulse aligned with the start of each UTC second.

The ESP32 combines both:

```text
GNSS satellites
      │
      ▼
  NEO-M8N
  ├── NMEA / UART ──► UTC date and time
  └── PPS ──────────► precise second boundary
      │
      ▼
    ESP32
      │
      ├── disciplined UTC clock
      └── NTP server (UDP/123)
```

The ESP32 continuously measures its own local timer against the GNSS PPS signal. This compensates for the normal drift of the ESP32 oscillator between PPS pulses.

The NTP server only answers requests while the time source is considered valid. If the PPS signal is lost or the GNSS time becomes stale, `NTP READY` becomes `NO` and the server stops providing time until synchronization is restored.

---

## Hardware required

### Main components

- 1 × ESP32 development board
  - Tested with an **ESP32-S3 N8R2 / ESP32-S3 DevKitC-style board**
  - A standard ESP32 is also sufficient if equivalent GPIO/UART pins are available
- 1 × GNSS receiver with PPS output
  - Tested with a **u-blox NEO-M8N module**
  - Required pins: `VCC`, `GND`, `TXD`, `RXD`, `PPS`
- 1 × compatible GNSS antenna
  - SMA antenna used with the tested module
- 5 × jumper wires
- 1 × USB cable for the ESP32

Optional:

- 1 × jumper between GPIO5 and GND to enable router mode
- enclosure, LEDs, or other finishing hardware if desired

---

## Wiring

The tested wiring is:

| NEO-M8N | ESP32-S3 | Function |
|---|---:|---|
| `VCC` | `3V3` | GNSS power |
| `GND` | `GND` | Ground |
| `TXD` | `GPIO18` | GNSS UART TX → ESP32 RX |
| `RXD` | `GPIO17` | ESP32 TX → GNSS UART RX |
| `PPS` | `GPIO4` | PPS timing input |

Network mode selection:

| GPIO5 state at boot | Network mode |
|---|---|
| Not connected | Access Point mode |
| Connected to `GND` | Router / Wi-Fi client mode |

### Wiring diagram

```text
NEO-M8N                 ESP32-S3
────────                 ────────
VCC   ─────────────────► 3V3
GND   ─────────────────► GND
TXD   ─────────────────► GPIO18
RXD   ◄───────────────── GPIO17
PPS   ─────────────────► GPIO4

                         GPIO5
                           │
                           └── GND  → Router mode

GPIO5 left open                    → Access Point mode
```

> The tested NEO-M8N breakout accepts 3.3 V power. Verify the specifications of your own GNSS breakout before connecting it.

---

## Network modes

### Access Point mode

Leave **GPIO5 unconnected** when the ESP32 boots.

The ESP32 creates its own Wi-Fi network:

```text
SSID:     ZeDART-Time
Password: ZeDART2026
NTP host: 192.168.4.1
Port:     UDP/123
```

This mode is useful for a completely standalone local timing network.

A connected client will normally receive an address in the `192.168.4.x` range.

Some smartphones may prefer mobile data when connected to a Wi-Fi network without Internet access. For testing from Android, mobile data may therefore need to be disabled temporarily so that traffic is actually sent through `ZeDART-Time`.

### Router mode

Connect **GPIO5 to GND before booting the ESP32**.

The ESP32 joins the Wi-Fi network configured in the firmware:

```cpp
constexpr char ROUTER_SSID[] = "MYSSID";
constexpr char ROUTER_PASSWORD[] = "MYPASSWORD";
```

The router assigns an IP address using DHCP.

The server can then be reached using either:

```text
<ESP32 DHCP address>
```

or, on systems supporting mDNS:

```text
zedart-time.local
```

The firmware also advertises the NTP service as:

```text
_ntp._udp
```

---

## Software requirements

### Arduino IDE

Install **Arduino IDE 2.x**.

### ESP32 board support

Install the Espressif ESP32 board package through Arduino IDE's Boards Manager.

Select the board corresponding to your ESP32 model. For the tested hardware, use the appropriate ESP32-S3 board definition.

### Required Arduino library

Install:

- **TinyGPSPlus** by Mikal Hart

The following libraries are provided by the ESP32 Arduino core and do not need to be installed separately:

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
```

---

## Firmware configuration

Before uploading the firmware, configure the router credentials if router mode will be used:

```cpp
constexpr char ROUTER_SSID[] = "YOUR_WIFI_SSID";
constexpr char ROUTER_PASSWORD[] = "YOUR_WIFI_PASSWORD";
```

The default access point configuration is:

```cpp
constexpr char AP_SSID[] = "ZeDART-Time";
constexpr char AP_PASSWORD[] = "ZeDART2026";
```

Hardware pins are defined near the top of the firmware:

```cpp
constexpr int GNSS_RX = 18;
constexpr int GNSS_TX = 17;
constexpr int PPS_PIN = 4;
constexpr int WIFI_MODE_PIN = 5;
```

Change these values if another ESP32 board or wiring layout is used.

---

## Installation

1. Wire the NEO-M8N to the ESP32 according to the table above.
2. Connect the GNSS antenna and place it where it has a reasonably clear view of the sky.
3. Connect the ESP32 to the computer over USB.
4. Open the firmware in Arduino IDE.
5. Install TinyGPSPlus if it is not already installed.
6. Select the correct ESP32 board and serial port.
7. Configure the router SSID and password if required.
8. Upload the sketch.
9. Open the Serial Monitor at **115200 baud**.
10. Wait for a GNSS fix and PPS lock.

A normal synchronized status looks similar to:

```text
------------------------------
GNSS FIX    : YES
SATELLITES  : 8
GNSS TIME   : VALID
PPS LOCK    : YES
PPS PERIOD  : 1000029.59 us
CLOCK VALID : YES
NTP READY   : YES
UNIX UTC    : 1792160330
```

When an NTP client queries the server, the serial monitor displays for example:

```text
NTP -> 192.168.0.37
```

---

## Time-source validation

The firmware checks several conditions before answering NTP requests:

- valid GNSS date and time
- recent GNSS time update
- PPS signal present
- several consecutive valid PPS periods
- initialized internal UTC clock

The status fields are:

```text
GNSS FIX
GNSS TIME
PPS LOCK
CLOCK VALID
NTP READY
```

If the PPS wire is disconnected while the server is running, the expected behavior is:

```text
PPS LOCK    : NO
CLOCK VALID : YES
NTP READY   : NO
```

The server therefore keeps its internal time reference but stops serving NTP until the precise PPS reference is restored.

After several valid PPS pulses are received again, `PPS LOCK` and `NTP READY` automatically return to `YES`.

---

## NTP characteristics

The server listens on the standard NTP port:

```text
UDP/123
```

It reports itself as a **Stratum 1** server whose primary reference is the GNSS receiver.

The NTP Reference ID is:

```text
GPS
```

The GNSS receiver is the only time source used by the firmware. Router mode is only a network transport mode; it does not use an Internet NTP server for synchronization.

---

## Testing

### Access Point mode

1. Boot the ESP32 with GPIO5 open.
2. Connect a client to `ZeDART-Time`.
3. Query NTP server `192.168.4.1`.

### Router mode

1. Power off the ESP32.
2. Connect GPIO5 to GND.
3. Boot the ESP32.
4. Check the assigned IP address in the Serial Monitor.
5. Query either that IP address or:

```text
zedart-time.local
```

A successful request should produce an `NTP -> ...` line in the Serial Monitor.

---

## Project status

Current prototype features:

- [x] GNSS UTC acquisition
- [x] PPS input
- [x] ESP32 clock disciplined against PPS
- [x] NTP server on UDP/123
- [x] Stratum 1 GNSS reference
- [x] Access Point mode
- [x] Router / Wi-Fi client mode
- [x] GPIO-based network mode selection
- [x] DHCP in router mode
- [x] `zedart-time.local` mDNS hostname
- [x] `_ntp._udp` service advertisement
- [x] PPS-loss detection
- [x] GNSS time freshness monitoring
- [x] Automatic refusal of NTP requests when the time source is not reliable

---

## Intended use

This device was developed as a local time source for the **ZeDART** project, in particular for systems handling ASTERIX traffic data where a reliable UTC reference is useful while keeping the traffic-processing machine independent from Internet-based time services.

It can also be used as a generic local GNSS-backed NTP server for other devices on an isolated or conventional LAN.
