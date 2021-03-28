// This minimal Azure Sphere app repeatedly toggles an LED. Use this app to test that
// installation of the device and SDK succeeded, and that you can build, deploy, and debug an app.

#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <pthread.h>

#include <applibs/log.h>
#include <applibs/gpio.h>
#include <applibs/pwm.h>
#include <applibs/i2c.h>
#include <applibs/adc.h>
#include <applibs/networking.h>
#include <applibs/eventloop.h>
#include <hw/plant_sk.h>

#include <iothub_client_core_common.h>
#include <iothub_device_client_ll.h>
#include <iothub_client_options.h>
#include <iothubtransportmqtt.h>
#include <iothub.h>
#include <azure_sphere_provisioning.h>
#include <iothub_security_factory.h>
#include <shared_util_options.h>

#include <macro_collections.h>

#include "event_loop_event.h"
#include "event_loop_timer.h"
#include "climatesensor.h"
#include "chirp.h"
#include "humidity.h"

/// Constants
const char IotHubHostname[] = "plantmonitor.azure-devices.net";
const char PacketFmt[] = "{\"meta\":{\"time\":%d,\"name\":\"plant0\"},\"data\":{\"lux\":%f,\"climate\":{\"tempurature\":%f,\"pressure\":%f,\"samples\":%d},\"soil\":{\"0x24\":%hu,\"0x26\":%hu},\"humidity\":%f}}";
const struct timespec UploadInterval = { .tv_sec = 600, .tv_nsec = 0 }; // TODO: every ten minutes
const struct timespec SampleInterval = { .tv_sec = 120, .tv_nsec = 0 }; // TODO: every two minutes
const struct timespec NetPollInterval = { .tv_sec = 5, .tv_nsec = 0 };
const struct timespec AzureAuthPollInterval = { .tv_sec = 30, .tv_nsec = 0 };
const struct timespec IoTDoWorkInterval = { .tv_sec = 0, .tv_nsec = 5e7 }; // 50 milliseconds
const struct timespec SoonInterval = { .tv_sec = 0, .tv_nsec = 1 };
const size_t PacketMaxBytes = 256;
const size_t QueueMaxCapacity = 50;
const size_t AdcSampleCount = 100;

typedef enum {
    State_Entry = 0,
    State_NoNetwork = 1,
    State_AzureAuth = 2,
    State_PeriodicUpload = 3
} MonitorState_t;

const char* str_monitor_state(MonitorState_t state) {
    switch (state) {
    case State_Entry: return "State_Entry";
    case State_NoNetwork: return "State_NoNetwork";
    case State_AzureAuth: return "State_AzureAuth";
    case State_PeriodicUpload: return "State_PeriodicUpload";
    default: return "Unknown";
    }
}

typedef enum {
    ExitCode_Success = 0,

    ExitCode_EventLoop_Create = 1,
    ExitCode_ConsumeEventLoopTimerEvent = 3,
    ExitCode_ConsumeEventLoopEvent = 4,
    ExitCode_CreateEventLoopEvent_Panic = 5,
    ExitCode_CreateEventLoopEvent_StateTransition = 5,
    ExitCode_CreateEventLoopDisarmedTimer_NetRdy = 7,
    ExitCode_CreateEventLoopDisarmedTimer_AzureAuth = 8,
    ExitCode_CreateEventLoopDisarmedTimer_Upload = 28,
    ExitCode_CreateEventLoopDisarmedTimer_DoWork = 27,
    ExitCode_CreateEventLoopPeriodicTimer_Sample = 9,
    ExitCode_UnknownState = 10,
    ExitCode_Networking_GetInterfaceConnectionStatus = 11,
    ExitCode_iothub_security_init = 12,
    ExitCode_deque_new_outbound = 13,
    ExitCode_deque_new_in_flight = 14,
    ExitCode_EventLoopFail = 15,

    ExitCode_QueueOverfill = 16,
    ExitCode_QueueingFailed = 17,
    ExitCode_malloc_fail = 18,
    ExitCode_lock_fail = 19,

    ExitCode_I2CMaster_Open_Climate = 20,
    ExitCode_I2CMaster_SetBusSpeed_Climate = 21,
    ExitCode_I2CMaster_SetTimeout_Climate = 22,

    ExitCode_ADC_Open = 23,
    ExitCode_ADC_SetReferenceVoltage = 24,

    ExitCode_PWM_Open_Status = 25,
    ExitCode_PWM_Open_User = 26,

    ExitCode_SigTerm = 254,
} ExitCode;

