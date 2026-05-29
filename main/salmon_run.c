#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_pm.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sleep.h"
#include "cJSON.h"

#include "lvgl.h"
#include "lodepng.h"
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(lv_font_pingfang_16_cjk);
LV_FONT_DECLARE(lv_font_pingfang_18_cjk);
LV_FONT_DECLARE(lv_font_pingfang_22_cjk);
LV_FONT_DECLARE(lv_font_pingfang_24_cjk);
#include "esp_lcd_sh8601.h"
#include "esp_io_expander_tca9554.h"

static const char *TAG = "salmon_run";
static SemaphoreHandle_t lvgl_mux = NULL;

#define LCD_HOST SPI2_HOST
#define TOUCH_HOST I2C_NUM_0
#define PIN_I2C_SCL GPIO_NUM_14
#define PIN_I2C_SDA GPIO_NUM_15
#define LCD_H_RES 368
#define LCD_V_RES 448
#define PIN_LCD_CS GPIO_NUM_12
#define PIN_LCD_PCLK GPIO_NUM_11
#define PIN_LCD_D0 GPIO_NUM_4
#define PIN_LCD_D1 GPIO_NUM_5
#define PIN_LCD_D2 GPIO_NUM_6
#define PIN_LCD_D3 GPIO_NUM_7
#define PIN_LCD_RST -1
#define PIN_BK_LIGHT -1
#define PIN_BUTTON GPIO_NUM_0

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define SCHEDULE_URL "https://splatoon3.ink/data/schedules.json"

#define LVGL_BUF_HEIGHT 10
#define LVGL_TICK_PERIOD_MS 10
#define LVGL_TASK_MAX_DELAY_MS 50
#define LVGL_TASK_MIN_DELAY_MS 5

static lv_obj_t *lbl_status = NULL;
static lv_obj_t *lbl_weapon_title = NULL;
static lv_obj_t *lbl_map_name = NULL;
static lv_obj_t *lbl_time_range = NULL;
static lv_obj_t *lbl_countdown = NULL;

static lv_obj_t *img_weapon[4] = {NULL};
static lv_img_dsc_t weapon_img_dsc[4] = {0};

static time_t shift_start_ts = 0;
static time_t shift_end_ts = 0;
static char shift_map_name[128] = "";
static char shift_map_img_url[256] = "";
static char shift_times_str[128] = "";
static char shift_weapon_imgs[4][256] = {{0}};
static int shift_weapon_count = 0;
static char shift_boss_img_url[256] = {0};
static volatile bool data_loaded = false;
static volatile bool countdown_ended = false;

static lv_obj_t *img_map = NULL;
static lv_obj_t *lbl_open_badge = NULL;
static lv_img_dsc_t map_img_dsc = {
    .header = { .cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .w = 1, .h = 1 },
    .data_size = 0, .data = NULL
};
static lv_obj_t *img_boss = NULL;
static lv_img_dsc_t boss_img_dsc = {
    .header = { .cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .w = 1, .h = 1 },
    .data_size = 0, .data = NULL
};

static uint8_t *rgba_to_rgb565a(const uint8_t *rgba, int w, int h) {
    uint8_t *out = malloc(w * h * 3);
    if (!out) return NULL;
    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];
        uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        out[i * 3 + 0] = (rgb565 >> 8) & 0xFF;
        out[i * 3 + 1] = rgb565 & 0xFF;
        out[i * 3 + 2] = a;
    }
    return out;
}

static const char *stage_cn(const char *en) {
    if (!en) return "?";
    if (strcmp(en, "Spawning Grounds") == 0) return "鲑坝";
    if (strcmp(en, "Marooner's Bay") == 0) return "漂浮落难船";
    if (strcmp(en, "Gone Fission Hydroplant") == 0) return "麦年海洋发电所";
    if (strcmp(en, "Salmonid Smokeyard") == 0) return "时不知鲑烟熏工房";
    if (strcmp(en, "Sockeye Station") == 0) return "新卷堡";
    if (strcmp(en, "Jammin' Salmon Junction") == 0) return "生筋子系统交流道遗址";
    if (strcmp(en, "Ruins of Ark Polaris") == 0) return "鲑鱼心脏斗技场";
    return en;
}

