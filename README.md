# qk-concept-challenges

This repository contains responses of "GKConcept technical-challenges" of Thierry Baldo

## Software

Current version of ESP-IDF is used
(https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)

Code should run on any ESP32 chip

## Requirements:

* Wi-Fi Access Point
* for challenge6, a LED is connected to pin 13

## External Servers

MQTT Broker which is "mqtt://public.mqtthq.com" which doesn't need an account.

To obtain current date, NPT server "pool.ntp.org" is used.

Using this MQTT Broker may fail sometimes.

## Tests

To test MQTT, Software "mosquitto" has been used with this command:

`mosquitto_pub -h public.mqtthq.com -p 1883 -t "/bigLebowski" -d -m "who are you man ?"`
