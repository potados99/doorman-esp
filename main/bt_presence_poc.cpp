#include "bt_presence_poc.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "aes/esp_aes.h"

namespace {

constexpr char kTag[] = "bt_poc";
constexpr char kBleDeviceName[] = "Doorman";
constexpr char kBleManufacturerName[] = "Potados";
constexpr char kBleModelNumber[] = "Doorman";
constexpr char kBleFirmwareVersion[] = "v1.0";
constexpr char kClassicDeviceName[] = "Doorman SPP";
constexpr char kClassicServerName[] = "DOORMAN_SPP";
constexpr uint32_t kBleFixedPasskey = 123456;

constexpr TickType_t kPairingWindow = pdMS_TO_TICKS(30000);
constexpr TickType_t kPairingLogInterval = pdMS_TO_TICKS(5000);
constexpr TickType_t kLoopDelay = pdMS_TO_TICKS(100);
constexpr TickType_t kBlePresenceTimeout = pdMS_TO_TICKS(3000);
constexpr TickType_t kClassicPresenceTimeout = pdMS_TO_TICKS(5000);
constexpr TickType_t kClassicProbeRetryDelay = pdMS_TO_TICKS(200);
constexpr int kMaxBleBondedDevices = 15;
constexpr int kMaxClassicBondedDevices = 15;
constexpr uint16_t kClassicPageTimeout = 0x0100;  // 160 ms

constexpr esp_spp_mode_t kSppMode = ESP_SPP_MODE_CB;
constexpr bool kSppEnableL2capErtm = true;
constexpr esp_spp_sec_t kSppSecMask = ESP_SPP_SEC_AUTHENTICATE;
constexpr esp_spp_role_t kSppRole = ESP_SPP_ROLE_SLAVE;

constexpr uint8_t kBleAdvFlags = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;

template <size_t N>
constexpr std::array<uint8_t, N + 12> make_ble_adv_raw_data(const char (&name)[N])
{
    std::array<uint8_t, N + 12> data = {};
    size_t pos = 0;

    data[pos++] = 0x02;
    data[pos++] = ESP_BLE_AD_TYPE_FLAG;
    data[pos++] = kBleAdvFlags;

    data[pos++] = 0x03;
    data[pos++] = ESP_BLE_AD_TYPE_16SRV_CMPL;
    data[pos++] = 0x0D;
    data[pos++] = 0x18;

    data[pos++] = 0x03;
    data[pos++] = ESP_BLE_AD_TYPE_APPEARANCE;
    data[pos++] = 0x40;
    data[pos++] = 0x03;

    data[pos++] = static_cast<uint8_t>(N);
    data[pos++] = ESP_BLE_AD_TYPE_NAME_CMPL;
    for (size_t i = 0; i < (N - 1); ++i) {
        data[pos++] = static_cast<uint8_t>(name[i]);
    }

    return data;
}

template <size_t N>
constexpr std::array<uint8_t, N + 13> make_ble_scan_rsp_raw_data(const char (&name)[N])
{
    std::array<uint8_t, N + 13> data = {};
    size_t pos = 0;

    data[pos++] = static_cast<uint8_t>(N);
    data[pos++] = ESP_BLE_AD_TYPE_NAME_CMPL;
    for (size_t i = 0; i < (N - 1); ++i) {
        data[pos++] = static_cast<uint8_t>(name[i]);
    }

    data[pos++] = 0x05;
    data[pos++] = ESP_BLE_AD_TYPE_16SRV_PART;
    data[pos++] = 0x0F;
    data[pos++] = 0x18;
    data[pos++] = 0x0A;
    data[pos++] = 0x18;

    data[pos++] = 0x05;
    data[pos++] = ESP_BLE_AD_TYPE_INT_RANGE;
    data[pos++] = 0x20;
    data[pos++] = 0x00;
    data[pos++] = 0x40;
    data[pos++] = 0x00;

    return data;
}

enum HrsIndex {
    kHrsSvc,
    kHrsMeasChar,
    kHrsMeasVal,
    kHrsMeasCfg,
    kHrsBodySensorChar,
    kHrsBodySensorVal,
    kHrsCtrlPointChar,
    kHrsCtrlPointVal,
    kHrsCount,
};

enum BasIndex {
    kBasSvc,
    kBasLevelChar,
    kBasLevelVal,
    kBasLevelCfg,
    kBasCount,
};

enum DisIndex {
    kDisSvc,
    kDisManufacturerChar,
    kDisManufacturerVal,
    kDisModelChar,
    kDisModelVal,
    kDisFirmwareChar,
    kDisFirmwareVal,
    kDisCount,
};

struct BlePeer {
    bool valid;
    bool has_identity_key;
    esp_ble_addr_type_t identity_addr_type;
    esp_bd_addr_t identity_addr;
    uint8_t irk[16];
    TickType_t last_seen_tick;
    int8_t last_rssi;
    esp_bd_addr_t last_adv_addr;
};

struct ClassicPeer {
    bool valid;
    esp_bd_addr_t bda;
    TickType_t last_seen_tick;
    char last_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
};

portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t s_presence_task_handle = nullptr;
TickType_t s_pairing_deadline = 0;

std::atomic<bool> s_pairing_mode{false};
std::atomic<bool> s_ble_local_privacy_ready{false};
std::atomic<bool> s_ble_advertising{false};
std::atomic<bool> s_ble_scan_requested{false};
std::atomic<bool> s_ble_scanning{false};
std::atomic<bool> s_classic_probe_in_flight{false};
std::atomic<int> s_next_classic_probe_index{0};

BlePeer s_ble_peers[kMaxBleBondedDevices] = {};
ClassicPeer s_classic_peers[kMaxClassicBondedDevices] = {};
int s_ble_peer_count = 0;
int s_classic_peer_count = 0;

uint16_t s_hrs_handle_table[kHrsCount] = {};
uint16_t s_bas_handle_table[kBasCount] = {};
uint16_t s_dis_handle_table[kDisCount] = {};
uint8_t s_ble_adv_config_mask = 0;

constexpr uint16_t kPrimaryServiceUuid = ESP_GATT_UUID_PRI_SERVICE;
constexpr uint16_t kCharacteristicDeclUuid = ESP_GATT_UUID_CHAR_DECLARE;
constexpr uint16_t kCharacteristicConfigUuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
constexpr uint16_t kHeartRateServiceUuid = ESP_GATT_UUID_HEART_RATE_SVC;
constexpr uint16_t kBatteryServiceUuid = ESP_GATT_UUID_BATTERY_SERVICE_SVC;
constexpr uint16_t kDeviceInfoServiceUuid = ESP_GATT_UUID_DEVICE_INFO_SVC;
constexpr uint16_t kHeartRateMeasUuid = ESP_GATT_HEART_RATE_MEAS;
constexpr uint16_t kBodySensorLocationUuid = ESP_GATT_BODY_SENSOR_LOCATION;
constexpr uint16_t kHeartRateCtrlPointUuid = ESP_GATT_HEART_RATE_CNTL_POINT;
constexpr uint16_t kBatteryLevelUuid = ESP_GATT_UUID_BATTERY_LEVEL;
constexpr uint16_t kManufacturerNameUuid = ESP_GATT_UUID_MANU_NAME;
constexpr uint16_t kModelNumberUuid = ESP_GATT_UUID_MODEL_NUMBER_STR;
constexpr uint16_t kFirmwareVersionUuid = ESP_GATT_UUID_FW_VERSION_STR;
constexpr uint8_t kCharPropNotify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
constexpr uint8_t kCharPropRead = ESP_GATT_CHAR_PROP_BIT_READ;
constexpr uint8_t kCharPropReadWrite = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE;
constexpr uint8_t kCharPropReadNotify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

uint8_t s_heart_measurement_ccc[2] = {0x00, 0x00};
uint8_t s_heart_measurement_val[2] = {0x00, 75};
uint8_t s_body_sensor_loc_val[1] = {0x01};
uint8_t s_heart_ctrl_point[1] = {0x00};
uint8_t s_battery_level_val[1] = {85};
uint8_t s_battery_level_ccc[2] = {0x00, 0x00};
constexpr auto s_ble_adv_raw_data = make_ble_adv_raw_data(kBleDeviceName);
constexpr auto s_ble_scan_rsp_raw_data = make_ble_scan_rsp_raw_data(kBleDeviceName);

esp_ble_adv_params_t s_ble_adv_params = {
    .adv_int_min = ESP_BLE_GAP_ADV_ITVL_MS(160),
    .adv_int_max = ESP_BLE_GAP_ADV_ITVL_MS(160),
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_RPA_PUBLIC,
    .peer_addr = {0},
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

esp_ble_scan_params_t s_ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_RPA_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = ESP_BLE_GAP_SCAN_ITVL_MS(50),
    .scan_window = ESP_BLE_GAP_SCAN_WIN_MS(50),
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

const esp_gatts_attr_db_t s_heart_rate_gatt_db[kHrsCount] = {
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kPrimaryServiceUuid)), ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(kHeartRateServiceUuid), reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kHeartRateServiceUuid))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicDeclUuid)), ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(kCharPropNotify), reinterpret_cast<uint8_t *>(const_cast<uint8_t *>(&kCharPropNotify))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kHeartRateMeasUuid)), ESP_GATT_PERM_READ,
         sizeof(s_heart_measurement_val), sizeof(s_heart_measurement_val), s_heart_measurement_val},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicConfigUuid)),
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t), sizeof(s_heart_measurement_ccc), s_heart_measurement_ccc},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicDeclUuid)), ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(kCharPropRead), reinterpret_cast<uint8_t *>(const_cast<uint8_t *>(&kCharPropRead))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kBodySensorLocationUuid)),
         ESP_GATT_PERM_READ_ENCRYPTED, sizeof(s_body_sensor_loc_val), sizeof(s_body_sensor_loc_val), s_body_sensor_loc_val},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicDeclUuid)), ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(kCharPropReadWrite), reinterpret_cast<uint8_t *>(const_cast<uint8_t *>(&kCharPropReadWrite))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kHeartRateCtrlPointUuid)),
         ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
         sizeof(s_heart_ctrl_point), sizeof(s_heart_ctrl_point), s_heart_ctrl_point},
    },
};