static void fmt_time(time_t t, char *buf, size_t len) {
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, len, "%d/%d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static bool flush_ready(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
    return false;
}

static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *m) {
    esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)d->user_data, a->x1, a->y1, a->x2 + 1, a->y2 + 1, m);
}

static void round_cb(lv_disp_drv_t *d, lv_area_t *a) {
    a->x1 = (a->x1 >> 1) << 1; a->y1 = (a->y1 >> 1) << 1;
    a->x2 = ((a->x2 >> 1) << 1) + 1; a->y2 = ((a->y2 >> 1) << 1) + 1;
}

static void tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

static bool lock_lvgl(int tmo) {
    return xSemaphoreTake(lvgl_mux, tmo < 0 ? portMAX_DELAY : pdMS_TO_TICKS(tmo)) == pdTRUE;
}
static void unlock_lvgl(void) { xSemaphoreGive(lvgl_mux); }

static void lvgl_task(void *arg) {
    while (1) {
        if (lock_lvgl(-1)) {
            uint32_t d = lv_timer_handler();
            unlock_lvgl();
            if (d < LVGL_TASK_MIN_DELAY_MS) d = LVGL_TASK_MIN_DELAY_MS;
            if (d > LVGL_TASK_MAX_DELAY_MS) d = LVGL_TASK_MAX_DELAY_MS;
            vTaskDelay(pdMS_TO_TICKS(d));
        } else vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void countdown_update(lv_timer_t *t) {
    if (!lbl_countdown || !data_loaded) return;
    time_t now;
    time(&now);
    if (now >= shift_end_ts) {
        lv_label_set_text(lbl_countdown, "已结束");
        if (!countdown_ended) countdown_ended = true;
        return;
    }
    double diff = difftime(shift_end_ts, now);
    int h = (int)(diff / 3600);
    int m = (int)((diff - h * 3600) / 60);
    int s = (int)(diff - h * 3600 - m * 60);
    char buf[48];
    if (h > 0)
        snprintf(buf, sizeof(buf), "还剩%d小时%02d分%02d秒", h, m, s);
    else
        snprintf(buf, sizeof(buf), "还剩%d分%02d秒", m, s);
    lv_label_set_text(lbl_countdown, buf);
}

static int http_get(const char *url, uint8_t **out, size_t *out_len) {
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 20000, .buffer_size = 4096, .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return -1; }

    int cl = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    ESP_LOGI(TAG, "HTTP status: %d, content-length: %d", status, cl);
    if (status != 200) { esp_http_client_close(c); esp_http_client_cleanup(c); return -1; }
    if (cl <= 0) cl = 65536;

    int total = cl + 1;
    uint8_t *buf = malloc(total);
    if (!buf) { esp_http_client_close(c); esp_http_client_cleanup(c); return -1; }

    int off = 0, r;
    while ((r = esp_http_client_read(c, (char *)buf + off, total - off - 1)) > 0) {
        off += r;
        if (off >= total - 1024) {
            total += 16384;
            uint8_t *nb = realloc(buf, total);
            if (!nb) { free(buf); esp_http_client_close(c); esp_http_client_cleanup(c); return -1; }
            buf = nb;
        }
    }
    buf[off] = 0;
    *out = buf; *out_len = off;
    esp_http_client_close(c); esp_http_client_cleanup(c);
    return off;
}

static bool parse_schedule(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGE(TAG, "JSON parse fail"); return false; }

    cJSON *cg = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "coopGroupingSchedule");
    cJSON *nodes = cJSON_GetObjectItem(cJSON_GetObjectItem(cg, "regularSchedules"), "nodes");
    if (!nodes || !cJSON_IsArray(nodes)) { cJSON_Delete(root); return false; }

    time_t now; time(&now);
    cJSON *cur = NULL;

    for (int i = 0; i < cJSON_GetArraySize(nodes); i++) {
        cJSON *n = cJSON_GetArrayItem(nodes, i);
        cJSON *startItem = cJSON_GetObjectItem(n, "startTime");
        cJSON *endItem = cJSON_GetObjectItem(n, "endTime");
        if (!startItem || !endItem || !startItem->valuestring || !endItem->valuestring) continue;
        const char *s = startItem->valuestring;
        const char *e = endItem->valuestring;
        struct tm st = {0}, et = {0};
        sscanf(s, "%d-%d-%dT%d:%d:%dZ", &st.tm_year, &st.tm_mon, &st.tm_mday, &st.tm_hour, &st.tm_min, &st.tm_sec);
        st.tm_year -= 1900; st.tm_mon -= 1;
        sscanf(e, "%d-%d-%dT%d:%d:%dZ", &et.tm_year, &et.tm_mon, &et.tm_mday, &et.tm_hour, &et.tm_min, &et.tm_sec);
        et.tm_year -= 1900; et.tm_mon -= 1;

        time_t stt = mktime(&st) + 8 * 3600;
        time_t ett = mktime(&et) + 8 * 3600;
        if (now >= stt && now < ett) { cur = n; shift_start_ts = stt; shift_end_ts = ett; break; }
    }

    if (!cur) { cJSON_Delete(root); return false; }

    cJSON *setting = cJSON_GetObjectItem(cur, "setting");
    cJSON *stage = cJSON_GetObjectItem(setting, "coopStage");
    if (stage) {
        cJSON *nm = cJSON_GetObjectItem(stage, "name");
        if (nm && nm->valuestring) strncpy(shift_map_name, nm->valuestring, sizeof(shift_map_name) - 1);
        cJSON *stage_img = cJSON_GetObjectItem(stage, "image");
        if (stage_img) {
            cJSON *stage_url = cJSON_GetObjectItem(stage_img, "url");
            if (stage_url && stage_url->valuestring) strncpy(shift_map_img_url, stage_url->valuestring, sizeof(shift_map_img_url) - 1);
        }
    }

    cJSON *boss = cJSON_GetObjectItem(setting, "boss");
    if (boss) {
        cJSON *boss_name = cJSON_GetObjectItem(boss, "name");
        if (boss_name && boss_name->valuestring) {
            const char *bn = boss_name->valuestring;
            if (strcmp(bn, "Megalodontia") == 0)
                strncpy(shift_boss_img_url, "https://splatoon3.ink/assets/king-megalodontia-D9zENODp.png", sizeof(shift_boss_img_url) - 1);
            else if (strcmp(bn, "Cohozuna") == 0)
                strncpy(shift_boss_img_url, "https://splatoon3.ink/assets/king-cohozuna-C2i2-iKq.png", sizeof(shift_boss_img_url) - 1);
            else if (strcmp(bn, "Triumvirate") == 0)
                strncpy(shift_boss_img_url, "https://splatoon3.ink/assets/king-triumvirate-DJIjCid3.png", sizeof(shift_boss_img_url) - 1);
            else if (strcmp(bn, "Horrorboros") == 0)
                strncpy(shift_boss_img_url, "https://splatoon3.ink/assets/king-horrorboros-B21AjmXk.png", sizeof(shift_boss_img_url) - 1);
        }
    }

    char sb[32], eb[32];
    fmt_time(shift_start_ts, sb, sizeof(sb));
    fmt_time(shift_end_ts, eb, sizeof(eb));
    snprintf(shift_times_str, sizeof(shift_times_str), "%s ~ %s", sb, eb);

    cJSON *wpn = cJSON_GetObjectItem(setting, "weapons");
    shift_weapon_count = 0;
    if (wpn && cJSON_IsArray(wpn)) {
        int n = cJSON_GetArraySize(wpn);
        if (n > 4) n = 4;
        for (int i = 0; i < n; i++) {
            cJSON *w = cJSON_GetArrayItem(wpn, i);
            cJSON *wi = cJSON_GetObjectItem(w, "image");
            if (wi) {
                cJSON *wu = cJSON_GetObjectItem(wi, "url");
                if (wu && wu->valuestring) strncpy(shift_weapon_imgs[shift_weapon_count], wu->valuestring, sizeof(shift_weapon_imgs[0]) - 1);
            }
            shift_weapon_count++;
        }
    }

    cJSON_Delete(root);
    if (shift_weapon_count != 4)
        ESP_LOGW(TAG, "Expected 4 weapons, got %d", shift_weapon_count);

    ESP_LOGI(TAG, "Shift: %s, %s", shift_map_name, shift_times_str);
    return true;
}

