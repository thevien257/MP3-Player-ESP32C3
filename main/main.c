#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <dirent.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"
#include "mp3dec.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_http_server.h"

// Add these includes at the top with other includes
#include "esp_wifi_types.h"
#include "nvs.h"

// Add these as global variables
static sdmmc_host_t global_host;
static sdspi_device_config_t global_slot_config;
static sdmmc_card_t *global_card = NULL;

static const char *TAG = "MP3_PLAYER";

// --- VOLUME SETTING (0-100) ---
#define VOLUME_PERCENT 5

// --- PIN CONFIGURATION ---
// I2S
#define I2S_BCK_IO GPIO_NUM_4
#define I2S_WS_IO GPIO_NUM_5
#define I2S_DO_IO GPIO_NUM_6

// SD Card
#define SD_MISO GPIO_NUM_3
#define SD_MOSI GPIO_NUM_7
#define SD_CLK GPIO_NUM_2
#define SD_CS GPIO_NUM_10

// OLED
#define OLED_SDA GPIO_NUM_19
#define OLED_SCL GPIO_NUM_18

// === WiFi Configuration ===
#define WIFI_SSID "Ha Tinh"
#define WIFI_PASS "98764321"

// === CRITICAL OPTIMIZATION: Increase buffer to 64KB ===
// #define UPLOAD_BUFFER_SIZE (4 * 1024)
// #define RECEIVE_BUFFER_SIZE (4 * 1024)

// --- DEFINES FOR DYNAMIC SIZES ---
#define MP3_BUF_SIZE_PLAYING (16 * 1024)
#define UPLOAD_BUF_SIZE_WIFI (16 * 1024)
#define RECV_BUF_SIZE_WIFI (16 * 1024)

// Add these defines near WiFi configuration section
#define DEFAULT_AP_SSID "MP3Player_Config"
#define DEFAULT_AP_PASS "12345678"
#define NVS_WIFI_NAMESPACE "wifi_config"
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64

// Add these global variables
char stored_ssid[MAX_SSID_LEN] = "";
char stored_password[MAX_PASS_LEN] = "";
bool wifi_config_mode = false;
httpd_handle_t config_server = NULL;

// === STATIC BUFFERS - Tránh fragmentation ===
uint8_t *input_buffer = NULL;       // Allocated only during playback
uint8_t *upload_buffer_ptr = NULL;  // Allocated only during WiFi
uint8_t *receive_buffer_ptr = NULL; // Allocated only during WiFi

static size_t buffer_index = 0;
static FILE *upload_file = NULL;
static size_t total_received = 0;
static int64_t upload_start_time = 0;

httpd_handle_t server = NULL;
bool isWifiInitialized = false; // To prevent double-init crash

#define MOUNT_POINT "/sdcard"

// --- CRITICAL BUFFER SETTINGS FOR ESP32-C3 SINGLE CORE ---
// #define MP3_INPUT_BUFFER_SIZE (8 * 1024)
#define PCM_FRAME_SAMPLES (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)

short output_buffer[PCM_FRAME_SAMPLES];
// uint8_t input_buffer[MP3_INPUT_BUFFER_SIZE];

u8g2_t u8g2;
i2s_chan_handle_t tx_handle = NULL;

// Global variables
TaskHandle_t playbackTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;
volatile bool stopPlayback = false;
volatile bool isPlayerActive = false; // Flag to tell us if play_file is using memory

FILE *currentAudioFile = NULL;
size_t currentFileSize = 0;
size_t currentFilePosition = 0;

// Button Pin Configuration
#define BTN_MENU 0
#define BTN_CENTER 1
#define BTN_UP 8
#define BTN_DOWN 9
#define BTN_LEFT 20
#define BTN_RIGHT 21

// Button states
volatile bool buttonPressed[6] = {false, false, false, false, false, false};
volatile uint32_t lastButtonTime[6] = {0, 0, 0, 0, 0, 0};
const uint32_t DEBOUNCE_DELAY_MS = 250;

// Menu system
typedef enum
{
    MODE_PLAYING,
    MODE_MENU,
    MODE_PLAYLIST,
    MODE_VOLUME
} MenuMode;

MenuMode currentMode = MODE_PLAYING;
int menuSelection = 0;
const int menuItems = 7;

typedef enum
{
    AUTOPLAY_OFF,
    AUTOPLAY_ON,
    AUTOPLAY_RANDOM
} AutoPlayMode;

AutoPlayMode autoPlayMode = AUTOPLAY_ON;

// Playback state
bool isPlaying = false;
bool isPaused = false;
uint32_t playbackStartTime = 0;
uint32_t totalPausedTime = 0;
uint32_t pauseStartTime = 0;

volatile bool changeTrack = false;
volatile int nextTrackIndex = 0;

// Animation state
int waveformPhase = 0;
int volumeAnimCurrent = 5;
int volumeAnimTarget = 5;

// Current track info
char currentTrackName[400] = "Unknown";
int currentTrack = 0;
int totalTracks = 0;

// Display variables
char lastButtonName[20] = "None";
int loading_frame = 0;

// Playlist scrolling state
int playlistSelection = 0;
uint32_t playlistScrollStartTime = 0;
int playlistScrollOffset = 0;
uint32_t lastPlaylistScrollTime = 0;

// Playlist structure
typedef struct
{
    char filepath[256];
    char displayname[128];
    char shortname[32];
} PlaylistItem;

PlaylistItem *playlist = NULL;
int playlistSize = 0;
int playlistCapacity = 0;

void show_ready_screen(int track_count);
void show_playing_screen(void);
void handle_buttons(void);
void show_playlist_screen(void);
void show_volume_screen(void);
void show_loading_screen(const char *message);                 // Added
void show_error_screen(const char *error, const char *detail); // Added
void show_wifi_info_screen(void);                              // Added
static httpd_handle_t start_webserver(void);                   // Added
bool add_to_playlist(const char *filepath, const char *displayname);
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

// Add near the top with other helper functions
static void sync_directory(const char *filepath)
{
    // Extract directory path
    char dirpath[256];
    strncpy(dirpath, filepath, sizeof(dirpath) - 1);
    dirpath[sizeof(dirpath) - 1] = '\0';

    char *last_slash = strrchr(dirpath, '/');
    if (last_slash)
    {
        *last_slash = '\0'; // Truncate to get directory path

        // Open and sync the directory
        DIR *dir = opendir(dirpath);
        if (dir)
        {
            int dir_fd = dirfd(dir);
            if (dir_fd >= 0)
            {
                fsync(dir_fd); // Flush directory metadata
            }
            closedir(dir);
        }
    }
}

// WiFi Configuration Storage Functions
esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        printf("Error opening NVS: %d\n", err);
        return err;
    }

    err = nvs_set_str(nvs_handle, "ssid", ssid);
    if (err != ESP_OK)
    {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, "password", password);
    if (err != ESP_OK)
    {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK)
    {
        strncpy(stored_ssid, ssid, MAX_SSID_LEN - 1);
        strncpy(stored_password, password, MAX_PASS_LEN - 1);
        printf("WiFi credentials saved: %s\n", ssid);
    }

    return err;
}

esp_err_t load_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        printf("No saved WiFi credentials\n");
        return err;
    }

    size_t ssid_len = MAX_SSID_LEN;
    size_t pass_len = MAX_PASS_LEN;

    err = nvs_get_str(nvs_handle, "ssid", stored_ssid, &ssid_len);
    if (err == ESP_OK)
    {
        err = nvs_get_str(nvs_handle, "password", stored_password, &pass_len);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK)
    {
        printf("Loaded WiFi credentials: %s\n", stored_ssid);
    }

    return err;
}

esp_err_t clear_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
        return err;

    nvs_erase_all(nvs_handle);
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    memset(stored_ssid, 0, MAX_SSID_LEN);
    memset(stored_password, 0, MAX_PASS_LEN);

    printf("WiFi credentials cleared\n");
    return err;
}

// URL decode helper function
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src)
    {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b)))
        {
            if (a >= 'a')
                a -= 'a' - 'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a')
                b -= 'a' - 'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        }
        else if (*src == '+')
        {
            *dst++ = ' ';
            src++;
        }
        else
        {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// HTTP Handlers for WiFi Configuration
static esp_err_t config_root_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html><head>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>MP3 Player WiFi Setup</title>"
        "<style>"
        "body{font-family:Arial;margin:0;padding:20px;background:#f0f0f0}"
        ".container{max-width:400px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
        "h2{color:#333;margin-top:0}"
        "input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:4px}"
        "button{width:100%;padding:12px;background:#4CAF50;color:white;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin-top:10px}"
        "button:hover{background:#45a049}"
        ".info{background:#e7f3ff;padding:10px;border-radius:4px;margin:10px 0;font-size:14px}"
        ".clear-btn{background:#f44336;margin-top:20px}"
        ".clear-btn:hover{background:#da190b}"
        ".status{padding:10px;margin:10px 0;border-radius:4px;display:none}"
        ".success{background:#d4edda;color:#155724}"
        ".error{background:#f8d7da;color:#721c24}"
        "</style>"
        "</head><body>"
        "<div class='container'>"
        "<h2>MP3 Player WiFi Setup</h2>"
        "<div class='info'>Configure your WiFi credentials to enable music upload</div>"
        "<form id='wifiForm'>"
        "<label>WiFi Network Name (SSID):</label>"
        "<input type='text' id='ssid' name='ssid' placeholder='Enter WiFi SSID' required maxlength='31'>"
        "<label>WiFi Password:</label>"
        "<input type='password' id='password' name='password' placeholder='Enter WiFi Password' required maxlength='63'>"
        "<button type='submit'>Save & Connect</button>"
        "</form>"
        "<div id='status' class='status'></div>"
        "<button class='clear-btn' onclick='clearConfig()'>Clear Saved WiFi</button>"
        "<div class='info' style='margin-top:20px'>"
        "After saving, the device will restart and connect to your WiFi network. "
        "You can then access the upload page at the device's IP address."
        "</div>"
        "</div>"
        "<script>"
        "document.getElementById('wifiForm').onsubmit = function(e) {"
        "  e.preventDefault();"
        "  var ssid = document.getElementById('ssid').value;"
        "  var pass = document.getElementById('password').value;"
        "  var status = document.getElementById('status');"
        "  fetch('/save?ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pass))"
        "  .then(r => r.text())"
        "  .then(data => {"
        "    status.className = 'status success';"
        "    status.style.display = 'block';"
        "    status.textContent = 'Saved! Device will restart in 3 seconds...';"
        "    setTimeout(() => { status.textContent = 'Restarting now...'; }, 3000);"
        "  })"
        "  .catch(err => {"
        "    status.className = 'status error';"
        "    status.style.display = 'block';"
        "    status.textContent = 'Error: ' + err;"
        "  });"
        "};"
        "function clearConfig() {"
        "  if(confirm('Clear saved WiFi credentials?')) {"
        "    fetch('/clear').then(() => {"
        "      var status = document.getElementById('status');"
        "      status.className = 'status success';"
        "      status.style.display = 'block';"
        "      status.textContent = 'WiFi credentials cleared! Restarting...';"
        "      setTimeout(() => location.reload(), 2000);"
        "    });"
        "  }"
        "}"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t config_save_handler(httpd_req_t *req)
{
    char buf[200];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing parameters");
        return ESP_FAIL;
    }

    char ssid_encoded[MAX_SSID_LEN] = {0};
    char password_encoded[MAX_PASS_LEN] = {0};
    char ssid[MAX_SSID_LEN] = {0};
    char password[MAX_PASS_LEN] = {0};

    // Get encoded values
    if (httpd_query_key_value(buf, "ssid", ssid_encoded, sizeof(ssid_encoded)) != ESP_OK ||
        httpd_query_key_value(buf, "password", password_encoded, sizeof(password_encoded)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid parameters");
        return ESP_FAIL;
    }

    // Decode URL encoding (e.g., "Ha%20Tinh" -> "Ha Tinh")
    url_decode(ssid, ssid_encoded);
    url_decode(password, password_encoded);

    if (strlen(ssid) == 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
        return ESP_FAIL;
    }

    printf("Decoded SSID: %s\n", ssid); // This will now show "Ha Tinh" correctly

    esp_err_t err = save_wifi_credentials(ssid, password);
    if (err == ESP_OK)
    {
        httpd_resp_sendstr(req, "OK");
        // Schedule restart after 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
    }

    return err;
}
static esp_err_t config_clear_handler(httpd_req_t *req)
{
    esp_err_t err = clear_wifi_credentials();
    if (err == ESP_OK)
    {
        httpd_resp_sendstr(req, "OK");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    else
    {
        httpd_resp_send_500(req);
    }
    return err;
}

// Start WiFi AP Configuration Server
static httpd_handle_t start_config_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 6144;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = config_root_handler};
        httpd_register_uri_handler(server, &root);

        httpd_uri_t save = {
            .uri = "/save",
            .method = HTTP_GET,
            .handler = config_save_handler};
        httpd_register_uri_handler(server, &save);

        httpd_uri_t clear = {
            .uri = "/clear",
            .method = HTTP_GET,
            .handler = config_clear_handler};
        httpd_register_uri_handler(server, &clear);

        printf("Config server started\n");
        return server;
    }

    return NULL;
}

