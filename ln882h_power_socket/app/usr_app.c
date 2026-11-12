#include "osal/osal.h"
#include "utils/debug/log.h"
#include "wifi.h"
#include "wifi_port.h"
#include "netif/ethernetif.h"
#include "wifi_manager.h"
#include "lwip/tcpip.h"
#include "drv_adc_measure.h"
#include "utils/debug/ln_assert.h"
#include "utils/system_parameter.h"
#include "utils/sysparam_factory_setting.h"
#include "utils/ln_psk_calc.h"
#include "utils/power_mgmt/ln_pm.h"
#include "hal/hal_adc.h"
#include "ln_nvds.h"
#include "ln_wifi_err.h"
#include "ln_misc.h"
#include "ln882h.h"
#include "hal/hal_flash.h"
#include "usr_app.h"
#include "config.h"
#include "bl0937.h"
#include "https_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PM_DEFAULT_SLEEP_MODE             (ACTIVE)
#define PM_WIFI_DEFAULT_PS_MODE           (WIFI_NO_POWERSAVE)
#define WIFI_TEMP_CALIBRATE               (1)

#define WIFI_USER_USE_WPA3                (1) /* 0: Not use; 1: Use */

#define POWER_TASK_STACK_SIZE             (2048)
#define NET_TASK_STACK_SIZE               (16384)

#define NET_RECONNECT_INTERVAL_MS         (30000u)
#define NET_POST_RETRY_COUNT              (3)
#define NET_POST_RETRY_DELAY_MS           (10000u)

#if (LWIP_DHCP == 0)
    #define LOCAL_STATIC_IP_ADDR "192.168.1.123"
    #define LOCAL_STATIC_GW_ADDR "192.168.1.1"
    #define LOCAL_STATIC_NM_ADDR "255.255.255.0"
#endif

#if WIFI_TEMP_CALIBRATE
static OS_Thread_t g_temp_cal_thread;
#define TEMP_APP_TASK_STACK_SIZE          (4*256) //Byte
#endif

static OS_Thread_t g_power_thread;
static OS_Thread_t g_net_thread;

/* declaration */
static void wifi_init_sta(void);
static void power_task_entry(void *params);
static void net_task_entry(void *params);
static void temp_cal_app_task_entry(void *params);

static uint8_t mac_addr[6]        = {0x00, 0x50, 0xC2, 0x5E, 0x88, 0x99};
static uint8_t psk_value[40]      = {0x0};

bool g_STA_static_IP = false;

void vApplicationIdleHook(void);
void vApplicationIdleHook(void)
{
}

wifi_sta_connect_t connect = {
    .ssid    = POWER_SOCKET_WIFI_SSID,
    .pwd     = POWER_SOCKET_WIFI_PASSWORD,
    .bssid   = NULL,
    .psk_value = NULL,
};

wifi_scan_cfg_t scan_cfg = {
        .channel   = 0,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = 20,
};

