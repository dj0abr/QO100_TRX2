[English](README.md) · [Deutsch](README.de.md)

# QO100_TRX2

QO-100 software transceiver using an ADALM-Pluto and an SBC (Raspberry Pi, Odroid, …).
Also works on Debian-based Linux desktop PCs.
This project is a fully functional software-based transceiver for QO-100, operated from a web browser.

![QO100_TRX2 WebGUI](docs/WebGUI.png)

## System Overview

![QO100 Multichannel Web Transceiver block diagram](docs/blockschaltbild_english.png)

A single Pluto serves four parallel receive channels and two transmit paths.
**RX1/TX1** provide voice operation in the browser, while **RX2/TX2** connect external digital programs such as WSJT-X or fldigi through virtual audio devices.
**RX3** monitors FT8 and **RX4** the PSK beacon on fixed frequencies configurable in Setup; both decoders run independently of normal radio operation and send their text to the WebGUI.
The two transmit paths are combined into a shared IQ signal for the Pluto.

## Hardware Requirements

- Pluto SDR, connected via USB or a USB-to-Ethernet adapter.
- SBC such as Raspberry Pi 4, Odroid C4/N2, or a desktop PC running Debian, Ubuntu, Linux Mint, etc. **Use a 64-bit OS.**
- USB sound stick if the computer used for audio has no microphone connector; microphone and speakers/headphones connect to the **browser computer**.
- Satellite dish, QO-100 feed/LNB and a 2.4 GHz amplifier, e.g. the AMSAT-DL 46 dB amplifier.

## Installation

Open a terminal on the SBC or Linux PC:

```sh
sudo apt update
sudo apt install -y git
git clone https://github.com/dj0abr/QO100_TRX2.git
cd QO100_TRX2
./build_all.sh --install-deps
```

The script installs the required packages and builds everything. On an SBC, missing libraries are built automatically for its processor.
For later builds, just run `./build_all.sh`.

### Pluto over Ethernet

- Connect a Pluto-compatible USB Ethernet adapter using a **USB OTG cable**. Supply **5 V** through an OTG Y-cable or Pluto's separate power connector.
- In Pluto's `config.txt`, leave **`ipaddr_eth` empty** under `[USB_ETHERNET]` to obtain an IP via DHCP. Safely eject the virtual USB drive after editing.
- Find the IP in your router, preferably reserve it via DHCP, and enter it under **Setup → Pluto connection: Network**.
- **Avoid address conflicts:** Pluto's USB network (default `192.168.2.x`) and Ethernet LAN must use different subnets.