static void wifi_init_sta(void);

static void time_sync_after_wifi(void);

static void fetch_worker(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    wifi_init_sta();

    /* Wait for IP address */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    int wait = 0;
    while (netif && wait < 30) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait++;
        ESP_LOGI(TAG, "Waiting for IP... %d", wait);
    }

    /* SNTP must be called AFTER TCP/IP stack (netif) is initialized */
    time_sync_after_wifi();

    int retries = 0;
    while (retries < 3) {
        if ((retries++) > 0) vTaskDelay(pdMS_TO_TICKS(2000));
        char *json = NULL; size_t jlen = 0;
        if (http_get(SCHEDULE_URL, (uint8_t **)&json, &jlen) < 0) continue;

        ESP_LOGI(TAG, "Got %zu bytes JSON", jlen);

        if (parse_schedule(json)) {
            free(json);
            data_loaded = true;

            if (lock_lvgl(-1)) {
                lv_label_set_text(lbl_status, "");
                lv_label_set_text(lbl_map_name, stage_cn(shift_map_name));
                lv_label_set_text(lbl_time_range, shift_times_str);

                for (int i = 0; i < 4; i++) {
                    if (i < shift_weapon_count) {
                        lv_obj_clear_flag(img_weapon[i], LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_add_flag(img_weapon[i], LV_OBJ_FLAG_HIDDEN);
                    }
                }
                countdown_update(NULL);
                lv_obj_invalidate(lbl_map_name);
                lv_obj_invalidate(lbl_time_range);
                lv_obj_invalidate(lbl_countdown);
                unlock_lvgl();
            }

            for (int i = 0; i < shift_weapon_count; i++) {
                if (shift_weapon_imgs[i][0] == 0) continue;
                uint8_t *png = NULL; size_t png_len = 0;
                if (http_get(shift_weapon_imgs[i], &png, &png_len) > 0 && png) {
                    if (lock_lvgl(-1)) {
                        unsigned sw, sh;
                        uint8_t *rgba = NULL;
                        if (lodepng_decode_memory(&rgba, &sw, &sh, png, png_len, LCT_RGBA, 8) == 0) {
                            free(png); png = NULL;
                            int dw = 80, dh = 80;
                            if (sw > sh) dh = sh * dw / sw; else dw = sw * dh / sh;
                            uint8_t *scaled = malloc(dw * dh * 4);
                            if (scaled) {
                                for (int y = 0; y < dh; y++)
                                    for (int x = 0; x < dw; x++) {
                                        int sx = x * sw / dw, sy = y * sh / dh;
                                        memcpy(&scaled[(y*dw+x)*4], &rgba[(sy*sw+sx)*4], 4);
                                    }
                                free(rgba);
                                uint8_t *rgb565 = rgba_to_rgb565a(scaled, dw, dh);
                                free(scaled);
                                if (rgb565) {
                                    weapon_img_dsc[i].header.w = dw;
                                    weapon_img_dsc[i].header.h = dh;
                                    weapon_img_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
                                    weapon_img_dsc[i].data = rgb565;
                                    weapon_img_dsc[i].data_size = dw * dh * 3;
                                    lv_img_set_src(img_weapon[i], &weapon_img_dsc[i]);
                                    lv_img_set_zoom(img_weapon[i], 256);
                                }
                            } else {
                                free(rgba);
                            }
                        } else {
                            free(png); png = NULL;
                        }
                        unlock_lvgl();
                    } else {
                        free(png); png = NULL;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (shift_map_img_url[0]) {
                uint8_t *png = NULL; size_t len = 0;
                if (http_get(shift_map_img_url, &png, &len) > 0 && png) {
                    if (lock_lvgl(-1)) {
                        unsigned sw, sh;
                        uint8_t *rgba = NULL;
                        if (lodepng_decode_memory(&rgba, &sw, &sh, png, len, LCT_RGBA, 8) == 0) {
                            free(png); png = NULL;
                            int dw = 378;
                            int dh = sh * dw / sw;
                            if (dh > 200) { dh = 200; dw = sw * dh / sh; }
                            uint8_t *scaled = malloc(dw * dh * 4);
                            if (scaled) {
                                for (int y = 0; y < dh; y++)
                                    for (int x = 0; x < dw; x++) {
                                        int sx = x * sw / dw, sy = y * sh / dh;
                                        memcpy(&scaled[(y*dw+x)*4], &rgba[(sy*sw+sx)*4], 4);
                                    }
                                free(rgba);
                                uint8_t *rgb565 = rgba_to_rgb565a(scaled, dw, dh);
                                free(scaled);
                                if (rgb565) {
                                    map_img_dsc.header.w = dw;
                                    map_img_dsc.header.h = dh;
                                    map_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
                                    map_img_dsc.data = rgb565;
                                    map_img_dsc.data_size = dw * dh * 3;
                                    lv_img_set_src(img_map, &map_img_dsc);
                                    lv_obj_align(img_map, LV_ALIGN_TOP_MID, 0, 158);
                                    lv_obj_clear_flag(img_map, LV_OBJ_FLAG_HIDDEN);
                                    lv_obj_clear_flag(lbl_open_badge, LV_OBJ_FLAG_HIDDEN);
                                }
                            } else {
                                free(rgba);
                            }
                        } else {
                            free(png); png = NULL;
                        }
                        unlock_lvgl();
                    } else {
                        free(png); png = NULL;
                    }
                }
            }
            if (shift_boss_img_url[0]) {
                uint8_t *png = NULL; size_t len = 0;
                if (http_get(shift_boss_img_url, &png, &len) > 0 && png) {
                    if (lock_lvgl(-1)) {
                        unsigned sw, sh;
                        uint8_t *rgba = NULL;
                        if (lodepng_decode_memory(&rgba, &sw, &sh, png, len, LCT_RGBA, 8) == 0) {
                            free(png); png = NULL;
                            int dw = 50, dh = 50;
                            if (sw > sh) dh = sh * dw / sw; else dw = sw * dh / sh;
                            uint8_t *scaled = malloc(dw * dh * 4);
                            if (scaled) {
                                for (int y = 0; y < dh; y++)
                                    for (int x = 0; x < dw; x++) {
                                        int sx = x * sw / dw, sy = y * sh / dh;
                                        memcpy(&scaled[(y*dw+x)*4], &rgba[(sy*sw+sx)*4], 4);
                                    }
                                free(rgba);
                                uint8_t *rgb565 = rgba_to_rgb565a(scaled, dw, dh);
                                free(scaled);
                                if (rgb565) {
                                    boss_img_dsc.header.w = dw;
                                    boss_img_dsc.header.h = dh;
                                    boss_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
                                    boss_img_dsc.data = rgb565;
                                    boss_img_dsc.data_size = dw * dh * 3;
                                    lv_img_set_src(img_boss, &boss_img_dsc);
                                    lv_obj_align(img_boss, LV_ALIGN_TOP_MID, -120, 90);
                                    lv_obj_clear_flag(img_boss, LV_OBJ_FLAG_HIDDEN);
                                }
                            } else {
                                free(rgba);
                            }
                        } else {
                            free(png); png = NULL;
                        }
                        unlock_lvgl();
                    } else {
                        free(png); png = NULL;
                    }
                }
            }
            break;
        }
        free(json);
    }

    if (!data_loaded && lock_lvgl(-1)) {
        lv_label_set_text(lbl_status, "Fetch Failed");
        unlock_lvgl();
    }

    esp_wifi_disconnect();
    esp_wifi_stop();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (countdown_ended) {
            countdown_ended = false;
            ESP_LOGI(TAG, "Countdown ended, refreshing in 5s...");
            vTaskDelay(pdMS_TO_TICKS(5000));

            esp_wifi_start();
            esp_wifi_connect();
            ESP_LOGI(TAG, "WiFi reconnecting...");

            int wait = 0;
            while (wait < 15) {
                esp_netif_ip_info_t ip;
                if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) break;
                vTaskDelay(pdMS_TO_TICKS(1000));
                wait++;
            }

            int ok = 0;
            for (int r = 0; r < 3 && !ok; r++) {
                if (r > 0) vTaskDelay(pdMS_TO_TICKS(2000));
                char *json = NULL; size_t jlen = 0;
                if (http_get(SCHEDULE_URL, (uint8_t **)&json, &jlen) < 0) continue;
                if (parse_schedule(json)) {
                    free(json);
                    data_loaded = true;
                    if (lock_lvgl(-1)) {
                        lv_label_set_text(lbl_map_name, stage_cn(shift_map_name));
                        lv_label_set_text(lbl_time_range, shift_times_str);
                        countdown_update(NULL);
                        lv_obj_invalidate(lbl_map_name);
                        lv_obj_invalidate(lbl_time_range);
                        lv_obj_invalidate(lbl_countdown);
                        unlock_lvgl();
                    }
                    ok = 1;
                } else {
                    free(json);
                }
            }

            esp_wifi_disconnect();
            esp_wifi_stop();
            ESP_LOGI(TAG, "Refresh %s", ok ? "OK" : "failed");
        }
    }
}

static void wifi_init_sta(void) {
    static bool inited = false;
    if (!inited) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        wifi_config_t wc = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        inited = true;
    }
    esp_wifi_start();
    esp_wifi_connect();
    ESP_LOGI(TAG, "WiFi connecting...");
}

