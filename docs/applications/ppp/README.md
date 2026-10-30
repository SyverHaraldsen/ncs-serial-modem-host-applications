# PPP Host Application

The [PPP Host Application](https://github.com/nrfconnect/ncs-serial-modem-host-applications/blob/main/applications/ppp/) runs on an nRF54L15 DK or nRF54LM20B DK wired to an nRF9151 DK running the Serial Modem firmware, and on the nRF93M1 DK with its on-board nRF93M1 modem. Cellular data goes through the modem over PPP, and the host connects to nRF Cloud over **CoAP/DTLS** itself. The host stores its nRF Cloud credentials with the TLS credentials shell in TF-M Protected Storage.

> [!TIP]
> New here? Follow the getting-started guide for your hardware to set up the hardware, build, flash, and connect to nRF Cloud: [nRF54 DK with an nRF9151 DK](../../getting-started/ppp-nrf54.md) or [nRF93M1 DK](../../getting-started/ppp-nrf93m1.md). This page is the reference for the application.

## Supported hardware

| Host setup | Board target | Host console |
|------------|--------------|--------------|
| nRF54L15 DK + nRF9151 DK | `nrf54l15dk/nrf54l15/cpuapp/ns` | VCOM1 |
| nRF54LM20B DK + nRF9151 DK | `nrf54lm20dk/nrf54lm20b/cpuapp/ns` | VCOM0 |
| nRF54LM20B DK + nRF7002-EB II + nRF9151 DK (Wi-Fi® location) | `nrf54lm20dk/nrf54lm20b/cpuapp/ns` with the location overlay | VCOM0 |
| nRF93M1 DK | `nrf93m1dk/nrf54l15/cpuapp/ns` | VCOM0 (`uart20`) |

The nRF9151 SMA DK can replace the nRF9151 DK in every setup. See [Set up the hardware](../../getting-started/ppp-nrf54.md#step-3-set-up-the-hardware) for the Board Configurator settings and wiring of the nRF54 host setups.

### Differences between the boards

All boards build the same modules from the same source. Board-specific behavior is selected through Kconfig and the board files:

| | nRF54 host DKs | nRF93M1 DK |
|---|---|---|
| Modem | nRF9151 DK running Serial Modem, wired to the host | On-board nRF93M1 |
| Location | Wi-Fi scan on the host through the Location library and the nRF7002-EB II. Only built with the location overlay | Cell and Wi-Fi measurements from the modem over AT commands. Always built |
| Battery | Not included | nPM1300 fuel gauge sampling |
| nRF Cloud software type | `smha-91m1` | `smha-93m1` |

The rest of the configuration is shared in [`prj.conf`](https://github.com/nrfconnect/ncs-serial-modem-host-applications/blob/main/applications/ppp/prj.conf). The board files only hold the UART, flash, battery and Memfault identity settings.

## Modules

The application follows the shared [Architecture](../../architecture.md). It consists of the following modules:

- **[Main module](modules/main.md)**: Implements the business logic and controls the overall application behavior. Uniquely, it is not in the `modules` folder.
- **[Network module](modules/network.md)**: Brings the PPP link to the modem up and down, and tracks connectivity status.
- **[Cloud module](modules/cloud.md)**: Handles communication with nRF Cloud using CoAP.
- **[FOTA module](modules/fota.md)**: Manages firmware over-the-air updates of the host application.
- **[Location module](modules/location.md)**: Collects Wi-Fi and cell measurements for the nRF Cloud location service, with one backend per board type.
- **Battery module**: Samples the nPM1300 fuel gauge. Only built on the nRF93M1 DK.
- **Buttons module**: Publishes DK button presses, and their `button` shell equivalents, on `button_chan`. It has no state machine or thread of its own. See [Buttons and shell commands](application-behavior.md#buttons-and-shell-commands).
- **LED module**: Drives the host RGB LED with the color and blink pattern that the Main module publishes on `led_chan`. Only built on the nRF93M1 DK. See [LED indication](application-behavior.md#led-indication).
- **Modem AT module**: Serializes access to the modem AT pipe behind `modem_at_run()`. It has no zbus channel and no state machine, and is called directly by the Location module's modem AT backend and the host shell. See [Reaching Serial Modem AT commands](#reaching-serial-modem-at-commands).

The following diagram shows how the modules interact. Each module owns one channel, and both the requests sent to a module and the notifications it publishes go on that channel.

![Modules and the zbus channels they communicate over](diagrams/modules.svg)

The following steps show the simplified flow of a typical operation:

1. The Network module connects on startup, and publishes `NETWORK_CONNECTED` on the `network_chan` channel once the PPP link carries IP traffic.
1. The Main module responds by requesting a cloud connection with `CLOUD_CONNECT` on the `cloud_chan` channel. The Cloud module waits for valid time and installed credentials, connects to nRF Cloud, and publishes `CLOUD_CONNECTED`.
1. While the cloud connection is up, the Main module runs a periodic **cloud synchronization**: it sends a device message, triggers a location search, polls the device shadow, requests a FOTA poll, and posts pending remote debugging data.
1. A location search runs asynchronously. The Location module collects its measurements and publishes them as `LOCATION_SEARCH_WIFI_CELL_RESULT`, which the Main module forwards to the Cloud module for resolution by the nRF Cloud location service.
1. If the FOTA poll finds a job, the FOTA module downloads the update and asks the Main module to reboot with `FOTA_REBOOT_REQUEST`.

See [Application behavior](application-behavior.md) for the full startup sequence, cloud synchronization, buttons and shell commands, and configuration options.

## Remote debugging

The application reports crashes and device health metrics to nRF Cloud through the Memfault SDK. The data goes through the nRF Cloud CoAP session (`CONFIG_MEMFAULT_USE_NRF_CLOUD_COAP`), and the cloud module posts pending data on every cloud synchronization.

See [Remote debugging with nRF Cloud](remote-debugging.md) for how to view device data in nRF Cloud, upload `zephyr.elf`, and verify data collection.

### Reaching Serial Modem AT commands

`CONFIG_APP_MODEM_AT_SHELL` exposes the modem's AT interface over the host shell using the CMUX user pipe, for modem commands that have no host-side equivalent:

```shell
uart:~$ at "AT#XLOG=1"
OK
```

> [!NOTE]
> The pipe only exists once the modem has attached, CMUX runtime power save closes it again when idle (the command then answers `modem is not ready`, so retry), and any host reboot resets the modem and turns logging back off.

Module documentation:

- [Main module](modules/main.md) — business logic, cloud synchronization, FOTA coordination
- [Network module](modules/network.md) — PPP link to the modem
- [Cloud module](modules/cloud.md) — nRF Cloud CoAP session, device messages, shadow
- [FOTA module](modules/fota.md) — firmware updates of the host application
- [Location module](modules/location.md) — Wi-Fi and cell based positioning

## References

- [nRF Cloud CoAP library](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/networking/nrf_cloud_coap.html)
- [nRF Cloud Utils](https://github.com/nRFCloud/utils)