#define DEQUE_PARAMS (deque, deque, QUEUE_MAX, , IOTHUB_MESSAGE_HANDLE)
C_MACRO_COLLECTIONS_EXTENDED(CMC, DEQUE, DEQUE_PARAMS, )

typedef struct deque deque_t;
typedef struct deque_iter deque_iter_t;

typedef struct {
    int adc;
    int i2c_climate;
    int status_pwm;
    int user_pwm;
} fd_t;

typedef struct {
    climate_t climate;
    humidity_t humidity;
    chirp_t soil_moisture_1;
    chirp_t soil_moisture_2;
    fd_t fds;
} sensors_t;

typedef struct {
    climate_data_t climate_data;
    humidity_data_t humidity_data;
    chirp_data_t soil_1_data;
    chirp_data_t soil_2_data;
    double lux;
} sensor_values_t;

typedef struct {
    pthread_mutex_t pkt_queues_lock;
    deque_t* pkt_outbound;
    deque_t* pkt_in_flight;

    EventLoop* loop;
    EventLoopEvent_t* sigterm_event;
    EventLoopEvent_t* state_transition_event;
    EventLoopTimer* no_network_timer;
    EventLoopTimer* azure_auth_timer;
    EventLoopTimer* upload_timer;
    EventLoopTimer* dowork_timer;
    EventLoopTimer* sample_timer;
    IOTHUB_DEVICE_CLIENT_LL_HANDLE iothub_handle;
    sensors_t sensors;

    MonitorState_t cur_state;
    MonitorState_t requested_state;

    ExitCode last_thread_exit_code;
} application_state_t;

// Global Variables
volatile EventLoopEvent_t* sigterm_event = NULL;

// functions
void clear_fds(fd_t* fds) {
    fds->adc = -1;
    fds->i2c_climate = -1;
    fds->status_pwm = -1;
    fds->user_pwm = -1;
}

void init_sensors(sensors_t* sensors) {
    memset(sensors, 0, sizeof(*sensors));
    clear_fds(&sensors->fds);
}

void zero_application_state(application_state_t* app_state) {
    memset(app_state, 0, sizeof(*app_state));

    app_state->last_thread_exit_code = ExitCode_Success;
    app_state->cur_state = State_Entry;
    app_state->requested_state = State_Entry;

    init_sensors(&app_state->sensors);
}

ExitCode start_system_devices(fd_t* fds) {
    if ((fds->adc = ADC_Open(LIGHT_ADC_CONTROLLER)) < 0)
        return ExitCode_ADC_Open;
    if (ADC_SetReferenceVoltage(fds->adc, LIGHT_ADC_CHANNEL, 2.5f) < 0)
        return ExitCode_ADC_SetReferenceVoltage;

    if ((fds->i2c_climate = I2CMaster_Open(CLIMATE_I2C_CONTROLLER)) < 0)
        return ExitCode_I2CMaster_Open_Climate;
    if (I2CMaster_SetBusSpeed(fds->i2c_climate, I2C_BUS_SPEED_STANDARD) < 0)
        return ExitCode_I2CMaster_SetBusSpeed_Climate;
    if (I2CMaster_SetTimeout(fds->i2c_climate, 100) < 0)
        return ExitCode_I2CMaster_SetTimeout_Climate;

    PwmState state;
    state.enabled = true;
    state.dutyCycle_nsec = 0;
    state.period_nsec = 100000;
    state.polarity = PWM_Polarity_Inversed;
    if ((fds->status_pwm = PWM_Open(STATUS_PWM_CONTROLLER)) < 0)
        return ExitCode_PWM_Open_Status;
    PWM_Apply(fds->status_pwm, APP_STATUS_PWM_CHANNEL, &state);
    PWM_Apply(fds->status_pwm, WLAN_STATUS_PWM_CHANNEL, &state);
    if ((fds->user_pwm = PWM_Open(USER_PWM_CONTROLLER)) < 0)
        return ExitCode_PWM_Open_User;
    PWM_Apply(fds->user_pwm, USER_LED_RED_PWM_CHANNEL, &state);
    PWM_Apply(fds->user_pwm, USER_LED_GREEN_PWM_CHANNEL, &state);
    PWM_Apply(fds->user_pwm, USER_LED_BLUE_PWM_CHANNEL, &state);
    
    return ExitCode_Success;
}