// Initialize WiFi in AP mode for configuration
void start_wifi_config_mode(void)
{
    wifi_config_mode = true;

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = DEFAULT_AP_SSID,
            .password = DEFAULT_AP_PASS,
            .ssid_len = strlen(DEFAULT_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 2}};

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    printf("WiFi AP started: %s\n", DEFAULT_AP_SSID);
    printf("Password: %s\n", DEFAULT_AP_PASS);
    printf("Connect and go to: http://192.168.4.1\n");
}

// Modified wifi_init_sta to use stored credentials
static void wifi_init_sta_stored(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, stored_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, stored_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    esp_wifi_start();

    esp_netif_set_default_netif(sta_netif);
}

// === Helper: Xóa playlist cũ để giải phóng RAM ===
void free_playlist(void)
{
    if (playlist != NULL)
    {
        free(playlist);
        playlist = NULL;
    }
    playlistSize = 0;
    playlistCapacity = 0;
    printf("Playlist cleared.\n");
}

// === Helper: Quét thẻ nhớ tìm file MP3 ===
void scan_mp3_files(void)
{
    printf("Scanning SD card for MP3 files...\n");

    // Đảm bảo playlist trống trước khi scan
    free_playlist();

    DIR *dir = opendir("/sdcard");
    struct dirent *ent;

    if (dir)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            // Kiểm tra đuôi file .mp3 hoặc .MP3
            if (strstr(ent->d_name, ".mp3") || strstr(ent->d_name, ".MP3"))
            {
                size_t name_len = strlen(ent->d_name);
                // Giới hạn độ dài tên file để tránh tràn bộ nhớ
                if (name_len < 240)
                {
                    char filepath[256];
                    snprintf(filepath, sizeof(filepath), "/sdcard/%s", ent->d_name);

                    char displayname[128];
                    strncpy(displayname, ent->d_name, sizeof(displayname) - 1);
                    displayname[sizeof(displayname) - 1] = '\0';

                    // Xóa đuôi .mp3 khi hiển thị cho đẹp
                    char *ext = strstr(displayname, ".mp3");
                    if (ext)
                        *ext = '\0';
                    ext = strstr(displayname, ".MP3");
                    if (ext)
                        *ext = '\0';

                    if (!add_to_playlist(filepath, displayname))
                    {
                        printf("Playlist full or RAM full!\n");
                        break;
                    }
                }
            }
        }
        closedir(dir);
    }
    else
    {
        printf("Failed to open directory /sdcard\n");
    }

    printf("Scan finished. Found %d tracks.\n", playlistSize);
}

// === WiFi Event Handler ===
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    }
}

// === Helper: Flush buffer với error handling ===
static inline esp_err_t flush_buffer_to_sd(FILE *file, const uint8_t *buffer, size_t size)
{
    if (size == 0)
        return ESP_OK;

    size_t aligned_size = (size / 512) * 512;
    size_t remainder = size % 512;

    if (aligned_size > 0)
    {
        size_t written = fwrite(buffer, 1, aligned_size, file);
        if (written != aligned_size)
        {
            return ESP_FAIL;
        }
    }

    if (remainder > 0)
    {
        size_t written = fwrite(buffer + aligned_size, 1, remainder, file);
        if (written != remainder)
        {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

// === Initialize WiFi with Performance Optimizations ===
static void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    esp_wifi_set_max_tx_power(84);

    esp_wifi_start();

    esp_netif_set_default_netif(sta_netif);
}

bool add_to_playlist(const char *filepath, const char *displayname)
{
    if (playlistSize >= playlistCapacity)
    {
        int newCapacity = playlistCapacity + 20;
        PlaylistItem *newPlaylist = (PlaylistItem *)realloc(playlist, newCapacity * sizeof(PlaylistItem));

        if (newPlaylist == NULL)
        {
            return false;
        }

        playlist = newPlaylist;
        playlistCapacity = newCapacity;
    }

    strncpy(playlist[playlistSize].filepath, filepath, sizeof(playlist[playlistSize].filepath) - 1);
    playlist[playlistSize].filepath[sizeof(playlist[playlistSize].filepath) - 1] = '\0';

    strncpy(playlist[playlistSize].displayname, displayname, sizeof(playlist[playlistSize].displayname) - 1);
    playlist[playlistSize].displayname[sizeof(playlist[playlistSize].displayname) - 1] = '\0';

    playlistSize++;
    return true;
}

void vietnamese_to_ascii(const char *input, char *output, size_t output_size)
{
    size_t in_idx = 0;
    size_t out_idx = 0;
    size_t input_len = strlen(input);

    while (in_idx < input_len && out_idx < output_size - 1)
    {
        unsigned char c = input[in_idx];

        if (c < 0x80)
        {
            output[out_idx++] = c;
            in_idx++;
        }
        else if (c >= 0xC0 && in_idx + 1 < input_len)
        {
            unsigned char c2 = input[in_idx + 1];
            uint16_t utf16 = ((c & 0x1F) << 6) | (c2 & 0x3F);

            if (c >= 0xE0 && in_idx + 2 < input_len)
            {
                unsigned char c3 = input[in_idx + 2];
                utf16 = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
                in_idx += 3;
            }
            else
            {
                in_idx += 2;
            }

            char replacement = 0;

            if ((utf16 >= 0x00E0 && utf16 <= 0x00E3) || utf16 == 0x00E1 ||
                utf16 == 0x1EA3 || utf16 == 0x1EA1 || utf16 == 0x0103 ||
                (utf16 >= 0x1EAF && utf16 <= 0x1EB7) || utf16 == 0x00E2 ||
                (utf16 >= 0x1EA5 && utf16 <= 0x1EAD))
            {
                replacement = 'a';
            }
            else if ((utf16 >= 0x00C0 && utf16 <= 0x00C3) || utf16 == 0x00C1 ||
                     utf16 == 0x1EA2 || utf16 == 0x1EA0 || utf16 == 0x0102 ||
                     (utf16 >= 0x1EAE && utf16 <= 0x1EB6) || utf16 == 0x00C2 ||
                     (utf16 >= 0x1EA4 && utf16 <= 0x1EAC))
            {
                replacement = 'A';
            }
            else if ((utf16 >= 0x00E8 && utf16 <= 0x00E9) || utf16 == 0x1EBB ||
                     utf16 == 0x1EBD || utf16 == 0x1EB9 || utf16 == 0x00EA ||
                     (utf16 >= 0x1EBF && utf16 <= 0x1EC7))
            {
                replacement = 'e';
            }
            else if ((utf16 >= 0x00C8 && utf16 <= 0x00C9) || utf16 == 0x1EBA ||
                     utf16 == 0x1EBC || utf16 == 0x1EB8 || utf16 == 0x00CA ||
                     (utf16 >= 0x1EBE && utf16 <= 0x1EC6))
            {
                replacement = 'E';
            }
            else if ((utf16 >= 0x00EC && utf16 <= 0x00ED) || utf16 == 0x1EC9 || utf16 == 0x1ECB)
            {
                replacement = 'i';
            }
            else if ((utf16 >= 0x00CC && utf16 <= 0x00CD) || utf16 == 0x1EC8 || utf16 == 0x1ECA)
            {
                replacement = 'I';
            }
            else if ((utf16 >= 0x00F2 && utf16 <= 0x00F5) || utf16 == 0x00F3 ||
                     utf16 == 0x1ECF || utf16 == 0x1ECD || utf16 == 0x00F4 ||
                     (utf16 >= 0x1ED1 && utf16 <= 0x1ED9) || utf16 == 0x01A1 ||
                     (utf16 >= 0x1EDB && utf16 <= 0x1EE3))
            {
                replacement = 'o';
            }
            else if ((utf16 >= 0x00D2 && utf16 <= 0x00D5) || utf16 == 0x00D3 ||
                     utf16 == 0x1ECE || utf16 == 0x1ECC || utf16 == 0x00D4 ||
                     (utf16 >= 0x1ED0 && utf16 <= 0x1ED8) || utf16 == 0x01A0 ||
                     (utf16 >= 0x1EDA && utf16 <= 0x1EE2))
            {
                replacement = 'O';
            }
            else if ((utf16 >= 0x00F9 && utf16 <= 0x00FA) || utf16 == 0x1EE7 ||
                     utf16 == 0x0169 || utf16 == 0x1EE5 || utf16 == 0x01B0 ||
                     (utf16 >= 0x1EE9 && utf16 <= 0x1EF1))
            {
                replacement = 'u';
            }
            else if ((utf16 >= 0x00D9 && utf16 <= 0x00DA) || utf16 == 0x1EE6 ||
                     utf16 == 0x0168 || utf16 == 0x1EE4 || utf16 == 0x01AF ||
                     (utf16 >= 0x1EE8 && utf16 <= 0x1EF0))
            {
                replacement = 'U';
            }
            else if (utf16 == 0x00FD || (utf16 >= 0x1EF3 && utf16 <= 0x1EF9) || utf16 == 0x1EF5)
            {
                replacement = 'y';
            }
            else if (utf16 == 0x00DD || (utf16 >= 0x1EF2 && utf16 <= 0x1EF8) || utf16 == 0x1EF4)
            {
                replacement = 'Y';
            }
            else if (utf16 == 0x0111)
            {
                replacement = 'd';
            }
            else if (utf16 == 0x0110)
            {
                replacement = 'D';
            }
            else if (utf16 < 128)
            {
                replacement = (char)utf16;
            }

            if (replacement != 0)
            {
                output[out_idx++] = replacement;
            }
        }
        else
        {
            in_idx++;
        }
    }

    output[out_idx] = '\0';
}

void display_update_task(void *pvParameters)
{
    uint32_t last_update = 0;
    const uint32_t DISPLAY_UPDATE_INTERVAL = 150;

    while (1)
    {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (now - last_update >= DISPLAY_UPDATE_INTERVAL)
        {
            if (currentMode == MODE_PLAYING && isPlaying)
            {
                show_playing_screen();
            }
            else if (currentMode == MODE_PLAYLIST)
            {
                show_playlist_screen();
            }
            else if (currentMode == MODE_VOLUME)
            {
                show_volume_screen();
            }
            last_update = now;
        }

        handle_buttons();

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void IRAM_ATTR menuISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    if (now - lastButtonTime[0] > DEBOUNCE_DELAY_MS)
    {
        buttonPressed[0] = true;
        lastButtonTime[0] = now;
    }
}

static void IRAM_ATTR centerISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    if (now - lastButtonTime[1] > DEBOUNCE_DELAY_MS)
    {
        buttonPressed[1] = true;
        lastButtonTime[1] = now;
    }
}

static void IRAM_ATTR upISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    if (now - lastButtonTime[2] > DEBOUNCE_DELAY_MS)
    {
        buttonPressed[2] = true;
        lastButtonTime[2] = now;
    }
}

static void IRAM_ATTR downISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    if (now - lastButtonTime[3] > DEBOUNCE_DELAY_MS)
    {
        buttonPressed[3] = true;
        lastButtonTime[3] = now;
    }
}

