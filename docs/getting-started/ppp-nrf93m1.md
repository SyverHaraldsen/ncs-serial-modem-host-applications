# Get started with the PPP Host Application on the nRF93M1 DK

This guide takes you from an [nRF93M1 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF93M1-DK) to a device that sends data to nRF Cloud. The [PPP Host Application](../applications/ppp/README.md) runs on the DK's nRF54L15 host. It reaches the internet through the on-board nRF93M1 modem over PPP, and connects to nRF Cloud over CoAP/DTLS itself.

![This guide covers steps 3 to 6: Set up the hardware, Build and flash, Connect to nRF Cloud, and Verify](../images/getting-started-steps-3-6.svg)

Before you start, complete [Step 1: Choose your application](index.md#step-1-choose-your-application). To build from source, also complete [Step 2: Set up the repository and toolchain](setup.md).

## Step 3: Set up the hardware

The host and the modem are on the same DK, so there is no wiring. This guide only flashes the nRF54L15 host. The nRF93M1 must run Serial Modem firmware with PPP and CMUX enabled, and keeps it.

1. Connect the nRF93M1 DK to your computer over USB and turn it on.
1. Run `nrfutil device list` and note the DK's serial number and its **VCOM0** port. VCOM0 is the host console (`uart20`). On macOS the port looks like `/dev/cu.usbmodem*`, on Linux like `/dev/ttyACM*`, and on Windows like `COM*`.

**Checkpoint:** `nrfutil device list` shows the nRF93M1 DK.

## Step 4: Build and flash

> [!IMPORTANT]
> `west flash --recover` and `nrfutil device recover` erase the whole chip, including the device credentials you install in step 5. Use them only for the first flash. After you have installed credentials, re-flash with a plain `west flash`, or with `nrfutil device program` without the `recover` command, to keep them.

1. Flash the host firmware. Choose one of the following options:

    - **Build from source:** Run the following commands inside the toolchain shell (step 2.4 in [Set up the repository and toolchain](setup.md)), from `smha-workspace/project`:

        ```shell
        cd applications/ppp
        west build -b nrf93m1dk/nrf54l15/cpuapp/ns -p
        west flash --recover
        ```

    - **Pre-built firmware:** Download the `ppp-nrf93m1-*` zip from the [latest release](https://github.com/nrfconnect/ncs-serial-modem-host-applications/releases), extract it, and flash `merged.hex`. See [Release artifacts](../release-artifacts.md) for what the zip contains.

        ```shell
        nrfutil device recover --serial-number <dk_serial_number>
        nrfutil device program --firmware merged.hex --serial-number <dk_serial_number>
        nrfutil device reset --serial-number <dk_serial_number>
        ```

1. Open a serial terminal at 115200 baud on VCOM0 and reset the DK.
1. Note the **Device ID** in the boot log. You need it in step 5:

    ```text
    <inf> nrf_cloud_info: Device ID: <16-hex-device-id>
    ```

    This 16-character hex ID comes from the nRF54L15 hardware ID. It is not the modem UUID.

1. Wait for the PPP link through the modem to come up.

**Checkpoint:** The console logs the following:

```text
<inf> main: Network connected
```

## Step 5: Connect to nRF Cloud

The host connects to nRF Cloud with its own certificate and private key. You create a certificate authority (CA) once, use it to install credentials on the device, and register the device in your nRF Cloud project.

Run the following commands from a normal terminal, not the toolchain shell. The credential installer uses the console port, so close the serial terminal on VCOM0 first.

1. Install the nRF Cloud command-line tools (once per computer):

    ```shell
    pip3 install nrfcloud-utils "memfault-cli>=1.12.0"
    ```

1. Create a self-signed CA certificate (once, reuse it for all your devices). Replace `US` with your two-letter country code:

    ```shell
    create_ca_cert -c US -f self_
    ```

    This creates `self_<serial>_ca.pem`, `self_<serial>_prv.pem`, and `self_<serial>_pub.pem`. Keep the CA private key safe.

1. Install the device credentials on the host. Replace `<device_id>` with the Device ID from step 4.3, and `<console_port>` with the VCOM0 port:

    ```shell
    device_credentials_installer \
      --ca self_<serial>_ca.pem \
      --ca-key self_<serial>_prv.pem \
      --id-str <device_id> \
      -s -d --verify --coap --local-cert --cmd-type tls_cred_shell \
      --port <console_port>
    ```

    On success, it saves an onboarding file named `onboard.csv`. The command also saves the device certificate and private key as PEM files in the current directory. Delete the device private key file afterwards.

1. Register the device in your nRF Cloud project, as described in the [device onboarding quickstart](https://docs.nrfcloud.com/docs/best-practices/device-onboarding-quickstart). In the [nRF Cloud dashboard](https://nrfcloud.nordicsemi.com/), create an Organization Auth Token under **Admin** > **Organization Auth Tokens**, and copy the organization and project slugs from **Project Settings** > **General**. Then run:

    ```shell
    nrfcloud --org-token <org_auth_token> --org <org_slug> --project <project_slug> \
      device create --csv onboard.csv
    ```

    The command prints `<device_id>: OK` when nRF Cloud has created the device.

1. Open the serial terminal on VCOM0 again.

**Checkpoint:** The cloud module retries every 10 seconds, so within a few seconds the log shows the following:

```text
<inf> main: Cloud connected
```

If the CA certificate or private key is missing, each attempt logs what is missing instead and retries until you install the credentials:

```text
<wrn> cloud: Missing nRF Cloud credentials (see the application documentation)
<wrn> cloud:   - CA cert (run device_credentials_installer --coap)
```

If the connection is refused, check that you onboarded the host **Device ID** from the boot log, not the modem UUID, and re-run `nrfcloud device create`.

## Step 6: Verify

When the cloud connects, the application runs its first **cloud synchronization**. It sends a device message (`{"appId":"SMHA","messageType":"DATA","data":"hello"}`), requests a location fix from the cell and Wi-Fi® measurements of the modem, polls the device shadow, checks for firmware updates, and uploads pending remote debugging data. The synchronization repeats every 10 minutes while the cloud stays connected (`CONFIG_APP_MAIN_SYNC_INTERVAL_SECONDS`).

1. Wait for the first synchronization to complete in the console, or press **Button 1** on the DK to start one right away. The location step logs the resolved position:

    ```text
    <dbg> cloud.cloud_location_request_handle: Location: 63.421552, 10.437707 Uncertainty: 16m Type: WIFI
    ```

1. Log in to [nRF Cloud](https://nrfcloud.nordicsemi.com/) and go to **Fleet** > **Devices**. Click the device with your Device ID to open its page.
1. On the **Overview** tab, check the following:
    - **Location**: the map shows the latest location fix.
    - **Last message**: the most recent device message, with `"data":"hello"` from the synchronization. Select **Inspect recent data** to see earlier messages.

Heartbeats and metrics appear in the linked nRF Cloud project.

## Next steps

| I want to | Read |
|-----------|------|
| Update the host firmware over the air | [Application FOTA](../app-fota.md) |
| See crashes and device metrics remotely | [Remote debugging with nRF Cloud](../applications/ppp/remote-debugging.md) |
| Learn the buttons and shell commands | [Buttons and shell commands](../applications/ppp/application-behavior.md#buttons-and-shell-commands) |
| Understand what the application does at runtime | [Application behavior](../applications/ppp/application-behavior.md) |
| Understand the module design | [Architecture](../architecture.md) and the [PPP modules](../applications/ppp/README.md#modules) |
| Contribute changes | [CI and contribution](../ci-and-contribution.md) |

---

**Previous:** [Step 2: Set up the repository and toolchain](setup.md) | **Up:** [Getting started](index.md)