void stop_system_devices(fd_t* fds) {
    if (fds->adc != -1)
        close(fds->adc);
    if (fds->i2c_climate != -1)
        close(fds->i2c_climate);
    if (fds->status_pwm != -1)
        close(fds->status_pwm);
    if (fds->user_pwm != -1)
        close(fds->user_pwm);
}

void start_or_restart_sensors(sensors_t* sensors) {
    if (!ClimateSensorIsOk(&sensors->climate) && ClimateSensorInit(&sensors->climate, sensors->fds.i2c_climate) < 0)
        Log_Debug("Failed to initialize climate sensor\n");
    if (!ChirpIsOk(&sensors->soil_moisture_1) && ChirpInit(&sensors->soil_moisture_1, sensors->fds.i2c_climate, CHIRP_ADDR_1) < 0)
        Log_Debug("Failed to initialize soil moisture 1\n");
    if (!ChirpIsOk(&sensors->soil_moisture_2) && ChirpInit(&sensors->soil_moisture_2, sensors->fds.i2c_climate, CHIRP_ADDR_2) < 0)
        Log_Debug("Failed to initialize soil moisture 2\n");
    if (!HumidityIsOk(&sensors->humidity) && HumidityInit(&sensors->humidity, sensors->fds.i2c_climate) < 0)
        Log_Debug("Failed to initialize humidity sensor\n");
}

sensor_values_t sample_sensors(sensors_t* sensors) {
    sensor_values_t ret = { 0 };
    ClimateSensorMeasure(&sensors->climate, &ret.climate_data);
    HumidityMeasure(&sensors->humidity, &ret.humidity_data);
    ChirpMeasure(&sensors->soil_moisture_1, &ret.soil_1_data);
    ChirpMeasure(&sensors->soil_moisture_2, &ret.soil_2_data);
    
    uint64_t sum = 0;
    size_t sample_count = 0;
    for (size_t i = 0; i < AdcSampleCount; i++) {
        uint32_t adc_value;
        int err = ADC_Poll(sensors->fds.adc, LIGHT_ADC_CHANNEL, &adc_value);
        if (!err) {
            sum += adc_value;
            sample_count++;
        }
    }

    if (sample_count > 0) {
        const double avg = (double)sum / (double)sample_count;
        ret.lux = (2.5 * avg / 4095.0) * 1000000.0 / (3650.0 * 0.1428);
    }
    else 
        ret.lux = 0;

    return ret;
}

IOTHUB_MESSAGE_HANDLE serialize_sensor_data(const sensor_values_t* values, const struct timespec* time) {
    char pkt[PacketMaxBytes];
    int res = snprintf(pkt, PacketMaxBytes, PacketFmt,
        time->tv_sec,
        values->lux,
        values->climate_data.avg_tempurature,
        values->climate_data.avg_pressure,
        values->climate_data.num_samples,
        values->soil_1_data.soil_moisture,
        values->soil_2_data.soil_moisture,
        values->humidity_data.humidity);

    if (res < 0) {
        Log_Debug("Failed to serialize sensor readings");
        return NULL;
    }

    return IoTHubMessage_CreateFromString(pkt);
}

ExitCode start_peripherals(application_state_t* app_state) {
    ExitCode ret = start_system_devices(&app_state->sensors.fds);
    if (ret != ExitCode_Success)
        return ret;
    start_or_restart_sensors(&app_state->sensors);
    return ExitCode_Success;
}

void set_indicator_color(int PWMfd, unsigned int red, unsigned int green, unsigned int blue) {
    PwmState state;
    state.enabled = true;
    state.dutyCycle_nsec = 0;
    state.period_nsec = 255000;
    state.polarity = PWM_Polarity_Inversed;

    state.dutyCycle_nsec = red * 100;
    PWM_Apply(PWMfd, USER_LED_RED_PWM_CHANNEL, &state);
    state.dutyCycle_nsec = green * 100;
    PWM_Apply(PWMfd, USER_LED_GREEN_PWM_CHANNEL, &state);
    state.dutyCycle_nsec = blue * 100;
    PWM_Apply(PWMfd, USER_LED_BLUE_PWM_CHANNEL, &state);
}