static void IRAM_ATTR leftISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    if (now - lastButtonTime[4] > DEBOUNCE_DELAY_MS)
    {
        buttonPressed[4] = true;
        lastButtonTime[4] = now;
    }
}

static void IRAM_ATTR rightISR(void *arg)
{
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    if (now - lastButtonTime[5] > DEBOUNCE_DELAY_MS)
    {
        buttonPressed[5] = true;
        lastButtonTime[5] = now;
    }
}

void setup_buttons(void)
{
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    gpio_install_isr_service(0);

    io_conf.pin_bit_mask = (1ULL << BTN_MENU);
    gpio_config(&io_conf);
    gpio_isr_handler_add(BTN_MENU, menuISR, NULL);

    io_conf.pin_bit_mask = (1ULL << BTN_CENTER);
    gpio_config(&io_conf);
    gpio_isr_handler_add(BTN_CENTER, centerISR, NULL);

    io_conf.pin_bit_mask = (1ULL << BTN_UP);
    gpio_config(&io_conf);
    gpio_isr_handler_add(BTN_UP, upISR, NULL);

    io_conf.pin_bit_mask = (1ULL << BTN_DOWN);
    gpio_config(&io_conf);
    gpio_isr_handler_add(BTN_DOWN, downISR, NULL);

    // io_conf.pin_bit_mask = (1ULL << BTN_LEFT);
    // gpio_config(&io_conf);
    // gpio_isr_handler_add(BTN_LEFT, leftISR, NULL);

    // io_conf.pin_bit_mask = (1ULL << BTN_RIGHT);
    // gpio_config(&io_conf);
    // gpio_isr_handler_add(BTN_RIGHT, rightISR, NULL);
}

bool is_button_pressed(int button_index)
{
    if (buttonPressed[button_index])
    {
        buttonPressed[button_index] = false;
        return true;
    }
    return false;
}

int debug_input_bytes_consumed = 0;
float debug_progress_percent = 0.0f;

void show_playlist_screen(void)
{
    u8g2_ClearBuffer(&u8g2);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, 12);
    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
    const char *headerText = "PLAYLIST";
    int headerWidth = u8g2_GetStrWidth(&u8g2, headerText);
    u8g2_DrawStr(&u8g2, (128 - headerWidth) / 2, 10, headerText);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);

    int startIdx = playlistSelection > 0 ? playlistSelection - 1 : 0;
    int endIdx = startIdx + 4;
    if (endIdx > playlistSize)
        endIdx = playlistSize;

    for (int i = startIdx; i < endIdx; i++)
    {
        int y = 14 + (i - startIdx) * 11;

        char trackNum[25];
        snprintf(trackNum, sizeof(trackNum), "%d.", i + 1);
        u8g2_DrawStr(&u8g2, 2, y + 7, trackNum);

        char asciiName[128];
        vietnamese_to_ascii(playlist[i].displayname, asciiName, sizeof(asciiName));

        if (i == playlistSelection)
        {
            u8g2_DrawRFrame(&u8g2, 0, y - 1, 128, 10, 2);

            size_t asciiLen = strlen(asciiName);
            int maxChars = 16;

            if (asciiLen > maxChars)
            {
                uint32_t currentTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                uint32_t timeSinceSelection = currentTime - playlistScrollStartTime;

                if (timeSinceSelection >= 500)
                {
                    if (currentTime - lastPlaylistScrollTime > 200)
                    {
                        playlistScrollOffset++;
                        lastPlaylistScrollTime = currentTime;
                    }

                    char extendedText[256];
                    snprintf(extendedText, sizeof(extendedText), "%s  -  ", asciiName);
                    int loopLength = strlen(extendedText);
                    int startPos = playlistScrollOffset % loopLength;

                    char scrolledText[32];
                    for (int j = 0; j < maxChars; j++)
                    {
                        scrolledText[j] = extendedText[(startPos + j) % loopLength];
                    }
                    scrolledText[maxChars] = '\0';

                    u8g2_DrawStr(&u8g2, 18, y + 7, scrolledText);
                }
                else
                {
                    char truncated[32];
                    strncpy(truncated, asciiName, maxChars);
                    truncated[maxChars] = '\0';
                    u8g2_DrawStr(&u8g2, 18, y + 7, truncated);
                }
            }
            else
            {
                u8g2_DrawStr(&u8g2, 18, y + 7, asciiName);
            }
        }
        else
        {
            if (strlen(asciiName) > 16)
            {
                char truncated[32];
                strncpy(truncated, asciiName, 16);
                truncated[16] = '\0';
                u8g2_DrawStr(&u8g2, 18, y + 7, truncated);
            }
            else
            {
                u8g2_DrawStr(&u8g2, 18, y + 7, asciiName);
            }
        }
    }

    u8g2_SendBuffer(&u8g2);
}

static uint32_t last_display_hash = 0;

static inline uint32_t calculate_display_hash(void)
{
    return (uint32_t)currentTrack ^
           (uint32_t)(currentFilePosition >> 10) ^
           (isPaused ? 0x80000000 : 0) ^
           (volumeAnimCurrent << 16);
}

void show_playing_screen(void)
{
    uint32_t new_hash = calculate_display_hash();
    if (new_hash == last_display_hash && !isPaused)
    {
        return;
    }
    last_display_hash = new_hash;

    u8g2_ClearBuffer(&u8g2);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, 12);

    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);

    if (isPaused)
    {
        u8g2_DrawBox(&u8g2, 4, 3, 2, 6);
        u8g2_DrawBox(&u8g2, 8, 3, 2, 6);
    }
    else
    {
        u8g2_DrawTriangle(&u8g2, 4, 2, 4, 10, 10, 6);

        waveformPhase = (waveformPhase + 1) % 20;
        for (int i = 0; i < 3; i++)
        {
            int h = 2 + (abs((waveformPhase + i * 4) % 12 - 6));
            int y = 6 - h / 2;
            u8g2_DrawBox(&u8g2, 14 + i * 3, y, 2, h);
        }
    }

    char trackInfo[24];
    snprintf(trackInfo, sizeof(trackInfo), "%d/%d", currentTrack + 1, totalTracks);
    int textWidth = u8g2_GetStrWidth(&u8g2, trackInfo);
    u8g2_DrawStr(&u8g2, 128 - textWidth - 2, 9, trackInfo);

    u8g2_SetDrawColor(&u8g2, 1);

    char asciiTrackName[128];
    vietnamese_to_ascii(currentTrackName, asciiTrackName, sizeof(asciiTrackName));

    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);

    int maxChars = 21;
    size_t nameLen = strlen(asciiTrackName);

    static uint32_t lastScrollTime = 0;
    static int scrollOffset = 0;
    static int lastPlayingTrack = -1;

    if (lastPlayingTrack != currentTrack)
    {
        scrollOffset = 0;
        lastScrollTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
        lastPlayingTrack = currentTrack;
    }

    if (nameLen > maxChars)
    {
        uint32_t currentTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t timeSinceStart = currentTime - playbackStartTime;

        if (timeSinceStart >= 2000 && !isPaused)
        {
            if (currentTime - lastScrollTime > 200)
            {
                scrollOffset++;
                lastScrollTime = currentTime;
            }

            char extendedText[256];
            snprintf(extendedText, sizeof(extendedText), "%s  -  ", asciiTrackName);
            int loopLength = strlen(extendedText);
            int startPos = scrollOffset % loopLength;

            char scrolledText[32];
            for (int j = 0; j < maxChars; j++)
            {
                scrolledText[j] = extendedText[(startPos + j) % loopLength];
            }
            scrolledText[maxChars] = '\0';

            int nameX = (128 - maxChars * 6) / 2;
            u8g2_DrawStr(&u8g2, nameX, 24, scrolledText);
        }
        else
        {
            char truncated[32];
            strncpy(truncated, asciiTrackName, maxChars);
            truncated[maxChars] = '\0';
            int nameX = (128 - maxChars * 6) / 2;
            u8g2_DrawStr(&u8g2, nameX, 24, truncated);
        }
    }
    else
    {
        int nameWidth = u8g2_GetStrWidth(&u8g2, asciiTrackName);
        int nameX = (128 - nameWidth) / 2;
        u8g2_DrawStr(&u8g2, nameX, 24, asciiTrackName);
    }

    uint32_t elapsed;
    if (isPaused)
    {
        elapsed = (pauseStartTime - playbackStartTime - totalPausedTime) / 1000;
    }
    else
    {
        elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS - playbackStartTime - totalPausedTime) / 1000;
    }

    int minutes = elapsed / 60;
    int seconds = elapsed % 60;

    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    char timeStr[10];
    snprintf(timeStr, sizeof(timeStr), "%d:%02d", minutes, seconds);
    u8g2_DrawStr(&u8g2, 8, 36, timeStr);

    int barX = 44;
    int barY = 31;
    int barWidth = 76;
    int barHeight = 6;

    u8g2_DrawRFrame(&u8g2, barX, barY, barWidth, barHeight, 2);

    int progress = 0;
    if (currentFileSize > 0 && currentAudioFile != NULL && currentFilePosition > 0)
    {
        unsigned long long numerator = (unsigned long long)currentFilePosition * (unsigned long long)(barWidth - 2);
        progress = (int)(numerator / currentFileSize);

        if (progress < 0)
        {
            progress = 0;
        }
        if (progress > (barWidth - 2))
        {
            progress = barWidth - 2;
        }
    }

    if (progress > 0 && progress <= (barWidth - 2) && !isPaused)
    {
        u8g2_DrawBox(&u8g2, barX + 1, barY + 1, (u8g2_uint_t)progress, barHeight - 2);
    }

    if (isPaused)
    {
        u8g2_DrawBox(&u8g2, 58, 42, 4, 12);
        u8g2_DrawBox(&u8g2, 66, 42, 4, 12);
    }
    else
    {
        for (int i = 0; i < 8; i++)
        {
            int barH = 2 + (abs((waveformPhase + i * 3) % 16 - 8));
            int x = 40 + i * 6;
            int y = 56 - barH;
            u8g2_DrawBox(&u8g2, x, y, 4, barH);
        }
    }

    u8g2_DrawTriangle(&u8g2, 3, 59, 3, 63, 7, 61);
    u8g2_DrawBox(&u8g2, 7, 60, 2, 3);

    if (volumeAnimCurrent > 20)
    {
        u8g2_DrawLine(&u8g2, 10, 60, 10, 62);
    }
    if (volumeAnimCurrent > 50)
    {
        u8g2_DrawLine(&u8g2, 12, 59, 12, 63);
    }
    if (volumeAnimCurrent > 80)
    {
        u8g2_DrawLine(&u8g2, 14, 58, 14, 64);
    }

    int volBarX = 18;
    int volBarY = 59;
    int volBarWidth = 82;
    int volBarHeight = 5;

    u8g2_DrawRFrame(&u8g2, volBarX, volBarY, volBarWidth, volBarHeight, 1);
    int volFill = (volumeAnimCurrent * (volBarWidth - 2)) / 100;
    if (volFill > 0)
    {
        u8g2_DrawBox(&u8g2, volBarX + 1, volBarY + 1, volFill, volBarHeight - 2);
    }

    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    char volText[5];
    snprintf(volText, sizeof(volText), "%d%%", volumeAnimCurrent);
    u8g2_DrawStr(&u8g2, 105, 63, volText);

    u8g2_SendBuffer(&u8g2);
}