const esp_gatts_attr_db_t s_battery_gatt_db[kBasCount] = {
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kPrimaryServiceUuid)), ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(kBatteryServiceUuid), reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kBatteryServiceUuid))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicDeclUuid)), ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(kCharPropReadNotify), reinterpret_cast<uint8_t *>(const_cast<uint8_t *>(&kCharPropReadNotify))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kBatteryLevelUuid)), ESP_GATT_PERM_READ,
         sizeof(s_battery_level_val), sizeof(s_battery_level_val), s_battery_level_val},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicConfigUuid)),
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t), sizeof(s_battery_level_ccc), s_battery_level_ccc},
    },
};

const esp_gatts_attr_db_t s_device_info_gatt_db[kDisCount] = {
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kPrimaryServiceUuid)), ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(kDeviceInfoServiceUuid), reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kDeviceInfoServiceUuid))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicDeclUuid)), ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(kCharPropRead), reinterpret_cast<uint8_t *>(const_cast<uint8_t *>(&kCharPropRead))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kManufacturerNameUuid)), ESP_GATT_PERM_READ,
         sizeof(kBleManufacturerName) - 1, sizeof(kBleManufacturerName) - 1,
         reinterpret_cast<uint8_t *>(const_cast<char *>(kBleManufacturerName))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicDeclUuid)), ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(kCharPropRead), reinterpret_cast<uint8_t *>(const_cast<uint8_t *>(&kCharPropRead))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kModelNumberUuid)), ESP_GATT_PERM_READ,
         sizeof(kBleModelNumber) - 1, sizeof(kBleModelNumber) - 1,
         reinterpret_cast<uint8_t *>(const_cast<char *>(kBleModelNumber))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kCharacteristicDeclUuid)), ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(kCharPropRead), reinterpret_cast<uint8_t *>(const_cast<uint8_t *>(&kCharPropRead))},
    },
    {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(const_cast<uint16_t *>(&kFirmwareVersionUuid)), ESP_GATT_PERM_READ,
         sizeof(kBleFirmwareVersion) - 1, sizeof(kBleFirmwareVersion) - 1,
         reinterpret_cast<uint8_t *>(const_cast<char *>(kBleFirmwareVersion))},
    },
};