static void time_sync_after_wifi(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    struct tm ti = {0};
    int r = 0;
    while (ti.tm_year < (2025 - 1900) && ++r < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time_t n; time(&n); localtime_r(&n, &ti);
    }
    setenv("TZ", "CST-8", 1); tzset();
    ESP_LOGI(TAG, "Time synced");
}

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0x80}, 1, 0},
};

static void button_sleep_task(void *arg) {
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        if (gpio_get_level(PIN_BUTTON) == 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (gpio_get_level(PIN_BUTTON) == 0) {
                ESP_LOGI(TAG, "Button held, restarting...");
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void) {
    nvs_flash_init();

    esp_pm_config_t pm = { .max_freq_mhz = 80, .min_freq_mhz = 80, .light_sleep_enable = false };
    esp_pm_configure(&pm);

    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);

    i2c_config_t i2c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = PIN_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 200000,
    };
    i2c_param_config(I2C_NUM_0, &i2c);
    i2c_driver_install(I2C_NUM_0, i2c.mode, 0, 0, 0);

    esp_io_expander_handle_t iox = NULL;
    esp_io_expander_new_i2c_tca9554(I2C_NUM_0, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &iox);
    esp_io_expander_set_dir(iox, 0, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_dir(iox, 1, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_dir(iox, 2, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_dir(iox, 7, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(iox, 0, 1);
    esp_io_expander_set_level(iox, 1, 1);
    esp_io_expander_set_level(iox, 2, 1);
    esp_io_expander_set_level(iox, 7, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_io_expander_set_level(iox, 7, 1);

    spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        LCD_H_RES * LCD_V_RES * 2);
    spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);

    static lv_disp_drv_t dd;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t ioc = SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, flush_ready, &dd);
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &ioc, &io);

    sh8601_vendor_config_t vc = { .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1} };

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t dc = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vc,
    };
    esp_lcd_new_panel_sh8601(io, &dc, &panel);
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    lv_init();
    lv_extra_init();
    lv_color_t *b1 = heap_caps_malloc(LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *b2 = heap_caps_malloc(LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(b1 && b2);

    static lv_disp_draw_buf_t db;
    lv_disp_draw_buf_init(&db, b1, b2, LCD_H_RES * LVGL_BUF_HEIGHT);
    lv_disp_drv_init(&dd);
    dd.hor_res = LCD_H_RES; dd.ver_res = LCD_V_RES;
    dd.flush_cb = flush_cb; dd.rounder_cb = round_cb;
    dd.draw_buf = &db; dd.user_data = panel;
    lv_disp_t *disp = lv_disp_drv_register(&dd);
    lv_disp_set_bg_color(disp, lv_color_black());
    (void)disp;

    esp_timer_handle_t lvgl_timer;
    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lvgl_tick" };
    esp_timer_create(&ta, &lvgl_timer);
    esp_timer_start_periodic(lvgl_timer, LVGL_TICK_PERIOD_MS * 1000);

    lvgl_mux = xSemaphoreCreateMutex();
    xTaskCreate(lvgl_task, "LVGL", 4096, NULL, 2, NULL);

    if (lock_lvgl(-1)) {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);

        img_map = lv_img_create(lv_scr_act());
        lv_obj_add_flag(img_map, LV_OBJ_FLAG_HIDDEN);

        lbl_open_badge = lv_label_create(lv_scr_act());
        lv_obj_set_style_bg_color(lbl_open_badge, lv_color_make(0x60, 0x3B, 0xFF), 0);
        lv_obj_set_style_bg_opa(lbl_open_badge, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(lbl_open_badge, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl_open_badge, &lv_font_pingfang_18_cjk, 0);
        lv_obj_set_style_border_color(lbl_open_badge, lv_color_white(), 0);
        lv_obj_set_style_border_width(lbl_open_badge, 2, 0);
        lv_obj_set_style_pad_hor(lbl_open_badge, 10, 0);
        lv_obj_set_style_pad_ver(lbl_open_badge, 4, 0);
        lv_obj_set_style_radius(lbl_open_badge, 4, 0);
        lv_label_set_text(lbl_open_badge, "开放!");
        lv_obj_align(lbl_open_badge, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_add_flag(lbl_open_badge, LV_OBJ_FLAG_HIDDEN);

        img_boss = lv_img_create(lv_scr_act());
        lv_obj_add_flag(img_boss, LV_OBJ_FLAG_HIDDEN);

        int wa = LCD_H_RES / 4;
        for (int i = 0; i < 4; i++) {
            int x = (i - 1) * wa + (wa - 80) / 2;
            if (i == 0) x = -(wa * 3 / 2) + (wa - 80) / 2;
            else if (i == 1) x = -(wa / 2) + (wa - 80) / 2;
            else if (i == 2) x = (wa / 2) + (wa - 80) / 2;
            else x = (wa * 3 / 2) + (wa - 80) / 2;

            img_weapon[i] = lv_img_create(lv_scr_act());
            lv_img_set_zoom(img_weapon[i], 256);
            lv_obj_align(img_weapon[i], LV_ALIGN_TOP_MID, x, 384);
            lv_obj_add_flag(img_weapon[i], LV_OBJ_FLAG_HIDDEN);
        }

        lbl_map_name = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_color(lbl_map_name, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl_map_name, &lv_font_pingfang_18_cjk, 0);
        lv_label_set_text(lbl_map_name, "");
        lv_obj_set_style_bg_color(lbl_map_name, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lbl_map_name, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(lbl_map_name, 8, 0);
        lv_obj_set_style_pad_ver(lbl_map_name, 4, 0);
        lv_obj_set_style_text_letter_space(lbl_map_name, 0, 0);
        lv_obj_align(lbl_map_name, LV_ALIGN_TOP_MID, 0, 153);

        lbl_time_range = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_color(lbl_time_range, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl_time_range, &lv_font_pingfang_22_cjk, 0);
        lv_obj_set_style_text_letter_space(lbl_time_range, 0, 0);
        lv_label_set_text(lbl_time_range, "");
        lv_obj_align(lbl_time_range, LV_ALIGN_TOP_MID, 0, 51);

        lbl_countdown = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_color(lbl_countdown, lv_color_make(0xFE, 0xEF, 0x65), 0);
        lv_obj_set_style_text_font(lbl_countdown, &lv_font_pingfang_24_cjk, 0);
        lv_obj_set_style_text_letter_space(lbl_countdown, 0, 0);
        lv_label_set_text(lbl_countdown, "");
        lv_obj_align(lbl_countdown, LV_ALIGN_TOP_MID, 30, 105);

        lbl_status = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl_status, &lv_font_pingfang_16_cjk, 0);
        lv_label_set_text(lbl_status, "Loading...");
        lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 0);

        lbl_weapon_title = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_color(lbl_weapon_title, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl_weapon_title, &lv_font_pingfang_18_cjk, 0);
        lv_obj_set_style_text_letter_space(lbl_weapon_title, 0, 0);
        lv_label_set_text(lbl_weapon_title, "发放武器");
        lv_obj_align(lbl_weapon_title, LV_ALIGN_TOP_MID, 0, 368);

        lv_timer_create(countdown_update, 1000, NULL);

        unlock_lvgl();
    }

    xTaskCreate(fetch_worker, "fetch", 8192, NULL, 4, NULL);
}