void show_wifi_info_screen(void)
{
    u8g2_ClearBuffer(&u8g2);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, 12);
    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
    const char *headerText = "WiFi Upload";
    int headerWidth = u8g2_GetStrWidth(&u8g2, headerText);
    u8g2_DrawStr(&u8g2, (128 - headerWidth) / 2, 10, headerText);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);

    // Get IP address
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;

    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
    {
        char ip_str[20];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

        u8g2_DrawStr(&u8g2, 10, 25, "Open browser:");
        u8g2_DrawStr(&u8g2, 10, 38, "http://");
        u8g2_DrawStr(&u8g2, 46, 38, ip_str);

        u8g2_DrawStr(&u8g2, 10, 53, "Then upload MP3");
    }
    else
    {
        u8g2_DrawStr(&u8g2, 20, 32, "WiFi not");
        u8g2_DrawStr(&u8g2, 20, 45, "connected!");
    }

    u8g2_SendBuffer(&u8g2);
}

void show_menu_screen(void)
{
    u8g2_ClearBuffer(&u8g2);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, 12);

    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
    const char *headerText = "MENU";
    int headerWidth = u8g2_GetStrWidth(&u8g2, headerText);
    u8g2_DrawStr(&u8g2, (128 - headerWidth) / 2, 10, headerText);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);

    const char *items[] = {"Play/Pause", "Stop", "Volume", "Playlist",
                           "Auto-Play", "WiFi Upload", "WiFi Config"};

    // Show only 5 items at a time with scrolling
    int startIdx = (menuSelection > 2) ? menuSelection - 2 : 0;
    int endIdx = startIdx + 5;
    if (endIdx > menuItems)
    {
        endIdx = menuItems;
        startIdx = endIdx - 5;
        if (startIdx < 0)
            startIdx = 0;
    }

    for (int i = startIdx; i < endIdx; i++)
    {
        int displayIdx = i - startIdx;
        int y = 14 + displayIdx * 10;

        if (i == menuSelection)
        {
            u8g2_DrawRBox(&u8g2, 2, y - 1, 124, 10, 2);

            u8g2_SetDrawColor(&u8g2, 0);
            u8g2_DrawStr(&u8g2, 8, y + 7, ">");
            u8g2_DrawStr(&u8g2, 20, y + 7, items[i]);

            u8g2_SetDrawColor(&u8g2, 1);
        }
        else
        {
            u8g2_DrawStr(&u8g2, 8, y + 7, " ");
            u8g2_DrawStr(&u8g2, 20, y + 7, items[i]);
        }

        if (i == 4) // Auto-Play status
        {
            const char *statusText;
            if (autoPlayMode == AUTOPLAY_OFF)
                statusText = "[OFF]";
            else if (autoPlayMode == AUTOPLAY_ON)
                statusText = "[ON]";
            else
                statusText = "[RND]";

            int statusWidth = u8g2_GetStrWidth(&u8g2, statusText);

            if (i == menuSelection)
            {
                u8g2_SetDrawColor(&u8g2, 0);
                u8g2_DrawStr(&u8g2, 126 - statusWidth - 4, y + 7, statusText);
                u8g2_SetDrawColor(&u8g2, 1);
            }
            else
            {
                u8g2_DrawStr(&u8g2, 126 - statusWidth - 4, y + 7, statusText);
            }
        }
    }

    u8g2_SendBuffer(&u8g2);
}

// === Start WiFi Mode (Free MP3 RAM -> Alloc WiFi RAM) ===
bool start_wifi_mode(void)
{
    // 1. Free MP3 buffer
    if (input_buffer != NULL)
    {
        free(input_buffer);
        input_buffer = NULL;
        printf("MEMORY: Freed MP3 Buffer\n");
    }

    // === NEW: Force garbage collection ===
    vTaskDelay(pdMS_TO_TICKS(100)); // Let FreeRTOS clean up

    // Print memory status
    printf("Free heap BEFORE WiFi alloc: %lu bytes\n", esp_get_free_heap_size());
    printf("Min heap ever: %lu bytes\n", esp_get_minimum_free_heap_size());

    // 2. Try allocating WiFi Buffers
    upload_buffer_ptr = (uint8_t *)heap_caps_malloc(UPLOAD_BUF_SIZE_WIFI, MALLOC_CAP_8BIT);

    if (upload_buffer_ptr == NULL)
    {
        printf("FAILED to alloc upload buffer (%d KB)\n", UPLOAD_BUF_SIZE_WIFI / 1024);
        printf("Free heap: %lu bytes\n", esp_get_free_heap_size());

        // Restore MP3 buffer
        input_buffer = (uint8_t *)malloc(MP3_BUF_SIZE_PLAYING);
        return false;
    }

    receive_buffer_ptr = (uint8_t *)heap_caps_malloc(RECV_BUF_SIZE_WIFI, MALLOC_CAP_8BIT);

    if (receive_buffer_ptr == NULL)
    {
        printf("FAILED to alloc receive buffer (%d KB)\n", RECV_BUF_SIZE_WIFI / 1024);
        printf("Free heap: %lu bytes\n", esp_get_free_heap_size());

        // Clean up and restore
        free(upload_buffer_ptr);
        upload_buffer_ptr = NULL;
        input_buffer = (uint8_t *)malloc(MP3_BUF_SIZE_PLAYING);
        return false;
    }

    printf("SUCCESS: WiFi buffers allocated\n");
    printf("Free heap AFTER alloc: %lu bytes\n", esp_get_free_heap_size());

    // 4. Initialize WiFi Stack (Only once per boot)
    if (!isWifiInitialized)
    {
        wifi_init_sta_stored(); // Uses stored credentials
        isWifiInitialized = true;
    }

    // 5. Connect
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    return true;
}

// === Stop WiFi Mode (Free WiFi RAM -> Alloc MP3 RAM) ===
void stop_wifi_mode(void)
{
    // 1. Stop Web Server
    if (server)
    {
        httpd_stop(server);
        server = NULL;
    }

    // 2. Stop WiFi to save internal RAM
    esp_wifi_disconnect();
    esp_wifi_stop();

    // 3. Free WiFi Buffers
    if (upload_buffer_ptr)
    {
        free(upload_buffer_ptr);
        upload_buffer_ptr = NULL;
    }
    if (receive_buffer_ptr)
    {
        free(receive_buffer_ptr);
        receive_buffer_ptr = NULL;
    }

    // 4. Restore MP3 Buffer
    if (input_buffer == NULL)
    {
        input_buffer = (uint8_t *)malloc(MP3_BUF_SIZE_PLAYING);
        if (input_buffer == NULL)
        {
            printf("CRITICAL: Failed to re-alloc MP3 buffer. System Halted.\n");
            show_error_screen("Memory Error", "Restart Req");
            while (1)
                vTaskDelay(100);
        }
    }
    printf("MEMORY: Restored MP3 Buffer\n");
}

