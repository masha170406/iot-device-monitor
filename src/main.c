#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>

#include <mosquitto.h>
#include <cjson/cJSON.h>

#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT 1883

#define DEFAULT_TEMP_THRESHOLD 30.0
#define DEFAULT_HUMIDITY_THRESHOLD 80.0
#define DEFAULT_BATTERY_THRESHOLD 20.0
#define DEFAULT_INACTIVE_TIMEOUT 30

#define SUMMARY_INTERVAL 10
#define MAX_DEVICES 100

struct Device {
    char device_id[100];
    double temperature;
    double humidity;
    double battery;
    long long timestamp;
    time_t last_seen;
    bool inactive;
};

struct Device devices[MAX_DEVICES];
int device_count = 0;

char *broker_host = DEFAULT_HOST;
int broker_port = DEFAULT_PORT;
char *output_file = NULL;

double temp_threshold = DEFAULT_TEMP_THRESHOLD;
double humidity_threshold = DEFAULT_HUMIDITY_THRESHOLD;
double battery_threshold = DEFAULT_BATTERY_THRESHOLD;

int inactive_timeout = DEFAULT_INACTIVE_TIMEOUT;

FILE *log_file = NULL;

volatile sig_atomic_t running = 1;

pthread_mutex_t devices_mutex = PTHREAD_MUTEX_INITIALIZER;


// handle Ctrl+C
void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}


// print help
void print_help(const char *program_name)
{
    printf("Usage: %s [options]\n\n", program_name);

    printf("Options:\n");
    printf("  -H HOST    MQTT broker hostname (default: localhost)\n");
    printf("  -P PORT    MQTT broker port (default: 1883)\n");
    printf("  -o FILE    Save telemetry and alerts to a file\n");
    printf("  -t VALUE   Temperature alert threshold (default: 30)\n");
    printf("  -u VALUE   Humidity alert threshold (default: 80)\n");
    printf("  -b VALUE   Battery alert threshold (default: 20)\n");
    printf("  -i SECONDS Inactive device timeout (default: 30)\n");
    printf("  -h         Show this help message\n");
}


// parse a number from a command line argument
int parse_double_argument(const char *argument, double *result)
{
    char *end_pointer;

    errno = 0;
    double value = strtod(argument, &end_pointer);

    if (errno == ERANGE ||
        end_pointer == argument ||
        *end_pointer != '\0' ||
        !isfinite(value)) {
        return 0;
    }

    *result = value;
    return 1;
}


// format a Unix timestamp into HH:MM:SS
void format_hhmmss(long long timestamp, char *buffer, size_t buffer_size)
{
    time_t message_time = (time_t)timestamp;
    struct tm time_info;

    if (localtime_r(&message_time, &time_info) != NULL) {
        strftime(buffer, buffer_size, "%H:%M:%S", &time_info);
    } else {
        snprintf(buffer, buffer_size, "Unknown time");
    }
}


// write telemetry block to console and optionally to log file
void write_telemetry_output(
    const char *time_string,
    const struct Device *device,
    int total_devices
)
{
    printf("\n[%s] Device: %s\n", time_string, device->device_id);
    printf("Temperature: %.1f C\n", device->temperature);
    printf("Humidity: %.1f %%\n", device->humidity);
    printf("Battery: %.0f %%\n", device->battery);
    printf("Known devices: %d\n", total_devices);

    if (log_file != NULL) {
        fprintf(log_file, "\n[%s] Device: %s\n", time_string, device->device_id);
        fprintf(log_file, "Temperature: %.1f C\n", device->temperature);
        fprintf(log_file, "Humidity: %.1f %%\n", device->humidity);
        fprintf(log_file, "Battery: %.0f %%\n", device->battery);
        fprintf(log_file, "Known devices: %d\n", total_devices);
        fflush(log_file);
    }
}


// Find an existing device or create a new one
struct Device *find_or_create_device(const char *device_id)
{
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i].device_id, device_id) == 0) {
            return &devices[i];
        }
    }

    if (device_count >= MAX_DEVICES) {
        fprintf(stderr, "Error: Maximum number of devices reached.\n");
        return NULL;
    }

    struct Device *device = &devices[device_count];

    strcpy(device->device_id, device_id);

    device->temperature = 0.0;
    device->humidity = 0.0;
    device->battery = 0.0;
    device->timestamp = 0;
    device->last_seen = time(NULL);
    device->inactive = false;

    device_count++;

    return device;
}


