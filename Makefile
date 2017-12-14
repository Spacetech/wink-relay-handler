CXX=${ANDROID_NDK}/toolchains/arm-linux-androideabi-4.9/prebuilt/linux-x86_64/bin/arm-linux-androideabi-g++
CPPFLAGS=-D__ANDROID_API__=18 -std=c++11 -IMQTTPacket/src -IMQTTClient/src -IMQTTClient/src/linux --sysroot ${ANDROID_NDK}/my-android-toolchain/sysroot/ -g
LDFLAGS=-D__ANDROID_API__=18 -llog
CXXFLAGS=-D__ANDROID_API__=18

MQTTPACKET=MQTTPacket/src/MQTTFormat.c MQTTPacket/src/MQTTPacket.c MQTTPacket/src/MQTTDeserializePublish.c MQTTPacket/src/MQTTConnectClient.c MQTTPacket/src/MQTTSubscribeClient.c MQTTPacket/src/MQTTSerializePublish.c MQTTPacket/src/MQTTConnectServer.c MQTTPacket/src/MQTTSubscribeServer.c MQTTPacket/src/MQTTUnsubscribeServer.c MQTTPacket/src/MQTTUnsubscribeClient.c

MQTTCLIENT=MQTTClient/src/linux/linux.cpp

wink-handler: wink-handler.cpp ini.c ${MQTTPACKET} ${MQTTCLIENT}

clean:
	rm -f wink-handler