void handle_buttons(void)
{
    // MENU button - Toggle menu
    if (is_button_pressed(0))
    {
        if (currentMode == MODE_MENU)
        {
            currentMode = MODE_PLAYING;
            if (isPlaying)
            {
                show_playing_screen();
            }
            else
            {
                show_ready_screen(totalTracks);
            }
        }
        else if (currentMode == MODE_PLAYLIST)
        {
            currentMode = MODE_MENU;
            show_menu_screen();
        }
        else if (currentMode == MODE_VOLUME)
        {
            currentMode = MODE_MENU;
            show_menu_screen();
        }
        else
        {
            currentMode = MODE_MENU;
            menuSelection = 0;
            show_menu_screen();
        }
        return;
    }

    // UP button
    if (is_button_pressed(2))
    {
        if (currentMode == MODE_MENU)
        {
            menuSelection = (menuSelection - 1 + menuItems) % menuItems;
            show_menu_screen();
        }
        else if (currentMode == MODE_PLAYLIST)
        {
            playlistSelection = (playlistSelection - 1 + playlistSize) % playlistSize;
            playlistScrollStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            playlistScrollOffset = 0;
            lastPlaylistScrollTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            show_playlist_screen();
        }
        else if (currentMode == MODE_VOLUME)
        {
            volumeAnimCurrent = (volumeAnimCurrent + 5) > 100 ? 100 : (volumeAnimCurrent + 5);
            volumeAnimTarget = volumeAnimCurrent;
            show_volume_screen();
        }
        else if (currentMode == MODE_PLAYING)
        {
            volumeAnimCurrent = (volumeAnimCurrent + 5) > 100 ? 100 : (volumeAnimCurrent + 5);
            volumeAnimTarget = volumeAnimCurrent;
            if (isPlaying)
            {
                show_playing_screen();
            }
        }
    }

    // DOWN button
    if (is_button_pressed(3))
    {
        if (currentMode == MODE_MENU)
        {
            menuSelection = (menuSelection + 1) % menuItems;
            show_menu_screen();
        }
        else if (currentMode == MODE_PLAYLIST)
        {
            playlistSelection = (playlistSelection + 1) % playlistSize;
            playlistScrollStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            playlistScrollOffset = 0;
            lastPlaylistScrollTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
            show_playlist_screen();
        }
        else if (currentMode == MODE_VOLUME)
        {
            volumeAnimCurrent = (volumeAnimCurrent - 5) < 0 ? 0 : (volumeAnimCurrent - 5);
            volumeAnimTarget = volumeAnimCurrent;
            show_volume_screen();
        }
        else if (currentMode == MODE_PLAYING)
        {
            volumeAnimCurrent = (volumeAnimCurrent - 5) < 0 ? 0 : (volumeAnimCurrent - 5);
            volumeAnimTarget = volumeAnimCurrent;
            if (isPlaying)
            {
                show_playing_screen();
            }
        }
    }

    // LEFT button
    if (is_button_pressed(4))
    {
        if (currentMode == MODE_MENU)
        {
            currentMode = MODE_PLAYING;
            if (isPlaying)
            {
                show_playing_screen();
            }
            else
            {
                show_ready_screen(totalTracks);
            }
        }
        else if (currentMode == MODE_PLAYLIST)
        {
            currentMode = MODE_MENU;
            show_menu_screen();
        }
        else if (currentMode == MODE_PLAYING)
        {
            if (currentTrack > 0)
            {
                nextTrackIndex = currentTrack - 1;
                changeTrack = true;
                stopPlayback = true;
                isPaused = false;
            }
        }
    }

    // RIGHT button
    if (is_button_pressed(5))
    {
        if (currentMode == MODE_PLAYING)
        {
            if (currentTrack < playlistSize - 1)
            {
                nextTrackIndex = currentTrack + 1;
                changeTrack = true;
                stopPlayback = true;
                isPaused = false;
            }
        }
    }

    // CENTER button
    if (is_button_pressed(1))
    {
        if (currentMode == MODE_MENU)
        {
            switch (menuSelection)
            {
            case 0: // Play/Pause
                if (isPaused)
                {
                    isPaused = false;
                    totalPausedTime += (xTaskGetTickCount() * portTICK_PERIOD_MS - pauseStartTime);
                }
                else if (!isPlaying)
                {
                    isPlaying = true;
                    isPaused = false;
                    playbackStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    totalPausedTime = 0;
                }
                else
                {
                    isPaused = true;
                    pauseStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                }
                currentMode = MODE_PLAYING;
                show_playing_screen();
                break;

            case 1: // Stop
                if (isPlaying || isPaused)
                {
                    stopPlayback = true;
                    isPlaying = false;
                    isPaused = false;
                    playbackStartTime = 0;
                    totalPausedTime = 0;
                    pauseStartTime = 0;
                    strcpy(currentTrackName, "Unknown");
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                currentMode = MODE_PLAYING;
                show_ready_screen(totalTracks);
                break;

            case 2: // Volume
                currentMode = MODE_VOLUME;
                show_volume_screen();
                break;

            case 3: // Playlist
                currentMode = MODE_PLAYLIST;
                playlistSelection = currentTrack;
                playlistScrollStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                playlistScrollOffset = 0;
                lastPlaylistScrollTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                show_playlist_screen();
                break;

            case 4: // Auto-Play
                if (autoPlayMode == AUTOPLAY_OFF)
                    autoPlayMode = AUTOPLAY_ON;
                else if (autoPlayMode == AUTOPLAY_ON)
                    autoPlayMode = AUTOPLAY_RANDOM;
                else
                    autoPlayMode = AUTOPLAY_OFF;
                show_menu_screen();
                break;

            case 5: // WiFi Upload
                // 1. Tell player to stop
                if (isPlaying || currentAudioFile != NULL || isPlayerActive)
                {
                    show_loading_screen("Stopping Audio...");
                    stopPlayback = true;
                    isPlaying = false;
                    isPaused = false;
                    int safety_timeout = 0;
                    while (isPlayerActive && safety_timeout < 50)
                    {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        safety_timeout++;
                    }
                }

                show_loading_screen("Starting WiFi...");
                vTaskDelay(pdMS_TO_TICKS(100));

                if (start_wifi_mode())
                {
                    server = start_webserver();

                    if (server)
                    {
                        while (1)
                        {
                            show_wifi_info_screen();
                            // Chờ nút Menu để thoát
                            if (is_button_pressed(BTN_MENU) || gpio_get_level(BTN_MENU) == 0)
                            {
                                vTaskDelay(pdMS_TO_TICKS(50));
                                while (gpio_get_level(BTN_MENU) == 0)
                                    vTaskDelay(10);
                                break;
                            }
                            vTaskDelay(pdMS_TO_TICKS(200));
                        }
                    }
                    else
                    {
                        show_error_screen("Server Error", "Failed Start");
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }

                    // 1. Dừng WiFi và phục hồi bộ nhớ MP3
                    show_loading_screen("Stopping WiFi...");
                    stop_wifi_mode();

                    // 2. === CẬP NHẬT PLAYLIST MỚI === (Code mới thêm)
                    show_loading_screen("Updating Files...");
                    scan_mp3_files(); // Scan lại thẻ nhớ

                    // 3. Cập nhật biến toàn cục
                    totalTracks = playlistSize;

                    // Reset bài hát về đầu danh sách để tránh lỗi nếu danh sách thay đổi
                    currentTrack = 0;
                    strcpy(currentTrackName, "Updated");
                }
                else
                {
                    show_error_screen("Memory Full", "Reboot Req");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }

                show_menu_screen();
                break;
            case 6: // WiFi Config
                show_loading_screen("WiFi Config Mode");

                if (isPlaying || isPlayerActive)
                {
                    stopPlayback = true;
                    isPlaying = false;
                    int timeout = 0;
                    while (isPlayerActive && timeout < 50)
                    {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        timeout++;
                    }
                }

                if (server)
                {
                    httpd_stop(server);
                    server = NULL;
                }

                start_wifi_config_mode();
                config_server = start_config_webserver();

                if (config_server)
                {
                    while (1)
                    {
                        u8g2_ClearBuffer(&u8g2);
                        u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
                        u8g2_DrawStr(&u8g2, 5, 15, "WiFi Config Mode");
                        u8g2_DrawStr(&u8g2, 5, 30, "Connect to:");
                        u8g2_DrawStr(&u8g2, 10, 42, DEFAULT_AP_SSID);
                        u8g2_DrawStr(&u8g2, 5, 55, "Open: 192.168.4.1");
                        u8g2_SendBuffer(&u8g2);

                        if (is_button_pressed(0))
                            break;
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }

                    httpd_stop(config_server);
                    esp_wifi_stop();
                }
                show_menu_screen();
                break;
            }
        }
        else if (currentMode == MODE_PLAYLIST)
        {
            nextTrackIndex = playlistSelection;
            changeTrack = true;
            stopPlayback = true;

            currentMode = MODE_PLAYING;
            isPaused = false;

            strcpy(currentTrackName, "Loading...");
            show_playing_screen();
        }
        else if (currentMode == MODE_PLAYING)
        {
            if (!isPlaying && !isPaused)
            {
                isPlaying = true;
                isPaused = false;
                currentTrack = 0;
                stopPlayback = false;
                playbackStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                totalPausedTime = 0;
                strcpy(currentTrackName, "Loading...");
                show_playing_screen();
            }
            else if (isPaused)
            {
                isPaused = false;
                totalPausedTime += (xTaskGetTickCount() * portTICK_PERIOD_MS - pauseStartTime);
                show_playing_screen();
            }
            else if (isPlaying)
            {
                isPaused = true;
                pauseStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                show_playing_screen();
            }
        }
    }
}
void show_ready_screen(int track_count)
{
    u8g2_ClearBuffer(&u8g2);

    int noteX = 64;
    int noteY = 18;

    u8g2_DrawBox(&u8g2, noteX + 3, noteY - 6, 2, 10);
    u8g2_DrawDisc(&u8g2, noteX, noteY + 4, 4, U8G2_DRAW_ALL);
    u8g2_DrawTriangle(&u8g2, noteX + 5, noteY - 6, noteX + 5, noteY - 2, noteX + 9, noteY - 4);

    for (int i = 0; i < 3; i++)
    {
        int waveX = noteX - 18 + i * 4;
        u8g2_DrawLine(&u8g2, waveX, noteY, waveX, noteY - 4);
        u8g2_DrawPixel(&u8g2, waveX + 1, noteY - 5);
    }
    for (int i = 0; i < 3; i++)
    {
        int waveX = noteX + 12 + i * 4;
        u8g2_DrawLine(&u8g2, waveX, noteY, waveX, noteY - 4);
        u8g2_DrawPixel(&u8g2, waveX - 1, noteY - 5);
    }

    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);

    u8g2_DrawLine(&u8g2, 25, 32, 103, 32);

    char trackInfo[20];
    snprintf(trackInfo, sizeof(trackInfo), "%d Tracks", track_count);
    int trackWidth = u8g2_GetStrWidth(&u8g2, trackInfo);
    u8g2_DrawStr(&u8g2, (128 - trackWidth) / 2, 45, trackInfo);

    u8g2_DrawTriangle(&u8g2, 20, 52, 20, 60, 28, 56);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&u8g2, 32, 60, "Play");

    u8g2_DrawLine(&u8g2, 75, 52, 85, 52);
    u8g2_DrawLine(&u8g2, 75, 56, 85, 56);
    u8g2_DrawLine(&u8g2, 75, 60, 85, 60);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&u8g2, 88, 60, "Menu");

    u8g2_SendBuffer(&u8g2);
}

void show_loading_screen(const char *message)
{
    u8g2_ClearBuffer(&u8g2);

    int bar_width = ((loading_frame % 10) * 12);
    u8g2_DrawFrame(&u8g2, 4, 30, 120, 4);
    u8g2_DrawBox(&u8g2, 4, 30, bar_width, 4);

    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);

    int msg_width = u8g2_GetStrWidth(&u8g2, message);
    u8g2_DrawStr(&u8g2, (128 - msg_width) / 2, 18, message);

    u8g2_SendBuffer(&u8g2);

    loading_frame++;
}

void show_error_screen(const char *error, const char *detail)
{
    u8g2_ClearBuffer(&u8g2);

    u8g2_DrawLine(&u8g2, 54, 8, 74, 28);
    u8g2_DrawLine(&u8g2, 74, 8, 54, 28);
    u8g2_DrawCircle(&u8g2, 64, 18, 14, U8G2_DRAW_ALL);

    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);

    int err_width = u8g2_GetStrWidth(&u8g2, error);
    u8g2_DrawStr(&u8g2, (128 - err_width) / 2, 38, error);

    int detail_width = u8g2_GetStrWidth(&u8g2, detail);
    u8g2_DrawStr(&u8g2, (128 - detail_width) / 2, 52, detail);

    u8g2_SendBuffer(&u8g2);
}

void init_i2s()
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_AUTO,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 1020,
        .auto_clear = true,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_GPIO_UNUSED,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    printf("I2S: 8 desc × 1020 frames = %d bytes DMA buffer\n", 8 * 1020 * 4);
}

// void init_sd()
// {
//     esp_vfs_fat_sdmmc_mount_config_t mount_config = {
//         .format_if_mount_failed = false,
//         .max_files = 5,
//         .allocation_unit_size = 64 * 1024,
//         .use_one_fat = false,
//         .disk_status_check_enable = false};