// publish an alert
void publish_alert(
    struct mosquitto *mosq,
    const char *device_id,
    const char *alert_type,
    double value,
    long long timestamp
)
{
    char topic[256];

    snprintf(
        topic,
        sizeof(topic),
        "iot/devices/%s/alert",
        device_id
    );

    cJSON *alert_json = cJSON_CreateObject();

    if (alert_json == NULL) {
        fprintf(stderr, "Error creating alert JSON.\n");
        return;
    }

    if (cJSON_AddStringToObject(
            alert_json, "device_id", device_id) == NULL ||
        cJSON_AddStringToObject(
            alert_json, "alert", alert_type) == NULL ||
        cJSON_AddNumberToObject(
            alert_json, "value", value) == NULL ||
        cJSON_AddNumberToObject(
            alert_json, "timestamp", (double)timestamp) == NULL) {

        fprintf(stderr, "Error building alert JSON.\n");
        cJSON_Delete(alert_json);
        return;
    }

    char *payload = cJSON_PrintUnformatted(alert_json);

    if (payload == NULL) {
        cJSON_Delete(alert_json);
        return;
    }

    int result = mosquitto_publish(
        mosq,
        NULL,
        topic,
        (int)strlen(payload),
        payload,
        0,
        false
    );

    if (result == MOSQ_ERR_SUCCESS) {
        printf("[ALERT] %s: %s = %.2f\n",
               device_id, alert_type, value);

        if (log_file != NULL) {
            fprintf(
                log_file,
                "[ALERT] Device: %s | Alert: %s | Value: %.2f | Timestamp: %lld\n",
                device_id,
                alert_type,
                value,
                timestamp
            );

            fflush(log_file);
        }
    } else {
        fprintf(
            stderr,
            "Error publishing alert: %s\n",
            mosquitto_strerror(result)
        );
    }

    cJSON_free(payload);
    cJSON_Delete(alert_json);
}


// process incoming telemetry
void process_message(
    struct mosquitto *mosq,
    const struct mosquitto_message *msg
)
{
    cJSON *json = cJSON_ParseWithLength(
        (const char *)msg->payload,
        (size_t)msg->payloadlen
    );

    if (json == NULL) {
        fprintf(stderr, "Error: Invalid JSON received.\n");
        return;
    }

    cJSON *device_id_json =
        cJSON_GetObjectItemCaseSensitive(json, "device_id");

    cJSON *temperature_json =
        cJSON_GetObjectItemCaseSensitive(json, "temperature");

    cJSON *humidity_json =
        cJSON_GetObjectItemCaseSensitive(json, "humidity");

    cJSON *battery_json =
        cJSON_GetObjectItemCaseSensitive(json, "battery");

    cJSON *timestamp_json =
        cJSON_GetObjectItemCaseSensitive(json, "timestamp");

    if (!cJSON_IsString(device_id_json) ||
        device_id_json->valuestring == NULL ||
        device_id_json->valuestring[0] == '\0' ||
        !cJSON_IsNumber(temperature_json) ||
        !cJSON_IsNumber(humidity_json) ||
        !cJSON_IsNumber(battery_json) ||
        !cJSON_IsNumber(timestamp_json)) {

        fprintf(stderr, "Error: Missing or invalid JSON fields.\n");
        cJSON_Delete(json);
        return;
    }

    const char *device_id = device_id_json->valuestring;

    double temperature = temperature_json->valuedouble;
    double humidity = humidity_json->valuedouble;
    double battery = battery_json->valuedouble;
    double timestamp_value = timestamp_json->valuedouble;

    if (!isfinite(temperature) ||
        !isfinite(humidity) ||
        !isfinite(battery) ||
        !isfinite(timestamp_value) ||
        temperature < -100.0 ||
        temperature > 200.0 ||
        humidity < 0.0 ||
        humidity > 100.0 ||
        battery < 0.0 ||
        battery > 100.0 ||
        timestamp_value < 0.0 ||
        timestamp_value > 9007199254740991.0 ||
        floor(timestamp_value) != timestamp_value) {

        fprintf(stderr, "Error: Invalid telemetry values.\n");
        cJSON_Delete(json);
        return;
    }

    long long timestamp = (long long)timestamp_value;

    pthread_mutex_lock(&devices_mutex);

    struct Device *current_device =
        find_or_create_device(device_id);

    if (current_device == NULL) {
        pthread_mutex_unlock(&devices_mutex);
        cJSON_Delete(json);
        return;
    }

    current_device->temperature = temperature;
    current_device->humidity = humidity;
    current_device->battery = battery;
    current_device->timestamp = timestamp;
    current_device->last_seen = time(NULL);

    if (current_device->inactive) {
        printf("[ONLINE] Device %s is sending messages again.\n",
               current_device->device_id);

        if (log_file != NULL) {
            fprintf(log_file, "[ONLINE] Device %s is sending messages again.\n",
                    current_device->device_id);
        }

        current_device->inactive = false;
    }

    char time_string[20];
    format_hhmmss(timestamp, time_string, sizeof(time_string));

    write_telemetry_output(time_string, current_device, device_count);

    if (temperature > temp_threshold) {
        publish_alert(
            mosq,
            device_id,
            "HIGH_TEMPERATURE",
            temperature,
            timestamp
        );
    }

    if (humidity > humidity_threshold) {
        publish_alert(
            mosq,
            device_id,
            "HIGH_HUMIDITY",
            humidity,
            timestamp
        );
    }

    if (battery < battery_threshold) {
        publish_alert(
            mosq,
            device_id,
            "LOW_BATTERY",
            battery,
            timestamp
        );
    }

    pthread_mutex_unlock(&devices_mutex);

    cJSON_Delete(json);
}