void app_panic(application_state_t* app, ExitCode code) {
    app->last_thread_exit_code = code;
    EventLoop_Stop(app->loop);
}

void _app_request_transition(application_state_t* app, MonitorState_t next_state, int line, const char* func) {
    // Log_Debug("%s:%i requesting to %s\n", func, line, str_monitor_state(next_state)); Uncomment for debugging, not threadsafe
    app->requested_state = next_state;
    PostEventLoopEvent(app->state_transition_event);
}

#define APP_REQUEST_TRANSITION(APP, NEXT_STATE) _app_request_transition(APP, NEXT_STATE, __LINE__, __func__)

void handle_state_transition(EventLoopEvent_t* event, void* ctx) {
    application_state_t* app_state = (application_state_t*)ctx;
    eventfd_t out = 0;
    int err = ConsumeEventLoopEvent(event, &out);
    if (err) {
        app_panic(app_state, ExitCode_ConsumeEventLoopEvent);
        return;
    }

    Log_Debug("Got state transition %s with %i requests\n", str_monitor_state(app_state->requested_state), (int)out);
    
    if (app_state->cur_state != app_state->requested_state) {
        Log_Debug("Transitioning from state %s to state %s\n", str_monitor_state(app_state->cur_state), str_monitor_state(app_state->requested_state));

        // cancel all actions from previous states
        DisarmEventLoopTimer(app_state->no_network_timer);
        DisarmEventLoopTimer(app_state->azure_auth_timer);
        DisarmEventLoopTimer(app_state->dowork_timer);
        DisarmEventLoopTimer(app_state->upload_timer);

        // TODO: zero length initial timer is hacky
        switch (app_state->requested_state) {
        case State_NoNetwork:
            set_indicator_color(app_state->sensors.fds.user_pwm, 255, 0, 0);
            SetEventLoopTimerPeriod(app_state->no_network_timer, &SoonInterval, &NetPollInterval);
            break;
        case State_AzureAuth:
            set_indicator_color(app_state->sensors.fds.user_pwm, 255, 128, 0);
            SetEventLoopTimerPeriod(app_state->azure_auth_timer, &SoonInterval, &AzureAuthPollInterval);
            SetEventLoopTimerPeriod(app_state->dowork_timer, &SoonInterval, &IoTDoWorkInterval);
            break;
        case State_PeriodicUpload:
            set_indicator_color(app_state->sensors.fds.user_pwm, 0, 255, 0);
            SetEventLoopTimerPeriod(app_state->upload_timer, &SoonInterval, &UploadInterval);
            SetEventLoopTimerPeriod(app_state->dowork_timer, &SoonInterval, &IoTDoWorkInterval);
            break;
        default:
            app_panic(app_state, ExitCode_UnknownState);
            return;
        }

        app_state->cur_state = app_state->requested_state;
    }
}

void handle_no_network(EventLoopTimer* timer, void* ctx) {
    application_state_t* app_state = (application_state_t*)ctx;
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        app_panic(app_state, ExitCode_ConsumeEventLoopTimerEvent);
        return;
    }

    Networking_InterfaceConnectionStatus status;
    if (Networking_GetInterfaceConnectionStatus("wlan0", &status) == 0) {
        if (status & Networking_InterfaceConnectionStatus_ConnectedToInternet)
            APP_REQUEST_TRANSITION(app_state, State_AzureAuth);
    }
    else if (errno != EAGAIN) {
        app_panic(app_state, ExitCode_Networking_GetInterfaceConnectionStatus);
        return;
    }
}

void azure_status_cb_unsafe(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* ctx)
{
    application_state_t* app_state = (application_state_t*)ctx;
    if (result == IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED) {
        if (reason == IOTHUB_CLIENT_CONNECTION_NO_NETWORK)
            APP_REQUEST_TRANSITION(app_state, State_NoNetwork);
        else 
            APP_REQUEST_TRANSITION(app_state, State_AzureAuth);
    }
    if (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED)
        APP_REQUEST_TRANSITION(app_state, State_PeriodicUpload);
}