char *bda_to_str(const uint8_t *bda, char *str, size_t size)
{
    if (bda == nullptr || str == nullptr || size < 18) {
        return nullptr;
    }

    std::snprintf(
        str,
        size,
        "%02x:%02x:%02x:%02x:%02x:%02x",
        bda[0],
        bda[1],
        bda[2],
        bda[3],
        bda[4],
        bda[5]);
    return str;
}

bool is_resolvable_private_address(const uint8_t *addr)
{
    return addr != nullptr && ((addr[0] & 0xC0U) == 0x40U);
}

bool resolve_rpa_with_irk(const uint8_t rpa[6], const uint8_t irk[16])
{
    if (!is_resolvable_private_address(rpa)) {
        return false;
    }

    uint8_t plaintext[16] = {};
    uint8_t ciphertext[16] = {};
    plaintext[13] = rpa[0];
    plaintext[14] = rpa[1];
    plaintext[15] = rpa[2];

    esp_aes_context aes;
    esp_aes_init(&aes);

    bool matched = false;
    if (esp_aes_setkey(&aes, irk, 128) == 0 &&
        esp_aes_crypt_ecb(&aes, ESP_AES_ENCRYPT, plaintext, ciphertext) == 0) {
        uint8_t reversed[16] = {};
        for (int i = 0; i < 16; ++i) {
            reversed[i] = ciphertext[15 - i];
        }

        matched = reversed[0] == rpa[5] &&
                  reversed[1] == rpa[4] &&
                  reversed[2] == rpa[3];
    }

    esp_aes_free(&aes);
    return matched;
}