Details: [AMSAT-DL: Pluto networking](https://wiki.amsat-dl.org/doku.php?id=de:tricks:pluto_network).

## Run the Software

From the project folder:

```sh
./start_all.sh
```

Open the GUI in your browser:

- **On the same computer:** <http://localhost:8080/>
- **From another computer:** `http://<SBC-IP>:8080/`, or your HTTPS reverse-proxy address.

**Remote microphone access requires HTTPS; localhost also works over HTTP.**
Leave the terminal open. **Ctrl+C** stops all programs.

## Configuration

1. The GUI connects automatically. If disconnected, click **Verbinden** (Connect). Look for **Backend verbunden**.
2. Open **Setup**: choose USB or network/IP for Pluto, set the LNB reference (normally 25 MHz), RX/TX corrections and TX attenuation for your station. Click **Änderungen speichern** (Save changes) and wait for **Hardware bereit**.
3. Click **Audio starten** (Start audio) and allow microphone access. Select your playback and recording devices in **Setup → Audio** if needed.

![Connection and audio controls](docs/verbindung.png)

<details>
<summary>Setup screen</summary>

![Setup](docs/setup.png)

</details>

The first browser gets control; additional browsers can watch and listen. The connection stays open without user activity. PTT turns off automatically after five minutes at the latest.

## Operation

### The two waterfalls

**Fullband** shows the entire QO-100 narrowband transponder. **Zoom** shows ±25 kHz around the selected receiver for fine tuning. Click **RX1** or **RX2** to select its RX/TX pair; zoom and audio follow your selection.

### Mouse tuning

Works in both the spectrum and waterfall:

| Action | Result |
|---|---|
| Left-click | Move the selected receiver and its TX by the same amount, keeping the offset. |
| Right-click | Tune only the selected receiver. |
| Mouse wheel | Tune the selected receiver: 250 Hz steps in Fullband, 25 Hz in Zoom. |
| Ctrl + wheel | Move the selected receiver and TX together. |
| Shift + wheel | Tune TX only. |

Double-click an RX frequency to enter it directly; click the TX frequency for direct TX entry.

The readout to the right of the downlink display shows the downlink frequency under the mouse pointer. It works over the spectrum, waterfall and frequency axis in Fullband and the RX1/RX2 zoom.

![Receiver and transmitter frequencies](docs/frequency.png)

### Waterfall menu — Wasserfall

Choose the colour, spectrum smoothing (**Moving Average**) and display levels. Larger averaging values give a steadier spectrum; lower values respond faster. Adjust **Untergrenze/Obergrenze** for a clear waterfall.

![Waterfall settings](docs/wasserfallsetup.png)

### Filter menu — Filter

Choose an RX1 bandwidth preset or adjust the **lower/upper cutoff** separately. The passband is highlighted in the RX1 zoom. **Rauschreduktion** reduces audio noise; start low and increase to taste.

![RX1 filter settings](docs/filter.png)

### QSO controls

- **RX1 → TX:** move TX to RX1's matching uplink frequency.
- **TX → RX1:** move RX1 to TX's matching downlink frequency.
- **RX volume:** speaker/headphone volume while receiving. The smaller red **TX slider** below controls receive audio during TX1 PTT and defaults to mute. The speaker button toggles between zero and the previous TX level. The TX monitoring level is saved in this browser.
- **Kompressor:** raises average speech loudness. **100%** is the normal starting point; **0%** bypasses it.
- **PTT:** transmit by holding the button. In **Setup**, you can choose click-on/click-off operation and optionally enable the spacebar.

![QSO controls](docs/operation1.png)

### Bandplan and frequency display

The bandplan identifies the operating areas. Click **↻** at the right of the Fullband scale to switch between bandplan, downlink, uplink and Pluto frequencies.

![Bandplan display switch](docs/bandplan.png)

## Four Receivers

- **RX1:** normal QO-100 voice operation, with adjustable audio filter and noise reduction.
- **RX2/TX2:** independent digital pair for FLdigi/WSJT-X, with virtual audio devices and additional RX2 monitoring in the browser. [Setup and levels](docs/digital-audio.md).
- **RX3:** FT8 monitor.
- **RX4:** PSK beacon monitor.

**Note on using RX2 with WSJT-X:** Under **Settings → Audio**, select these devices:

| Setting | Device |
|---|---|
| Input | `QO100_RX2_input` |
| Output | `QO100_TX2` |

WSJT-X also lists internal endpoints such as `QO100_RX2.Monitor` and `QO100_TX2.Monitor`. Use the devices listed above for RX2/TX2 operation.

Set both USB carrier frequencies in whole Hz under **Setup → Monitoring receivers** and save. **Show decoder text** above the zoom opens the output; radio buttons select **RX3 · FT8** or **RX4 · PSK**. Both sources are received concurrently and buffered separately. `./start_all.sh` starts both decoders.

![FT8 decoded messages](docs/decoderoutput.png)

Allow about 15–30 seconds plus decoding time for the first messages. Keep the SBC/PC clock synchronized.

For technical details, see [DEVELOPMENT.md](DEVELOPMENT.md).

PSK beacon reception via RX4: [instructions](docs/beacon-decoder.md).