//     spi_bus_config_t bus_cfg = {
//         .mosi_io_num = SD_MOSI,
//         .miso_io_num = SD_MISO,
//         .sclk_io_num = SD_CLK,
//         .quadwp_io_num = -1,
//         .quadhd_io_num = -1,
//         .max_transfer_sz = (32 * 1024) + 64,
//         .flags = SPICOMMON_BUSFLAG_MASTER,
//     };

//     ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

//     sdmmc_host_t host = SDSPI_HOST_DEFAULT();
//     host.slot = SPI2_HOST;
//     host.max_freq_khz = 20000;
//     host.command_timeout_ms = 20000;

//     sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
//     slot_config.gpio_cs = SD_CS;
//     slot_config.host_id = host.slot;

//     sdmmc_card_t *card;
//     esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);

//     if (ret != ESP_OK)
//     {
//         printf("SD mount failed, retrying at 10MHz...\n");
//         host.max_freq_khz = 10000;
//         ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
//         if (ret != ESP_OK)
//         {
//             printf("SD mount failed: 0x%x\n", ret);
//             show_error_screen("SD Mount Fail", "Check Connection");
//             while (1)
//                 vTaskDelay(100);
//         }
//     }

//     printf("SD: %s, %lluMB @ %dkHz\n", card->cid.name,
//            ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024),
//            host.max_freq_khz);

//     global_host = host;
//     global_slot_config = slot_config;
//     global_card = card;
// }

void init_sd()
{
    show_loading_screen("Init SD Card");
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,  // Enable formatting
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
        .use_one_fat = false,
        .disk_status_check_enable = false
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (32 * 1024) + 64,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
    
    vTaskDelay(pdMS_TO_TICKS(200)); // Longer delay

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;
    host.command_timeout_ms = 10000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = host.slot;

    sdmmc_card_t *card = NULL;
    
    esp_err_t ret = ESP_FAIL;
    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        printf("\n===========================================\n");
        printf("SD Card Mount Failed - Error 0x%x\n", ret);
        printf("===========================================\n");
        
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            printf("Your SD card does not support SDIO commands.\n");
            printf("\nPossible Solutions:\n");
            printf("1. Use a different SD card (SanDisk/Samsung)\n");
            printf("2. Use a Class 10 or UHS-I card\n");
            printf("3. Add 10k pull-up resistors on:\n");
            printf("   - MISO (GPIO %d)\n", SD_MISO);
            printf("   - MOSI (GPIO %d)\n", SD_MOSI);
            printf("   - CLK  (GPIO %d)\n", SD_CLK);
            printf("   - CS   (GPIO %d)\n", SD_CS);
            printf("4. Try reformatting the card on PC (FAT32)\n");
            
            show_error_screen("Incompatible Card", "Use Modern SD");
        } else {
            show_error_screen("SD Mount Fail", "Check Wiring");
        }
        
        while (1) vTaskDelay(100);
    }

    printf("\n===========================================\n");
    printf("SD Card Mounted Successfully!\n");
    printf("Name: %s\n", card->cid.name);
    printf("Size: %llu MB\n", ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    printf("Speed: %d kHz\n", host.max_freq_khz);
    printf("===========================================\n");

    global_host = host;
    global_slot_config = slot_config;
    global_card = card;
}

// Add this function
void remount_sd_card(void)
{
    printf("Remounting SD card to refresh FAT cache...\n");
    
    // Unmount
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, global_card);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Remount
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
        .use_one_fat = false,
        .disk_status_check_enable = false
    };

    sdmmc_card_t *card_new;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &global_host,
                                            &global_slot_config, &mount_config, &card_new);
    
    if (ret == ESP_OK)
    {
        global_card = card_new;
        printf("SD card remounted successfully\n");
    }
    else
    {
        printf("SD remount failed: 0x%x\n", ret);
    }
}

static inline void apply_volume_fast(int16_t *samples, size_t count)
{
    size_t count4 = count / 4;
    size_t remainder = count % 4;

    while (count4--)
    {
        int32_t s0 = ((int32_t)samples[0] * volumeAnimCurrent) / 100;
        int32_t s1 = ((int32_t)samples[1] * volumeAnimCurrent) / 100;
        int32_t s2 = ((int32_t)samples[2] * volumeAnimCurrent) / 100;
        int32_t s3 = ((int32_t)samples[3] * volumeAnimCurrent) / 100;

        if (s0 > 32767)
            s0 = 32767;
        else if (s0 < -32768)
            s0 = -32768;
        if (s1 > 32767)
            s1 = 32767;
        else if (s1 < -32768)
            s1 = -32768;
        if (s2 > 32767)
            s2 = 32767;
        else if (s2 < -32768)
            s2 = -32768;
        if (s3 > 32767)
            s3 = 32767;
        else if (s3 < -32768)
            s3 = -32768;

        samples[0] = (int16_t)s0;
        samples[1] = (int16_t)s1;
        samples[2] = (int16_t)s2;
        samples[3] = (int16_t)s3;

        samples += 4;
    }

    while (remainder--)
    {
        int32_t s = ((int32_t)*samples * volumeAnimCurrent) / 100;
        if (s > 32767)
            s = 32767;
        if (s < -32768)
            s = -32768;
        *samples++ = (int16_t)s;
    }
}

// Add this helper function
bool validate_file_clusters(const char *filename)
{
    FILE *test_file = fopen(filename, "rb");
    if (!test_file)
    {
        printf("Cannot open file for validation\n");
        return false;
    }
    
    // Get file size
    fseek(test_file, 0, SEEK_END);
    long file_size = ftell(test_file);
    fseek(test_file, 0, SEEK_SET);
    
    printf("Validating file: %s (%ld bytes)\n", filename, file_size);
    
    // Try reading at multiple points throughout the file
    const int test_points = 20;  // ← INCREASED from 10 to 20 for better coverage
    uint8_t test_buffer[512];
    bool all_ok = true;
    
    for (int i = 0; i < test_points; i++)
    {
        // Test at 0%, 5%, 10%, ..., 95%
        long test_position = (file_size / test_points) * i;
        
        if (fseek(test_file, test_position, SEEK_SET) != 0)
        {
            printf("  FAIL at position %ld (seek error)\n", test_position);
            all_ok = false;
            break;
        }
        
        size_t read_bytes = fread(test_buffer, 1, sizeof(test_buffer), test_file);
        if (read_bytes != sizeof(test_buffer) && i < test_points - 1)
        {
            printf("  FAIL at position %ld (read error: %zu bytes)\n", test_position, read_bytes);
            all_ok = false;
            break;
        }
        
        printf("  OK at %ld (%d%%)\n", test_position, (i * 100) / test_points);
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay between seeks
    }
    
    // === ADD: Explicitly test the last 1KB of the file ===
    if (all_ok && file_size > 1024)
    {
        printf("  Testing end of file...\n");
        if (fseek(test_file, file_size - 1024, SEEK_SET) != 0)
        {
            printf("  FAIL at end (seek error)\n");
            all_ok = false;
        }
        else
        {
            size_t read_bytes = fread(test_buffer, 1, 512, test_file);
            if (read_bytes != 512)
            {
                printf("  FAIL at end (read error: %zu bytes)\n", read_bytes);
                all_ok = false;
            }
            else
            {
                printf("  OK at end (100%%)\n");
            }
        }
    }
    
    fclose(test_file);
    printf("Validation %s\n", all_ok ? "PASSED" : "FAILED");
    return all_ok;
}

void play_file(const char *filename)
{
    printf("Playing: %s\n", filename);

    //     // === ADD: Validate file first ===
    // if (!validate_file_clusters(filename))
    // {
    //     show_error_screen("File Corrupted", "Cluster Chain Bad");
    //     vTaskDelay(pdMS_TO_TICKS(2000));
    //     isPlayerActive = false;
    //     return;
    // }

    if (input_buffer == NULL)
    {
        printf("Error: input_buffer is NULL!\n");
        return;
    }

    // === LOCK: Tell system we are using the buffer ===
    isPlayerActive = true;

    // ... (Keep your existing I2S setup code) ...
    i2s_channel_disable(tx_handle);
    vTaskDelay(pdMS_TO_TICKS(10));

    // ... (Keep clock config code) ...
    i2s_std_clk_config_t clk_cfg_reset = I2S_STD_CLK_DEFAULT_CONFIG(44100);
    i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg_reset);
    // ... (Keep slot config code) ...
    i2s_std_slot_config_t slot_cfg_reset = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    slot_cfg_reset.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    i2s_channel_reconfig_std_slot(tx_handle, &slot_cfg_reset);
    i2s_channel_enable(tx_handle);

    FILE *f = fopen(filename, "rb");
    if (!f)
    {
        printf("Failed to open: %s\n", filename);
        show_error_screen("Open Failed", "Cannot read file");
        isPlayerActive = false; // Unlock before returning
        return;
    }

    // ... (Keep setvbuf, fseek, variable setups) ...
    setvbuf(f, NULL, _IOFBF, 4096);
    currentAudioFile = f;
    fseek(f, 0, SEEK_END);
    currentFileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    currentFilePosition = 0;

    // ... (Keep track name logic) ...
    if (currentTrack >= 0 && currentTrack < playlistSize)
    {
        strncpy(currentTrackName, playlist[currentTrack].displayname, sizeof(currentTrackName) - 1);
        currentTrackName[sizeof(currentTrackName) - 1] = '\0';
    }
    else
    {
        strcpy(currentTrackName, "Unknown");
    }

    isPlaying = true;
    isPaused = false;
    stopPlayback = false;
    playbackStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
    totalPausedTime = 0;
    pauseStartTime = 0;

    HMP3Decoder hMP3Decoder = MP3InitDecoder();
    if (!hMP3Decoder)
    {
        printf("MP3 decoder init failed\n");
        fclose(f);
        currentAudioFile = NULL;
        isPlaying = false;
        isPlayerActive = false; // Unlock before returning
        return;
    }

    int bytes_in_buffer = 0;
    uint8_t *read_ptr = input_buffer;
    size_t total_input_bytes_processed = 0;
    bool sample_rate_configured = false;
    int current_sample_rate = 44100;

    // === MAIN DECODE LOOP ===
    while (1)
    {
        // Check stop flag
        if (stopPlayback || !isPlaying)
            break;

        if (isPaused)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // ... (Keep all your reading and decoding logic exactly the same) ...
        int bytes_to_read = MP3_BUF_SIZE_PLAYING - bytes_in_buffer;
        if (bytes_to_read > 0)
        {
            if (bytes_in_buffer > 0 && read_ptr != input_buffer)
            {
                memmove(input_buffer, read_ptr, bytes_in_buffer);
            }
            read_ptr = input_buffer;
            int bytes_read = fread(input_buffer + bytes_in_buffer, 1, bytes_to_read, f);
            if (bytes_read == 0 && bytes_in_buffer == 0)
                break;
            bytes_in_buffer += bytes_read;
        }

        int offset = MP3FindSyncWord(read_ptr, bytes_in_buffer);
        if (offset < 0)
        {
            bytes_in_buffer = 0;
            continue;
        }
        read_ptr += offset;
        bytes_in_buffer -= offset;

        uint8_t *ptr_before_decode = read_ptr;
        int err = MP3Decode(hMP3Decoder, &read_ptr, &bytes_in_buffer, output_buffer, 0);

        if (err == ERR_MP3_NONE)
        {
            // ... (Keep sample rate check, volume, i2s write) ...
            int input_bytes_consumed = read_ptr - ptr_before_decode;
            MP3FrameInfo frameInfo;
            MP3GetLastFrameInfo(hMP3Decoder, &frameInfo);

            if (!sample_rate_configured)
            {
                current_sample_rate = frameInfo.samprate;
                i2s_channel_disable(tx_handle);
                i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(current_sample_rate);
                i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
                i2s_channel_enable(tx_handle);
                sample_rate_configured = true;
            }

            apply_volume_fast(output_buffer, frameInfo.outputSamps);

            size_t bytes_to_write = frameInfo.outputSamps * sizeof(short);
            size_t bytes_written = 0;
            uint8_t *write_ptr = (uint8_t *)output_buffer;

            while (bytes_written < bytes_to_write)
            {
                if (stopPlayback || !isPlaying || isPaused)
                    break;
                size_t chunk_written;
                size_t chunk_size = bytes_to_write - bytes_written;
                if (chunk_size > 2048)
                    chunk_size = 2048;
                i2s_channel_write(tx_handle, write_ptr + bytes_written, chunk_size, &chunk_written, portMAX_DELAY);
                bytes_written += chunk_written;
            }
            if (bytes_written == bytes_to_write)
            {
                total_input_bytes_processed += input_bytes_consumed;
                currentFilePosition = total_input_bytes_processed;
            }
        }
        else if (err == ERR_MP3_INDATA_UNDERFLOW)
        {
            continue;
        }
        else
        {
            if (bytes_in_buffer > 0)
            {
                read_ptr++;
                bytes_in_buffer--;
            }
        }
    }

    // === CLEANUP ===
    MP3FreeDecoder(hMP3Decoder);
    fclose(f);
    currentAudioFile = NULL;
    currentFileSize = 0;
    currentFilePosition = 0;

    // === UNLOCK: Tell system we are done with the buffer ===
    isPlayerActive = false;
    printf("Playback Loop Finished. Safe to free.\n");
}

