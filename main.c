#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#include <applibs/log.h>
#include <applibs/gpio.h>
#include <applibs/adc.h>
#include <applibs/pwm.h>
#include <applibs/networking.h>
#include <applibs/storage.h>
#include <applibs/rtc.h>
#include <applibs/i2c.h>

#include <azureiot/iothub_client_core_common.h>
#include <azureiot/iothub_device_client_ll.h>
#include <azureiot/iothub_client_options.h>
#include <azureiot/iothubtransportmqtt.h>
#include <azureiot/iothub.h>
#include <azureiot/azure_sphere_provisioning.h>

#include <uev.h>
#include <sac/queue.h>

// By default, this sample's CMake build targets hardware that follows the MT3620
// Reference Development Board (RDB) specification, such as the MT3620 Dev Kit from
// Seeed Studios.
//
// To target different hardware, you'll need to update the CMake build. The necessary
// steps to do this vary depending on if you are building in Visual Studio, in Visual
// Studio Code or via the command line.
//
// See https://github.com/Azure/azure-sphere-samples/tree/master/Hardware for more details.
#include <hw/plant_sk.h>

#include "pulse.h"
#include "poll.h"
#include "climatesensor.h"
#include "soilmoisture.h"
#include "humidity.h"

#include <pthread.h>

typedef struct {
	int ADC;
	int I2cClimate;
	int Status_PWM;
	int User_PWM;
} fd_t;

/// Constants
const char iot_scope_id[] = "0ne000A8C6B";
const char data_fmt[] = "{\"meta\":{\"time\":%d,\"name\":\"plant0\"},\"data\":{\"lux\":%f,\"climate\":{\"tempurature\":%f,\"pressure\":%f,\"samples\":%d},\"soil\":{\"0x24\":%hu,\"0x26\":%hu},\"humidity\":%f}}";
const time_t upload_interval = 600;
const time_t sample_interval = 120;
const size_t packet_max = 256;
#define QUEUE_MAX 25U

/// Packet Management
static volatile sig_atomic_t terminationRequired = false;
#define STATE_NO_NETWORK 0
#define STATE_NO_CREDS 1
#define STATE_PERIODIC_UPLOAD 2
#define STATE_IDLE 3

static volatile sig_atomic_t cur_state = 0;
static volatile sig_atomic_t desired_state = 0;

SAC_QUEUE_GENERATE(q, queue_t, static, char*, QUEUE_MAX);
queue_t pkt_to_send_queue;
queue_t pkt_sent_queue;
pthread_mutex_t pkt_queue_lock;
pthread_mutex_t state_transition_lock;

typedef struct {
	uev_ctx_t* ctx;
	uev_t* move_state_w, * upload_timer_w, * poll_sensors_w;
	fd_t* fds;
	// pulse_t* wlan_pulse, * app_pulse;
	poll_t* net_rdy_poll, * azure_rdy_poll;
	IOTHUB_DEVICE_CLIENT_LL_HANDLE iot_handle;
	const char* iot_scope_id;
} on_net_rdy_t;

void set_indicator_color(int PWMfd, unsigned int red, unsigned int green, unsigned int blue) {
	PwmState state;
	state.enabled = true;
	state.dutyCycle_nsec = 0;
	state.period_nsec = 255000;
	state.polarity = PWM_Polarity_Inversed;

	state.dutyCycle_nsec = red * 100;
	PWM_Apply(PWMfd, USER_LED_RED_PWM_CHANNEL, &state);
	int err = errno;
	state.dutyCycle_nsec = green * 100;
	PWM_Apply(PWMfd, USER_LED_GREEN_PWM_CHANNEL, &state);
	state.dutyCycle_nsec = blue * 100;
	PWM_Apply(PWMfd, USER_LED_BLUE_PWM_CHANNEL, &state);
}