void refresh_ble_bond_cache()
{
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num > kMaxBleBondedDevices) {
        dev_num = kMaxBleBondedDevices;
    }

    esp_ble_bond_dev_t dev_list[kMaxBleBondedDevices] = {};
    if (dev_num > 0) {
        esp_err_t err = esp_ble_get_bond_device_list(&dev_num, dev_list);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to get BLE bond list: %s", esp_err_to_name(err));
            return;
        }
    }

    taskENTER_CRITICAL(&s_state_lock);
    std::memset(s_ble_peers, 0, sizeof(s_ble_peers));
    s_ble_peer_count = dev_num;
    for (int i = 0; i < dev_num; ++i) {
        s_ble_peers[i].valid = true;
        s_ble_peers[i].last_rssi = -127;

        if ((dev_list[i].bond_key.key_mask & ESP_BLE_ID_KEY_MASK) != 0) {
            s_ble_peers[i].has_identity_key = true;
            s_ble_peers[i].identity_addr_type = dev_list[i].bond_key.pid_key.addr_type;
            std::memcpy(s_ble_peers[i].identity_addr, dev_list[i].bond_key.pid_key.static_addr, ESP_BD_ADDR_LEN);
            for (size_t j = 0; j < sizeof(s_ble_peers[i].irk); ++j) {
                s_ble_peers[i].irk[j] = dev_list[i].bond_key.pid_key.irk[sizeof(s_ble_peers[i].irk) - 1 - j];
            }
        } else {
            s_ble_peers[i].identity_addr_type = dev_list[i].bd_addr_type;
            std::memcpy(s_ble_peers[i].identity_addr, dev_list[i].bd_addr, ESP_BD_ADDR_LEN);
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(kTag, "BLE bonded peers: %d", dev_num);
    for (int i = 0; i < dev_num; ++i) {
        char addr_str[18] = {};
        ESP_LOGI(
            kTag,
            "  BLE[%d] %s addr_type=%u id_key=%s",
            i,
            bda_to_str(s_ble_peers[i].identity_addr, addr_str, sizeof(addr_str)),
            s_ble_peers[i].identity_addr_type,
            s_ble_peers[i].has_identity_key ? "yes" : "no");
    }
}

void refresh_classic_bond_cache()
{
    int dev_num = esp_bt_gap_get_bond_device_num();
    if (dev_num > kMaxClassicBondedDevices) {
        dev_num = kMaxClassicBondedDevices;
    }

    esp_bd_addr_t dev_list[kMaxClassicBondedDevices] = {};
    if (dev_num > 0) {
        esp_err_t err = esp_bt_gap_get_bond_device_list(&dev_num, dev_list);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to get classic bond list: %s", esp_err_to_name(err));
            return;
        }
    }

    taskENTER_CRITICAL(&s_state_lock);
    std::memset(s_classic_peers, 0, sizeof(s_classic_peers));
    s_classic_peer_count = dev_num;
    for (int i = 0; i < dev_num; ++i) {
        s_classic_peers[i].valid = true;
        std::memcpy(s_classic_peers[i].bda, dev_list[i], ESP_BD_ADDR_LEN);
    }
    taskEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(kTag, "Classic bonded peers: %d", dev_num);
    for (int i = 0; i < dev_num; ++i) {
        char addr_str[18] = {};
        ESP_LOGI(kTag, "  BR/EDR[%d] %s", i, bda_to_str(s_classic_peers[i].bda, addr_str, sizeof(addr_str)));
    }
}

void configure_ble_advertising()
{
    s_ble_adv_config_mask = 0x03;

    esp_err_t err = esp_ble_gap_config_adv_data_raw(const_cast<uint8_t *>(s_ble_adv_raw_data.data()), s_ble_adv_raw_data.size());
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to configure BLE raw advertising data: %s", esp_err_to_name(err));
        return;
    }

    err = esp_ble_gap_config_scan_rsp_data_raw(const_cast<uint8_t *>(s_ble_scan_rsp_raw_data.data()), s_ble_scan_rsp_raw_data.size());
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to configure BLE raw scan response data: %s", esp_err_to_name(err));
    }
}

