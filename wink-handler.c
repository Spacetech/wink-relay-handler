#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <MQTTClient.h>
#include <android/log.h>

#include "ini.h"

#define LOG_TAG "WinkHandler"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MQTT_BUFFER_SIZE 256

struct configuration {
	char *username;
	char *password;
	char *host;
	char *clientid;
	char *topic_prefix;
	int port;
	int screen_timeout;
	int startup_power_on;
	int enable_upper_button;
	int enable_lower_button;
	int proximity_threshold;
};

static struct configuration config;

enum Relay {
	RelayUpper,
	RelayLower
};

void set_relay(enum Relay relay, int on) {
	signal(SIGPIPE, SIG_IGN);

	int fd = open(relay == RelayUpper ? "/sys/class/gpio/gpio203/value" : "/sys/class/gpio/gpio204/value", O_RDWR);
	char power = on == 1 ? '1' : '0';

	write(fd, &power, 1);
	close(fd);	
}

void handle_relay_upper(MessageData *md)
{
	MQTTMessage *message = md->message;

	LOGD("Received upper relay message - '%s' [length: %d]", message->payload, message->payloadlen);
	
	if (strncmp(message->payload, "ON", message->payloadlen) == 0) {
		set_relay(RelayUpper, 1);
	} else if (strncmp(message->payload, "OFF", message->payloadlen) == 0) {
		set_relay(RelayUpper, 0);
	}
}

void handle_relay_lower(MessageData *md)
{
	MQTTMessage *message = md->message;
	
	LOGD("Received lower relay message - '%s' [length: %d]", message->payload, message->payloadlen);

	if (strncmp(message->payload, "ON", message->payloadlen) == 0) {
		set_relay(RelayLower, 1);
	} else if (strncmp(message->payload, "OFF", message->payloadlen) == 0) {
		set_relay(RelayLower, 0);
	}
}

int mqtt_connect(Network *n, MQTTClient *c, char *buf, char *readbuf) {
	int ret;
	MQTTPacket_connectData data = MQTTPacket_connectData_initializer;

	LOGD("Initializing network");
	
	NetworkInit(n);

	if (config.host == NULL || config.port == 0) {
		LOGE("Invalid mqtt configuration");
		return 1;
	}

	LOGD("Connecting to network");

	ret = NetworkConnect(n, config.host, config.port);
	if (ret < 0) {
		LOGE("Network connect failed - %d", ret);
		return ret;
	}

	MQTTClientInit(c, n, 1000, buf, MQTT_BUFFER_SIZE, readbuf, MQTT_BUFFER_SIZE);

	data.willFlag = 0;
	data.MQTTVersion = 4;
	if (config.clientid != NULL) {
		data.clientID.cstring = config.clientid;
	} else {
		data.clientID.cstring = "Wink_Relay";
	}
	data.keepAliveInterval = 10;
	data.cleansession = 1;
	if (config.username != NULL) {
		data.username.cstring = config.username;
	}
	if (config.password != NULL) {
		data.password.cstring = config.password;
	}
	
	LOGD("Connecting to mqtt");
	
	ret = MQTTConnect(c, &data);
	
	if (ret < 0) {
		LOGE("MQTT connect failed - %d", ret);
		return ret;
	}
	
	LOGD("MQTT connected!");

	return 0;
}

void mqtt_connect_loops(Network *n, MQTTClient *c, char *buf, char *readbuf, char *upperTopic, char *lowerTopic) {
	LOGD("MQTT connect loops - start");
		
	while (mqtt_connect(n, c, buf, readbuf) != 0) {
		sleep(10);
	}

	LOGD("Subscribing - '%s'", upperTopic);
	while (MQTTSubscribe(c, upperTopic, 0, handle_relay_upper) != 0) {
		LOGD("Failed to subscribe to relays/upper");
		sleep(10);
	}
	
	LOGD("Subscribing - '%s'", lowerTopic);
	while (MQTTSubscribe(c, lowerTopic, 0, handle_relay_lower) != 0) {
		LOGD("Failed to subscribe to relays/lower");
		sleep(10);
	}
	
	LOGD("MQTT connect loops - end");
}

