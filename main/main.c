#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
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

// --- CRITICAL BUFFER SETTINGS FOR ESP32-C3 SINGLE CORE ---
#define MP3_INPUT_BUFFER_SIZE (16 * 1024)
#define PCM_FRAME_SAMPLES (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)

short output_buffer[PCM_FRAME_SAMPLES];
uint8_t input_buffer[MP3_INPUT_BUFFER_SIZE];

u8g2_t u8g2;
i2s_chan_handle_t tx_handle = NULL;

// Global variables
TaskHandle_t playbackTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;
volatile bool stopPlayback = false;

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
    MODE_PLAYLIST
} MenuMode;

MenuMode currentMode = MODE_PLAYING;
int menuSelection = 0;
const int menuItems = 5;

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
    const uint32_t DISPLAY_UPDATE_INTERVAL = 150; // Update every 100ms instead of 150ms

    while (1)
    {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Update display at fixed interval
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
            last_update = now;
        }

        // Check buttons more frequently (every 20ms for better responsiveness)
        handle_buttons();

        vTaskDelay(pdMS_TO_TICKS(100)); // Reduced from 50ms to 20ms
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

    io_conf.pin_bit_mask = (1ULL << BTN_LEFT);
    gpio_config(&io_conf);
    gpio_isr_handler_add(BTN_LEFT, leftISR, NULL);

    io_conf.pin_bit_mask = (1ULL << BTN_RIGHT);
    gpio_config(&io_conf);
    gpio_isr_handler_add(BTN_RIGHT, rightISR, NULL);
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

// Cache for reducing unnecessary redraws
static uint32_t last_display_hash = 0;

static inline uint32_t calculate_display_hash(void)
{
    // Simple hash to detect if display needs updating
    return (uint32_t)currentTrack ^
           (uint32_t)(currentFilePosition >> 10) ^
           (isPaused ? 0x80000000 : 0) ^
           (volumeAnimCurrent << 16);
}