void request_ble_scan_mode()
{
    if (s_ble_scanning.load()) {
        return;
    }

    s_ble_scan_requested.store(true);

    if (!s_ble_local_privacy_ready.load()) {
        return;
    }

    if (s_ble_advertising.load()) {
        esp_err_t err = esp_ble_gap_stop_advertising();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "Failed to stop BLE advertising: %s", esp_err_to_name(err));
        }
        return;
    }

    esp_err_t err = esp_ble_gap_set_scan_params(&s_ble_scan_params);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Failed to set BLE scan params: %s", esp_err_to_name(err));
    }
}

void close_pairing_window()
{
    if (!s_pairing_mode.exchange(false)) {
        return;
    }

    ESP_LOGI(kTag, "Initial 30s pairing window ended. Switching to scan/probe mode");

    esp_err_t classic_err = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (classic_err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to close classic discoverability: %s", esp_err_to_name(classic_err));
    }

    esp_err_t ble_err = esp_ble_gap_stop_advertising();
    if (ble_err != ESP_OK && ble_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Failed to stop BLE advertising: %s", esp_err_to_name(ble_err));
    }

    request_ble_scan_mode();
}

void update_ble_presence(const esp_ble_gap_cb_param_t::ble_scan_result_evt_param &scan_rst)
{
    int matched_index = -1;

    taskENTER_CRITICAL(&s_state_lock);
    for (int i = 0; i < s_ble_peer_count; ++i) {
        if (!s_ble_peers[i].valid) {
            continue;
        }

        bool matched = std::memcmp(scan_rst.bda, s_ble_peers[i].identity_addr, ESP_BD_ADDR_LEN) == 0;
        if (!matched && s_ble_peers[i].has_identity_key) {
            matched = resolve_rpa_with_irk(scan_rst.bda, s_ble_peers[i].irk);
        }

        if (matched) {
            matched_index = i;
            s_ble_peers[i].last_seen_tick = xTaskGetTickCount();
            s_ble_peers[i].last_rssi = static_cast<int8_t>(scan_rst.rssi);
            std::memcpy(s_ble_peers[i].last_adv_addr, scan_rst.bda, ESP_BD_ADDR_LEN);
            break;
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (matched_index >= 0) {
        char identity_str[18] = {};
        char adv_str[18] = {};
        uint8_t adv_name_len = 0;
        uint8_t *adv_name = esp_ble_resolve_adv_data_by_type(
            const_cast<uint8_t *>(scan_rst.ble_adv),
            scan_rst.adv_data_len + scan_rst.scan_rsp_len,
            ESP_BLE_AD_TYPE_NAME_CMPL,
            &adv_name_len);
        if (adv_name == nullptr) {
            adv_name = esp_ble_resolve_adv_data_by_type(
                const_cast<uint8_t *>(scan_rst.ble_adv),
                scan_rst.adv_data_len + scan_rst.scan_rsp_len,
                ESP_BLE_AD_TYPE_NAME_SHORT,
                &adv_name_len);
        }

        ESP_LOGI(
            kTag,
            "BLE present peer[%d] id=%s adv=%s addr_type=%u rssi=%d name=%.*s",
            matched_index,
            bda_to_str(s_ble_peers[matched_index].identity_addr, identity_str, sizeof(identity_str)),
            bda_to_str(scan_rst.bda, adv_str, sizeof(adv_str)),
            scan_rst.ble_addr_type,
            scan_rst.rssi,
            adv_name_len,
            adv_name != nullptr ? reinterpret_cast<const char *>(adv_name) : "");
    }
}

void maybe_start_classic_probe()
{
    if (s_pairing_mode.load() || s_classic_probe_in_flight.load()) {
        return;
    }

    ClassicPeer peers[kMaxClassicBondedDevices] = {};
    int peer_count = 0;

    taskENTER_CRITICAL(&s_state_lock);
    peer_count = s_classic_peer_count;
    if (peer_count > kMaxClassicBondedDevices) {
        peer_count = kMaxClassicBondedDevices;
    }
    std::memcpy(peers, s_classic_peers, sizeof(peers));
    taskEXIT_CRITICAL(&s_state_lock);

    if (peer_count <= 0) {
        return;
    }

    int index = s_next_classic_probe_index.fetch_add(1);
    if (index < 0) {
        index = 0;
        s_next_classic_probe_index.store(1);
    }
    index %= peer_count;

    esp_err_t err = esp_bt_gap_read_remote_name(peers[index].bda);
    if (err == ESP_OK) {
        s_classic_probe_in_flight.store(true);
    } else {
        ESP_LOGW(kTag, "Classic probe start failed: %s", esp_err_to_name(err));
    }
}

void update_classic_presence(const uint8_t *bda, const uint8_t *name)
{
    taskENTER_CRITICAL(&s_state_lock);
    for (int i = 0; i < s_classic_peer_count; ++i) {
        if (!s_classic_peers[i].valid) {
            continue;
        }

        if (std::memcmp(bda, s_classic_peers[i].bda, ESP_BD_ADDR_LEN) == 0) {
            s_classic_peers[i].last_seen_tick = xTaskGetTickCount();
            if (name != nullptr) {
                std::snprintf(
                    s_classic_peers[i].last_name,
                    sizeof(s_classic_peers[i].last_name),
                    "%s",
                    reinterpret_cast<const char *>(name));
            }
            break;
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

void presence_task(void *arg)
{
    TickType_t last_pairing_log = 0;
    TickType_t last_classic_probe = 0;

    while (true) {
        TickType_t now = xTaskGetTickCount();

        if (s_pairing_mode.load()) {
            if (now >= s_pairing_deadline) {
                close_pairing_window();
            } else if (now - last_pairing_log >= kPairingLogInterval) {
                last_pairing_log = now;
                uint32_t remaining_ms = (s_pairing_deadline - now) * portTICK_PERIOD_MS;
                ESP_LOGI(
                    kTag,
                    "Pairing mode active for %u ms more. BLE='%s', Classic='%s'",
                    remaining_ms,
                    kBleDeviceName,
                    kClassicDeviceName);
            }
        } else {
            if (now - last_classic_probe >= kClassicProbeRetryDelay) {
                last_classic_probe = now;
                maybe_start_classic_probe();
            }
        }

        vTaskDelay(kLoopDelay);
    }
}

void classic_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char addr_str[18] = {};

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(
                kTag,
                "Classic auth success: %s [%s]",
                reinterpret_cast<const char *>(param->auth_cmpl.device_name),
                bda_to_str(param->auth_cmpl.bda, addr_str, sizeof(addr_str)));
            refresh_classic_bond_cache();
        } else {
            ESP_LOGE(kTag, "Classic auth failed: status=%d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
        ESP_LOGI(kTag, "Classic PIN requested by %s, replying with 1234", bda_to_str(param->pin_req.bda, addr_str, sizeof(addr_str)));
        ESP_ERROR_CHECK(esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code));
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(kTag, "Classic SSP confirm for %s, value=%06" PRIu32 " -> auto-accept",
                 bda_to_str(param->cfm_req.bda, addr_str, sizeof(addr_str)),
                 param->cfm_req.num_val);
        ESP_ERROR_CHECK(esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true));
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(kTag, "Classic SSP passkey for %s: %06" PRIu32,
                 bda_to_str(param->key_notif.bda, addr_str, sizeof(addr_str)),
                 param->key_notif.passkey);
        break;

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        s_classic_probe_in_flight.store(false);
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            update_classic_presence(param->read_rmt_name.bda, param->read_rmt_name.rmt_name);
            ESP_LOGI(
                kTag,
                "Classic present: %s name=%s",
                bda_to_str(param->read_rmt_name.bda, addr_str, sizeof(addr_str)),
                reinterpret_cast<const char *>(param->read_rmt_name.rmt_name));
        }
        break;

    default:
        break;
    }
}

void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    char addr_str[18] = {};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status != ESP_SPP_SUCCESS) {
            ESP_LOGE(kTag, "SPP init failed: %d", param->init.status);
            break;
        }
        ESP_LOGI(kTag, "SPP initialized, starting SPP server");
        ESP_ERROR_CHECK(esp_spp_start_srv(kSppSecMask, kSppRole, 0, kClassicServerName));
        break;

    case ESP_SPP_START_EVT:
        if (param->start.status != ESP_SPP_SUCCESS) {
            ESP_LOGE(kTag, "SPP server start failed: %d", param->start.status);
            break;
        }
        ESP_LOGI(kTag, "SPP server started, handle=%" PRIu32, param->start.handle);
        ESP_ERROR_CHECK(esp_bt_gap_set_device_name(kClassicDeviceName));
        if (s_pairing_mode.load()) {
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
        }
        break;

    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGI(
            kTag,
            "SPP connection opened: %s handle=%" PRIu32,
            bda_to_str(param->srv_open.rem_bda, addr_str, sizeof(addr_str)),
            param->srv_open.handle);
        break;

    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(kTag, "SPP connection closed: status=%d handle=%" PRIu32, param->close.status, param->close.handle);
        break;

    default:
        break;
    }
}