static int config_handler(void* data, const char* section, const char* name,
			  const char* value)
{
	if (strcmp(name, "user") == 0) {
		config.username = strdup(value);
	} else if (strcmp(name, "password") == 0) {
		config.password = strdup(value);
	} else if (strcmp(name, "clientid") == 0) {
		config.clientid = strdup(value);
	} else if (strcmp(name, "topic_prefix") == 0) {
		config.topic_prefix = strdup(value);
	} else if (strcmp(name, "host") == 0) {
		config.host = strdup(value);
	} else if (strcmp(name, "port") == 0) {
		config.port = atoi(value);
	} else if (strcmp(name, "screen_timeout") == 0) {
		config.screen_timeout = atoi(value);
	} else if (strcmp(name, "startup_power_on") == 0) {
		config.startup_power_on = atoi(value);
	} else if (strcmp(name, "enable_upper_button") == 0) {
		config.enable_upper_button = atoi(value);
	} else if (strcmp(name, "enable_lower_button") == 0) {
		config.enable_lower_button = atoi(value);
	} else if (strcmp(name, "proximity_threshold") == 0) {
		config.proximity_threshold = atoi(value);
	}	
	
	return 1;
}

int main() {
	int ret;
	int uswitch, lswitch, input, screen, relay1, relay2, temp, humid, prox;
	int uswitchstate=1;
	int lswitchstate=1;
	char urelaystate=' ';
	char lrelaystate=' ';
	int last_input = time(NULL);
	struct input_event event;
	unsigned char buf[MQTT_BUFFER_SIZE], readbuf[MQTT_BUFFER_SIZE];
	char buffer[30];
	char power = '1';
	char payload[30];
	char proxdata[100];
	int temperature, humidity, last_temperature = -1, last_humidity = -1;
	long proximity;
	Network n;
	MQTTClient c;
	MQTTMessage message;
	struct rlimit limits;
	char *prefix, topic[1024], upperTopic[1024], lowerTopic[1024];
	int timeout;

	LOGD("Main");
	
	if (ini_parse("/sdcard/mqtt.ini", config_handler, NULL) < 0) {
		LOGD("Can't load /sdcard/mqtt.ini");
		return 1;
	}

	if (config.topic_prefix != NULL) {
		prefix = config.topic_prefix;
	} else {
		prefix = strdup("Relay");
	}

	if (config.screen_timeout == 0) {
		timeout = 10;
	} else {
		timeout = config.screen_timeout;
	}
	
	if (config.proximity_threshold == 0) {
		config.proximity_threshold = 5000;
	}
	
	LOGD("Configuration:");
	LOGD("\tUsername: %s", config.username);
	LOGD("\tPassword length: %d", strlen(config.password));
	LOGD("\tHost: %s", config.host);
	LOGD("\tClient id: %s", config.clientid);
	LOGD("\tTopic prefix: %s", config.topic_prefix);
	LOGD("\tPort: %d", config.port);
	LOGD("\tStartup power on: %d", config.startup_power_on);
	LOGD("\tEnable upper button: %d", config.enable_upper_button);
	LOGD("\tEnable lower button: %d", config.enable_lower_button);
	LOGD("\tProximity threshold: %d", config.proximity_threshold);
	
	LOGD("Opening devices...");

	uswitch = open("/sys/class/gpio/gpio8/value", O_RDONLY);
	lswitch = open("/sys/class/gpio/gpio7/value", O_RDONLY);
	screen = open("/sys/class/gpio/gpio30/value", O_RDWR);
	relay1 = open("/sys/class/gpio/gpio203/value", O_RDWR);
	relay2 = open("/sys/class/gpio/gpio204/value", O_RDWR);
	input = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	temp = open("/sys/bus/i2c/devices/2-0040/temp1_input", O_RDONLY);
	humid = open("/sys/bus/i2c/devices/2-0040/humidity1_input", O_RDONLY);
	prox = open("/sys/devices/platform/imx-i2c.2/i2c-2/2-005a/input/input3/ps_input_data", O_RDONLY);

	if (config.startup_power_on == 1) {
		write(relay1, &power, 1);
		write(relay2, &power, 1);
	}

	lseek(screen, 0, SEEK_SET);
	read(screen, &power, sizeof(power));

	limits.rlim_cur = RLIM_INFINITY;
	limits.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_CORE, &limits);
	
	sprintf(upperTopic, "%s/relays/upper", prefix);
	sprintf(lowerTopic, "%s/relays/lower", prefix);
	
	mqtt_connect_loops(&n, &c, buf, readbuf, upperTopic, lowerTopic);

	while (1) {
		lseek(relay1, 0, SEEK_SET);
		read(relay1, buffer, sizeof(buffer));
		if (buffer[0] != urelaystate) {
			message.qos = 1;
			message.payload = payload;
			if (buffer[0] == '0') {
				sprintf(payload, "OFF");
			} else {
				sprintf(payload, "ON");
			}
			message.payloadlen = strlen(payload);
			sprintf(topic, "%s/relays/upper_state", prefix);
			MQTTPublish(&c, topic, &message);
			urelaystate = buffer[0];
			
			LOGD("Relay changed state - upper");
		}

		lseek(relay2, 0, SEEK_SET);
		read(relay2, buffer, sizeof(buffer));
		if (buffer[0] != lrelaystate) {
			message.qos = 1;
			message.payload = payload;
			if (buffer[0] == '0') {
				sprintf(payload, "OFF");
			} else {
				sprintf(payload, "ON");
			}
			message.payloadlen = strlen(payload);
			sprintf(topic, "%s/relays/lower_state", prefix);
			MQTTPublish(&c, topic, &message);
			lrelaystate = buffer[0];
			
			LOGD("Relay changed state - lower");
		}

		lseek(uswitch, 0, SEEK_SET);
		read(uswitch, buffer, sizeof(buffer));
		if (buffer[0] == '0' && uswitchstate == 1) {
			uswitchstate = 0;
			message.qos = 1;
			message.payload = payload;
			sprintf(payload, "on");
			message.payloadlen = strlen(payload);
			sprintf(topic, "%s/switches/upper", prefix);
			MQTTPublish(&c, topic, &message);
			
			LOGD("Switch changed state - upper");
			
			if (config.enable_upper_button == 1) {
				set_relay(RelayUpper, urelaystate == '0' ? 1 : 0);
			}
			
		} else if (buffer[0] == '1' && uswitchstate == 0) {
			uswitchstate = 1;
		}
		
		lseek(lswitch, 0, SEEK_SET);
		read(lswitch, buffer, sizeof(buffer));
		if (buffer[0] == '0' && lswitchstate == 1) {
			lswitchstate = 0;
			message.qos = 1;
			message.payload = payload;
			sprintf(payload, "on");
			message.payloadlen = strlen(payload);
			sprintf(topic, "%s/switches/lower", prefix);
			MQTTPublish(&c, topic, &message);
			
			LOGD("Switch changed state - lower");
			
			if (config.enable_lower_button == 1) {
				set_relay(RelayLower, lrelaystate == '0' ? 1 : 0);
			}
			
		} else if (buffer[0] == '1' && lswitchstate == 0) {
			lswitchstate = 1;
		}

		lseek(temp, 0, SEEK_SET);
		read(temp, buffer, sizeof(buffer));
		temperature = atoi(buffer);
		if (abs(temperature - last_temperature) > 100) {
			message.qos = 1;
			message.payload = payload;
			sprintf(payload, "%f", temperature/1000.0);
			message.payloadlen = strlen(payload);
			sprintf(topic, "%s/sensors/temperature", prefix);
			MQTTPublish(&c, topic, &message);
			last_temperature = temperature;
		}

		lseek(humid, 0, SEEK_SET);
		read(humid, buffer, sizeof(buffer));
		humidity = atoi(buffer);
		if (abs(humidity - last_humidity) > 100) {
			message.qos = 1;
			message.payload = payload;
			sprintf(payload, "%f", humidity/1000.0);
			message.payloadlen = strlen(payload);
			sprintf(topic, "%s/sensors/humidity", prefix);
			MQTTPublish(&c, topic, &message);
			last_humidity = humidity;
		}

		lseek(prox, 0, SEEK_SET);
		read(prox, proxdata, sizeof(proxdata));
		proximity = strtol(proxdata, NULL, 10);
		if (proximity >= config.proximity_threshold) {
			last_input = time(NULL);
			if (power != '1') {
				power = '1';
				write(screen, &power, sizeof(power));
			}
		}

		while (read(input, &event, sizeof(event)) > 0) {
			last_input = time(NULL);
			if (power != '1') {
				power = '1';
				write(screen, &power, sizeof(power));
			}
		}
		
		if ((time(NULL) - last_input > timeout) && power == '1') {
			power = '0';
			write(screen, &power, sizeof(power));
		}
		
		ret = MQTTYield(&c, 100);
		if (ret != 0) {
			LOGE("MQTT yield failed - %d - going to reconnect", ret);
			mqtt_connect_loops(&n, &c, buf, readbuf, upperTopic, lowerTopic);
		}
	}
}