void show_playing_screen(void)
{
    // Skip update if nothing changed (for smoother playback)
    uint32_t new_hash = calculate_display_hash();
    if (new_hash == last_display_hash && !isPaused)
    {
        return; // Nothing changed, skip redraw
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

    const char *items[] = {"Play/Pause", "Stop", "Volume", "Playlist", "Auto-Play"};

    for (int i = 0; i < menuItems; i++)
    {
        int y = 14 + i * 10;

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

        if (i == 4)
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

void handle_buttons(void)
{
    bool needsImmediateUpdate = false;

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
        else
        {
            currentMode = MODE_MENU;
            menuSelection = 0;
            show_menu_screen();
        }
        return; // Early return for instant response
    }

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

    if (is_button_pressed(1))
    {
        if (currentMode == MODE_MENU)
        {
            switch (menuSelection)
            {
            case 0:
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

            case 1:
                stopPlayback = true;
                isPlaying = false;
                isPaused = false;
                currentMode = MODE_PLAYING;
                show_ready_screen(totalTracks);
                break;

            case 2:
                break;

            case 3:
                currentMode = MODE_PLAYLIST;
                playlistSelection = currentTrack;
                playlistScrollStartTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                playlistScrollOffset = 0;
                lastPlaylistScrollTime = xTaskGetTickCount() * portTICK_PERIOD_MS;
                show_playlist_screen();
                break;

            case 4:
                if (autoPlayMode == AUTOPLAY_OFF)
                    autoPlayMode = AUTOPLAY_ON;
                else if (autoPlayMode == AUTOPLAY_ON)
                    autoPlayMode = AUTOPLAY_RANDOM;
                else
                    autoPlayMode = AUTOPLAY_OFF;
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

void init_sd()
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .use_one_fat = false};

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;
    host.command_timeout_ms = 2000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = host.slot;

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK)
    {
        printf("SD mount failed, retrying at 10MHz...\n");
        host.max_freq_khz = 10000;
        ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
        if (ret != ESP_OK)
        {
            printf("SD mount failed: 0x%x\n", ret);
            show_error_screen("SD Mount Fail", "Check Connection");
            while (1)
                vTaskDelay(100);
        }
    }

    printf("SD: %s, %lluMB @ %dkHz\n", card->cid.name,
           ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024),
           host.max_freq_khz);
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

void play_file(const char *filename)
{
    printf("Playing: %s\n", filename);

    // **CRITICAL FIX: Reset I2S channel before playing new track**
    i2s_channel_disable(tx_handle);
    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to ensure clean stop

    // Reconfigure I2S to default 44100Hz
    i2s_std_clk_config_t clk_cfg_reset = I2S_STD_CLK_DEFAULT_CONFIG(44100);
    i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg_reset);

    i2s_std_slot_config_t slot_cfg_reset = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO);
    slot_cfg_reset.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    i2s_channel_reconfig_std_slot(tx_handle, &slot_cfg_reset);

    i2s_channel_enable(tx_handle);

    FILE *f = fopen(filename, "rb");
    if (!f)
    {
        printf("Failed to open: %s\n", filename);
        show_error_screen("Open Failed", "Cannot read file");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    // Smaller FILE buffer for large files
    setvbuf(f, NULL, _IOFBF, 4096);

    // Store file handle and size globally for progress tracking
    currentAudioFile = f;
    fseek(f, 0, SEEK_END);
    currentFileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    currentFilePosition = 0;

    // Update track name for UI
    if (currentTrack >= 0 && currentTrack < playlistSize)
    {
        strncpy(currentTrackName, playlist[currentTrack].displayname, sizeof(currentTrackName) - 1);
        currentTrackName[sizeof(currentTrackName) - 1] = '\0';
    }
    else
    {
        strcpy(currentTrackName, "Unknown");
    }

    // Set playback state flags
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
        return;
    }

    int bytes_in_buffer = 0;
    uint8_t *read_ptr = input_buffer;

    // Track actual bytes processed for progress bar
    size_t total_input_bytes_processed = 0;

    // Sample rate detection flag
    bool sample_rate_configured = false;
    int current_sample_rate = 44100; // Default assumption

    printf("Starting playback loop...\n");

    while (1)
    {
        // Check if playback was stopped
        if (stopPlayback || !isPlaying)
        {
            break;
        }

        // If paused, just wait - don't process audio
        if (isPaused)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // ============================================================
        // STEP 1: REFILL INPUT BUFFER
        // ============================================================
        int bytes_to_read = MP3_INPUT_BUFFER_SIZE - bytes_in_buffer;

        if (bytes_to_read > 0)
        {
            // Move remaining data to start of buffer
            if (bytes_in_buffer > 0 && read_ptr != input_buffer)
            {
                memmove(input_buffer, read_ptr, bytes_in_buffer);
            }
            read_ptr = input_buffer;

            // Read new data
            int bytes_read = fread(input_buffer + bytes_in_buffer, 1, bytes_to_read, f);
            
            if (bytes_read == 0)
            {
                if (bytes_in_buffer == 0)
                {
                    // End of file
                    break;
                }
                // Try to process remaining data
            }
            
            bytes_in_buffer += bytes_read;
        }

        // ============================================================
        // STEP 2: FIND SYNC WORD
        // ============================================================
        int offset = MP3FindSyncWord(read_ptr, bytes_in_buffer);
        
        if (offset < 0)
        {
            // No sync found, need more data
            bytes_in_buffer = 0;
            continue;
        }

        read_ptr += offset;
        bytes_in_buffer -= offset;

        // Save pointer before decode for progress tracking
        uint8_t *ptr_before_decode = read_ptr;

        // ============================================================
        // STEP 3: DECODE ONE MP3 FRAME
        // ============================================================
        int err = MP3Decode(hMP3Decoder, &read_ptr, &bytes_in_buffer, output_buffer, 0);

        if (err == ERR_MP3_NONE)
        {
            // Calculate input bytes consumed by this frame
            int input_bytes_consumed = read_ptr - ptr_before_decode;

            // Get frame info
            MP3FrameInfo frameInfo;
            MP3GetLastFrameInfo(hMP3Decoder, &frameInfo);

            // **FIX: Configure I2S sample rate on first successful decode**
            if (!sample_rate_configured)
            {
                current_sample_rate = frameInfo.samprate;
                printf("Detected sample rate: %d Hz\n", current_sample_rate);

                // Disable I2S channel
                i2s_channel_disable(tx_handle);
                vTaskDelay(pdMS_TO_TICKS(10));

                // Reconfigure with correct sample rate
                i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(current_sample_rate);
                esp_err_t ret = i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
                if (ret != ESP_OK)
                {
                    printf("Failed to reconfigure I2S clock: 0x%x\n", ret);
                }

                // Reconfigure slot config
                i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                    I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_STEREO);
                slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
                ret = i2s_channel_reconfig_std_slot(tx_handle, &slot_cfg);
                if (ret != ESP_OK)
                {
                    printf("Failed to reconfigure I2S slot: 0x%x\n", ret);
                }

                // Re-enable I2S channel
                ret = i2s_channel_enable(tx_handle);
                if (ret != ESP_OK)
                {
                    printf("Failed to re-enable I2S: 0x%x\n", ret);
                }

                sample_rate_configured = true;
                printf("I2S reconfigured to %d Hz\n", current_sample_rate);
            }

            // Apply volume with optimized function
            apply_volume_fast(output_buffer, frameInfo.outputSamps);

            // ============================================================
            // STEP 4: WRITE TO I2S IN CHUNKS
            // ============================================================
            size_t bytes_to_write = frameInfo.outputSamps * sizeof(short);
            size_t bytes_written = 0;
            uint8_t *write_ptr = (uint8_t *)output_buffer;

            // Process audio in smaller chunks for better responsiveness
            while (bytes_written < bytes_to_write)
            {
                // Check for stop/pause more frequently
                if (stopPlayback || !isPlaying || isPaused)
                {
                    break;
                }

                size_t chunk_written;
                size_t chunk_size = bytes_to_write - bytes_written;
                if (chunk_size > 2048)
                    chunk_size = 2048;

                esp_err_t ret = i2s_channel_write(tx_handle, write_ptr + bytes_written,
                                                  chunk_size, &chunk_written, portMAX_DELAY);

                if (ret != ESP_OK)
                {
                    printf("I2S write error: 0x%x\n", ret);
                    break;
                }

                bytes_written += chunk_written;
            }

            // Update progress tracking ONLY after successful I2S write
            if (bytes_written == bytes_to_write)
            {
                total_input_bytes_processed += input_bytes_consumed;
                currentFilePosition = total_input_bytes_processed;
            }
        }
        else if (err == ERR_MP3_INDATA_UNDERFLOW)
        {
            // Need more data
            continue;
        }
        else
        {
            // Decode error - skip bad byte and continue
            // Don't break playback on minor errors
            if (bytes_in_buffer > 0)
            {
                read_ptr++;
                bytes_in_buffer--;
            }
        }
    }

    MP3FreeDecoder(hMP3Decoder);
    fclose(f);
    printf("Playback finished\n");

    // Clear file tracking
    currentAudioFile = NULL;
    currentFileSize = 0;
    currentFilePosition = 0;
}

