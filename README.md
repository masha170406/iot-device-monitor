# MQTT IoT Device Monitor

## Description

This project is a simple IoT device monitor written in C. It connects to an MQTT broker, receives sensor data from multiple devices, and checks if any readings exceed the configured limits.

The program also keeps track of known devices, shows their latest readings, detects inactive devices, and publishes alerts when needed.

## Requirements

- Ubuntu or another Linux system
- GCC
- Make
- Mosquitto MQTT broker
- Mosquitto client tools
- libmosquitto
- cJSON

## Installation

Install the required packages:

```bash
sudo apt update
sudo apt install build-essential mosquitto mosquitto-clients libmosquitto-dev libcjson-dev
```

## Build

Open the project directory and run:

```bash
make
```

To remove the compiled program:

```bash
make clean
```

## Running the program

Start the Mosquitto broker:

```bash
sudo systemctl start mosquitto
```

Run the monitor:

```bash
./iot_monitor
```

You can also use command-line options:

```bash
./iot_monitor -H localhost -P 1883 -o monitor.log
```

Available options:

- `-H HOST` – MQTT broker hostname
- `-P PORT` – MQTT broker port
- `-o FILE` – save telemetry and alerts to a file
- `-t VALUE` – temperature alert threshold
- `-u VALUE` – humidity alert threshold
- `-b VALUE` – battery alert threshold
- `-i SECONDS` – time before a device is marked inactive
- `-h` – show help

Example with custom thresholds:

```bash
./iot_monitor -t 35 -u 75 -b 25 -i 10
```

## Testing

The project includes a Python MQTT simulator called `mqtt_simulator.py`.

Install its Python dependency:

```bash
python3 -m venv venv
source venv/bin/activate
pip install "paho-mqtt>=2.0"
```

Start the simulator:

```bash
python mqtt_simulator.py
```

The monitor receives messages from:

```text
iot/devices/+/telemetry
```

To view published alerts in another terminal:

```bash
mosquitto_sub -h localhost -t 'iot/devices/+/alert' -v
```

## Features

- Connects to an MQTT broker.
- Receives telemetry from multiple devices.
- Reads and validates JSON messages.
- Displays temperature, humidity, and battery readings.
- Keeps a summary of known devices.
- Shows the time since each device last sent a message.
- Detects inactive devices and reports when they come back online.
- Publishes alerts for high temperature, high humidity, and low battery.
- Allows alert thresholds and inactivity timeout to be changed.
- Can save telemetry and alerts to a log file.
- Attempts to reconnect if the MQTT connection is lost.
- Stops safely when Ctrl+C is pressed.

## Alert thresholds

The default alert limits are:

- Temperature above 30°C: `HIGH_TEMPERATURE`
- Humidity above 80%: `HIGH_HUMIDITY`
- Battery below 20%: `LOW_BATTERY`

Alerts are published to:

```text
iot/devices/DEVICE_ID/alert
```

## Stopping the program

Press `Ctrl+C` to stop the monitor.

## Testing

The application was tested using the provided Python MQTT simulator.

The following tests were performed:

- Connected to the local Mosquitto broker.
- Received telemetry from multiple devices.
- Parsed incoming JSON messages.
- Detected high temperature, high humidity, and low battery.
- Published alerts to the corresponding MQTT topics.
- Tested the alert messages using mosquitto_sub.
- Tested stopping the application with Ctrl+C.

The application successfully received telemetry and published alerts during testing.

## Known limitations

- The application supports a maximum of 100 devices.
- Device information is stored in memory and is lost when the application exits.
- Alerts are published with QoS 0.
- The application uses fixed default alert thresholds unless changed through command-line options.
