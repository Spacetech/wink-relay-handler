wink-handler
============

This is a simple app that can be run on a Wink Relay to turn it into a generic MQTT device. It will send button pushes and sensor data to the configured MQTT server, and will accept commands to turn on and off the built-in relays.

Download
--------

Grab the zip file from https://github.com/mjg59/wink-relay-handler/releases and extract it.

Building
--------

Note: you don't need to do this if you've downloaded the binary - just use the wink-handler file from the zip archive.

You'll need the Android NDK installed. Run ANDROID_NDK=/path/to/android/Ndk make

Installing
----------

You'll need adb access to a rooted Wink Relay. Disable the existing Wink control software by running

```
pm disable com.quirky.android.wink.projectone
```

as root. Remount /system read-write:

```
mount -o rw,remount /system
```

Delete /system/bin/edisonwink:

```
rm /system/bin/edisonwink
```

adb push wink-handler to /sdcard and then copy it over edisonwink and fix permissions:

```
cp /sdcard/wink-handler /system/bin/edisonwink
chmod 755 /system/bin/edisonwink
```

Now write a config file that looks something like:

```
host=192.168.1.5
port=1883
user=username
password=password
clientid=Wink_Relay1
topic_prefix=Relay1
screen_timeout=20
startup_power_on=1
enable_upper_button=1
enable_lower_button=1
proximity_threshold=5000
```
and put that in /sdcard/mqtt.ini on the Wink Relay.

host: Hostname or IP address of the MQTT broker  
port: Port of the MQTT broker  
user: Username used to authenticate to the MQTT broker (optional)  
password: Password used to authenticate to the MQTT broker (optional)  
clientid: Client ID passed to the broker (optional - Wink_Relay if not provided)  
topic_prefix: Prefix to the topics presented by the device (optional - Relay if not provided)  
screen_timeout: Time in seconds until the screen turns off after a touch or proximity detection (optional - 10s if not provided)
startup_power_on: Set to 1 if you want the relays to turn on when the device powers up
enable_upper_button: Set to 1 if you want the upper button to toggle the upper relay
enable_lower_button: Set to 1 if you want the lower button to toggle the lower relay
proximity_threshold: Proximity sensor threshold - Defaults to 5000

Finally, reset your Relay.

```
reboot
```

It will then reboot - the relays will default to on post-boot.

Sensors
-------

Temperature information will be posted to the

```
Relay/sensors/temperature
```

state topic. Humidity information will be posted to the

```
Relay/sensors/humidity
```

state topic.

Buttons
-------

Button presses will be posted to

```
Relay/switches/upper
```

and

```
Relay/switches/lower
```

state topics.


Relays
------

Relays can be controlled via the

```
Relay/relays/upper
```

and

```
Relay/relays/lower
```

command topics, and will report their state to the

```
Relay/relays/upper_state
```

and

```
Relay/relays/lower_state
```

state topics.


Screen
------

The screen will report its state to the following topic

```
Relay/screen/state
```


Screen control
--------------

The screen will automatically turn on if the screen is touched and off 10 seconds later. It will also turn on and remain on if the proximity sensor is triggered, turning off 10 seconds after the last proximity detection.

Debugging
--------------

Debug logs are written to logcat

```
adb logcat
```

Look for lines starting with with D/WinkHandler or E/WinkHandler