void handle_azure_auth(EventLoopTimer* timer, void* ctx) {
    application_state_t* app_state = (application_state_t*)ctx;
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        app_panic(app_state, ExitCode_ConsumeEventLoopTimerEvent);
        return;
    }

    if (app_state->iothub_handle != NULL) {
        IoTHubDeviceClient_LL_Destroy(app_state->iothub_handle);
        app_state->iothub_handle = NULL;
    }

    Networking_InterfaceConnectionStatus status;
    if (Networking_GetInterfaceConnectionStatus("wlan0", &status) != 0) {
        if (errno != EAGAIN)
            app_panic(app_state, ExitCode_Networking_GetInterfaceConnectionStatus);
        return;
    }
    if (!(status & Networking_InterfaceConnectionStatus_ConnectedToInternet)) {
        APP_REQUEST_TRANSITION(app_state, State_NoNetwork);
        return;
    }

    if (iothub_security_init(IOTHUB_SECURITY_TYPE_X509) != 0) {
        app_panic(app_state, ExitCode_iothub_security_init);
        return;
    }
    app_state->iothub_handle =
        IoTHubDeviceClient_LL_CreateWithAzureSphereFromDeviceAuth(IotHubHostname, MQTT_Protocol);
    if (app_state->iothub_handle == NULL) {
        /// not going to panic here, instead retry
        Log_Debug("IoTHubDeviceClient_LL_CreateFromDeviceAuth returned NULL.\n");
    }
    else {
        int device_id_for_daa = 1;
        if (IoTHubDeviceClient_LL_SetOption(app_state->iothub_handle, "SetDeviceId", &device_id_for_daa)  != IOTHUB_CLIENT_OK)
            Log_Debug("Failure setting Azure IoT Hub client option \"SetDeviceId\".\n");
        //bool url_encode_on = true;
        //if (IoTHubDeviceClient_LL_SetOption(app_state->iothub_handle, OPTION_AUTO_URL_ENCODE_DECODE, &url_encode_on) != IOTHUB_CLIENT_OK)
        //    Log_Debug("Failure setting Azure IoT Hub client option \"OPTION_AUTO_URL_ENCODE_DECODE\".\n");
        if (IoTHubDeviceClient_LL_SetRetryPolicy(app_state->iothub_handle, IOTHUB_CLIENT_RETRY_INTERVAL, 30) != IOTHUB_CLIENT_OK)
            Log_Debug("Failure setting Azure IoT Hub client retry policy.\n");
        if (IoTHubDeviceClient_LL_SetConnectionStatusCallback(app_state->iothub_handle, azure_status_cb_unsafe, app_state) != IOTHUB_CLIENT_OK)
            Log_Debug("Failure setting Azure IoT Hub client connection status callback.\n");
    }

    // asynchronously wait for azure status callback, or retry b/c it took too long
    IoTHubDeviceClient_LL_DoWork(app_state->iothub_handle);

    // cleanup
    iothub_security_deinit();
}

void azure_send_cb_unsafe(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context)
{
    application_state_t* app_state = (application_state_t*)context;
    
    if (pthread_mutex_lock(&app_state->pkt_queues_lock)) {
        app_panic(app_state, ExitCode_lock_fail);
        return;
    }

    IOTHUB_MESSAGE_HANDLE maybe_sent = deque_front(app_state->pkt_in_flight);
    deque_pop_front(app_state->pkt_in_flight);

    if (result != IOTHUB_CLIENT_CONFIRMATION_OK) {
        if (deque_count(app_state->pkt_outbound) >= QueueMaxCapacity) {
            app_panic(app_state, ExitCode_QueueOverfill);
            IoTHubMessage_Destroy(maybe_sent);
        }
        else if (!deque_push_back(app_state->pkt_outbound, maybe_sent)) {
            app_panic(app_state, ExitCode_QueueingFailed);
            IoTHubMessage_Destroy(maybe_sent);
        }
    }
    else
        IoTHubMessage_Destroy(maybe_sent);

    pthread_mutex_unlock(&app_state->pkt_queues_lock);
}

