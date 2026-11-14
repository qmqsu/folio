/* Pure host regression: link the complete production PMU driver, not a parser copy.
 * SDK calls below only exchange in-memory wire frames; no hardware or threads.
 */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "read_pico_pmu.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"

#define OLD_TIME UINT32_C(0x65abcdef)
#define NEW_TIME UINT32_C(0x66fedcba)
#define REJECTED_TIME UINT32_C(0x78012345)
#define BOOT_ID UINT32_C(0x12345678)

typedef enum {
    FAULT_NONE, FAULT_CRC, FAULT_MAGIC, FAULT_SEQUENCE,
    FAULT_WRITE, FAULT_READ, FAULT_READ_TIMEOUT, FAULT_REGISTER_SELECT
} fault_t;

typedef struct {
    uint16_t status;
    uint8_t length;
    uint32_t seconds;
    uint8_t synced;
    fault_t fault;
} reply_t;

static int s_bus_token, s_dev_token;
static uint8_t s_request[PMU_FRAME_SIZE];
static uint8_t s_selected_reg;
static reply_t s_reply;
static uint16_t s_non_time_status = PMU_STATUS_OK;
static unsigned s_checks, s_failures, s_time_writes, s_bus_resets;
static unsigned s_combined_reads, s_fallback_reads;
static const char* s_case = "setup";

#define CHECK(condition) do { \
    ++s_checks; \
    if (!(condition)) { \
        ++s_failures; \
        fprintf(stderr, "FAIL [%s] line %d: %s\n", s_case, __LINE__, #condition); \
    } \
} while (0)

static uint16_t get16(const uint8_t* p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

/* Independent nibble-table CCITT-FALSE, checked against the standard vector.
 * Do not use the driver's CRC implementation to manufacture its own oracle.
 */
static uint16_t wire_crc(const uint8_t* bytes, size_t size) {
    static const uint16_t table[16] = {
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
        0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef
    };
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        crc = (uint16_t)((crc << 4) ^ table[(crc >> 12) ^ (bytes[i] >> 4)]);
        crc = (uint16_t)((crc << 4) ^ table[(crc >> 12) ^ (bytes[i] & 0x0f)]);
    }
    return crc;
}

static void seal(uint8_t* raw, size_t size) {
    assert(size >= 2);
    put16(raw + size - 2, wire_crc(raw, size - 2));
}

static bool is_time_request(void) {
    return get16(s_request + 8) == PMU_CMD_TIME_GET;
}

static esp_err_t register_reply(uint8_t reg, uint8_t* raw, size_t size) {
    memset(raw, 0, size);
    switch (reg) {
        case PMU_REG_IDENTITY:
            assert(size == PMU_IDENTITY_SIZE);
            memcpy(raw, "PMU1", 4);
            raw[4] = PMU_PROTOCOL_MAJOR;
            raw[5] = PMU_PROTOCOL_MINOR;
            raw[6] = PMU_FW_MAJOR;
            raw[7] = PMU_FW_MINOR;
            raw[8] = PMU_FW_PATCH;
            put32(raw + 12, BOOT_ID);
            seal(raw, size);
            break;
        case PMU_REG_STATUS:
            assert(size == PMU_STATUS_SIZE);
            raw[2] = PMU_PWR_RUNNING;
            put16(raw + 40, get16(s_request + 6));
            seal(raw, size);
            break;
        case PMU_REG_QUICK_BATTERY:
            assert(size == PMU_QUICK_BATTERY_SIZE);
            seal(raw, size);
            break;
        case PMU_REG_EVENT_COUNT:
            assert(size == 1);
            break;
        case PMU_REG_RESPONSE: {
            assert(size == PMU_FRAME_SIZE);
            bool time = is_time_request();
            if (time && s_reply.fault == FAULT_READ_TIMEOUT) return ESP_ERR_TIMEOUT;
            if (time && (s_reply.fault == FAULT_READ ||
                         s_reply.fault == FAULT_REGISTER_SELECT)) return ESP_FAIL;
            raw[0] = PMU_FRAME_MAGIC;
            raw[1] = 1;
            raw[2] = PMU_FRAME_KIND_RESP;
            raw[4] = PMU_PROTOCOL_MAJOR;
            raw[5] = PMU_PROTOCOL_MINOR;
            memcpy(raw + 6, s_request + 6, 4); /* Matching sequence and command. */
            put16(raw + 10, time ? s_reply.status : s_non_time_status);
            put32(raw + 14, BOOT_ID);
            if (time) {
                raw[12] = s_reply.length;
                /* Deliberately plausible bytes beyond short declared lengths. */
                put32(raw + 18, s_reply.seconds);
                put16(raw + 22, 0x3412);
                raw[24] = s_reply.synced;
                raw[25] = 0xa5;
                if (s_reply.fault == FAULT_MAGIC) raw[0] ^= 1;
                if (s_reply.fault == FAULT_SEQUENCE) put16(raw + 6, 0);
            }
            seal(raw, size);
            if (time && s_reply.fault == FAULT_CRC) raw[62] ^= 1;
            break;
        }
        default:
            assert(!"Unexpected register: extend the fixture explicitly");
    }
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
    const i2c_device_config_t* config, i2c_master_dev_handle_t* device) {
    assert(bus == &s_bus_token && config != NULL && device != NULL);
    assert(config->device_address == PMU_I2C_ADDR);
    *device = &s_dev_token;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
    assert(device == &s_dev_token);
    return ESP_OK;
}

esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus) {
    assert(bus == &s_bus_token);
    ++s_bus_resets;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
    const uint8_t* data, size_t size, int timeout_ms) {
    assert(device == &s_dev_token && data != NULL && timeout_ms == 100);
    if (size == 1) {
        s_selected_reg = data[0];
        if (is_time_request() && s_selected_reg == PMU_REG_RESPONSE &&
            s_reply.fault == FAULT_REGISTER_SELECT) return ESP_FAIL;
        return ESP_OK;
    }
    assert(size == 1 + PMU_FRAME_SIZE && data[0] == PMU_REG_COMMAND);
    memcpy(s_request, data + 1, PMU_FRAME_SIZE);
    assert(s_request[0] == PMU_FRAME_MAGIC && s_request[2] == PMU_FRAME_KIND_REQ);
    assert(get16(s_request + 6) != 0);
    assert(wire_crc(s_request, 62) == get16(s_request + 62));
    if (is_time_request()) {
        ++s_time_writes;
        assert(s_request[12] == 0);
        if (s_reply.fault == FAULT_WRITE) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t device,
    uint8_t* data, size_t size, int timeout_ms) {
    assert(device == &s_dev_token && timeout_ms == 100);
    ++s_fallback_reads;
    return register_reply(s_selected_reg, data, size);
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
    const uint8_t* write_data, size_t write_size,
    uint8_t* read_data, size_t read_size, int timeout_ms) {
    assert(device == &s_dev_token && write_size == 1 && timeout_ms == 100);
    ++s_combined_reads;
    return register_reply(write_data[0], read_data, read_size);
}

void vTaskDelay(TickType_t ticks) { (void)ticks; }
void esp_rom_delay_us(uint32_t us) { (void)us; }

static reply_t valid_reply(uint32_t seconds, uint8_t synced) {
    return (reply_t){PMU_STATUS_OK, 8, seconds, synced, FAULT_NONE};
}

static void init_driver(void) {
    CHECK(read_pico_pmu_deinit() == ESP_OK);
    memset(s_request, 0, sizeof(s_request));
    s_non_time_status = PMU_STATUS_OK;
    s_reply = valid_reply(OLD_TIME, 1);
    read_pico_pmu_config_t config = READ_PICO_PMU_CONFIG_DEFAULT();
    CHECK(read_pico_pmu_init(&s_bus_token, &config) == ESP_OK);
    CHECK(read_pico_pmu_ready());
    CHECK(!read_pico_pmu_get()->time_ok);
}

static void read_valid(uint32_t seconds, uint8_t synced) {
    s_reply = valid_reply(seconds, synced);
    CHECK(read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) == ESP_OK);
    const pmu_snapshot_t* snap = read_pico_pmu_get();
    CHECK(snap->time_ok);
    CHECK(snap->unix_sec == seconds);
    CHECK(snap->time_synced == synced);
    CHECK(snap->last_err == ESP_OK);
}