void ble_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        s_ble_adv_config_mask &= static_cast<uint8_t>(~0x01U);
        if (s_ble_adv_config_mask == 0 && s_pairing_mode.load()) {
            ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&s_ble_adv_params));
        }
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        s_ble_adv_config_mask &= static_cast<uint8_t>(~0x02U);
        if (s_ble_adv_config_mask == 0 && s_pairing_mode.load()) {
            ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&s_ble_adv_params));
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_ble_advertising.store(true);
            ESP_LOGI(kTag, "BLE advertising started for '%s'", kBleDeviceName);
        } else {
            ESP_LOGE(kTag, "BLE advertising start failed: status=%u", param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_ble_advertising.store(false);
        ESP_LOGI(kTag, "BLE advertising stopped");
        if (s_ble_scan_requested.load()) {
            request_ble_scan_mode();
        }
        break;

    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
        if (param->local_privacy_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(kTag, "BLE local privacy config failed: status=%u", param->local_privacy_cmpl.status);
            break;
        }
        s_ble_local_privacy_ready.store(true);
        ESP_LOGI(kTag, "BLE local privacy configured");
        if (s_pairing_mode.load()) {
            configure_ble_advertising();
        } else {
            request_ble_scan_mode();
        }
        break;

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        ESP_ERROR_CHECK(esp_ble_gap_start_scanning(0));
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_ble_scanning.store(true);
            ESP_LOGI(kTag, "BLE continuous scan started");
        } else {
            ESP_LOGE(kTag, "BLE scan start failed: status=%u", param->scan_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_ble_scanning.store(false);
        ESP_LOGI(kTag, "BLE scan stopped");
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            update_ble_presence(param->scan_rst);
        }
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_ERROR_CHECK(esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true));
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        ESP_LOGI(kTag, "BLE passkey requested -> replying with %06" PRIu32, kBleFixedPasskey);
        ESP_ERROR_CHECK(esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, kBleFixedPasskey));
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        ESP_LOGI(kTag, "BLE numeric comparison request value=%06" PRIu32 " -> auto-accept", param->ble_security.key_notif.passkey);
        ESP_ERROR_CHECK(esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true));
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(kTag, "BLE passkey notify: %06" PRIu32, param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_KEY_EVT:
        ESP_LOGI(kTag, "BLE key exchanged, type=%u", param->ble_security.ble_key.key_type);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        char addr_str[18] = {};
        ESP_LOGI(
            kTag,
            "BLE auth complete: success=%s addr=%s addr_type=%u",
            param->ble_security.auth_cmpl.success ? "yes" : "no",
            bda_to_str(param->ble_security.auth_cmpl.bd_addr, addr_str, sizeof(addr_str)),
            param->ble_security.auth_cmpl.addr_type);
        if (!param->ble_security.auth_cmpl.success) {
            ESP_LOGI(kTag, "BLE pairing failed, reason=0x%x", param->ble_security.auth_cmpl.fail_reason);
        }
        refresh_ble_bond_cache();
        break;
    }

    default:
        break;
    }
}

