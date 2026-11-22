
import json
import random
import time

import paho.mqtt.client as mqtt

BROKER = "localhost"
PORT = 1883

DEVICES = {
    "sensor_01": {
        "temperature": 24.0,
        "humidity": 55.0,
        "battery": 85
    },
    "sensor_02": {
        "temperature": 29.0,
        "humidity": 60.0,
        "battery": 45
    },
    "sensor_03": {
        "temperature": 22.0,
        "humidity": 50.0,
        "battery": 18
    }
}


def main():
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="iot_simulator"
    )

    client.connect(BROKER, PORT, 60)
    client.loop_start()

    print("MQTT simulator started")

    try:
        while True:
            for device_id, values in DEVICES.items():

                values["temperature"] += random.uniform(-1.0, 1.5)
                values["humidity"] += random.uniform(-2.0, 2.0)
                values["humidity"] = max(
                    0, min(100, values["humidity"])
                )

                if random.random() < 0.2:
                    values["battery"] = max(
                        0, values["battery"] - 1
                    )

                # Occasionally simulate abnormal conditions
                if random.random() < 0.1:
                    values["temperature"] = random.uniform(31, 38)

                if random.random() < 0.1:
                    values["humidity"] = random.uniform(81, 95)

                payload = {
                    "device_id": device_id,
                    "temperature": round(
                        values["temperature"], 2
                    ),
                    "humidity": round(
                        values["humidity"], 2
                    ),
                    "battery": values["battery"],
                    "timestamp": int(time.time())
                }

                topic = (
                    f"iot/devices/{device_id}/telemetry"
                )

                client.publish(
                    topic,
                    json.dumps(payload),
                    qos=0
                )

                print(f"Published: {topic} -> {payload}")

            time.sleep(2)

    except KeyboardInterrupt:
        print("\nStopping simulator...")

    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()