void handle_upload(EventLoopTimer* timer, void* ctx) {
    application_state_t* app_state = (application_state_t*)ctx;
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        app_panic(app_state, ExitCode_ConsumeEventLoopTimerEvent);
        return;
    }

    if (pthread_mutex_lock(&app_state->pkt_queues_lock)) {
        app_panic(app_state, ExitCode_lock_fail);
        return;
    }

    while (!deque_empty(app_state->pkt_outbound) && deque_count(app_state->pkt_in_flight) < QueueMaxCapacity) {
        IOTHUB_MESSAGE_HANDLE to_send = deque_front(app_state->pkt_outbound);
        IOTHUB_CLIENT_RESULT res = IoTHubDeviceClient_LL_SendEventAsync(
            app_state->iothub_handle, to_send, azure_send_cb_unsafe, app_state);
        if (res != IOTHUB_CLIENT_OK) {
            Log_Debug("Requesting IoTHub send failed with error %i\n", res);
            APP_REQUEST_TRANSITION(app_state, State_NoNetwork);
            goto cleanup;
        }

        Log_Debug("Sent message\n");

        deque_pop_front(app_state->pkt_outbound);
        if (!deque_push_back(app_state->pkt_in_flight, to_send)) {
            app_panic(app_state, ExitCode_QueueingFailed);
            IoTHubMessage_Destroy(to_send);
            goto cleanup;
        }
    }

    IoTHubDeviceClient_LL_DoWork(app_state->iothub_handle);

cleanup:
    pthread_mutex_unlock(&app_state->pkt_queues_lock);
}

void handle_do_work(EventLoopTimer* timer, void* ctx) {
    application_state_t* app_state = (application_state_t*)ctx;
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        app_panic(app_state, ExitCode_ConsumeEventLoopTimerEvent);
        return;
    }

    IoTHubDeviceClient_LL_DoWork(app_state->iothub_handle);
}

void handle_sample(EventLoopTimer* timer, void* ctx) {
    application_state_t* app_state = (application_state_t*)ctx;
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        app_panic(app_state, ExitCode_ConsumeEventLoopTimerEvent);
        return;
    }
    
    if (pthread_mutex_lock(&app_state->pkt_queues_lock)) {
        app_panic(app_state, ExitCode_lock_fail);
        return;
    }

    if (deque_count(app_state->pkt_outbound) >= QueueMaxCapacity) {
        app_panic(app_state, ExitCode_QueueOverfill);
        goto cleanup;
    }

    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    sensor_values_t sample = sample_sensors(&app_state->sensors);
    IOTHUB_MESSAGE_HANDLE handle = serialize_sensor_data(&sample, &time);

    if (handle == NULL) {
        // TODO: should panic here or not?
        Log_Debug("Failed to serialize sensor readings\n");
        goto cleanup;
    }

    Log_Debug("Queueing message with body \"%s\"\n", IoTHubMessage_GetString(handle));
    if (!deque_push_back(app_state->pkt_outbound, handle)) {
        app_panic(app_state, ExitCode_QueueingFailed);
        IoTHubMessage_Destroy(handle);
    }

cleanup:
    pthread_mutex_unlock(&app_state->pkt_queues_lock);
}

void sigterm_handler(int signalNumber) {
    if (sigterm_event)
        PostEventLoopEvent(sigterm_event);
}

void handle_sigterm_event(EventLoopEvent_t* event, void* ctx) {
    application_state_t* app_state = (application_state_t*)ctx;
    eventfd_t out = 0;
    ConsumeEventLoopEvent(event, &out);

    Log_Debug("Got SIGTERM, aborting...\n");
    EventLoop_Stop(app_state->loop);
}

