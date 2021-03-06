# motor-on-roller-blind-ws
WebSocket based version of [motor-on-roller-blind](https://github.com/nidayand/motor-on-roller-blind). I.e. there is no need of an MQTT server but MQTT is supported as well - you can control it with WebSockets and/or with MQTT messages.

3d parts for printing are available on Thingiverse.com: ["motor on a roller blind"](https://www.thingiverse.com/thing:2392856)

 1. A tiny webserver is setup on the esp8266 that will serve one page to the client
 2. Upon powering on the first time WIFI credentials, a hostname and - optional - MQTT server details is to be configured. Connect your computer to a new WIFI hotspot named **BlindsConnectAP**. Password = **nidayand**
 3. Connect to your normal WIFI with your client and go to the IP address of the device - or if you have an mDNS supported device (e.g. iOS, OSX or have Bonjour installed) you can go to http://{hostname}.local. If you don't know the IP-address of the device check your router for the leases (or check the serial console in the Arduino IDE or check the `/raw/esp8266/register` MQTT message if you are using an MQTT server)
 4. As the webpage is loaded it will connect through a websocket directly to the device to progress updates and to control the device. If any other client connects the updates will be in sync.
 5. Go to the Settings page to calibrate the motor with the start and end positions of the roller blind. Follow the instructions on the page

# MQTT
- When it connects to WIFI and MQTT it will send a "register" message to topic `/raw/esp8266/register` with a payload containing chip-id and IP-address
- A message to `/raw/esp8266/[chip-id]/in` will steer the blind according to the "payload actions" below
- Updates from the device will be sent to topic `/raw/esp8266/[chip-id]/out`

### If you don't want to use MQTT
Simply do not enter any string in the MQTT server form field upon WIFI configuration of the device (step 3 above)

## Payload options
- `(start)` - (calibrate) Sets the current position as top position
- `(max)` - (calibrate) Sets the current position as max position. Set `start` before you define `max` as `max` is a relative position to `start`
- `(0)` - (manual mode) Will stop the curtain
- `(-1)` - (manual mode) Will open the curtain. Requires `(0)` to stop the motor
- `(1)`- (manual mode) Will close the curtain. Requires `(0)` to stop the motor
- `0-100` - (auto mode) A number between 0-100 to set % of opened blind. Requires calibration before use. E.g. `50` will open it to 50%

![enter image description here](https://user-images.githubusercontent.com/2181965/31178217-a5351678-a918-11e7-9611-3e8256c873a4.png) ![enter image description here](https://user-images.githubusercontent.com/2181965/31178216-a4f7194a-a918-11e7-85dd-8e189cfc031c.png)

## TODO:
* Refactor code. Break out code into smaller parts
* Properly push state of MotorSpeed to web-gui
* Implement a generic way of pushing config back to web gui
* Add feature to support admin/password of mqtt (first setup)
* Add feature to support mqtt over ssl (first setup)
* Add the following to web-gui settings:
    * Change mqtt topic
    * Change pins used by stepper motor
    * Button for OTA update
* Use wifi mesh for devices: See: https://github.com/PhracturedBlue/ESP8266MQTTMesh
* Enable OTA software updates through a webserver. See http://arduino-esp8266.readthedocs.io/en/latest/ota_updates/readme.html#http-server

## Upload firmware:

```
esptool.py --port /dev/cu.wchusbserial1420 --baud 115200 write_flash 0x00000 /Users/robin/Downloads/nodemcu-master-9-modules-2017-12-29-19-08-00-float.bin
```