// MQTT connection callback
void on_connect(
    struct mosquitto *mosq,
    void *userdata,
    int result
)
{
    (void)userdata;

    if (result == 0) {
        printf("Connected to MQTT broker.\n");

        int subscribe_result = mosquitto_subscribe(
            mosq,
            NULL,
            "iot/devices/+/telemetry",
            0
        );

        if (subscribe_result == MOSQ_ERR_SUCCESS) {
            printf("Subscribed to iot/devices/+/telemetry\n");
        } else {
            fprintf(
                stderr,
                "Subscription error: %s\n",
                mosquitto_strerror(subscribe_result)
            );
        }
    } else {
        fprintf(
            stderr,
            "MQTT connection failed: %s\n",
            mosquitto_connack_string(result)
        );
    }
}


// MQTT message callback
void on_message(
    struct mosquitto *mosq,
    void *userdata,
    const struct mosquitto_message *msg
)
{
    (void)userdata;

    process_message(mosq, msg);
}


// print device summary and detect inactive devices
void print_device_summary(void)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&devices_mutex);

    printf("\nDEVICE SUMMARY\n");
    printf("Known devices: %d\n", device_count);

    if (log_file != NULL) {
        fprintf(log_file, "\nDEVICE SUMMARY\n");
        fprintf(log_file, "Known devices: %d\n", device_count);
    }

    if (device_count == 0) {
        printf("No devices have sent telemetry yet.\n");
        if (log_file != NULL) {
            fprintf(log_file, "No devices have sent telemetry yet.\n");
        }
    }

    for (int i = 0; i < device_count; i++) {
        struct Device *device = &devices[i];

        double seconds_since_message = difftime(
            now,
            device->last_seen
        );

        if (seconds_since_message >= inactive_timeout &&
            !device->inactive) {

            device->inactive = true;

            printf(
                "[INACTIVE] Device %s has not sent a message for %.0f seconds.\n",
                device->device_id,
                seconds_since_message
            );

            if (log_file != NULL) {
                fprintf(
                    log_file,
                    "[INACTIVE] Device %s has not sent a message for %.0f seconds.\n",
                    device->device_id,
                    seconds_since_message
                );
            }
        }

        char time_string[20];
        format_hhmmss(device->timestamp, time_string, sizeof(time_string));

        printf("\n[%s] Device: %s\n", time_string, device->device_id);
        printf("Temperature: %.1f C\n", device->temperature);
        printf("Humidity: %.1f %%\n", device->humidity);
        printf("Battery: %.0f %%\n", device->battery);
        printf("Last message: %.0f seconds ago\n", seconds_since_message);
        printf("Status: %s\n", device->inactive ? "INACTIVE" : "ACTIVE");

        if (log_file != NULL) {
            fprintf(log_file, "\n[%s] Device: %s\n", time_string, device->device_id);
            fprintf(log_file, "Temperature: %.1f C\n", device->temperature);
            fprintf(log_file, "Humidity: %.1f %%\n", device->humidity);
            fprintf(log_file, "Battery: %.0f %%\n", device->battery);
            fprintf(log_file, "Last message: %.0f seconds ago\n", seconds_since_message);
            fprintf(log_file, "Status: %s\n", device->inactive ? "INACTIVE" : "ACTIVE");
        }
    }

    printf("\n");
    if (log_file != NULL) {
        fprintf(log_file, "\n");
        fflush(log_file);
    }

    pthread_mutex_unlock(&devices_mutex);
}