// TODO: ensure atomic state transitions to avoid doubling events
void move_state(uev_t* w, void* arg, int events) {
	on_net_rdy_t* data = (on_net_rdy_t*)arg;

	pthread_mutex_lock(&state_transition_lock); 
	{
		if (cur_state != desired_state) {
			// cancel all things from previous states
			uev_timer_stop(data->upload_timer_w);
			poll_stop(data->net_rdy_poll);
			poll_stop(data->azure_rdy_poll);
			// depending on the next state, start the operation
			if (desired_state == STATE_IDLE) {
				Log_Debug("Idling for data...");
				set_indicator_color(data->fds->User_PWM, 255, 0, 0);
			}
			else if (desired_state == STATE_NO_NETWORK) {
				// turn off the timer for the wlan light, and set the light solid
				Log_Debug("Internet down!\n");
				set_indicator_color(data->fds->User_PWM, 0, 255, 0);
				// start checking our connection to azure services
				poll_start(data->ctx, data->net_rdy_poll);
			}
			else if (desired_state == STATE_NO_CREDS) {
				// turn off the timer for the wlan light, and set the light solid
				// pulse_stop(data->wlan_pulse, true);
				// pulse_start(data->ctx, data->app_pulse);
				Log_Debug("Internet connected!\n");
				set_indicator_color(data->fds->User_PWM, 255, 128, 0);
				// start checking our connection to azure services
				poll_start(data->ctx, data->azure_rdy_poll);
			}
			else if (desired_state == STATE_PERIODIC_UPLOAD) {
				Log_Debug("On Azure Ready called!\n");
				// pulse_stop(data->app_pulse, true);
				// pulse_stop(data->wlan_pulse, true);
				set_indicator_color(data->fds->User_PWM, 0, 255, 0);
				uev_timer_set(data->upload_timer_w, 1, 1000 * upload_interval);
				uev_timer_start(data->upload_timer_w);
			}
			cur_state = desired_state;
		}
	}
	pthread_mutex_unlock(&state_transition_lock);
}

/// <summary>
///     Converts AZURE_SPHERE_PROV_RETURN_VALUE to a string.
/// </summary>
static const char* getAzureSphereProvisioningResultString(
	AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult)
{
	switch (provisioningResult.result) {
	case AZURE_SPHERE_PROV_RESULT_OK:
		return "AZURE_SPHERE_PROV_RESULT_OK";
	case AZURE_SPHERE_PROV_RESULT_INVALID_PARAM:
		return "AZURE_SPHERE_PROV_RESULT_INVALID_PARAM";
	case AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY:
		return "AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY";
	case AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY:
		return "AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY";
	case AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR:
		return "AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR";
	case AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR:
		return "AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR";
	default:
		return "UNKNOWN_RETURN_VALUE";
	}
}

/// <summary>
///     Converts the IoT Hub connection status reason to a string.
/// </summary>
static const char* GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason)
{
	static char* reasonString = "unknown reason";
	switch (reason) {
	case IOTHUB_CLIENT_CONNECTION_NO_PING_RESPONSE:
		reasonString = "IOTHUB_CLIENT_CONNECTION_NO_PING_RESPONSE";
		break;
	case IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN:
		reasonString = "IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN";
		break;
	case IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED:
		reasonString = "IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED";
		break;
	case IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL:
		reasonString = "IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL";
		break;
	case IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED:
		reasonString = "IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED";
		break;
	case IOTHUB_CLIENT_CONNECTION_NO_NETWORK:
		reasonString = "IOTHUB_CLIENT_CONNECTION_NO_NETWORK";
		break;
	case IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR:
		reasonString = "IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR";
		break;
	case IOTHUB_CLIENT_CONNECTION_OK:
		reasonString = "IOTHUB_CLIENT_CONNECTION_OK";
		break;
	}
	return reasonString;
}

void cleanup_exit(uev_t* w, void* arg, int events)
{
	if (UEV_ERROR != events)
		terminationRequired = true;
}