void show_volume_screen(void)
{
    u8g2_ClearBuffer(&u8g2);

    int centerX = 64;
    int iconY = 16;

    if (volumeAnimCurrent == 0)
    {
        u8g2_DrawBox(&u8g2, centerX - 6, iconY, 2, 8);

        u8g2_DrawLine(&u8g2, centerX - 4, iconY - 1, centerX + 1, iconY - 3);
        u8g2_DrawLine(&u8g2, centerX - 4, iconY + 9, centerX + 1, iconY + 11);
        u8g2_DrawLine(&u8g2, centerX + 1, iconY - 3, centerX + 1, iconY + 11);
        u8g2_DrawLine(&u8g2, centerX - 4, iconY - 1, centerX - 4, iconY + 9);

        for (int y = 0; y < 10; y++)
        {
            int width = 1 + (y / 2);
            u8g2_DrawLine(&u8g2, centerX - 4, iconY + y, centerX - 4 + width, iconY + y);
        }

        u8g2_DrawLine(&u8g2, centerX + 3, iconY - 2, centerX + 10, iconY + 9);
        u8g2_DrawLine(&u8g2, centerX + 4, iconY - 2, centerX + 11, iconY + 9);
        u8g2_DrawLine(&u8g2, centerX + 3, iconY + 9, centerX + 10, iconY - 2);
        u8g2_DrawLine(&u8g2, centerX + 4, iconY + 9, centerX + 11, iconY - 2);
    }
    else
    {
        u8g2_DrawBox(&u8g2, centerX - 6, iconY, 2, 8);

        u8g2_DrawLine(&u8g2, centerX - 4, iconY - 1, centerX + 1, iconY - 3);
        u8g2_DrawLine(&u8g2, centerX - 4, iconY + 9, centerX + 1, iconY + 11);
        u8g2_DrawLine(&u8g2, centerX + 1, iconY - 3, centerX + 1, iconY + 11);
        u8g2_DrawLine(&u8g2, centerX - 4, iconY - 1, centerX - 4, iconY + 9);

        for (int y = 0; y < 10; y++)
        {
            int width = 1 + (y / 2);
            u8g2_DrawLine(&u8g2, centerX - 4, iconY + y, centerX - 4 + width, iconY + y);
        }

        if (volumeAnimCurrent > 0)
        {
            u8g2_DrawLine(&u8g2, centerX + 4, iconY + 2, centerX + 4, iconY + 6);
            u8g2_DrawPixel(&u8g2, centerX + 5, iconY + 1);
            u8g2_DrawPixel(&u8g2, centerX + 5, iconY + 7);
        }

        if (volumeAnimCurrent > 33)
        {
            u8g2_DrawLine(&u8g2, centerX + 7, iconY, centerX + 7, iconY + 8);
            u8g2_DrawPixel(&u8g2, centerX + 8, iconY - 1);
            u8g2_DrawPixel(&u8g2, centerX + 8, iconY + 9);
        }

        if (volumeAnimCurrent > 66)
        {
            u8g2_DrawLine(&u8g2, centerX + 10, iconY - 2, centerX + 10, iconY + 10);
            u8g2_DrawPixel(&u8g2, centerX + 11, iconY - 3);
            u8g2_DrawPixel(&u8g2, centerX + 11, iconY + 11);
        }
    }

    u8g2_SetFont(&u8g2, u8g2_font_inb38_mn);
    char volText[5];
    snprintf(volText, sizeof(volText), "%d", volumeAnimCurrent);
    int volWidth = u8g2_GetStrWidth(&u8g2, volText);

    u8g2_DrawStr(&u8g2, (128 - volWidth) / 2, 50, volText);

    u8g2_SetFont(&u8g2, u8g2_font_helvB12_tr);
    u8g2_DrawStr(&u8g2, (128 - volWidth) / 2 + volWidth + 3, 47, "%");

    int barY = 56;
    int barWidth = 120;
    int barHeight = 4;
    int barX = (128 - barWidth) / 2;

    u8g2_DrawRFrame(&u8g2, barX, barY, barWidth, barHeight, 2);

    int fillWidth = (volumeAnimCurrent * (barWidth - 2)) / 100;
    if (fillWidth > 0)
    {
        u8g2_DrawRBox(&u8g2, barX + 1, barY + 1, fillWidth, barHeight - 2, 1);
    }

    u8g2_SendBuffer(&u8g2);
}

// === HTTP Handlers ===

static esp_err_t root_handler(httpd_req_t *req)
{
    extern const unsigned char upload_html_start[] asm("_binary_upload_html_start");
    extern const unsigned char upload_html_end[] asm("_binary_upload_html_end");
    const size_t upload_html_size = (upload_html_end - upload_html_start);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)upload_html_start, upload_html_size);
    return ESP_OK;
}

static esp_err_t list_handler(httpd_req_t *req)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    struct dirent *entry;
    bool first = true;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_REG)
        {
            char path[300];
            snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, entry->d_name);

            struct stat st;
            if (stat(path, &st) == 0)
            {
                char json_entry[400];
                snprintf(json_entry, sizeof(json_entry),
                         "%s{\"name\":\"%s\",\"size\":%ld}",
                         first ? "" : ",", entry->d_name, st.st_size);
                httpd_resp_sendstr_chunk(req, json_entry);
                first = false;
            }
        }
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (ret != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing parameter");
        return ESP_FAIL;
    }

    char param[64];
    if (httpd_query_key_value(buf, "file", param, sizeof(param)) == ESP_OK)
    {
        char filepath[200];
        snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, param);

        if (unlink(filepath) == 0)
        {
            httpd_resp_sendstr(req, "Deleted");
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        }
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid parameter");
    }
    return ESP_OK;
}

static void sanitize_filename(char *dest, const char *src, size_t max_len)
{
    const char *ext = strrchr(src, '.');
    if (!ext)
        ext = "";

    size_t j = 0;
    size_t limit = (max_len > 60) ? 50 : (max_len - 10);

    for (size_t i = 0; src[i] && j < limit; i++)
    {
        if (&src[i] == ext)
            break;

        unsigned char c = (unsigned char)src[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
        {
            dest[j++] = c;
        }
        else if (c == ' ')
        {
            dest[j++] = '_';
        }
    }

    if (j == 0)
    {
        dest[j++] = 'm';
        dest[j++] = 'u';
        dest[j++] = 's';
        dest[j++] = 'i';
        dest[j++] = 'c';
    }

    strcpy(dest + j, ext);
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    char buf[256];
    char raw_filename[128] = {0};
    char filename[128] = {0};

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK)
    {
        if (httpd_query_key_value(buf, "file", raw_filename, sizeof(raw_filename)) != ESP_OK)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
            return ESP_FAIL;
        }
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing params");
        return ESP_FAIL;
    }

    sanitize_filename(filename, raw_filename, sizeof(filename));

    char filepath[260];
    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);

    unlink(filepath);
    upload_file = fopen(filepath, "wb");
    if (!upload_file)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, strerror(errno));
        return ESP_FAIL;
    }

    // Optimize file buffer for SD card writing
    setvbuf(upload_file, NULL, _IONBF, 0);

    size_t remaining = req->content_len;
    buffer_index = 0;
    total_received = 0;
    upload_start_time = esp_timer_get_time();
    int64_t last_log_time = upload_start_time;
    int64_t last_yield_time = upload_start_time;

    esp_err_t ret = ESP_OK;
    bool upload_failed = false;

    while (remaining > 0 && !upload_failed)
    {
        // === CHANGED: Use Defined Size for WiFi ===
        size_t recv_size = MIN(remaining, RECV_BUF_SIZE_WIFI);

        // === CHANGED: Use Pointer receive_buffer_ptr ===
        int received = httpd_req_recv(req, (char *)receive_buffer_ptr, recv_size);

        if (received <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }
            ret = ESP_FAIL;
            upload_failed = true;
            break;
        }

        size_t bytes_to_copy = received;
        size_t copied = 0;

        while (copied < bytes_to_copy && !upload_failed)
        {
            // === CHANGED: Use Defined Size for WiFi ===
            size_t space_left = UPLOAD_BUF_SIZE_WIFI - buffer_index;
            size_t to_copy = MIN(space_left, bytes_to_copy - copied);

            // === CHANGED: Use Pointers ===
            memcpy(upload_buffer_ptr + buffer_index,
                   receive_buffer_ptr + copied, to_copy);

            buffer_index += to_copy;
            copied += to_copy;

            // === CHANGED: Use Defined Size ===
            if (buffer_index >= UPLOAD_BUF_SIZE_WIFI)
            {
                // === CHANGED: Use Pointer ===
                if (flush_buffer_to_sd(upload_file, upload_buffer_ptr, buffer_index) != ESP_OK)
                {
                    ret = ESP_FAIL;
                    upload_failed = true;
                    break;
                }
                buffer_index = 0;
            }
        }

        if (upload_failed)
        {
            break;
        }

        total_received += received;
        remaining -= received;

        int64_t now = esp_timer_get_time();
        if (now - last_log_time >= 500000)
        {
            last_log_time = now;
        }

        if (now - last_yield_time >= 100000)
        {
            taskYIELD();
            last_yield_time = now;
        }
    }

    // === CHANGED: Flush remaining using Pointer ===
    if (ret == ESP_OK && buffer_index > 0)
    {
        ret = flush_buffer_to_sd(upload_file, upload_buffer_ptr, buffer_index);
    }

    // === IMPROVED CLEANUP WITH PROPER SYNC ===
    // === IMPROVED CLEANUP WITH PROPER SYNC ===
    if (upload_file)
    {
        // 1. Flush C library buffers
        fflush(upload_file);

        // 2. Get file descriptor and sync file data
        int fd = fileno(upload_file);
        if (fd >= 0)
        {
            // Multiple sync attempts to ensure write
            for (int i = 0; i < 3; i++)
            {
                fsync(fd);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        // 3. Close file BEFORE unmounting
        fclose(upload_file);
        upload_file = NULL;

        // 4. CRITICAL: Unmount and remount to force filesystem consistency
        printf("Forcing filesystem sync...\n");

        // Unmount the SD card
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, global_card);
        vTaskDelay(pdMS_TO_TICKS(500));

        // Remount the SD card
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 64 * 1024,
        };

        sdmmc_card_t *card_new;
        esp_err_t mount_ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &global_host,
                                                      &global_slot_config, &mount_config, &card_new);

        if (mount_ret != ESP_OK)
        {
            printf("Failed to remount SD card!\n");
        }

        vTaskDelay(pdMS_TO_TICKS(200));

        printf("File write completed and synced: %s (%zu bytes)\n", filepath, total_received);
    }

    if (ret == ESP_OK)
    {
        httpd_resp_sendstr(req, "OK");
    }
    else
    {
        httpd_resp_send_500(req);
    }

    return ret;
}

