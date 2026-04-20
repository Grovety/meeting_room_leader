# Meeting Room

Meeting Room is an ESP-IDF firmware project for an ESP32-S3 based meeting-room panel with an `800x480` touch display.
The device is intended to run as the main room display in a local meeting-room setup.

It combines room status presentation, local device setup, weather, QR-based information pages, and communication with a smaller auxiliary `3.5-inch` panel.
The UI supports both English and Chinese.

## Features

- Main room dashboard for daily operation
- Meeting-room occupancy status and countdown display
- Weather widget on the main screen
- Fullscreen weather view with forecast and detail cards
- Local Wi-Fi onboarding through a setup portal
- Runtime device setup page available on the local network
- English / Chinese interface
- Adjustable sleep / wake behavior for the screen
- UDP-based integration with the auxiliary `3.5-inch` panel

## Hardware / Software

- Board: CrowPanel `5.0"` / `7.0"` (`v1.0` and `v1.1`)
- MCU: `ESP32-S3`
- Display: `800x480` touch panel
- Framework: [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
- Verified with ESP-IDF: `v5.5.1`
- Note: the project may also run on earlier ESP-IDF versions, but `v5.5.1` is the validated baseline
- UI stack: [LVGL](https://lvgl.io/)

## Project Layout

- `main/` - application startup, Wi-Fi, setup portal, time sync, sleep logic, auxiliary-panel UDP link
- `components/weather_module/` - weather service, weather widget, fullscreen weather UI
- `components/ui/` - LVGL screens and UI assets
- `components/language_manager/` - language selection and persistence
- `main/include/aux_udp_protocol.h` - UDP protocol shared with the auxiliary `3.5-inch` panel

## Quick Start

1. Install ESP-IDF and export the environment.
2. Clone this repository.
3. Open the project directory.
4. Set chip target:

```bash
idf.py set-target esp32s3
```

5. Build firmware:

```bash
idf.py build
```

6. Flash firmware:

```bash
idf.py -p <PORT> flash
```

7. Open monitor:

```bash
idf.py -p <PORT> monitor
```

You can also run the project without local compilation: use `firmware/flash_tool.exe`, which flashes prebuilt binaries from `firmware/binaries/`.

## Wi-Fi Setup

The project uses a two-step setup model.

### Initial Wi-Fi onboarding

If the device does not yet have working Wi-Fi credentials, it can start its own setup access point and configuration page.
That page is used to save the target Wi-Fi SSID and password.

Typical flow:
1. Power on the panel.
2. Connect to the panel's temporary setup network.
3. Open the setup page in a browser.
4. Enter the target Wi-Fi credentials.
5. Save settings.
6. Wait for the device to reconnect to the local network.

### Runtime device setup

Once the panel is already on the local network, it exposes a runtime `Device setup` page.
This page is intended for same-network configuration changes without reflashing the device.

The runtime setup page is used for:
- panel name
- weather API key
- company website link for QR page
- office map link for QR page

## Weather

The weather feature is integrated into the main panel UI.
It includes:
- current conditions
- feels-like temperature
- wind / humidity / pressure
- sunrise / sunset
- forecast cards
- metric / imperial unit switching

### Weather provider

Weather data is fetched from [OpenWeather](https://openweathermap.org/).
To enable live weather, provide your own API key in the runtime `Device setup` page.

Setup flow:
1. Connect the panel to Wi-Fi.
2. Open `Device setup` on the same local network.
3. Enter the OpenWeather API key.
4. Save settings.

The key is stored locally in NVS and used by the running weather module.

## Language Support

The firmware UI supports:
- English
- Chinese

Some weather description strings returned by the external weather provider may still remain in English.

## Sleep / Wake Behavior

The panel includes automatic sleep management.
The firmware can:
- track inactivity
- dim / sleep the display after the configured timeout
- wake on user interaction
- keep the panel active during an ongoing booking

Sleep-related settings are persisted in NVS.

## Integration With The Auxiliary `3.5-inch` Panel

This repository contains the main panel in a two-panel setup.
The smaller `3.5-inch` panel is a separate ESP-IDF project that works as an auxiliary room-status display.

### How the integration works

The main panel:
- broadcasts discovery / beacon packets
- publishes room-state packets over UDP
- accepts pairing requests from the auxiliary panel
- stores pairing state for the connected auxiliary device

The auxiliary panel:
- joins the same local Wi-Fi network
- discovers the main panel
- pairs with the selected main panel
- receives room state from the main panel and displays it locally

### Shared protocol

The shared packet format is defined in `main/include/aux_udp_protocol.h`.

Relevant details:
- transport: UDP
- port: `33334`
- packet types: beacon, state, pair request, pair ack, pair nack

### Typical deployment flow

1. Flash the main panel firmware from this repository.
2. Flash the auxiliary `3.5-inch` panel firmware from its repository.
3. Connect both devices to the same local Wi-Fi network.
4. Allow the auxiliary panel to discover the main panel.
5. Pair the auxiliary panel with the target room panel.
6. The auxiliary panel starts showing live room state from the main panel.

## Runtime Configuration Stored In NVS

The firmware stores runtime configuration such as:
- Wi-Fi credentials
- panel name
- weather API key
- weather units
- sleep timeout
- QR page links
- language selection
- auxiliary panel pairing state

## Third-Party Components

This repository includes third-party components under `components/`, including LVGL and other ESP-IDF dependencies.
Please review their original licenses before redistribution.