int main(int argc, char *argv[])
{
    int option;

    while ((option = getopt(argc, argv, "H:P:o:t:u:b:i:h")) != -1) {
        char *end_pointer;

        switch (option) {
            case 'H':
                if (optarg[0] == '\0') {
                    fprintf(stderr, "Error: Host cannot be empty.\n");
                    return 1;
                }

                broker_host = optarg;
                break;

            case 'P': {
                errno = 0;
                long port = strtol(optarg, &end_pointer, 10);

                if (errno == ERANGE ||
                    end_pointer == optarg ||
                    *end_pointer != '\0' ||
                    port < 1 ||
                    port > 65535) {

                    fprintf(stderr, "Error: Invalid port number.\n");
                    return 1;
                }

                broker_port = (int)port;
                break;
            }

            case 'o':
                output_file = optarg;
                break;

            case 't':
                if (!parse_double_argument(optarg, &temp_threshold) ||
                    temp_threshold < -100.0 ||
                    temp_threshold > 200.0) {

                    fprintf(stderr, "Error: Invalid temperature threshold.\n");
                    return 1;
                }
                break;

            case 'u':
                if (!parse_double_argument(optarg, &humidity_threshold) ||
                    humidity_threshold < 0.0 ||
                    humidity_threshold > 100.0) {

                    fprintf(stderr, "Error: Invalid humidity threshold.\n");
                    return 1;
                }
                break;

            case 'b':
                if (!parse_double_argument(optarg, &battery_threshold) ||
                    battery_threshold < 0.0 ||
                    battery_threshold > 100.0) {

                    fprintf(stderr, "Error: Invalid battery threshold.\n");
                    return 1;
                }
                break;

            case 'i': {
                errno = 0;
                long timeout = strtol(optarg, &end_pointer, 10);

                if (errno == ERANGE ||
                    end_pointer == optarg ||
                    *end_pointer != '\0' ||
                    timeout < 1 ||
                    timeout > 86400) {

                    fprintf(stderr, "Error: Invalid inactivity timeout.\n");
                    return 1;
                }

                inactive_timeout = (int)timeout;
                break;
            }

            case 'h':
                print_help(argv[0]);
                return 0;

            default:
                print_help(argv[0]);
                return 1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Error: Unexpected argument: %s\n", argv[optind]);
        print_help(argv[0]);
        return 1;
    }

    if (output_file != NULL) {
        log_file = fopen(output_file, "a");

        if (log_file == NULL) {
            perror("Could not open log file");
            return 1;
        }
    }

    int result = mosquitto_lib_init();

    if (result != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Could not initialize Mosquitto.\n");

        if (log_file != NULL) {
            fclose(log_file);
        }

        return 1;
    }

    struct mosquitto *mosq = mosquitto_new(
        "iot_monitor",
        true,
        NULL
    );

    if (mosq == NULL) {
        fprintf(stderr, "Could not create MQTT client.\n");

        mosquitto_lib_cleanup();

        if (log_file != NULL) {
            fclose(log_file);
        }

        return 1;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);

    mosquitto_reconnect_delay_set(mosq, 1, 10, true);

    result = mosquitto_connect_async(
        mosq,
        broker_host,
        broker_port,
        60
    );

    if (result != MOSQ_ERR_SUCCESS) {
        fprintf(
            stderr,
            "Could not start MQTT connection: %s\n",
            mosquitto_strerror(result)
        );

        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();

        if (log_file != NULL) {
            fclose(log_file);
        }

        return 1;
    }

    result = mosquitto_loop_start(mosq);

    if (result != MOSQ_ERR_SUCCESS) {
        fprintf(
            stderr,
            "Could not start MQTT loop: %s\n",
            mosquitto_strerror(result)
        );

        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();

        if (log_file != NULL) {
            fclose(log_file);
        }

        return 1;
    }

    signal(SIGINT, handle_signal);

    printf("IoT Device Monitor started.\n");
    printf("Broker: %s:%d\n", broker_host, broker_port);

    printf(
        "Alert thresholds: temperature > %.1f C, humidity > %.1f %%, battery < %.1f %%\n",
        temp_threshold,
        humidity_threshold,
        battery_threshold
    );

    printf("Inactive timeout: %d seconds\n", inactive_timeout);
    printf("Press Ctrl+C to stop.\n");

    time_t last_summary = 0;

    while (running) {
        time_t now = time(NULL);

        if (difftime(now, last_summary) >= SUMMARY_INTERVAL) {
            print_device_summary();
            last_summary = now;
        }

        sleep(1);
    }

    printf("\nStopping Monitoring...\n");

    mosquitto_disconnect(mosq);
    mosquitto_loop_stop(mosq, true);

    print_device_summary();

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();

    if (log_file != NULL) {
        fclose(log_file);
    }

    pthread_mutex_destroy(&devices_mutex);

    printf("Monitoring stopped.\n");

    return 0;
}
