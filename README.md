# ‘Geek szitman supercamera’ viewer

This repository is a proof-of-concept to use the ‘Geek szitman supercamera’ camera-based products.

<div align="center">

<h2>External View</h2>

![Demo Shot on iPhone](./assets/demo-phone.gif)

<h2>Endoscope View (Two Cam) </h2>

![Demo Shot on Camera](./assets/demo-cam.gif)

</div>



### Technical information

‘Geek szitman supercamera’ (2ce3:3828 or 0329:2022) is a camera chip (endoscope, glasses, ...) using the com.useeplus.protocol
(officially only working on iOS/Android devices with specific apps, such as ‘Usee Plus’).
Only firmware version 1.00 has been tested. USB descriptors can be found in file the `descriptors` folder.

**Contrary to the advertised specification**, the camera resolution is 640×480.


## Prerequisite (simple Ubuntu setup)

First install the required packages:

```bash
sudo apt-get update
sudo apt-get install -y \
    libusb-1.0-0-dev \
    pkg-config \
    ros-noetic-compressed-image-transport
```

If you are building this repo directly on the same machine, also install:

```bash
sudo apt-get install -y build-essential cmake libopencv-dev python3-pip
```

Create the USB rule file so the USB device can be accessed by non-root users:

```bash
echo 'SUBSYSTEMS=="usb", ENV{DEVTYPE}=="usb_device", ATTRS{idVendor}=="2ce3", ATTRS{idProduct}=="3828", MODE="0666"' | sudo tee /etc/udev/rules.d/99-supercamera.rules
```

Then reload and trigger the rules:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Important: disconnect and reconnect the USB cable so udev applies new permissions.

Verify permissions:

```bash
lsusb | grep 2ce3:3828
# Expected: Bus 001 Device 009: ID 2ce3:3828

ls -l /dev/bus/usb/001/009
# Expected: crw-rw-rw- ... /dev/bus/usb/001/009
```

The bus/device numbers (`001/009` above) must match your actual `lsusb` output.

## Build

Install dependencies (packages given assume a Debian-based system):

```bash
apt install build-essential cmake pkg-config libusb-1.0-0-dev libopencv-dev
```

Build with CMake:

```bash
cmake -S . -B build
cmake --build build -j
```

Legacy Makefile build is still available:

```bash
make
```

## Usage

Run the tool:

```bash
./build/out
```

It will display the camera feed in a GUI window.

- short press on the endoscope button will save the current frame in the `pics` folder
- long press on the endoscope button will switch between the two cameras
- press <kbd>q</kbd> or <kbd>Esc</kbd> in the GUI window to quit

### Real-time TCP streaming 

Run the sender:

```bash
./build/out_stream_sender --transport tcp --bind 0.0.0.0 --port 9000 --camera-count 2
```

Options:

- `--transport <tcp|udp>` (`udp` currently returns a not-implemented error)
- `--bind <ip>` (default: `0.0.0.0`)
- `--port <n>` (default: `9000`)
- `--camera-count <n>` (default: `1`, use `2` for two USB cameras)
- `--max-fps <n>` (default: `0`, meaning unlimited)
- `--log-every <n>` (default: `120`)

Protocol details are documented in `STREAM_PROTOCOL.md`.




### Python receiver (OpenCV)

You can receive and display the TCP stream with:

```bash
python3 scripts/stream_receiver.py --host 127.0.0.1 --port 9000
```

Receiver dependencies:

```bash
pip install opencv-python numpy
```

The receiver opens one OpenCV window per source (`source 0`, `source 1`). When multiple cameras are streamed, the receiver opens one OpenCV window per source.
Press <kbd>q</kbd> or <kbd>Esc</kbd> in the receiver window to quit.

### Stream over Wi-Fi (two PCs on same network)

Use this when camera/sender and receiver are on different PCs connected to the same Wi-Fi.

1. On PC A (camera + sender), start sender on all interfaces:

```bash
./build/out_stream_sender --transport tcp --bind 0.0.0.0 --port 9000 --camera-count 2
```

2. On PC A, get its LAN IP address (example commands):

```bash
hostname -I        # Linux
ipconfig getifaddr en0  # macOS (Wi-Fi interface)
```

Assume the IP is `192.168.1.50`.

3. On PC B (receiver), run:

```bash
python3 scripts/stream_receiver.py --host 192.168.1.50 --port 9000
```

4. If connection fails, check:

- both PCs are on the same subnet (for example `192.168.1.x`)
- PC A firewall allows inbound TCP port `9000`
- Wi-Fi/router guest isolation is disabled



### Troubleshooting

Feel free to open an issue.
Please recompile with `VERBOSE = 3` and include full logs.

If your hardware is different, do include its USB descriptors:

```bash
lsusb -vd $(lsusb | grep Geek | awk '{print $6}')
```

**Additional udev rule for alternate VID/PID**

Some devices enumerate as `0329:2022`. Add this rule if needed:

```bash
echo 'SUBSYSTEMS=="usb", ENV{DEVTYPE}=="usb_device", ATTRS{idVendor}=="0329", ATTRS{idProduct}=="2022", MODE="0666"' | sudo tee -a /etc/udev/rules.d/99-supercamera.rules
```

**Known issues:**

- `fatal: usb device not found`: check your device is properly plugged in. Check you have added udev rules properly. Try to run the program with root privileges: `sudo ./out`.