void OpenPeripherals(fd_t* fds) {
	PwmState state;
	state.enabled = true;
	state.dutyCycle_nsec = 0;
	state.period_nsec = 100000;
	state.polarity = PWM_Polarity_Inversed;

	if ((fds->ADC = ADC_Open(LIGHT_ADC_CONTROLLER)) < 0)
		Log_Debug("Failed to open ADC\n");
	if (ADC_SetReferenceVoltage(fds->ADC, LIGHT_ADC_CHANNEL, 2.5f) < 0)
		Log_Debug("Failed to set ADC reference voltage\n");

	if ((fds->I2cClimate = I2CMaster_Open(CLIMATE_I2C_CONTROLLER)) < 0)
		Log_Debug("Failed to open I2C bus for climate sensor\n");
	if (I2CMaster_SetBusSpeed(fds->I2cClimate, I2C_BUS_SPEED_STANDARD) < 0)
		Log_Debug("Failed to set I2C bus speed\n");
	if (I2CMaster_SetTimeout(fds->I2cClimate, 100) < 0)
		Log_Debug("Failed to set I2C bust timeout\n");
	if (ClimateSensorInit(fds->I2cClimate) < 0)
		Log_Debug("Failed to initialize climate sensor\n");
	if (SoilMoistureInit(fds->I2cClimate) < 0)
		Log_Debug("Failed to initialize climate sensor\n");
	if (HumidityInit(fds->I2cClimate) < 0)
		Log_Debug("Failed to initialize humidity sensor\n");

	if((fds->Status_PWM = PWM_Open(STATUS_PWM_CONTROLLER)) < 0)
		Log_Debug("Failed to open Status PWM\n");
	else {
		PWM_Apply(fds->Status_PWM, APP_STATUS_PWM_CHANNEL, &state);
		PWM_Apply(fds->Status_PWM, WLAN_STATUS_PWM_CHANNEL, &state);
	}
	if ((fds->User_PWM = PWM_Open(USER_PWM_CONTROLLER)) < 0)
		Log_Debug("Failed to open User PWM\n");
	else {
		PWM_Apply(fds->User_PWM, USER_LED_RED_PWM_CHANNEL, &state);
		PWM_Apply(fds->User_PWM, USER_LED_GREEN_PWM_CHANNEL, &state);
		PWM_Apply(fds->User_PWM, USER_LED_BLUE_PWM_CHANNEL, &state);
	}
}

int check_net_rdy(uev_t* w, void* arg, int events) {
	on_net_rdy_t* data = (on_net_rdy_t*)arg;
	if (UEV_ERROR == events) {
		Log_Debug("Error in on_net_rdy event\n");
		return -1;
	}
	bool net_is_rdy = false;
	if (Networking_IsNetworkingReady(&net_is_rdy) < 0) {
		Log_Debug("Call to network failed\n");
		return -1;
	}
	Log_Debug("Checked network!\n");

	if (net_is_rdy) {
		pthread_mutex_lock(&state_transition_lock);
		{
			desired_state = STATE_NO_CREDS;
			uev_event_post(data->move_state_w);
		}
		pthread_mutex_unlock(&state_transition_lock);
	}
	return net_is_rdy ? 1 : 0;
}

void azure_status_cb(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason,
	void* userContextCallback)
{
	on_net_rdy_t* data = (on_net_rdy_t*)userContextCallback;
	int next_state = 0;
	// TODO: add other enum values to if statements
	if (result == IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED) {
		if (reason == IOTHUB_CLIENT_CONNECTION_NO_NETWORK) {
			// restart network polling, indicating as such
			Log_Debug("No network callback?\n");
			next_state = cur_state;
		}
		else if (reason == IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN) {
			// restart azure provisioning
			next_state = STATE_NO_CREDS;
		}
	}
	else if (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED) {
		// start sensor polling!
		next_state = STATE_PERIODIC_UPLOAD;
	}

	pthread_mutex_lock(&state_transition_lock);
	{
		desired_state = next_state;
		uev_event_post(data->move_state_w);
	}
	pthread_mutex_unlock(&state_transition_lock);
}

