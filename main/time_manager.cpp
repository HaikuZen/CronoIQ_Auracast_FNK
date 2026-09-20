#include "time_manager.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <string>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "wifi_manager.h"

static const char *TAG = "time_manager";
static std::atomic<bool> s_synced{false};

// lwIP's SNTP client only ever stores a *pointer* to whatever server string
// it's given (sntp_setservername() does not copy it — see sntp.c) and keeps
// using that same pointer indefinitely, including for hourly re-syncs. So
// whatever we hand it must be backed by storage that outlives the function
// that calls esp_netif_sntp_init() — hence a static buffer, never a local
// std::string/std::string::c_str().
static char s_sntp_server_buf[64];

static void on_time_sync(struct timeval *tv) {
    (void)tv;
    ESP_LOGI(TAG, "NTP time synced");
    s_synced = true;
}

static bool resolve_hostname(const char *hostname, std::string &out_ip) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = nullptr;

    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0 || res == nullptr) {
        return false;
    }
    char ip_str[INET_ADDRSTRLEN] = {};
    auto *addr_in = reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
    inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
    out_ip = ip_str;
    freeaddrinfo(res);
    return true;
}

static void start_sntp_with_server(const char *server_ip_or_hostname) {
    strlcpy(s_sntp_server_buf, server_ip_or_hostname, sizeof(s_sntp_server_buf));
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_sntp_server_buf);
    sntp_config.start = true;
    sntp_config.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SNTP started (server=%s)", s_sntp_server_buf);
    }
}

static void time_manager_task(void *pvArgs) {
    auto *cfg = static_cast<NtpConfig *>(pvArgs);

    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    setenv("TZ", cfg->posix_tz.c_str(), 1);
    tzset();
    ESP_LOGI(TAG, "TZ set to %s", cfg->posix_tz.c_str());

    std::string hostname = cfg->server;
    delete cfg;

    // lwIP's own hostname-based DNS resolution for the SNTP server (its
    // dns_gethostbyname() call inside sntp_request()) was observed never
    // succeeding on real hardware, indefinitely, even though DNS and the
    // network path both work fine otherwise — confirmed independently with
    // `ntpdate -q <server>` from another host on the same network while
    // this device sat stuck on "not synced". The exact reason wasn't
    // isolated further (a debugger would be needed), but pre-resolving the
    // hostname ourselves and configuring SNTP with the literal IP address
    // sidesteps lwIP's internal resolution path entirely: its
    // dns_gethostbyname() recognizes a literal IP string immediately, with
    // no DNS query involved at all, so whatever was going wrong there
    // can't matter anymore.
    std::string resolved_ip;
    if (resolve_hostname(hostname.c_str(), resolved_ip)) {
        ESP_LOGI(TAG, "Pre-resolved NTP server \"%s\" to %s", hostname.c_str(), resolved_ip.c_str());
        start_sntp_with_server(resolved_ip.c_str());
    } else {
        ESP_LOGW(TAG, "Could not pre-resolve NTP server \"%s\" — letting SNTP resolve it internally "
                      "(may hit the same issue)",
                 hostname.c_str());
        start_sntp_with_server(hostname.c_str());
    }

    // Check in periodically until synced: every minute for the first 10
    // minutes, then every 10 minutes indefinitely. If still unsynced after
    // a check, re-resolve and restart SNTP with the (possibly different —
    // "pool.ntp.org" round-robins across many independent real servers)
    // freshly-resolved address, since whatever address it's currently
    // using clearly isn't working and it won't recover from that on its
    // own.
    int elapsed_sec = 0;
    while (!s_synced.load()) {
        int delay_sec = (elapsed_sec < 600) ? 60 : 600;
        vTaskDelay(pdMS_TO_TICKS(delay_sec * 1000));
        elapsed_sec += delay_sec;
        if (s_synced.load()) break;

        ESP_LOGW(TAG, "NTP still not synced after ~%d minute(s)", elapsed_sec / 60);
        std::string ip;
        if (resolve_hostname(hostname.c_str(), ip)) {
            ESP_LOGI(TAG, "Re-resolved \"%s\" to %s — restarting SNTP with it", hostname.c_str(), ip.c_str());
            esp_netif_sntp_deinit();
            start_sntp_with_server(ip.c_str());
        } else {
            ESP_LOGW(TAG,
                     "DNS lookup for \"%s\" FAILED this time — this is a DNS/network problem, not "
                     "something a firmware retry can fix",
                     hostname.c_str());
        }
    }

    vTaskDelete(nullptr);
}

void time_manager_start(const NtpConfig &cfg) {
    // Waits for wifi_manager_is_connected() on a background task rather than
    // blocking app_main — esp_netif_sntp_init() needs a live network route,
    // and starting it before one exists just means silently-failing sync
    // attempts until Wi-Fi comes up anyway.
    auto *cfg_copy = new NtpConfig(cfg);
    xTaskCreate(time_manager_task, "time_manager_task", 4096, cfg_copy, tskIDLE_PRIORITY + 2, nullptr);
}

bool time_manager_is_synced(void) {
    return s_synced.load();
}

static void get_local_tm(struct tm *out_tm) {
    time_t now = time(nullptr);
    localtime_r(&now, out_tm);
}

void time_manager_get_time_str(char *buf, size_t len) {
    struct tm t;
    get_local_tm(&t);
    strftime(buf, len, "%H:%M:%S", &t);
}

void time_manager_get_date_str(char *buf, size_t len) {
    struct tm t;
    get_local_tm(&t);
    strftime(buf, len, "%d %B %Y", &t);
}

void time_manager_get_weekday_str(char *buf, size_t len) {
    struct tm t;
    get_local_tm(&t);
    strftime(buf, len, "%A", &t);
}
