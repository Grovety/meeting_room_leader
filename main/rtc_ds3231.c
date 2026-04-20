// rtc_ds3231.c  — extended: DS3231 (0x68) and PCF8563-family (0x51) support
// Put this file in components/rtc_ds3231/ and build normally.

#include "rtc_ds3231.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_master.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char* TAG = "rtc_ds3231";

/* Default I2C parameters — match main.cpp usage */
#ifndef I2C_PORT_NUM
#define I2C_PORT_NUM I2C_NUM_0
#endif
#ifndef I2C_MASTER_TIMEOUT_MS
#define I2C_MASTER_TIMEOUT_MS 1000
#endif

/* Known candidate addresses we will test */
static uint8_t rtc_addr = 0;
typedef enum {
    RTC_TYPE_UNKNOWN = 0,
    RTC_TYPE_DS3231,
    RTC_TYPE_PCF8563
} rtc_type_t;
static rtc_type_t rtc_type = RTC_TYPE_UNKNOWN;

/* BCD helpers */
static inline uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)((v / 10) << 4) | (v % 10); }

/* low level i2c helpers for register read/write with 8-bit register pointer */
static esp_err_t i2c_write_bytes_addr(uint8_t dev_addr, uint8_t reg, const uint8_t* data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (! cmd) {
        ESP_LOGW(TAG, "i2c_write_bytes_addr: failed to allocate command link");
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    if (len) {
        i2c_master_write(cmd, data, len, true);
    }
    i2c_master_stop(cmd);
    esp_err_t r = ESP_ERR_TIMEOUT;
    if (i2c_master_lock(I2C_MASTER_TIMEOUT_MS)) {
        r = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
        i2c_master_unlock();
    }
    i2c_cmd_link_delete(cmd);
    return r;
}

static esp_err_t i2c_read_bytes_addr(uint8_t dev_addr, uint8_t reg, uint8_t* data, size_t len)
{
    if (! data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (! cmd) {
        ESP_LOGW(TAG, "i2c_read_bytes_addr: failed to allocate command link");
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + (len - 1), I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t r = ESP_ERR_TIMEOUT;
    if (i2c_master_lock(I2C_MASTER_TIMEOUT_MS)) {
        r = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
        i2c_master_unlock();
    }
    i2c_cmd_link_delete(cmd);
    return r;
}

/* Try simple ACK probe at address */
static bool i2c_probe_addr(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (! cmd) {
        ESP_LOGW(TAG, "i2c_probe_addr: failed to allocate command link");
        return false;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t r = ESP_ERR_TIMEOUT;
    if (i2c_master_lock(100)) {
        r = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(100));
        i2c_master_unlock();
    }
    i2c_cmd_link_delete(cmd);
    return (r == ESP_OK);
}

/* Validate time fields roughly */
static bool validate_tm_fields(int sec, int min, int hour, int mday, int mon, int year)
{
    if (sec < 0 || sec > 59)
        return false;
    if (min < 0 || min > 59)
        return false;
    if (hour < 0 || hour > 23)
        return false;
    if (mday < 1 || mday > 31)
        return false;
    if (mon < 1 || mon > 12)
        return false;
    if (year < 2000 || year > 2099)
        return false; /* plausible range */
    return true;
}

/* Try to autodetect RTC: prefer DS3231 at 0x68, else PCF8563-family at 0x51.
   If found, set rtc_addr and rtc_type and return ESP_OK. */
esp_err_t rtc_ds3231_init(void)
{
    /* First: try DS3231 @0x68 (common) */
    const uint8_t ds_addr = 0x68;
    if (i2c_probe_addr(ds_addr)) {
        ESP_LOGI(TAG, "Device ACK at 0x%02X — probing as DS3231", ds_addr);

        /* quick sanity read of regs 0x00..0x06 */
        uint8_t buf[7];
        if (i2c_read_bytes_addr(ds_addr, 0x00, buf, sizeof(buf)) == ESP_OK) {
            /* convert minimal fields and validate */
            int sec  = bcd2bin(buf[0] & 0x7F);
            int min  = bcd2bin(buf[1] & 0x7F);
            int hour = bcd2bin(buf[2] & 0x3F);
            int mday = bcd2bin(buf[4] & 0x3F);
            int mon  = bcd2bin(buf[5] & 0x1F);
            int year = 2000 + bcd2bin(buf[6]);
            if (validate_tm_fields(sec, min, hour, mday, mon, year)) {
                rtc_addr = ds_addr;
                rtc_type = RTC_TYPE_DS3231;
                ESP_LOGI(TAG, "Detected DS3231 at 0x%02X (time looks valid)", ds_addr);
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Probe DS3231 at 0x%02X: time fields invalid, ignoring", ds_addr);
            }
        } else {
            ESP_LOGW(TAG, "Failed to read DS3231 registers at 0x%02X", ds_addr);
        }
    } else {
        ESP_LOGD(TAG, "No ACK at 0x%02X", ds_addr);
    }

    /* Try PCF8563-family typical address 0x51 */
    const uint8_t pcf_addr = 0x51;
    if (i2c_probe_addr(pcf_addr)) {
        ESP_LOGI(TAG, "Device ACK at 0x%02X — probing as PCF8563-family", pcf_addr);
        /* read 7 bytes from 0x02 (seconds..year) */
        uint8_t buf[7];
        if (i2c_read_bytes_addr(pcf_addr, 0x02, buf, sizeof(buf)) == ESP_OK) {
            /* PCF8563 bytes: sec, min, hour, day, weekday, month, year (BCD except weekday) */
            int sec            = bcd2bin(buf[0] & 0x7F);
            int min            = bcd2bin(buf[1] & 0x7F);
            int hour           = bcd2bin(buf[2] & 0x3F);
            int mday           = bcd2bin(buf[3] & 0x3F);
            int /*weekday*/ wk = (buf[4] & 0x07);
            (void)wk;
            int mon  = bcd2bin(buf[5] & 0x1F);
            int year = 2000 + bcd2bin(buf[6]);

            if (validate_tm_fields(sec, min, hour, mday, mon, year)) {
                rtc_addr = pcf_addr;
                rtc_type = RTC_TYPE_PCF8563;
                ESP_LOGI(TAG, "Detected PCF8563-family RTC at 0x%02X (time valid)", pcf_addr);
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Probe PCF8563 at 0x%02X: time fields invalid, ignoring", pcf_addr);
            }
        } else {
            ESP_LOGW(TAG, "Failed to read PCF8563 registers at 0x%02X", pcf_addr);
        }
    } else {
        ESP_LOGD(TAG, "No ACK at 0x%02X", pcf_addr);
    }

    /* Nothing recognized */
    rtc_addr = 0;
    rtc_type = RTC_TYPE_UNKNOWN;
    ESP_LOGW(TAG, "rtc_ds3231_init: no supported RTC detected on I2C bus");
    return ESP_FAIL;
}

/* Read epoch (localtime) from the detected RTC. Returns true on success. */
bool rtc_ds3231_read_epoch(time_t* out_epoch)
{
    if (! out_epoch)
        return false;
    if (rtc_addr == 0 || rtc_type == RTC_TYPE_UNKNOWN) {
        ESP_LOGW(TAG, "rtc read: unknown rtc_addr/type, call rtc_ds3231_init() first");
        return false;
    }

    if (rtc_type == RTC_TYPE_DS3231) {
        uint8_t buf[7];
        if (i2c_read_bytes_addr(rtc_addr, 0x00, buf, sizeof(buf)) != ESP_OK) {
            ESP_LOGW(TAG, "DS3231 read failed at 0x%02X", rtc_addr);
            return false;
        }
        int sec  = bcd2bin(buf[0] & 0x7F);
        int min  = bcd2bin(buf[1] & 0x7F);
        int hour = bcd2bin(buf[2] & 0x3F);
        int mday = bcd2bin(buf[4] & 0x3F);
        int mon  = bcd2bin(buf[5] & 0x1F);
        int year = 2000 + bcd2bin(buf[6]);

        if (! validate_tm_fields(sec, min, hour, mday, mon, year)) {
            ESP_LOGW(TAG, "DS3231 returned invalid time fields");
            return false;
        }
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_sec  = sec;
        tmv.tm_min  = min;
        tmv.tm_hour = hour;
        tmv.tm_mday = mday;
        tmv.tm_mon  = mon - 1;
        tmv.tm_year = year - 1900;
        *out_epoch  = mktime(&tmv);
        return (*out_epoch > 1600000000);
    }

    if (rtc_type == RTC_TYPE_PCF8563) {
        uint8_t buf[7];
        if (i2c_read_bytes_addr(rtc_addr, 0x02, buf, sizeof(buf)) != ESP_OK) {
            ESP_LOGW(TAG, "PCF8563 read failed at 0x%02X", rtc_addr);
            return false;
        }
        int sec            = bcd2bin(buf[0] & 0x7F);
        int min            = bcd2bin(buf[1] & 0x7F);
        int hour           = bcd2bin(buf[2] & 0x3F);
        int mday           = bcd2bin(buf[3] & 0x3F);
        int /*weekday*/ wk = buf[4] & 0x07;
        (void)wk;
        int mon  = bcd2bin(buf[5] & 0x1F);
        int year = 2000 + bcd2bin(buf[6]);

        if (! validate_tm_fields(sec, min, hour, mday, mon, year)) {
            ESP_LOGW(TAG, "PCF8563 returned invalid time fields");
            return false;
        }
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_sec  = sec;
        tmv.tm_min  = min;
        tmv.tm_hour = hour;
        tmv.tm_mday = mday;
        tmv.tm_mon  = mon - 1;
        tmv.tm_year = year - 1900;
        *out_epoch  = mktime(&tmv);
        return (*out_epoch > 1600000000);
    }

    return false;
}

/* Write epoch (localtime) into detected RTC. Returns true on success. */
bool rtc_ds3231_write_epoch(time_t epoch)
{
    if (rtc_addr == 0 || rtc_type == RTC_TYPE_UNKNOWN) {
        ESP_LOGW(TAG, "rtc write: unknown rtc_addr/type");
        return false;
    }

    struct tm tmv;
    localtime_r(&epoch, &tmv);

    if (! validate_tm_fields(tmv.tm_sec, tmv.tm_min, tmv.tm_hour, tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900)) {
        ESP_LOGW(TAG, "Attempt to write suspicious time to RTC");
        /* still attempt write, but warn */
    }

    if (rtc_type == RTC_TYPE_DS3231) {
        uint8_t buf[7];
        buf[0] = bin2bcd(tmv.tm_sec);
        buf[1] = bin2bcd(tmv.tm_min);
        buf[2] = bin2bcd(tmv.tm_hour);
        buf[3] = bin2bcd((tmv.tm_wday == 0) ? 7 : tmv.tm_wday);
        buf[4] = bin2bcd(tmv.tm_mday);
        buf[5] = bin2bcd(tmv.tm_mon + 1);
        buf[6] = bin2bcd((tmv.tm_year + 1900) - 2000);
        if (i2c_write_bytes_addr(rtc_addr, 0x00, buf, sizeof(buf)) != ESP_OK) {
            ESP_LOGW(TAG, "DS3231 write failed at 0x%02X", rtc_addr);
            return false;
        }
        return true;
    }

    if (rtc_type == RTC_TYPE_PCF8563) {
        /* PCF8563 expects writes to registers 0x02..0x08 (sec..year) */
        uint8_t buf[7];
        buf[0] = bin2bcd(tmv.tm_sec);
        buf[1] = bin2bcd(tmv.tm_min);
        buf[2] = bin2bcd(tmv.tm_hour);
        buf[3] = bin2bcd(tmv.tm_mday);
        buf[4] = (uint8_t)(tmv.tm_wday & 0x07);
        buf[5] = bin2bcd(tmv.tm_mon + 1); /* month (1..12) */
        buf[6] = bin2bcd((tmv.tm_year + 1900) - 2000);
        if (i2c_write_bytes_addr(rtc_addr, 0x02, buf, sizeof(buf)) != ESP_OK) {
            ESP_LOGW(TAG, "PCF8563 write failed at 0x%02X", rtc_addr);
            return false;
        }
        return true;
    }

    return false;
}