int check_azure_rdy(uev_t* w, void* arg, int events) {
	// TODO: handle errors gracefully
	if (UEV_ERROR == events)
		Log_Debug("Error in check_azure_rdy event\n");
	else {
		on_net_rdy_t* data = (on_net_rdy_t*)arg;
		if (data->iot_handle != NULL) {
			IoTHubDeviceClient_LL_Destroy(data->iot_handle);
			data->iot_handle = NULL;
		}

		AZURE_SPHERE_PROV_RETURN_VALUE provResult =
			IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning(data->iot_scope_id, 10000,
				&data->iot_handle);
		Log_Debug("IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning returned '%s'.\n",
			getAzureSphereProvisioningResultString(provResult));

		if (provResult.result == AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR
			|| provResult.result == AZURE_SPHERE_PROV_RESULT_IOTHUB_CLIENT_ERROR
			|| provResult.result == AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR)
			return -1;

		if (provResult.result != AZURE_SPHERE_PROV_RESULT_OK)
			return 0;

		const int timeout = 20;
		if (IoTHubDeviceClient_LL_SetOption(data->iot_handle, OPTION_KEEP_ALIVE, &timeout) != IOTHUB_CLIENT_OK) {
			Log_Debug("ERROR: failure setting option \"%s\"\n", OPTION_KEEP_ALIVE);
			return -1;
		}

		IoTHubDeviceClient_LL_SetRetryPolicy(data->iot_handle, IOTHUB_CLIENT_RETRY_INTERVAL, 30);
		IoTHubDeviceClient_LL_SetConnectionStatusCallback(data->iot_handle, azure_status_cb, data);
		return 1;
	}
	return -1;
}

void upload_msg_cb(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context) {
	// if the result is good, deque from the sent queue
	// else move back to the to_send queue, and post that the network is down
	pthread_mutex_lock(&pkt_queue_lock);
	{
		char* pkt = q_peek(&pkt_sent_queue);
		q_dequeue(&pkt_sent_queue);
		if (result != IOTHUB_CLIENT_CONFIRMATION_OK) {
			Log_Debug("Could not send! attempting retransmission...\n");
			if (!q_enqueue(&pkt_to_send_queue, pkt)) {
				Log_Debug("Out of memory when retransmitting!\n");
				terminationRequired = true;
			}
		}
		else {
			free(pkt);
		}
	}
	pthread_mutex_unlock(&pkt_queue_lock);
}

