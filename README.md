# smartbms123-mqtt
I have build a LiFePO4 battery pack using [SmartBMS/123](https://123electric.eu/products/123smartbms-gen3/) as BMS. This BMS works fine but only uses a Bluetooth app to read data. This in not sufficient to fully understand how your pack is performing. 

This project is using an Arduino ESP32 connected to the SmartBMS/123 and publish all available data to a Mqtt-server. The data can later by used by, for example, HomeAssistant.

The code for decoding data from the SmartBMS is stolen from [TheRealKasumi](https://github.com/TheRealKasumi/123SmartBMS-Arduino), who did a fantastic work on how to calculate checksum and decode voltage and temperatures.

## Software
The 'end' module on the SmartBMS/123 array has a two pin extension port. The 'ext' pin is the actual data and the 'out' pin is ground. From here a 58 byte frame is sent every second at 9600 baud. Each frame has information about the battery pack as a whole, and also information from one cell. Which specific cell that provides its data is cycled so first frame contain data for cell 1, next frame is data for cell 2 and so on.

If the ESP is not able to connect to a wifi, it will enter configuration mode. This means that a network is created called 'configure-<mac>' (AP). Connect to that wifi and set up wifi, mqtt-server, etc.
You can also update the software in the ESP via network, using ArduinoOTA. To be able to debug you can telnet into the ESP.

When telneting into the ESP you can also send a few commands:

's' - show status<br>
'r' - reboot<br>
'c' - start configuration mode<br>
'q' - disconnect<br>

Each cycle data about the specific cell is published on the mqtt-server. General data about the pack, such as SoC, current, voltage is randomly published.

## Hardware
The ESP32 used is [AZDelivery Freenove Wroom](https://www.amazon.com/FREENOVE-ESP32-WROOM-Compatible-Wireless-Detailed/dp/B0C9THDPXP) but any dev board can be used.

## ESP32
![alt tag](/img/esp.jpg)
Connect pin 12 from the ESP board to 'EXT' on SmartBMS/123 'ext' in.

## Battery pack
This is an example of how I use SmartBMS/123 and Victron MultiPlus-II. The bank is a 40kWh LiFePO4 battery array (16s3p)
![alt tag](/img/pack.jpg)

## Homassistant and Grafana
Here is an example on how HomeAssistant can look to replace the SmartBMS/123 Bluetooth app. Also, I have had great benefit from Grafana when balancing the batteries and to see how they are affected over time.

For example you can configure all mqtt message something lite this:

```
mqtt:
  sensor:
    - name: "SmartBms cell 1 temperature"
      object_id: "smartbms_temperature_1"
      state_topic: "smartbms123/5CCF7FF0A403/temperature/1"
      value_template: '{{value | round(0) }}'
      expire_after: 900
      unit_of_measurement: '°C'
      device_class: temperature
      icon: mdi:home-thermometer-outline
      qos: 1

    - name: "SmartBms Pack voltage"
      object_id: "smartbms_pack_voltage"
      state_topic: "smartbms123/5CCF7FF0A403/pack-voltage"
      expire_after: 900
      unit_of_measurement: 'V'
      device_class: voltage
      icon: mdi:power
      qos: 1

... and so on ...
```
![alt tag](/img/homeassistant.jpg)

![alt tag](/img/grafana.jpg)
