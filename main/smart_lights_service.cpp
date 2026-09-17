#include "smart_lights_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "wifi_manager.h"

static const char *TAG = "smart_lights";

static constexpr uint32_t kPollIntervalMs = 5000;
static constexpr int kSocketTimeoutSec = 1;

// WiZ bulbs speak a small UDP/JSON protocol on port 38899 (the config
// default). getPilot asks for the current state; the reply mirrors it back
// under "result" — no auth, no discovery needed since we already have the
// IP from config.
static const char kWizGetPilot[] = R"({"method":"getPilot","params":{}})";

static bool make_dest_addr(const std::string &ip, uint16_t port, struct sockaddr_in &out) {
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &out.sin_addr) == 1;
}

static LightStatus poll_wiz(const SmartLightConfig &light) {
    LightStatus status;
    status.name = light.name;
    status.brand = light.brand;
    status.ip = light.ip;
    status.udp_port = light.udp_port;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed for \"%s\": errno %d", light.name.c_str(), errno);
        return status;
    }

    struct timeval tv = {};
    tv.tv_sec = kSocketTimeoutSec;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr;
    if (!make_dest_addr(light.ip, light.udp_port, dest_addr)) {
        ESP_LOGW(TAG, "Invalid ip \"%s\" for light \"%s\"", light.ip.c_str(), light.name.c_str());
        close(sock);
        return status;
    }

    int sent = sendto(sock, kWizGetPilot, strlen(kWizGetPilot), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (sent < 0) {
        ESP_LOGW(TAG, "sendto() failed for \"%s\": errno %d", light.name.c_str(), errno);
        close(sock);
        return status;
    }

    char buf[512];
    struct sockaddr_in from_addr = {};
    socklen_t from_len = sizeof(from_addr);
    int received = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from_addr, &from_len);
    close(sock);

    if (received <= 0) {
        // Timeout or error — treat as simply unreachable right now, not a
        // hard failure; the next poll cycle will try again.
        return status;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "Bad JSON from \"%s\": %.60s", light.name.c_str(), buf);
        return status;
    }

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsObject(result)) {
        status.online = true;

        const cJSON *state = cJSON_GetObjectItemCaseSensitive(result, "state");
        status.power = cJSON_IsTrue(state);

        const cJSON *dimming = cJSON_GetObjectItemCaseSensitive(result, "dimming");
        status.brightness = cJSON_IsNumber(dimming) ? (uint8_t)dimming->valuedouble : 100;

        const cJSON *scene_id = cJSON_GetObjectItemCaseSensitive(result, "sceneId");
        status.scene_id = cJSON_IsNumber(scene_id) ? (uint8_t)scene_id->valuedouble : 0;

        const cJSON *rj = cJSON_GetObjectItemCaseSensitive(result, "r");
        const cJSON *gj = cJSON_GetObjectItemCaseSensitive(result, "g");
        const cJSON *bj = cJSON_GetObjectItemCaseSensitive(result, "b");
        if (cJSON_IsNumber(rj) && cJSON_IsNumber(gj) && cJSON_IsNumber(bj)) {
            status.r = (uint8_t)rj->valuedouble;
            status.g = (uint8_t)gj->valuedouble;
            status.b = (uint8_t)bj->valuedouble;
        } else {
            // Tunable-white (CCT) mode reports "c"/"w"/"temp" instead of
            // rgb — approximate with a warm-white swatch rather than
            // rendering an actual Kelvin-accurate color.
            status.r = 255;
            status.g = 214;
            status.b = 170;
        }
    }

    cJSON_Delete(root);
    return status;
}

struct TaskArgs {
    std::vector<SmartLightConfig> lights;
    SmartLightsUpdateCb cb;
};

static void smart_lights_task(void *pvArgs) {
    auto *args = static_cast<TaskArgs *>(pvArgs);

    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    while (true) {
        std::vector<LightStatus> results;
        results.reserve(args->lights.size());

        for (const auto &light : args->lights) {
            if (light.brand == "WiZ") {
                results.push_back(poll_wiz(light));
            } else {
                LightStatus placeholder;
                placeholder.name = light.name;
                placeholder.brand = light.brand;
                placeholder.ip = light.ip;
                placeholder.udp_port = light.udp_port;
                results.push_back(placeholder);
            }
        }

        args->cb(results);
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

void smart_lights_service_start(const std::vector<SmartLightConfig> &lights, SmartLightsUpdateCb on_update) {
    if (lights.empty()) {
        ESP_LOGW(TAG, "No lights configured — smart lights service not started");
        return;
    }

    auto *args = new TaskArgs{lights, std::move(on_update)};
    xTaskCreate(smart_lights_task, "smart_lights_task", 4096, args, tskIDLE_PRIORITY + 2, nullptr);
}

// Fire-and-forget: send only, don't wait for/read WiZ's ack. The next poll
// cycle (kPollIntervalMs) picks up whatever the light's state actually
// ended up being, which is simpler and more robust than trying to keep an
// optimistic local copy in sync with reality.
static void send_wiz_command(const std::string &ip, uint16_t udp_port, const char *payload) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed sending to %s:%u: errno %d", ip.c_str(), udp_port, errno);
        return;
    }

    struct sockaddr_in dest_addr;
    if (!make_dest_addr(ip, udp_port, dest_addr)) {
        ESP_LOGW(TAG, "Invalid ip \"%s\"", ip.c_str());
        close(sock);
        return;
    }

    if (sendto(sock, payload, strlen(payload), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGW(TAG, "sendto() failed sending to %s:%u: errno %d", ip.c_str(), udp_port, errno);
    }
    close(sock);
}

void wiz_set_power(const std::string &ip, uint16_t udp_port, bool on) {
    char payload[64];
    snprintf(payload, sizeof(payload), R"({"method":"setPilot","params":{"state":%s}})", on ? "true" : "false");
    send_wiz_command(ip, udp_port, payload);
}

void wiz_set_brightness(const std::string &ip, uint16_t udp_port, uint8_t brightness_pct) {
    // WiZ firmware's practical dimming range is 10-100, not 0-100.
    brightness_pct = std::clamp<uint8_t>(brightness_pct, 10, 100);
    char payload[96];
    snprintf(payload, sizeof(payload), R"({"method":"setPilot","params":{"state":true,"dimming":%u}})",
             (unsigned)brightness_pct);
    send_wiz_command(ip, udp_port, payload);
}

void wiz_set_color(const std::string &ip, uint16_t udp_port, uint8_t r, uint8_t g, uint8_t b) {
    char payload[128];
    snprintf(payload, sizeof(payload), R"({"method":"setPilot","params":{"state":true,"r":%u,"g":%u,"b":%u}})",
             (unsigned)r, (unsigned)g, (unsigned)b);
    send_wiz_command(ip, udp_port, payload);
}

void wiz_set_scene(const std::string &ip, uint16_t udp_port, uint8_t scene_id) {
    char payload[64];
    snprintf(payload, sizeof(payload), R"({"method":"setPilot","params":{"sceneId":%u}})", (unsigned)scene_id);
    send_wiz_command(ip, udp_port, payload);
}