void upload_data(uev_t* w, void* arg, int events) {
	on_net_rdy_t* data = (on_net_rdy_t*)arg;

	Log_Debug("upload_data called\n");

	if (UEV_ERROR == events) {
		Log_Debug("Error in upload_data event\n");
		uev_timer_stop(data->upload_timer_w);
		terminationRequired = true;
		return;
	}
	
	// check the network status just to be sure
	bool net_is_rdy = false;
	if (Networking_IsNetworkingReady(&net_is_rdy) < 0) {
		Log_Debug("Call to network failed\n");
		return;
	}

	if (!net_is_rdy) {
		Log_Debug("Network disconnected, cannot send data\n");
		pthread_mutex_lock(&state_transition_lock);
		{
			desired_state = STATE_NO_NETWORK;
			uev_event_post(data->move_state_w);
		}
		pthread_mutex_unlock(&state_transition_lock);
		return;
	}
	
	// lock the queue so we can manipulate it
	pthread_mutex_lock(&pkt_queue_lock);
	{
		while (q_count(&pkt_to_send_queue) > 0) {
			// for every item in the send queue, send the message!
			char* buf = q_peek(&pkt_to_send_queue);
			q_dequeue(&pkt_to_send_queue);
			Log_Debug("Sending message:\"%s\"\n", buf);
			// convert it into a IoTHub message (copy number 2)
			IOTHUB_MESSAGE_HANDLE message_handle = IoTHubMessage_CreateFromString(buf);
			if (message_handle == NULL) {
				Log_Debug("Unable to create message handle\n");
				q_enqueue(&pkt_to_send_queue, buf);
				goto cleanup;
			}
			// send the message to the IoTHub (copy number 3)
			if (IoTHubDeviceClient_LL_SendEventAsync(data->iot_handle, message_handle, upload_msg_cb, NULL) != IOTHUB_CLIENT_OK) {
				Log_Debug("WARNING: failed to hand over the message to IoTHubClient\n");
				q_enqueue(&pkt_to_send_queue, buf);

				pthread_mutex_lock(&state_transition_lock);
				{
					desired_state = STATE_NO_CREDS;
					uev_event_post(data->move_state_w);
				}
				pthread_mutex_unlock(&state_transition_lock);
			}
			else {
				// add the message to the "sent" buffer, and restart the cycle!
				q_enqueue(&pkt_sent_queue, buf);
			}
			// remove copy #2
			IoTHubMessage_Destroy(message_handle);
		}
	}
cleanup:
	pthread_mutex_unlock(&pkt_queue_lock);
}

void poll_sensors(uev_t* w, void* arg, int events) {
	on_net_rdy_t* data = (on_net_rdy_t*)arg;

	if (UEV_ERROR == events) {
		Log_Debug("Error in poll_sensors event\n");
		uev_timer_stop(data->poll_sensors_w);
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		time_t next_trigger = now.tv_sec + sample_interval;
		next_trigger -= (next_trigger % sample_interval);
		uev_timer_set(data->poll_sensors_w, (next_trigger - now.tv_sec) * 1000, sample_interval * 1000);
		uev_timer_start(data->poll_sensors_w);
	}
	else {
		// read sensors
		struct timespec time;
		uint32_t adc_value = 0U;
		climate_data_t climate_sample;
		soil_data_t soil_sample;
		humidity_data_t humid_sample;
		clock_gettime(CLOCK_REALTIME, &time);
		ADC_Poll(data->fds->ADC, LIGHT_ADC_CHANNEL, &adc_value);
		float lux = (2.5f * (float)adc_value / 4095.0f) * 1000000.0f / (3650.0f * 0.1428f);
		ClimateSensorMeasure(&climate_sample);
		SoilMoistureMeasure(&soil_sample);
		HumidityMeasure(&humid_sample);
		// serialize sensor data (copy number 1)
		char* buffer = malloc(packet_max);
		int res = snprintf(buffer, packet_max, data_fmt, 
			time.tv_sec, 
			lux, 
			climate_sample.avg_tempurature, 
			climate_sample.avg_pressure, 
			climate_sample.num_samples,
			soil_sample.soil_moisture_24,
			soil_sample.soil_moisture_26,
			humid_sample.humidity);
		if (res < 0) {
			Log_Debug("Snprintf failed\n");
			free(buffer);
			return;
		}
		else
			Log_Debug("Sampled message:\"%s\"\n", buffer);
		// add message to sending queue
		bool status;
		size_t count = 0;
		pthread_mutex_lock(&pkt_queue_lock);
		{
			status = q_enqueue(&pkt_to_send_queue, buffer);
			// if the number of elements in the queue > 10, tell the device it's time to send
			count = q_count(&pkt_to_send_queue);
		}
		pthread_mutex_unlock(&pkt_queue_lock);
		if (!status) {
			Log_Debug("Memory queue full!\n");
			terminationRequired = true;
		}
		// if there are more than ten elements, move from idle to start
		if (count >= 10) {
			pthread_mutex_lock(&state_transition_lock);
			{
				if (cur_state == STATE_IDLE) {
					desired_state = STATE_NO_NETWORK;
					uev_event_post(data->move_state_w);
				}
			}
			pthread_mutex_unlock(&state_transition_lock);
		}
	}
}