static uint32_t djb_hash_hexdata(const char *input, uint32_t len)
{
    uint32_t hash = 5381;

    for (uint32_t i = 0; i < len; i++)
    {
        int c = input[i];
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

static void chip_uuid_generate_mac(uint8_t mac[6])
{
    uint8_t chip_uid[16] = {0};
    uint32_t hash = 0;
    hal_flash_read_unique_id(chip_uid);
    hash = djb_hash_hexdata((const char *)chip_uid, sizeof(chip_uid));
    mac[0] = 0x00;
    mac[1] = 0x50;
    mac[2] = (uint8_t)((hash) & 0xFF);
    mac[3] = (uint8_t)((hash >> 8) & 0xFF);
    mac[4] = (uint8_t)((hash >> 16) & 0xFF);
    mac[5] = (uint8_t)((hash >> 24) & 0xFF);
}

static void wifi_scan_complete_cb(void * arg)
{
    LN_UNUSED(arg);

    ln_list_t *list;
    uint8_t node_count = 0;
    ap_info_node_t *pnode;

    wifi_manager_ap_list_update_enable(LN_FALSE);

    // 1.get ap info list.
    wifi_manager_get_ap_list(&list, &node_count);

    // 2.print all ap info in the list.
    LN_LIST_FOR_EACH_ENTRY(pnode, ap_info_node_t, list,list)
    {
        uint8_t * mac = (uint8_t*)pnode->info.bssid;
        ap_info_t *ap_info = &pnode->info;

        LOG(LOG_LVL_INFO, "\tCH=%2d,RSSI= %3d,", ap_info->channel, ap_info->rssi);
        LOG(LOG_LVL_INFO, "BSSID:[%02X:%02X:%02X:%02X:%02X:%02X],SSID:\"%s\"\r\n", \
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ap_info->ssid);
    }

    wifi_manager_ap_list_update_enable(LN_TRUE);
}

static void wifi_connect_failed_cb(void * arg)
{
    wifi_sta_connect_failed_reason_t reason = *(wifi_sta_connect_failed_reason_t*)arg;
    LOG(LOG_LVL_INFO, "############ wifi_connect_failed!, reason = %d, please retry. ############\r\n", reason);
}

static void wifi_sta_apply_psk(void)
{
    connect.psk_value = NULL;
    if (strlen(connect.pwd) != 0) {
        if (0 == ln_psk_calc(connect.ssid, connect.pwd, psk_value, sizeof (psk_value))) {
            connect.psk_value = psk_value;
            hexdump(LOG_LVL_INFO, "psk value ", psk_value, sizeof(psk_value));
        }
    }
}

static void wifi_init_sta(void)
{
    sta_ps_mode_t ps_mode = PM_WIFI_DEFAULT_PS_MODE;

    //1. generate mac
    //ln_generate_random_mac(mac_addr);
    chip_uuid_generate_mac(mac_addr);
    sysparam_sta_mac_update((const uint8_t *)mac_addr);

    //2. net device(lwip)
    netdev_set_mac_addr(NETIF_IDX_STA, mac_addr);
    netdev_set_active(NETIF_IDX_STA);
    sysparam_sta_mac_update((const uint8_t *)mac_addr);

    // static ip address
    {
        #if (LWIP_DHCP == 0)
            tcpip_ip_info_t  ip_info;
            ip_info.ip.addr      = ipaddr_addr((const char *)LOCAL_STATIC_IP_ADDR);
            ip_info.gw.addr      = ipaddr_addr((const char *)LOCAL_STATIC_GW_ADDR);
            ip_info.netmask.addr = ipaddr_addr((const char *)LOCAL_STATIC_NM_ADDR);
            netdev_set_ip_info(NETIF_IDX_STA, &ip_info);
        #endif
    }

    //3. wifi start
    wifi_manager_reg_event_callback(WIFI_MGR_EVENT_STA_SCAN_COMPLETE, &wifi_scan_complete_cb);
    wifi_manager_reg_event_callback(WIFI_MGR_EVENT_STA_CONNECT_FAILED, &wifi_connect_failed_cb);
#if (WIFI_USER_USE_WPA3 == 1)
    extern void ln_wpa_sae_enable(void);
    ln_wpa_sae_enable();
#endif
    if(WIFI_ERR_NONE != wifi_sta_start(mac_addr, ps_mode)){
        LOG(LOG_LVL_ERROR, "[%s]wifi sta start filed!!!\r\n", __func__);
    }

    wifi_sta_apply_psk();
    wifi_sta_connect_v2(&connect, &scan_cfg, 30);
}

static bool net_post_reading(void)
{
    bl0937_reading_t r;
    char json[160];
    int len;
    int attempt;

    bl0937_get(&r);
    len = snprintf(json, sizeof(json),
        "{\"voltage\":%.2f,\"current\":%.3f,\"power\":%.2f,\"energy_wh\":%.3f,\"ts\":%lu}",
        (double)r.voltage, (double)r.current, (double)r.power,
        (double)r.energy_wh, (unsigned long)r.uptime_s);
    if ((len <= 0) || ((size_t)len >= sizeof(json))) {
        LOG(LOG_LVL_ERROR, "[net] json overflow\r\n");
        return false;
    }

    for (attempt = 0; attempt < NET_POST_RETRY_COUNT; attempt++) {
        if (https_post_json(POWER_SOCKET_POST_URL, json) == 0) {
            LOG(LOG_LVL_INFO, "[net] post ok\r\n");
            return true;
        }
        LOG(LOG_LVL_ERROR, "[net] post failed (%d/%d)\r\n", attempt + 1, NET_POST_RETRY_COUNT);
        if ((attempt + 1) < NET_POST_RETRY_COUNT) {
            OS_MsDelay(NET_POST_RETRY_DELAY_MS);
        }
    }

    return false;
}

static void power_task_entry(void *params)
{
    LN_UNUSED(params);

    bl0937_init();
    uint32_t last_ms = OS_TicksToMSecs(OS_GetTicks());

    while (1) {
        OS_MsDelay(1000);

        uint32_t now_ms = OS_TicksToMSecs(OS_GetTicks());
        uint32_t dt_ms = now_ms - last_ms;
        last_ms = now_ms;

        bl0937_tick((float)dt_ms);
    }
}

static void net_task_entry(void *params)
{
    LN_UNUSED(params);

    wifi_manager_init();
    wifi_init_sta();

    uint32_t last_reconnect_ms = OS_TicksToMSecs(OS_GetTicks());
    uint32_t next_post_ms = 0;
    bool have_ip = false;

    while (1) {
        uint32_t now_ms = OS_TicksToMSecs(OS_GetTicks());

        if (!netdev_got_ip()) {
            if (have_ip) {
                LOG(LOG_LVL_ERROR, "[net] ip lost\r\n");
                have_ip = false;
            }

            if ((uint32_t)(now_ms - last_reconnect_ms) >= NET_RECONNECT_INTERVAL_MS) {
                last_reconnect_ms = now_ms;
                LOG(LOG_LVL_INFO, "[net] reconnect wifi\r\n");
                wifi_sta_disconnect();
                wifi_sta_apply_psk();
                wifi_sta_connect_v2(&connect, &scan_cfg, 30);
            }

            OS_MsDelay(1000);
            continue;
        }

        if (!have_ip) {
            have_ip = true;
            next_post_ms = now_ms;
            LOG(LOG_LVL_INFO, "[net] got ip\r\n");
        }

        if ((int32_t)(now_ms - next_post_ms) >= 0) {
            if (net_post_reading()) {
                bl0937_save_energy();
            }
            now_ms = OS_TicksToMSecs(OS_GetTicks());
            next_post_ms = now_ms + (uint32_t)POWER_SOCKET_POST_INTERVAL_SEC * 1000u;
        } else {
            OS_MsDelay(1000);
        }
    }
}

static void temp_cal_app_task_entry(void *params)
{
    LN_UNUSED(params);
    uint8_t cnt = 0;
    int8_t cap_comp = 0;
    uint16_t adc_val = 0;
    int16_t curr_adc = 0;

    if (NVDS_ERR_OK == ln_nvds_get_xtal_comp_val((uint8_t *)&cap_comp)) {
        if ((uint8_t)cap_comp == 0xFF) {
            cap_comp = 0;
        }
    }

    drv_adc_init();

    wifi_temp_cal_init(drv_adc_read(ADC_CH0), cap_comp);

    while (1)
    {
        OS_MsDelay(1000);

        adc_val = drv_adc_read(ADC_CH0);
        wifi_do_temp_cal_period(adc_val);

        curr_adc = (adc_val & 0xFFF);

        cnt++;
        if ((cnt % 60) == 0) {
            LOG(LOG_LVL_INFO, "adc raw: %4d, temp_IC: %4d\r\n",
                    curr_adc, (int16_t)(25 + (curr_adc - 770) / 2.54f));
            LOG(LOG_LVL_INFO, "Total:%d; Free:%ld;\r\n", 
                    OS_HeapSizeGet(), (long)OS_GetFreeHeapSize());
        }
    }
}

void creat_usr_app_task(void)
{
    {
        ln_pm_sleep_mode_set(PM_DEFAULT_SLEEP_MODE);

        /**
         * CLK_G_EFUSE: For wifi temp calibration
         * CLK_G_BLE  CLK_G_I2S  CLK_G_WS2811  CLK_G_DBGH  CLK_G_SDIO  CLK_G_EFUSE  CLK_G_AES
        */
        ln_pm_always_clk_disable_select(CLK_G_I2S | CLK_G_WS2811 | CLK_G_SDIO | CLK_G_AES);

        /**
         * ADC0: For wifi temp calibration
         * TIM3: For wifi pvtcmd evm test
         * CLK_G_ADC  CLK_G_GPIOA  CLK_G_GPIOB  CLK_G_SPI0  CLK_G_SPI1  CLK_G_I2C0  CLK_G_UART1  CLK_G_UART2
         * CLK_G_WDT  CLK_G_TIM_REG  CLK_G_TIM1  CLK_G_TIM2  CLK_G_TIM3  CLK_G_TIM4  CLK_G_MAC  CLK_G_DMA
         * CLK_G_RF  CLK_G_ADV_TIMER  CLK_G_TRNG
        */
        ln_pm_lightsleep_clk_disable_select(CLK_G_GPIOA | CLK_G_GPIOB | CLK_G_SPI0 | CLK_G_SPI1 | CLK_G_I2C0 |
                                            CLK_G_UART1 | CLK_G_UART2 | CLK_G_WDT | CLK_G_TIM1 | CLK_G_TIM2 | CLK_G_MAC | CLK_G_DMA | CLK_G_RF | CLK_G_ADV_TIMER| CLK_G_TRNG);
    }

    if(OS_OK != OS_ThreadCreate(&g_power_thread, "PowerAPP", power_task_entry, NULL, OS_PRIORITY_BELOW_NORMAL, POWER_TASK_STACK_SIZE)) {
        LN_ASSERT(1);
    }

    if(OS_OK != OS_ThreadCreate(&g_net_thread, "NetAPP", net_task_entry, NULL, OS_PRIORITY_BELOW_NORMAL, NET_TASK_STACK_SIZE)) {
        LN_ASSERT(1);
    }

#if  WIFI_TEMP_CALIBRATE
    if(OS_OK != OS_ThreadCreate(&g_temp_cal_thread, "TempAPP", temp_cal_app_task_entry, NULL, OS_PRIORITY_BELOW_NORMAL, TEMP_APP_TASK_STACK_SIZE)) {
        LN_ASSERT(1);
    }
#endif

    /* print sdk version */
    {
        LOG(LOG_LVL_INFO, "LN882H SDK Ver: %s [build time:%s][0x%08x]\r\n",
                LN882H_SDK_VERSION_STRING, LN882H_SDK_BUILD_DATE_TIME, (unsigned int)LN882H_SDK_VERSION);
    }
}
