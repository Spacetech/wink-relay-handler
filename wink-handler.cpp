#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <android/log.h>

#include "ini.h"
#include "MQTTClient.h"
#include "linux.cpp"

#define LOG_TAG "WinkHandler"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct Configuration
{
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

static struct Configuration config;

enum class Relay
{
	Upper,
	Lower
};

void setRelay(Relay relay, bool on)
{
	signal(SIGPIPE, SIG_IGN);

	int fd = open(relay == Relay::Upper ? "/sys/class/gpio/gpio203/value" : "/sys/class/gpio/gpio204/value", O_RDWR);
	char screenPower = on == true ? '1' : '0';

	write(fd, &screenPower, 1);
	close(fd);
}

void onTopicMessage(Relay relay, char *payloadMessage, int payloadLength)
{
	LOGD("MQTT - Received %s relay message - '%s' [length: %d]", relay == Relay::Upper ? "upper" : "lower", payloadMessage, payloadLength);

	if (strncmp(payloadMessage, "ON", payloadLength) == 0)
	{
		setRelay(relay, true);
	}
	else if (strncmp(payloadMessage, "OFF", payloadLength) == 0)
	{
		setRelay(relay, false);
	}
}

void onUpperTopicMessageReceived(MQTT::MessageData &md)
{
	MQTT::Message &message = md.message;
	onTopicMessage(Relay::Upper, (char *)message.payload, message.payloadlen);
}

void onLowerTopicMessageReceived(MQTT::MessageData &md)
{
	MQTT::Message &message = md.message;
	onTopicMessage(Relay::Lower, (char *)message.payload, message.payloadlen);
}

static int config_handler(void *data, const char *section, const char *name, const char *value)
{
	if (strcmp(name, "user") == 0)
	{
		config.username = strdup(value);
	}
	else if (strcmp(name, "password") == 0)
	{
		config.password = strdup(value);
	}
	else if (strcmp(name, "clientid") == 0)
	{
		config.clientid = strdup(value);
	}
	else if (strcmp(name, "topic_prefix") == 0)
	{
		config.topic_prefix = strdup(value);
	}
	else if (strcmp(name, "host") == 0)
	{
		config.host = strdup(value);
	}
	else if (strcmp(name, "port") == 0)
	{
		config.port = atoi(value);
	}
	else if (strcmp(name, "screen_timeout") == 0)
	{
		config.screen_timeout = atoi(value);
	}
	else if (strcmp(name, "startup_power_on") == 0)
	{
		config.startup_power_on = atoi(value);
	}
	else if (strcmp(name, "enable_upper_button") == 0)
	{
		config.enable_upper_button = atoi(value);
	}
	else if (strcmp(name, "enable_lower_button") == 0)
	{
		config.enable_lower_button = atoi(value);
	}
	else if (strcmp(name, "proximity_threshold") == 0)
	{
		config.proximity_threshold = atoi(value);
	}

	return 1;
}

void publishMessage(MQTT::Client<IPStack, Countdown> *client, const char *topic, const char *payload, bool retain)
{
	if (client->isConnected())
	{
		MQTT::Message message;
		message.qos = MQTT::QOS1;
		message.retained = retain;
		message.dup = false;
		message.payload = (void *)payload;
		message.payloadlen = strlen(payload);

		int rc;
		if ((rc = client->publish(topic, message)) != 0)
		{
			LOGE("Failed to publish message for topic '%s' - %d", topic, rc);
		}
	}
}

int main()
{
	int rc;
	int upperSwitch, lowerSwitch, input, screen, upperRelay, lowerRelay, temp, humid, prox;
	bool upperSwitchState = true;
	bool lowerSwitchState = true;
	char upperRelayState = ' ';
	char lowerRelayState = ' ';
	int last_input = time(NULL);
	struct input_event event;
	char buffer[30];
	char screenPower = '1';
	char payload[30];
	char proxdata[100];
	int temperature, humidity, last_temperature = -1, last_humidity = -1;
	long proximity;
	int failedConnectionAttempts = 0;
	struct rlimit limits;
	char topic[1024], upperTopic[1024], lowerTopic[1024];

	LOGD("Main");

	if (ini_parse("/sdcard/mqtt.ini", config_handler, NULL) < 0)
	{
		LOGD("Can't load /sdcard/mqtt.ini");
		return 1;
	}

	if (config.topic_prefix == NULL)
	{
		config.topic_prefix = (char *)"Relay";
	}

	if (config.screen_timeout == 0)
	{
		config.screen_timeout = 10;
	}

	if (config.proximity_threshold == 0)
	{
		config.proximity_threshold = 5000;
	}

	LOGD("Configuration:");
	LOGD("\tUsername: %s", config.username);
	LOGD("\tPassword length: %d", strlen(config.password));
	LOGD("\tHost: %s", config.host);
	LOGD("\tClient id: %s", config.clientid);
	LOGD("\tTopic prefix: %s", config.topic_prefix);
	LOGD("\tPort: %d", config.port);
	LOGD("\tStartup screenPower on: %d", config.startup_power_on);
	LOGD("\tEnable upper button: %d", config.enable_upper_button);
	LOGD("\tEnable lower button: %d", config.enable_lower_button);
	LOGD("\tProximity threshold: %d", config.proximity_threshold);

	LOGD("Opening devices...");

	upperSwitch = open("/sys/class/gpio/gpio8/value", O_RDONLY);
	lowerSwitch = open("/sys/class/gpio/gpio7/value", O_RDONLY);
	screen = open("/sys/class/gpio/gpio30/value", O_RDWR);
	upperRelay = open("/sys/class/gpio/gpio203/value", O_RDWR);
	lowerRelay = open("/sys/class/gpio/gpio204/value", O_RDWR);
	input = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	temp = open("/sys/bus/i2c/devices/2-0040/temp1_input", O_RDONLY);
	humid = open("/sys/bus/i2c/devices/2-0040/humidity1_input", O_RDONLY);
	prox = open("/sys/devices/platform/imx-i2c.2/i2c-2/2-005a/input/input3/ps_input_data", O_RDONLY);

	if (config.startup_power_on == 1)
	{
		LOGD("Startup device screenPower on");

		write(upperRelay, &screenPower, 1);
		write(lowerRelay, &screenPower, 1);
	}

	lseek(screen, 0, SEEK_SET);
	read(screen, &screenPower, sizeof(screenPower));

	limits.rlim_cur = RLIM_INFINITY;
	limits.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_CORE, &limits);

	sprintf(upperTopic, "%s/relays/upper", config.topic_prefix);
	sprintf(lowerTopic, "%s/relays/lower", config.topic_prefix);

	IPStack ipstack = IPStack();

	MQTT::Client<IPStack, Countdown> client = MQTT::Client<IPStack, Countdown>(ipstack, 2000);

	MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
	data.MQTTVersion = 4;
	data.willFlag = 0;
	data.keepAliveInterval = 10;
	data.cleansession = 1;

	data.clientID.cstring = config.clientid != NULL ? config.clientid : (char *)"Wink_Relay";

	if (config.username != NULL)
	{
		data.username.cstring = config.username;
	}

	if (config.password != NULL)
	{
		data.password.cstring = config.password;
	}

	while (1)
	{
		lseek(upperRelay, 0, SEEK_SET);
		read(upperRelay, buffer, sizeof(buffer));
		if (upperRelayState != buffer[0])
		{
			upperRelayState = buffer[0];

			LOGD("Relay changed state - upper");

			sprintf(topic, "%s/relays/upper_state", config.topic_prefix);
			sprintf(payload, buffer[0] == '0' ? "OFF" : "ON");
			publishMessage(&client, topic, payload, true);
		}

		lseek(lowerRelay, 0, SEEK_SET);
		read(lowerRelay, buffer, sizeof(buffer));
		if (lowerRelayState != buffer[0])
		{
			lowerRelayState = buffer[0];

			LOGD("Relay changed state - lower");

			sprintf(topic, "%s/relays/lower_state", config.topic_prefix);
			sprintf(payload, buffer[0] == '0' ? "OFF" : "ON");
			publishMessage(&client, topic, payload, true);
		}

		lseek(upperSwitch, 0, SEEK_SET);
		read(upperSwitch, buffer, sizeof(buffer));
		if (upperSwitchState != (buffer[0] == '1'))
		{
			upperSwitchState = buffer[0] == '1';

			if (upperSwitchState)
			{
				LOGD("Switch changed state - upper");

				sprintf(topic, "%s/switches/upper", config.topic_prefix);
				sprintf(payload, "ON");
				publishMessage(&client, topic, payload, false);

				if (config.enable_upper_button == 1)
				{
					setRelay(Relay::Upper, upperRelayState == '0');
				}
			}
		}

		lseek(lowerSwitch, 0, SEEK_SET);
		read(lowerSwitch, buffer, sizeof(buffer));
		if (lowerSwitchState != (buffer[0] == '1'))
		{
			lowerSwitchState = buffer[0] == '1';

			if (lowerSwitchState)
			{
				LOGD("Switch changed state - lower");

				sprintf(topic, "%s/switches/lower", config.topic_prefix);
				sprintf(payload, "ON");
				publishMessage(&client, topic, payload, false);

				if (config.enable_lower_button == 1)
				{
					setRelay(Relay::Lower, lowerRelayState == '0');
				}
			}
		}

		lseek(temp, 0, SEEK_SET);
		read(temp, buffer, sizeof(buffer));
		temperature = atoi(buffer);
		if (abs(temperature - last_temperature) > 100)
		{
			last_temperature = temperature;

			sprintf(topic, "%s/sensors/temperature", config.topic_prefix);
			sprintf(payload, "%f", temperature / 1000.0);
			publishMessage(&client, topic, payload, true);
		}

		lseek(humid, 0, SEEK_SET);
		read(humid, buffer, sizeof(buffer));
		humidity = atoi(buffer);
		if (abs(humidity - last_humidity) > 100)
		{
			last_humidity = humidity;

			sprintf(topic, "%s/sensors/humidity", config.topic_prefix);
			sprintf(payload, "%f", humidity / 1000.0);
			publishMessage(&client, topic, payload, true);
		}

		lseek(prox, 0, SEEK_SET);
		read(prox, proxdata, sizeof(proxdata));
		proximity = strtol(proxdata, NULL, 10);
		if (proximity >= config.proximity_threshold)
		{
			last_input = time(NULL);
		}

		while (read(input, &event, sizeof(event)) > 0)
		{
			last_input = time(NULL);
		}

		bool shouldTurnOnScreen = (time(NULL) - last_input) <= config.screen_timeout;
		if (shouldTurnOnScreen && screenPower != '1')
		{
			screenPower = '1';
			write(screen, &screenPower, sizeof(screenPower));

			LOGD("Screen state changed - on");

			sprintf(topic, "%s/screen/state", config.topic_prefix);
			sprintf(payload, "ON");
			publishMessage(&client, topic, payload, true);
		}
		else if (!shouldTurnOnScreen && screenPower == '1')
		{
			screenPower = '0';
			write(screen, &screenPower, sizeof(screenPower));

			LOGD("Screen state changed - off");

			sprintf(topic, "%s/screen/state", config.topic_prefix);
			sprintf(payload, "OFF");
			publishMessage(&client, topic, payload, true);
		}

		if (client.isConnected())
		{
			client.yield(100);
		}
		else
		{
			LOGD("IPStack - Connecting...");

			ipstack.disconnect();

			while ((rc = ipstack.connect(config.host, config.port)) != 0)
			{
				LOGE("IPStack - Failed to connect - %d\n", rc);
				usleep(50000);
				continue;
			}

			LOGD("MQTT - Connecting...");

			if ((rc = client.connect(data)) == 0)
			{
				LOGD("MQTT - Connected");

				rc = client.subscribe(upperTopic, MQTT::QOS2, onUpperTopicMessageReceived);
				if (rc == 0)
				{
					rc = client.subscribe(lowerTopic, MQTT::QOS2, onLowerTopicMessageReceived);
					if (rc == 0)
					{
						failedConnectionAttempts = 0;

						LOGD("MQTT - All ready!");
					}
					else
					{
						LOGE("MQTT - Failed to subscribe to '%s' - %d\n", lowerTopic, rc);
						client.disconnect();
						usleep(50000);
					}
				}
				else
				{
					LOGE("MQTT - Failed to subscribe to '%s' - %d\n", upperTopic, rc);
					client.disconnect();
					usleep(50000);
				}
			}
			else
			{
				LOGE("MQTT - Failed to connect - %d", rc);
				failedConnectionAttempts++;

				if (failedConnectionAttempts > 5)
				{
					LOGE("MQTT - Too many failed connection attempts. Quitting...");
					return 1;
				}

				usleep(50000);
			}
		}
	}
}