void ble_gatts_callback(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(kTag, "BLE GATTS registered, app_id=%u", param->reg.app_id);
        ESP_ERROR_CHECK(esp_ble_gap_set_device_name(kBleDeviceName));
        ESP_ERROR_CHECK(esp_ble_gap_config_local_privacy(true));
        ESP_ERROR_CHECK(esp_ble_gatts_create_attr_tab(s_device_info_gatt_db, gatts_if, kDisCount, 0));
        ESP_ERROR_CHECK(esp_ble_gatts_create_attr_tab(s_battery_gatt_db, gatts_if, kBasCount, 1));
        ESP_ERROR_CHECK(esp_ble_gatts_create_attr_tab(s_heart_rate_gatt_db, gatts_if, kHrsCount, 2));
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(kTag, "BLE attr table create failed: status=0x%x", param->add_attr_tab.status);
            break;
        }

        if (param->add_attr_tab.svc_uuid.uuid.uuid16 == kDeviceInfoServiceUuid) {
            if (param->add_attr_tab.num_handle != kDisCount) {
                ESP_LOGE(kTag, "Device Info attr table handle count mismatch: %u", param->add_attr_tab.num_handle);
                break;
            }
            std::memcpy(s_dis_handle_table, param->add_attr_tab.handles, sizeof(s_dis_handle_table));
            ESP_ERROR_CHECK(esp_ble_gatts_start_service(s_dis_handle_table[kDisSvc]));
        } else if (param->add_attr_tab.svc_uuid.uuid.uuid16 == kBatteryServiceUuid) {
            if (param->add_attr_tab.num_handle != kBasCount) {
                ESP_LOGE(kTag, "Battery attr table handle count mismatch: %u", param->add_attr_tab.num_handle);
                break;
            }
            std::memcpy(s_bas_handle_table, param->add_attr_tab.handles, sizeof(s_bas_handle_table));
            ESP_ERROR_CHECK(esp_ble_gatts_start_service(s_bas_handle_table[kBasSvc]));
        } else if (param->add_attr_tab.svc_uuid.uuid.uuid16 == kHeartRateServiceUuid) {
            if (param->add_attr_tab.num_handle != kHrsCount) {
                ESP_LOGE(kTag, "Heart Rate attr table handle count mismatch: %u", param->add_attr_tab.num_handle);
                break;
            }
            std::memcpy(s_hrs_handle_table, param->add_attr_tab.handles, sizeof(s_hrs_handle_table));
            ESP_ERROR_CHECK(esp_ble_gatts_start_service(s_hrs_handle_table[kHrsSvc]));
        } else {
            ESP_LOGW(kTag, "Unexpected BLE service table uuid=0x%04x", param->add_attr_tab.svc_uuid.uuid.uuid16);
        }
        break;

    case ESP_GATTS_CONNECT_EVT: {
        char addr_str[18] = {};
        ESP_LOGI(kTag, "BLE connected: %s", bda_to_str(param->connect.remote_bda, addr_str, sizeof(addr_str)));
        ESP_ERROR_CHECK(esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT));
        break;
    }

    case ESP_GATTS_DISCONNECT_EVT: {
        char addr_str[18] = {};
        ESP_LOGI(kTag, "BLE disconnected: %s reason=0x%x",
                 bda_to_str(param->disconnect.remote_bda, addr_str, sizeof(addr_str)),
                 param->disconnect.reason);
        if (s_pairing_mode.load()) {
            ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&s_ble_adv_params));
        }
        break;
    }

    default:
        break;
    }
}

}  // namespace

