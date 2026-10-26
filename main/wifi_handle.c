#include "sdkconfig.h"

#include <string.h>
#include <stdint.h>
#include <sys/param.h>

#include "main/wifi_configuration.h"
#include "main/uart_bridge.h"

#include "components/DAP/include/gpio_op.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_log.h"

#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"

#ifdef CONFIG_IDF_TARGET_ESP8266
    #include "esp_event_loop.h"
    #define WIFI_AP_IF ESP_IF_WIFI_AP
#else
    #include "esp_event.h"
    #include "esp_netif.h"
    #define WIFI_AP_IF WIFI_IF_AP
#endif

#ifdef CONFIG_IDF_TARGET_ESP8266
    #define PIN_LED_WIFI_STATUS 15
#elif defined CONFIG_IDF_TARGET_ESP32
    #define PIN_LED_WIFI_STATUS 27
#elif defined CONFIG_IDF_TARGET_ESP32C3
    #define PIN_LED_WIFI_STATUS 10
#elif defined CONFIG_IDF_TARGET_ESP32S3
    #define PIN_LED_WIFI_STATUS 4
#else
    #error unknown hardware
#endif

#ifndef CONFIG_AP_SSID
#define CONFIG_AP_SSID          "DAP_ESP32"
#endif

#ifndef CONFIG_AP_PASSWORD
#define CONFIG_AP_PASSWORD      "12345678"
#endif

#ifndef CONFIG_AP_CHANNEL
#define CONFIG_AP_CHANNEL       1
#endif

#ifndef CONFIG_AP_MAX_CONN
#define CONFIG_AP_MAX_CONN      4
#endif

static EventGroupHandle_t wifi_event_group;
const int IPV4_GOTIP_BIT = BIT0;

#ifdef CONFIG_IDF_TARGET_ESP8266
static esp_err_t event_handler(void *ctx, system_event_t *event) {
    switch (event->event_id) {
    case SYSTEM_EVENT_AP_START:
        GPIO_SET_LEVEL_HIGH(PIN_LED_WIFI_STATUS);
        xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
        os_printf("SYSTEM EVENT AP START - SSID: %s\r\n", CONFIG_AP_SSID);
        break;
    case SYSTEM_EVENT_AP_STACONNECTED:
        os_printf("Station connected\r\n");
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        os_printf("Station disconnected\r\n");
#if (USE_UART_BRIDGE == 1)
        uart_bridge_close();
#endif
        break;
    default:
        break;
    }
    return ESP_OK;
}

#else /* CONFIG_IDF_TARGET_ESP8266 */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        GPIO_SET_LEVEL_HIGH(PIN_LED_WIFI_STATUS);
        xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
        os_printf("WiFi SoftAP started! SSID: %s\r\n", CONFIG_AP_SSID);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        os_printf("Station joined, AID=%d\r\n", event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        os_printf("Station left, AID=%d\r\n", event->aid);
#if (USE_UART_BRIDGE == 1)
        uart_bridge_close();
#endif
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
        char ip_str[16] = {0};
        esp_ip4addr_ntoa(&event->ip, ip_str, sizeof(ip_str));
        os_printf("Station assigned IP: %s\r\n", ip_str);
    }
}

#endif /* CONFIG_IDF_TARGET_ESP8266 */

static void wait_for_ip(void) {
    os_printf("Waiting for SoftAP to start...\r\n");
    xEventGroupWaitBits(wifi_event_group, IPV4_GOTIP_BIT, false, true, portMAX_DELAY);
    os_printf("SoftAP is active!\r\n");
}

void wifi_init(void) {
    GPIO_FUNCTION_SET(PIN_LED_WIFI_STATUS);
    GPIO_SET_DIRECTION_NORMAL_OUT(PIN_LED_WIFI_STATUS);

#ifdef CONFIG_IDF_TARGET_ESP8266
    tcpip_adapter_init();
#else
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
#endif

#if (USE_STATIC_IP == 1)
#ifdef CONFIG_IDF_TARGET_ESP8266
    tcpip_adapter_ip_info_t ip_info;
#define MY_IP4_ADDR(...) IP4_ADDR(__VA_ARGS__)
    MY_IP4_ADDR(&ip_info.ip, DAP_IP_ADDRESS);
    MY_IP4_ADDR(&ip_info.gw, DAP_IP_GATEWAY);
    MY_IP4_ADDR(&ip_info.netmask, DAP_IP_NETMASK);
#undef MY_IP4_ADDR
    tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
    tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);
    tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);
#else
    esp_netif_ip_info_t ip_info;
#define MY_IP4_ADDR(...) IP4_ADDR(__VA_ARGS__)
    MY_IP4_ADDR(&ip_info.ip, DAP_IP_ADDRESS);
    MY_IP4_ADDR(&ip_info.gw, DAP_IP_GATEWAY);
    MY_IP4_ADDR(&ip_info.netmask, DAP_IP_NETMASK);
#undef MY_IP4_ADDR
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(netif));
#endif
#endif // (USE_STATIC_IP == 1)

    wifi_event_group = xEventGroupCreate();

#ifdef CONFIG_IDF_TARGET_ESP8266
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
#else
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &ip_event_handler, NULL));
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = CONFIG_AP_SSID,
            .ssid_len = strlen(CONFIG_AP_SSID),
            .channel = CONFIG_AP_CHANNEL,
            .password = CONFIG_AP_PASSWORD,
            .max_connection = CONFIG_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    if (strlen(CONFIG_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_AP_IF, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    wait_for_ip();
}