void app_main(void)
{
    printf("=== ESP32-C3 MP3 Player with OLED ===\n");

#ifdef CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 160,
        .light_sleep_enable = false};
    esp_pm_configure(&pm_config);
    printf("CPU: 160MHz locked\n");
#else
    printf("CPU: Running at default speed\n");
#endif

    setup_buttons();

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

    show_loading_screen("Initializing...");

    show_loading_screen("Init I2S...");
    init_i2s();

    show_loading_screen("Init SD Card...");
    init_sd();

    show_loading_screen("Scanning Files...");

    DIR *dir = opendir("/sdcard");
    struct dirent *ent;
    playlistSize = 0;
    playlistCapacity = 0;
    playlist = NULL;

    if (dir)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            if (strstr(ent->d_name, ".mp3") || strstr(ent->d_name, ".MP3"))
            {
                size_t name_len = strlen(ent->d_name);
                if (name_len < 240)
                {
                    char filepath[256];
                    snprintf(filepath, sizeof(filepath), "/sdcard/%s", ent->d_name);

                    char displayname[128];
                    strncpy(displayname, ent->d_name, sizeof(displayname) - 1);
                    displayname[sizeof(displayname) - 1] = '\0';

                    char *ext = strstr(displayname, ".mp3");
                    if (ext)
                        *ext = '\0';
                    ext = strstr(displayname, ".MP3");
                    if (ext)
                        *ext = '\0';

                    if (!add_to_playlist(filepath, displayname))
                    {
                        break;
                    }
                }
            }
        }
        closedir(dir);
    }

    if (playlistSize > 0)
    {
        totalTracks = playlistSize;
        currentTrack = 0;
        show_ready_screen(playlistSize);

        xTaskCreate(display_update_task, "display_task", 4096, NULL, 5, &displayTaskHandle);

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
                            {
                                currentTrack++;
                            }
                            else
                            {
                                currentTrack = 0;
                            }
                        }
                        else if (autoPlayMode == AUTOPLAY_RANDOM)
                        {
                            currentTrack = rand() % playlistSize;
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