void do_work(uev_t* w, void* arg, int events) {
	on_net_rdy_t* data = (on_net_rdy_t*)arg;
	if (data->iot_handle) {
		IoTHubDeviceClient_LL_DoWork(data->iot_handle);
	}
}

int main(void)
{
	fd_t fds;
	// pulse_t app_pulse, wlan_pulse;
	uev_ctx_t ctx;
	uev_t sigterm_watcher, state_change_watcher, upload_timer_watcher, poll_sensors_watcher, do_work_watcher;
	poll_t net_rdy_poll, azure_rdy_poll;
	on_net_rdy_t on_net_rdy_arg;

	// startup and clear the queue
	pkt_to_send_queue = q_new();
	pkt_sent_queue = q_new();
	pthread_mutex_init(&pkt_queue_lock, NULL);
	pthread_mutex_init(&state_transition_lock, NULL);

	// open and reset all peripheraals
	OpenPeripherals(&fds);

	// configure events
	// pulse_configure(&app_pulse, fds.Status_PWM, APP_STATUS_PWM_CHANNEL, 100000, 80000, 5000, 100);
	// pulse_configure(&wlan_pulse, fds.Status_PWM, WLAN_STATUS_PWM_CHANNEL, 100000, 80000, 5000, 100);
	on_net_rdy_arg.ctx = &ctx;
	on_net_rdy_arg.move_state_w = &state_change_watcher;
	on_net_rdy_arg.upload_timer_w = &upload_timer_watcher;
	on_net_rdy_arg.poll_sensors_w = &poll_sensors_watcher;
	on_net_rdy_arg.fds = &fds;
	// on_net_rdy_arg.app_pulse = &app_pulse;
	// on_net_rdy_arg.wlan_pulse = &wlan_pulse;
	on_net_rdy_arg.net_rdy_poll = &net_rdy_poll;
	on_net_rdy_arg.azure_rdy_poll = &azure_rdy_poll;
	on_net_rdy_arg.iot_handle = NULL;
	on_net_rdy_arg.iot_scope_id = iot_scope_id;
	poll_configure(&net_rdy_poll, check_net_rdy, &on_net_rdy_arg, NULL, 1000);
	poll_configure(&azure_rdy_poll, check_azure_rdy, &on_net_rdy_arg, NULL, 1000);

	Log_Debug("Initializing uev event loop...\n");

	uev_init(&ctx);
	uev_signal_init(&ctx, &sigterm_watcher, cleanup_exit, NULL, SIGTERM);
	uev_event_init(&ctx, &state_change_watcher, move_state, &on_net_rdy_arg);
	uev_timer_init(&ctx, &upload_timer_watcher, upload_data, &on_net_rdy_arg, 100, UEV_NONE);
	uev_timer_stop(&upload_timer_watcher);
	// start the internet event chain
	desired_state = STATE_IDLE;
	uev_event_post(&state_change_watcher);
	// and the sensor event chain, which will run concurrently
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	time_t next_trigger = now.tv_sec + sample_interval;
	next_trigger -= (next_trigger % sample_interval);
	uev_timer_init(&ctx, &poll_sensors_watcher, poll_sensors, &on_net_rdy_arg, (next_trigger - now.tv_sec) * 1000, sample_interval * 1000);
	uev_timer_init(&ctx, &do_work_watcher, do_work, &on_net_rdy_arg, 100, 100);

	while (!terminationRequired && uev_run(&ctx, 0) >= 0);
	
	Log_Debug("Exiting...\n");
	
	uev_exit(&ctx);
	pthread_mutex_destroy(&state_transition_lock);
	pthread_mutex_destroy(&pkt_queue_lock);
	return 0;
}