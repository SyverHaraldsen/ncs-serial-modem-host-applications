# nRF93M1 AT Host Application

Minimal tracker application for the **nRF93M1 Serial Modem**. The host runs Zephyr's cellular
modem driver, which dials the modem and brings up a PPP link over a CMUX channel. The application
uses this only for connectivity management: `conn_mgr` powers the link up and down, and the PPP
interface's L4 connected or disconnected events tell the application when the network is available.
The application sends no IP traffic of its own over PPP. All cloud communication (telemetry,
location, TLS) instead goes through the modem's own nRF Cloud client through raw AT commands over a
separate CMUX user pipe. The host samples the battery state locally and periodically syncs it and
location fixes to nRF Cloud over that AT interface.

> [!TIP]
> New here? Follow [Get started with the nRF93M1 AT Host Application](../../getting-started/93m1-at.md) to build, flash, claim the device in nRF Cloud, and verify that data arrives. This page is the reference for the application.

## Supported hardware

| Hardware | Board target | Host console |
|----------|--------------|--------------|
| nRF93M1 DK | `nrf93m1dk/nrf54l15/cpuapp/ns` | VCOM0 (`uart20`) |

## Modules

The application follows the shared [Architecture](../../architecture.md). It consists of the Main, Network, Cloud, Location, Battery, Buttons, and Modem AT modules. See [Modules per application](../../architecture.md#modules-per-application) for what each one does in this application.

## Cloud synchronization

Once the modem has attached to the network it performs a cloud sync every 10 minutes (`CONFIG_APP_SYNC_BOOT_DELAY_SECONDS` and `CONFIG_APP_SYNC_INTERVAL`). Each sync does two things through the modem's nRF Cloud client:

- **Location:** sends `AT%NRFCLOUDLOCATION=7,1`. The modem collects single-cell, multicell and Wi-Fi measurements, nRF Cloud resolves them to a position, and the result is returned to the host. Change the method with `CONFIG_APP_LOCATION_METHOD` (1 single-cell, 2 multicell, 4 Wi-Fi, or a sum of them).
- **Battery:** sends the fuel gauge state of charge as a device message: `{"appId":"BATTERY","messageType":"DATA","data":"<percent>"}`. The fuel gauge is sampled every 60 seconds whether or not the device is connected (`CONFIG_APP_BATTERY_SAMPLE_INTERVAL`).

The modem is its own nRF Cloud client, so the device ID in nRF Cloud is the modem UUID from `AT%DEVICEUUID`. See [Verify](../../getting-started/93m1-at.md#step-6-verify) for where the data shows up in nRF Cloud.

## Shell commands

The console offers these shell commands:

| Command | Action |
| --- | --- |
| `at "<command>"` | Send an AT command to the modem and print the response. |
| `network connect` / `network disconnect` | Bring the cellular link up or down. |
| `button 1` / `button 2` | Simulate a press of the matching DK button. |

Pressing **Button 1** (or running `button 1`) starts a sync right away instead of waiting for the next interval.

## Troubleshooting

| Symptom | Meaning |
| --- | --- |
| Device listed under **Fleet → Devices**, but no location or messages visible | The device has not completed a synchronization yet. Check that the console logs the location and battery lines described in [Verify](../../getting-started/93m1-at.md#step-6-verify), or press **Button 1** to start a synchronization right away. |
| `modem_at: CoAP response: 4.01 Unauthorized (device may not be onboarded to nRF Cloud)` | nRF Cloud rejected the modem. Finish the [claiming steps](../../getting-started/93m1-at.md#step-5-connect-to-nrf-cloud) and check that the device is listed under your team. |
| `location: Failed to request location, network is down?` or `cloud: Failed to sent battery data, network is down?` | The AT command failed, usually because the modem has not attached yet. Check the link with `at AT+CEREG?`. |
| `main: battery_percent_get, error: -61` | The fuel gauge has no valid sample yet, so no battery message is sent this sync. Check that a battery is connected to the DK. |
| Location lines always report `Method: Single-cell` | Multicell and Wi-Fi positioning do not work while the modem is in RRC Connected mode, so nRF Cloud falls back to single-cell. |