static void check_rejected(reply_t reply, esp_err_t expected, bool seeded) {
    init_driver();
    if (seeded) read_valid(OLD_TIME, 1);
    uint32_t before_seconds = read_pico_pmu_get()->unix_sec;
    uint8_t before_synced = read_pico_pmu_get()->time_synced;
    unsigned writes = s_time_writes;
    unsigned resets = s_bus_resets;
    s_reply = reply;
    esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    const pmu_snapshot_t* snap = read_pico_pmu_get();
    CHECK(err == expected);
    CHECK(!snap->time_ok);
    CHECK(snap->unix_sec == before_seconds);
    CHECK(snap->time_synced == before_synced);
    if (reply.fault == FAULT_NONE) CHECK(snap->last_op_status == reply.status);
    if (reply.fault == FAULT_READ_TIMEOUT) CHECK(s_bus_resets > resets);
    CHECK(s_time_writes > writes);
    /* Invalidating a sample must not prevent the next genuine success. */
    read_valid(NEW_TIME, 0);
    read_valid(NEW_TIME + 1, 1);
}

static void test_valid(void) {
    s_case = "valid 8B / unsigned LE / synced and unset";
    init_driver();
    read_valid(OLD_TIME, 1);
    read_valid(UINT32_C(0x12345678), 1);
    read_valid(UINT32_MAX, 1);
    read_valid(0, 0); /* A well-formed unset RTC is parsed, not declared calibrated. */
    puts("PASS valid 8B: LE seconds, uint32 max, synced=0/1, unset=0");
}

static void test_lengths(void) {
    unsigned before = s_failures;
    char name[96];
    for (unsigned length = 0; length <= UINT8_MAX; ++length) {
        if (length == 8) continue;
        for (unsigned seeded = 0; seeded <= 1; ++seeded) {
            snprintf(name, sizeof(name), "CRC-valid length=%u, old_valid=%u", length, seeded);
            s_case = name;
            reply_t reply = valid_reply(REJECTED_TIME, 1);
            reply.length = (uint8_t)length;
            check_rejected(reply, ESP_ERR_INVALID_RESPONSE, seeded != 0);
        }
    }
    printf("%s CRC-valid lengths: 0..255 except 8, cold + old-valid + recovery\n",
           s_failures == before ? "PASS" : "FAIL");
}

static void test_statuses(void) {
    static const uint16_t statuses[] = {
        PMU_STATUS_ACCEPTED, PMU_STATUS_BUSY, PMU_STATUS_BAD_CRC,
        PMU_STATUS_BAD_MAGIC, PMU_STATUS_BAD_LENGTH, PMU_STATUS_UNSUPPORTED_VERSION,
        PMU_STATUS_UNKNOWN_COMMAND, PMU_STATUS_INVALID_ARGUMENT,
        PMU_STATUS_SEQUENCE_CONFLICT, PMU_STATUS_STALE_SESSION, PMU_STATUS_NOT_SUPPORTED,
        PMU_STATUS_INVALID_STATE, PMU_STATUS_NOT_ARMED, PMU_STATUS_TOKEN_EXPIRED,
        PMU_STATUS_PERMISSION_DENIED, PMU_STATUS_CONFIG_INVALID, PMU_STATUS_NVM_FAILURE,
        PMU_STATUS_ADC_FAILURE, PMU_STATUS_HOST_TIMEOUT, PMU_STATUS_INTERNAL_ERROR, 0xffff
    };
    unsigned before = s_failures;
    char name[96];
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        for (unsigned seeded = 0; seeded <= 1; ++seeded) {
            snprintf(name, sizeof(name), "CRC-valid status=0x%04x, old_valid=%u", statuses[i], seeded);
            s_case = name;
            reply_t reply = valid_reply(REJECTED_TIME, 1);
            reply.status = statuses[i];
            check_rejected(reply, ESP_ERR_INVALID_RESPONSE, seeded != 0);
        }
    }
    s_case = "ACCEPTED empty payload after valid sample";
    reply_t reply = valid_reply(REJECTED_TIME, 1);
    reply.status = PMU_STATUS_ACCEPTED;
    reply.length = 0;
    check_rejected(reply, ESP_ERR_INVALID_RESPONSE, true);
    printf("%s CRC-valid non-OK statuses: all 20 defined + unknown; ACCEPTED 8B/0B\n",
           s_failures == before ? "PASS" : "FAIL");
}

