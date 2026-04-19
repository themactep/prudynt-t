/**
 * daynightd.c - Standalone Day/Night Sensing Daemon for Ingenic-based cameras
 *
 * This daemon monitors light levels by parsing ISP statistics from procfs,
 * maintains a local history in SQLite (optional), exports metrics to MQTT (optional)
 * and Graphite/Grafana (optional), and automates the switching between Day and Night
 * modes via a helper script.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>

#ifdef ENABLE_SQLITE
#include <sqlite3.h>
#endif

#ifdef ENABLE_MQTT
#include <mosquitto.h>
#endif

/* Path to Ingenic ISP statistics in procfs */
#define ISP_STATS_PATH "/proc/jz/isp/isp-m0"
/* Local SQLite database path (tmpfs recommended) */
#define DB_PATH "/run/daynight.db"
/* Hardware control script */
#define TOGGLE_SCRIPT "/sbin/daynight"
/* Sampling interval in seconds */
#define SAMPLE_INTERVAL 5
/* Standard Graphite plaintext protocol port */
#define GRAPHITE_PORT 2003

/* Switching thresholds (as percentage) */
#define THRESHOLD_DAY_TO_NIGHT 15.0f
#define THRESHOLD_NIGHT_TO_DAY 80.0f
/* Sensor-specific gain limits for normalization */
#define MAX_ANALOG_GAIN 160.0f
#define MAX_DIGITAL_GAIN 80.0f

typedef struct {
    char mode[16];
    int integration_time;
    int max_integration_time;
    int analog_gain;
    int digital_gain;
    float brightness;
} LightStats;

static volatile int running = 1;

void signal_handler(int sig) {
    running = 0;
}

int parse_isp_stats(LightStats *stats) {
    FILE *f = fopen(ISP_STATS_PATH, "r");
    if (!f) return -1;

    char line[256];
    memset(stats, 0, sizeof(LightStats));
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "ISP Runing Mode :")) {
            sscanf(line, "ISP Runing Mode : %15s", stats->mode);
        } else if (strstr(line, "SENSOR Integration Time :")) {
            sscanf(line, "SENSOR Integration Time : %d", &stats->integration_time);
        } else if (strstr(line, "SENSOR Max Integration Time :")) {
            sscanf(line, "SENSOR Max Integration Time : %d", &stats->max_integration_time);
        } else if (strstr(line, "SENSOR analog gain :")) {
            sscanf(line, "SENSOR analog gain : %d", &stats->analog_gain);
        } else if (strstr(line, "SENSOR digital gain :")) {
            sscanf(line, "SENSOR digital gain : %d", &stats->digital_gain);
        }
    }
    fclose(f);

    if (stats->max_integration_time > 0) {
        float it_r = (float)stats->integration_time / stats->max_integration_time;
        float ag_r = (float)stats->analog_gain / MAX_ANALOG_GAIN;
        float dg_r = (float)stats->digital_gain / MAX_DIGITAL_GAIN;
        stats->brightness = (1.0f - (it_r * ag_r * dg_r)) * 100.0f;
    } else {
        stats->brightness = -1.0f;
    }
    return 0;
}

#ifdef ENABLE_SQLITE
void save_to_sqlite(sqlite3 *db, const LightStats *stats) {
    char query[256];
    snprintf(query, sizeof(query), 
             "INSERT INTO readings (timestamp, mode, brightness, it, ag, dg) "
             "VALUES (%ld, '%s', %.2f, %d, %d, %d);",
             time(NULL), stats->mode, stats->brightness, 
             stats->integration_time, stats->analog_gain, stats->digital_gain);
    sqlite3_exec(db, query, NULL, NULL, NULL);
}
#endif

#ifdef ENABLE_MQTT
void export_to_mqtt(struct mosquitto *mosq, const LightStats *stats) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"brightness\":%.2f, \"mode\":\"%s\", \"it\":%d}", 
             stats->brightness, stats->mode, stats->integration_time);
    mosquitto_publish(mosq, NULL, "camera/daynight/stats", strlen(buf), buf, 0, false);
}
#endif

#ifdef ENABLE_GRAPHITE
void export_to_graphite(const char *host, const LightStats *stats) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;
    
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(GRAPHITE_PORT);
    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        close(sock);
        return;
    }
    
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return;
    }
    
    char buf[256];
    long now = time(NULL);
    snprintf(buf, sizeof(buf), "camera.brightness %.2f %ld\ncamera.it %d %ld\n", 
             stats->brightness, now, stats->integration_time, now);
    send(sock, buf, strlen(buf), 0);
    close(sock);
}
#endif

void check_and_toggle(const LightStats *stats) {
    if (stats->brightness < 0) return;
    int is_night = (strcmp(stats->mode, "night") == 0);

    if (!is_night && stats->brightness < THRESHOLD_DAY_TO_NIGHT) {
        printf("Switching to Night (Brightness: %.2f%%)\n", stats->brightness);
        system(TOGGLE_SCRIPT " night");
    } else if (is_night && stats->brightness > THRESHOLD_NIGHT_TO_DAY) {
        printf("Switching to Day (Brightness: %.2f%%)\n", stats->brightness);
        system(TOGGLE_SCRIPT " day");
    }
}

int main(int argc, char **argv) {
#ifdef ENABLE_MQTT
    const char *mqtt_host = argc > 1 ? argv[1] : "localhost";
#endif
#ifdef ENABLE_GRAPHITE
    const char *graphite_host = argc > 2 ? argv[2] : NULL;
#endif

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

#ifdef ENABLE_SQLITE
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS readings (timestamp INTEGER, mode TEXT, brightness REAL, it INTEGER, ag INTEGER, dg INTEGER);", NULL, NULL, NULL);
#endif

#ifdef ENABLE_MQTT
    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("daynightd", true, NULL);
    if (mosq) {
        mosquitto_connect(mosq, mqtt_host, 1883, 60);
    }
#endif

    while (running) {
        LightStats stats;
        if (parse_isp_stats(&stats) == 0) {
#ifdef ENABLE_SQLITE
            save_to_sqlite(db, &stats);
#endif
#ifdef ENABLE_MQTT
            if (mosq) export_to_mqtt(mosq, &stats);
#endif
#ifdef ENABLE_GRAPHITE
            if (graphite_host) export_to_graphite(graphite_host, &stats);
#endif
            check_and_toggle(&stats);
        }
        sleep(SAMPLE_INTERVAL);
    }

#ifdef ENABLE_MQTT
    if (mosq) {
        mosquitto_destroy(mosq);
    }
    mosquitto_lib_cleanup();
#endif

#ifdef ENABLE_SQLITE
    sqlite3_close(db);
#endif

    return 0;
}