ExitCode init_application(application_state_t* state) {
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &action, NULL);

    pthread_mutex_init(&state->pkt_queues_lock, NULL);
    state->pkt_outbound = deque_new(QueueMaxCapacity, &(struct deque_fval){ 0 });
    if (state->pkt_outbound == NULL)
        return ExitCode_deque_new_outbound;
    state->pkt_in_flight = deque_new(QueueMaxCapacity, &(struct deque_fval){ 0 });
    if (state->pkt_in_flight == NULL)
        return ExitCode_deque_new_in_flight;

    state->loop = EventLoop_Create();
    if (state->loop == NULL)
        return ExitCode_EventLoop_Create;

    state->last_thread_exit_code = ExitCode_Success;
    state->sigterm_event = CreateEventLoopEvent(state->loop, handle_sigterm_event, state);
    if (state->sigterm_event == NULL)
        return ExitCode_CreateEventLoopEvent_Panic;
    sigterm_event = state->sigterm_event;
    
    state->state_transition_event = CreateEventLoopEvent(state->loop, handle_state_transition, state);
    if (state->state_transition_event == NULL)
        return ExitCode_CreateEventLoopEvent_StateTransition;

    state->no_network_timer = CreateEventLoopDisarmedTimer(state->loop, handle_no_network, state);
    if (state->no_network_timer == NULL)
        return ExitCode_CreateEventLoopDisarmedTimer_NetRdy;
    state->azure_auth_timer = CreateEventLoopDisarmedTimer(state->loop, handle_azure_auth, state);
    if (state->azure_auth_timer == NULL)
        return ExitCode_CreateEventLoopDisarmedTimer_AzureAuth;
    state->upload_timer = CreateEventLoopDisarmedTimer(state->loop, handle_upload, state);
    if (state->upload_timer == NULL)
        return ExitCode_CreateEventLoopDisarmedTimer_Upload;
    state->dowork_timer = CreateEventLoopDisarmedTimer(state->loop, handle_do_work, state);
    if (state->dowork_timer == NULL)
        return ExitCode_CreateEventLoopDisarmedTimer_DoWork;

    state->sample_timer = CreateEventLoopPeriodicTimer(state->loop, handle_sample, state, &SampleInterval);
    if (state->sample_timer == NULL)
        return ExitCode_CreateEventLoopPeriodicTimer_Sample;

    APP_REQUEST_TRANSITION(state, State_NoNetwork);

    return ExitCode_Success;
}

ExitCode run_application(application_state_t* state) {
    // Main loop
    while (state->last_thread_exit_code == ExitCode_Success) {
        EventLoop_Run_Result result = EventLoop_Run(state->loop, -1, false);
        // Continue if interrupted by signal, e.g. due to breakpoint being set.
        if (result == EventLoop_Run_Failed && errno != EINTR) {
            return ExitCode_EventLoopFail;
        }
    }
    return state->last_thread_exit_code;
}

void destroy_pkt_deque(deque_t* pkt_d) {
    while (!deque_empty(pkt_d)) {
        IoTHubMessage_Destroy(deque_front(pkt_d));
        deque_pop_front(pkt_d);
    }
    deque_free(pkt_d);
}

void destroy_application(application_state_t* state) {
    if (state->state_transition_event)
        DisposeEventLoopEvent(state->state_transition_event);
    if (state->sigterm_event) {
        DisposeEventLoopEvent(state->sigterm_event);
        sigterm_event = NULL;
    }
    if (state->no_network_timer)
        DisposeEventLoopTimer(state->no_network_timer);
    if (state->azure_auth_timer)
        DisposeEventLoopTimer(state->azure_auth_timer);
    if (state->upload_timer)
        DisposeEventLoopTimer(state->upload_timer);
    if (state->dowork_timer)
        DisposeEventLoopTimer(state->dowork_timer);
    if (state->sample_timer)
        DisposeEventLoopTimer(state->sample_timer);
    if (state->loop)
        EventLoop_Close(state->loop);
    if (state->iothub_handle)
        IoTHubDeviceClient_LL_Destroy(state->iothub_handle);

    if (state->pkt_outbound)
        destroy_pkt_deque(state->pkt_outbound);
    if (state->pkt_in_flight)
        destroy_pkt_deque(state->pkt_in_flight);
    pthread_mutex_destroy(&state->pkt_queues_lock);

    stop_system_devices(&state->sensors.fds);
    zero_application_state(state);
}

int main(void)
{
    application_state_t app_state;
    zero_application_state(&app_state);
    
    Log_Debug("Booting up!\n");

    Log_Debug("Initializing peripherals...");
    ExitCode exit = start_peripherals(&app_state);
    if (exit != ExitCode_Success)
        goto fail;
    Log_Debug("done\n");

    Log_Debug("Starting application handlers...");
    exit = init_application(&app_state);
    if (exit != ExitCode_Success)
        goto fail;
    Log_Debug("done\n");

    Log_Debug("Welcome azure sphere plant monitor!\n");
    exit = run_application(&app_state);

fail:
    Log_Debug("\nRunning failed with exit code %i and errno %s\n", exit, strerror(errno));
    destroy_application(&app_state);
    return exit;
}