static void test_failures(void) {
    static const struct { fault_t fault; esp_err_t error; const char* name; } cases[] = {
        {FAULT_CRC, ESP_ERR_INVALID_CRC, "bad frame CRC"},
        {FAULT_MAGIC, ESP_ERR_INVALID_RESPONSE, "bad magic, valid CRC"},
        {FAULT_SEQUENCE, ESP_ERR_INVALID_RESPONSE, "wrong sequence, valid CRC"},
        {FAULT_WRITE, ESP_FAIL, "command write failure"},
        {FAULT_READ, ESP_FAIL, "response read + fallback failure"},
        {FAULT_READ_TIMEOUT, ESP_ERR_TIMEOUT, "response timeout + bus reset"},
        {FAULT_REGISTER_SELECT, ESP_FAIL, "fallback register select failure"}
    };
    unsigned before = s_failures;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        s_case = cases[i].name;
        reply_t reply = valid_reply(REJECTED_TIME, 1);
        reply.fault = cases[i].fault;
        check_rejected(reply, cases[i].error, true);
    }
    s_case = "not initialized";
    CHECK(read_pico_pmu_deinit() == ESP_OK);
    CHECK(read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) == ESP_ERR_INVALID_STATE);
    CHECK(!read_pico_pmu_get()->time_ok);
    printf("%s transport/frame/invalid-state failures invalidate samples; subsequent recovery\n",
           s_failures == before ? "PASS" : "FAIL");
}

static void test_other_commands(void) {
    unsigned before = s_failures;
    s_case = "other commands retain ACCEPTED and cannot revive failed time";
    init_driver();
    read_valid(OLD_TIME, 1);
    s_non_time_status = PMU_STATUS_ACCEPTED;
    CHECK(read_pico_pmu_cmd(PMU_CMD_HOST_SOFT_SLEEP, NULL, 0) == ESP_OK);
    CHECK(read_pico_pmu_get()->time_ok);
    CHECK(read_pico_pmu_get()->unix_sec == OLD_TIME);
    s_non_time_status = PMU_STATUS_BUSY;
    CHECK(read_pico_pmu_cmd(PMU_CMD_HOST_SOFT_SLEEP, NULL, 0) == ESP_ERR_INVALID_RESPONSE);
    CHECK(read_pico_pmu_get()->time_ok);
    s_reply.status = PMU_STATUS_ACCEPTED;
    CHECK(read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) == ESP_ERR_INVALID_RESPONSE);
    CHECK(!read_pico_pmu_get()->time_ok);
    s_non_time_status = PMU_STATUS_ACCEPTED;
    CHECK(read_pico_pmu_cmd(PMU_CMD_HOST_READY, NULL, 0) == ESP_OK);
    CHECK(!read_pico_pmu_get()->time_ok);
    read_valid(NEW_TIME, 1);
    printf("%s other-command ACCEPTED semantics and time validity isolation\n",
           s_failures == before ? "PASS" : "FAIL");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    s_case = "CRC known vector";
    const uint8_t vector[] = "123456789";
    CHECK(wire_crc(vector, sizeof(vector) - 1) == 0x29b1);
    CHECK(pmu_crc16_ccitt_false(vector, sizeof(vector) - 1) == 0x29b1);
    puts("Real driver + public API + independent CRC-valid raw response fixture");
    test_valid();
    test_lengths();
    test_statuses();
    test_failures();
    test_other_commands();
    s_case = "teardown";
    CHECK(read_pico_pmu_deinit() == ESP_OK);
    printf("PMU TIME_GET: %u checks, %u failures; time commands=%u, combined reads=%u, fallback reads=%u\n",
           s_checks, s_failures, s_time_writes, s_combined_reads, s_fallback_reads);
    return s_failures ? 1 : 0;
}