// === NEW: Status Handler for Web Interface ===
static esp_err_t status_handler(httpd_req_t *req)
{
    wifi_ap_record_t wifidata;
    esp_err_t wifi_err = esp_wifi_sta_get_ap_info(&wifidata);
    int rssi = 0;

    if (wifi_err == ESP_OK)
    {
        rssi = wifidata.rssi;
    }

    // Get SD Card Total/Free space (Optional, adds a bit of delay but useful)
    FATFS *fs;
    DWORD fre_clust, free_sect, tot_sect;
    uint64_t total = 0, free = 0;

    if (f_getfree("0:", &fre_clust, &fs) == FR_OK)
    {
        tot_sect = (fs->n_fatent - 2) * fs->csize;
        free_sect = fre_clust * fs->csize;
        total = (uint64_t)tot_sect * 512;
        free = (uint64_t)free_sect * 512;
    }

    char json_response[256];
    // Create JSON with Heap, Min Heap, RSSI, and SD Storage
    snprintf(json_response, sizeof(json_response),
             "{\"heap\":%lu,\"min_heap\":%lu,\"rssi\":%d,\"sd_total\":%llu,\"sd_free\":%llu}",
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(),
             rssi,
             total,
             free);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    // === CRITICAL FIX FOR LARGE FILES ===
    // Increase timeouts. SD writing is slow; don't let the connection die.
    // === CHANGE: Set to 24 Hours (Virtually Infinite) ===
    config.recv_wait_timeout = 86400; // Increased from 60
    config.send_wait_timeout = 86400; // Increased from 60
    config.lru_purge_enable = true;
    config.max_open_sockets = 3;
    config.backlog_conn = 2;
    config.max_resp_headers = 8;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t list_uri = {
            .uri = "/list",
            .method = HTTP_GET,
            .handler = list_handler,
        };
        httpd_register_uri_handler(server, &list_uri);

        httpd_uri_t delete_uri = {
            .uri = "/delete",
            .method = HTTP_GET,
            .handler = delete_handler,
        };
        httpd_register_uri_handler(server, &delete_uri);

        httpd_uri_t upload_uri = {
            .uri = "/upload",
            .method = HTTP_POST,
            .handler = upload_handler,
        };
        httpd_register_uri_handler(server, &upload_uri);

        // // === NEW: Register Status Handler ===
        // httpd_uri_t status_uri = {
        //     .uri = "/status",
        //     .method = HTTP_GET,
        //     .handler = status_handler,
        // };
        // httpd_register_uri_handler(server, &status_uri);

        return server;
    }

    return NULL;
}

// Add this helper function before app_main()
void show_heap_info_screen(const char *title, uint32_t free_heap, uint32_t min_heap)
{
    u8g2_ClearBuffer(&u8g2);

    // Title bar
    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, 12);
    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
    int titleWidth = u8g2_GetStrWidth(&u8g2, title);
    u8g2_DrawStr(&u8g2, (128 - titleWidth) / 2, 10, title);

    // Content
    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);

    u8g2_DrawStr(&u8g2, 5, 25, "Free Heap:");
    char free_str[32];
    snprintf(free_str, sizeof(free_str), "%lu bytes", free_heap);
    u8g2_DrawStr(&u8g2, 10, 37, free_str);

    // Show in KB as well
    char free_kb[32];
    snprintf(free_kb, sizeof(free_kb), "(%lu KB)", free_heap / 1024);
    u8g2_DrawStr(&u8g2, 10, 47, free_kb);

    u8g2_DrawStr(&u8g2, 5, 58, "Min Heap:");
    char min_str[32];
    snprintf(min_str, sizeof(min_str), "%lu bytes", min_heap);
    u8g2_DrawStr(&u8g2, 10, 70, min_str); // This will be cut off, but visible partially

    u8g2_SendBuffer(&u8g2);
}

// Alternative: Two-screen version for better readability
void show_heap_info_detailed(const char *stage, uint32_t free_heap, uint32_t min_heap)
{
    // Screen 1: Free Heap
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, 12);
    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
    u8g2_DrawStr(&u8g2, 20, 10, stage);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
    u8g2_DrawStr(&u8g2, 10, 28, "Free Heap:");

    u8g2_SetFont(&u8g2, u8g2_font_helvB10_tr);
    char free_str[32];
    snprintf(free_str, sizeof(free_str), "%lu", free_heap);
    u8g2_DrawStr(&u8g2, 20, 43, free_str);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&u8g2, 85, 43, "bytes");

    char free_kb[32];
    snprintf(free_kb, sizeof(free_kb), "(%lu KB)", free_heap / 1024);
    u8g2_DrawStr(&u8g2, 30, 57, free_kb);

    u8g2_SendBuffer(&u8g2);
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Screen 2: Min Heap
    u8g2_ClearBuffer(&u8g2);
    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_DrawBox(&u8g2, 0, 0, 128, 12);
    u8g2_SetDrawColor(&u8g2, 0);
    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
    u8g2_DrawStr(&u8g2, 20, 10, stage);

    u8g2_SetDrawColor(&u8g2, 1);
    u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
    u8g2_DrawStr(&u8g2, 10, 28, "Min Heap:");

    u8g2_SetFont(&u8g2, u8g2_font_helvB10_tr);
    char min_str[32];
    snprintf(min_str, sizeof(min_str), "%lu", min_heap);
    u8g2_DrawStr(&u8g2, 20, 43, min_str);
    u8g2_SetFont(&u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&u8g2, 85, 43, "bytes");

    char min_kb[32];
    snprintf(min_kb, sizeof(min_kb), "(%lu KB)", min_heap / 1024);
    u8g2_DrawStr(&u8g2, 30, 57, min_kb);

    u8g2_SendBuffer(&u8g2);
    vTaskDelay(pdMS_TO_TICKS(2000));
}

void app_main(void)
{
    printf("=== ESP32-C3 MP3 Player with OLED & WiFi ===\n");

#ifdef CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 160,
        .light_sleep_enable = false};
    esp_pm_configure(&pm_config);
#endif

    // === ALLOCATE INITIAL MP3 BUFFER ===
    // We allocate this immediately on boot for music playback
    input_buffer = (uint8_t *)malloc(MP3_BUF_SIZE_PLAYING);
    if (input_buffer == NULL)
    {
        printf("CRITICAL: Failed to alloc initial MP3 buffer\n");
        // We can continue, but music won't play until memory is freed or reboot
    }

    // === Initialize Hardware ===
    u8g2_esp32_hal_t u8g2_hal = U8G2_ESP32_HAL_DEFAULT;
    u8g2_hal.bus.i2c.sda = OLED_SDA;
    u8g2_hal.bus.i2c.scl = OLED_SCL;
    u8g2_esp32_hal_init(u8g2_hal);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&u8g2, U8G2_R0,
                                           u8g2_esp32_i2c_byte_cb,
                                           u8g2_esp32_gpio_and_delay_cb);
    u8x8_SetI2CAddress(&u8g2.u8x8, 0x3C << 1);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);

    show_loading_screen("Init NVS");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load WiFi credentials
    load_wifi_credentials();
    if (strlen(stored_ssid) == 0)
    {
        strncpy(stored_ssid, WIFI_SSID, MAX_SSID_LEN - 1);
        strncpy(stored_password, WIFI_PASS, MAX_PASS_LEN - 1);
    }

    show_loading_screen("Init Buttons");
    setup_buttons();

    show_loading_screen("Init I2S");
    init_i2s();

    show_loading_screen("Init SD Card");
    init_sd();

    // === NOTE: WiFi is NOT initialized here anymore. ===
    // It is initialized on-demand in handle_buttons case 5.

    // === Scan Files ===
    show_loading_screen("Scanning Files");

    scan_mp3_files(); // Gọi hàm scan chúng ta vừa tạo

    xTaskCreate(display_update_task, "display_task", 8192, NULL, 5, &displayTaskHandle);

    if (playlistSize > 0)
    {
        totalTracks = playlistSize;
        currentTrack = 0;
        show_ready_screen(playlistSize);

        while (1)
        {
            if (changeTrack)
            {
                currentTrack = nextTrackIndex;
                changeTrack = false;
                stopPlayback = false;
                isPlaying = true;
                isPaused = false;
                playbackStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                totalPausedTime = 0;
            }

            if (isPlaying && !isPaused)
            {
                if (currentTrack >= 0 && currentTrack < playlistSize)
                {
                    stopPlayback = false;
                    play_file(playlist[currentTrack].filepath);

                    if (changeTrack)
                    {
                        currentTrack = nextTrackIndex;
                        changeTrack = false;
                        stopPlayback = false;
                        isPlaying = true;
                    }
                    else if (!stopPlayback)
                    {
                        if (autoPlayMode == AUTOPLAY_ON)
                        {
                            if (currentTrack < playlistSize - 1)
                                currentTrack++;
                            else
                                currentTrack = 0;
                        }
                        else if (autoPlayMode == AUTOPLAY_RANDOM)
                        {
                            int tempTrack = currentTrack;
                            currentTrack = esp_random() % playlistSize;
                            while (tempTrack == currentTrack)
                            {
                                currentTrack = esp_random() % playlistSize;
                            }
                        }
                        else
                        {
                            isPlaying = false;
                            isPaused = false;
                            strcpy(currentTrackName, "Unknown");
                            show_ready_screen(playlistSize);
                        }
                    }
                    else
                    {
                        isPlaying = false;
                        isPaused = false;
                        strcpy(currentTrackName, "Unknown");
                        show_ready_screen(playlistSize);
                    }
                }
                else
                {
                    isPlaying = false;
                    show_ready_screen(playlistSize);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    else
    {
        show_error_screen("No MP3 Files", "Add Music to SD");
        while (1)
            vTaskDelay(1000);
    }
}