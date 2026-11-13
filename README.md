# Serial Modem Host Applications

[![Build, Test, and Release](https://img.shields.io/github/actions/workflow/status/nrfconnect/ncs-serial-modem-host-applications/ci.yml?event=schedule&branch=main&label=Build%2C%20Test%2C%20and%20Release)](https://github.com/nrfconnect/ncs-serial-modem-host-applications/actions/workflows/ci.yml?query=branch%3Amain+event%3Aschedule)

Host-side firmware for Nordic Smart Modem modules. The applications in this repository run on a host MCU, use a Serial Modem for cellular connectivity, and send data to nRF Cloud. They are built on [nRF Connect SDK](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK) and share the modular **zbus + SMF** architecture of the [Asset Tracker Template](https://github.com/nrfconnect/Asset-Tracker-Template).

![The host MCU talks to the Serial Modem over UART, and the Serial Modem connects to nRF Cloud over LTE](docs/images/host-serial-modem.svg)

## Applications

| Your hardware | Application | What it does |
|---------------|-------------|--------------|
| nRF54L15 DK or nRF54LM20B DK, wired to an nRF9151 DK | [PPP Host Application](docs/applications/ppp/README.md) | The host connects to nRF Cloud over its own IP stack, with the modem carrying IP over PPP. Includes FOTA, remote debugging, and optional Wi-Fi location. |
| nRF93M1 DK | [PPP Host Application](docs/applications/ppp/README.md) | The same application on the integrated nRF93M1 modem, with FOTA, remote debugging, cell and Wi-Fi location, and battery reporting. |
| nRF93M1 DK | [nRF93M1 AT Host Application](docs/applications/93m1_at/README.md) | A minimal host. The modem connects to nRF Cloud with its built-in client, and the host drives it with AT commands. |
| nRF54LM20B DK, wired to an nRF9151 DK | [nRF91 AT Host Application](docs/applications/91m1_at/README.md) | A minimal location tracker. The Serial Modem onboards itself to nRF Cloud and resolves cell or GNSS location, and the host drives it with AT commands. |
| nRF7120 DK, or nRF54LM20B DK wired to an nRF93M1 DK | [Person detection video stream](docs/applications/person_detection_stream/README.md) | Streams camera video with person detection over UDP, on Wi-Fi or on LTE through the nRF93M1 modem. No cloud. |

## Get started

**[Getting started](docs/getting-started/index.md)** takes you from choosing your application to a device sending data to nRF Cloud, using the command line.

## License

See [LICENSE](LICENSE).