esp_err_t bt_presence_poc_start()
{
    s_pairing_mode.store(true);
    s_pairing_deadline = xTaskGetTickCount() + kPairingWindow;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_RETURN_ON_ERROR(esp_bt_gap_register_callback(classic_gap_callback), kTag, "classic GAP register failed");
    ESP_RETURN_ON_ERROR(esp_spp_register_callback(spp_callback), kTag, "SPP register failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(ble_gap_callback), kTag, "BLE GAP register failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(ble_gatts_callback), kTag, "BLE GATTS register failed");

    esp_spp_cfg_t spp_cfg = {
        .mode = kSppMode,
        .enable_l2cap_ertm = kSppEnableL2capErtm,
        .tx_buffer_size = 0,
    };
    ESP_RETURN_ON_ERROR(esp_spp_enhanced_init(&spp_cfg), kTag, "SPP init failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_app_register(0x55), kTag, "BLE GATTS app register failed");

    esp_bt_sp_param_t classic_param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t classic_iocap = ESP_BT_IO_CAP_NONE;
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_security_param(classic_param_type, &classic_iocap, sizeof(classic_iocap)), kTag, "classic SSP param failed");

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {};
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_pin(pin_type, 0, pin_code), kTag, "classic pin config failed");
    ESP_RETURN_ON_ERROR(esp_bt_gap_set_page_timeout(kClassicPageTimeout), kTag, "classic page timeout failed");

    esp_ble_auth_req_t ble_auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t ble_iocap = ESP_IO_CAP_KBDISP;
    uint8_t ble_key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;

    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &ble_auth_req, sizeof(ble_auth_req)), kTag, "BLE auth req config failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &ble_iocap, sizeof(ble_iocap)), kTag, "BLE IOCAP config failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &ble_key_size, sizeof(ble_key_size)), kTag, "BLE key size config failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(oob_support)), kTag, "BLE OOB config failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)), kTag, "BLE init key config failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)), kTag, "BLE rsp key config failed");

    refresh_ble_bond_cache();
    refresh_classic_bond_cache();

    char addr_str[18] = {};
    ESP_LOGI(kTag, "Local BT address: %s", bda_to_str(esp_bt_dev_get_address(), addr_str, sizeof(addr_str)));
    ESP_LOGI(kTag, "Starting dual-mode presence PoC: BLE Heart Rate + Classic SPP");
    ESP_LOGI(kTag, "BLE pairing persona: '%s', fixed passkey=%06" PRIu32 ", adv flags=0x%02x", kBleDeviceName, kBleFixedPasskey, kBleAdvFlags);

    BaseType_t task_ok = xTaskCreate(
        presence_task,
        "bt_presence_poc",
        6144,
        nullptr,
        5,
        &s_presence_task_handle);
    return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
