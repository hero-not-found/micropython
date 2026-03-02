/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 MicroPython contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/objlist.h"
#include "py/mperrno.h"

#if MICROPY_HW_USBH_MODEM

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "iot_usbh_modem.h"
#include "iot_usbh_cdc.h"
#include "at_3gpp_ts_27_007.h"
#include "shared/netutils/netutils.h"

static const char *TAG = "usbh_modem";

// Track if CDC driver is installed
static bool cdc_driver_installed = false;

// State constants exposed to Python
typedef enum {
    USBH_MODEM_STATE_INACTIVE = 0,
    USBH_MODEM_STATE_INSTALLED,
    USBH_MODEM_STATE_CONNECTING,
    USBH_MODEM_STATE_CONNECTED,
    USBH_MODEM_STATE_ERROR,
} usbh_modem_state_t;

typedef struct _network_usbh_modem_obj_t {
    mp_obj_base_t base;
    bool installed;
    usb_modem_id_t *modem_id_list;
    size_t modem_count;
} network_usbh_modem_obj_t;

// Singleton instance
static network_usbh_modem_obj_t *usbh_modem_singleton = NULL;

// USB device event callback for debugging/enumeration
static void usb_device_event_cb(usbh_cdc_device_event_t event, usbh_cdc_device_event_data_t *event_data, void *user_ctx) {
    if (event == CDC_HOST_DEVICE_EVENT_CONNECTED) {
        const usb_device_desc_t *dev_desc = event_data->new_dev.device_desc;
        const usb_config_desc_t *cfg_desc = event_data->new_dev.active_config_desc;

        ESP_LOGI(TAG, "USB Device Connected:");
        ESP_LOGI(TAG, "  VID: 0x%04X, PID: 0x%04X", dev_desc->idVendor, dev_desc->idProduct);
        ESP_LOGI(TAG, "  Device Class: 0x%02X, SubClass: 0x%02X, Protocol: 0x%02X",
                 dev_desc->bDeviceClass, dev_desc->bDeviceSubClass, dev_desc->bDeviceProtocol);
        ESP_LOGI(TAG, "  Num Configurations: %d", dev_desc->bNumConfigurations);
        ESP_LOGI(TAG, "  Config Num Interfaces: %d", cfg_desc->bNumInterfaces);

        // Print interface details by parsing the configuration descriptor
        int offset = 0;
        const uint8_t *p = (const uint8_t *)cfg_desc;
        while (offset < cfg_desc->wTotalLength) {
            const usb_standard_desc_t *desc = (const usb_standard_desc_t *)(p + offset);
            if (desc->bLength == 0) break;

            if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
                const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
                ESP_LOGI(TAG, "  Interface %d: Class=0x%02X SubClass=0x%02X Protocol=0x%02X NumEP=%d",
                         intf->bInterfaceNumber, intf->bInterfaceClass,
                         intf->bInterfaceSubClass, intf->bInterfaceProtocol,
                         intf->bNumEndpoints);
            }
            offset += desc->bLength;
        }
    } else if (event == CDC_HOST_DEVICE_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "USB Device Disconnected (addr=%d)", event_data->dev_gone.dev_addr);
    }
}

// Install CDC driver if not already installed
static esp_err_t ensure_cdc_driver_installed(void) {
    if (cdc_driver_installed) {
        return ESP_OK;
    }

    usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_coreid = -1,  // Any core
        .skip_init_usb_host_driver = false,
    };

    esp_err_t err = usbh_cdc_driver_install(&cdc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install CDC driver: %s", esp_err_to_name(err));
        return err;
    }

    // Register callback to enumerate all USB devices (match any)
    err = usbh_cdc_register_dev_event_cb(ESP_USB_DEVICE_MATCH_ID_ANY, usb_device_event_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register device event callback: %s", esp_err_to_name(err));
        // Continue anyway, enumeration is just for debugging
    }

    cdc_driver_installed = true;
    ESP_LOGI(TAG, "CDC driver installed");
    return ESP_OK;
}

static void network_usbh_modem_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_netif_t *netif = usbh_modem_get_netif();

    mp_printf(print, "USBModem(installed=%s", self->installed ? "True" : "False");

    if (netif != NULL && esp_netif_is_netif_up(netif)) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            mp_printf(print, ", ip=" IPSTR, IP2STR(&ip_info.ip));
        }
    }
    mp_printf(print, ")");
}

// Helper to convert Python tuple to usb_modem_id_t
static void parse_modem_tuple(mp_obj_t tuple, usb_modem_id_t *modem_id) {
    mp_obj_t *items;
    size_t len;
    mp_obj_get_array(tuple, &len, &items);

    if (len < 4 || len > 5) {
        mp_raise_ValueError(MP_ERROR_TEXT("modem tuple must be (vid, pid, modem_itf, at_itf[, name])"));
    }

    modem_id->match_id.match_flags = USB_DEVICE_ID_MATCH_VID_PID;
    modem_id->match_id.idVendor = mp_obj_get_int(items[0]);
    modem_id->match_id.idProduct = mp_obj_get_int(items[1]);
    modem_id->modem_itf_num = mp_obj_get_int(items[2]);
    modem_id->at_itf_num = mp_obj_get_int(items[3]);

    if (len == 5 && items[4] != mp_const_none) {
        modem_id->name = mp_obj_str_get_str(items[4]);
    } else {
        modem_id->name = NULL;
    }
}

// Constructor: USBModem(modem_list)
static mp_obj_t network_usbh_modem_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    // Check if already installed
    if (usbh_modem_singleton != NULL && usbh_modem_singleton->installed) {
        mp_raise_OSError(MP_EALREADY);
    }

    // Parse modem list
    mp_obj_t *modem_items;
    size_t modem_count;
    mp_obj_get_array(args[0], &modem_count, &modem_items);

    if (modem_count == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("modem list cannot be empty"));
    }

    // Allocate object and modem ID list (+1 for null terminator)
    network_usbh_modem_obj_t *self = mp_obj_malloc_with_finaliser(network_usbh_modem_obj_t, type);
    self->modem_id_list = m_new0(usb_modem_id_t, modem_count + 1);
    self->modem_count = modem_count;
    self->installed = false;

    // Parse each modem definition
    for (size_t i = 0; i < modem_count; i++) {
        parse_modem_tuple(modem_items[i], &self->modem_id_list[i]);
        ESP_LOGI(TAG, "Modem[%d]: VID=0x%04x PID=0x%04x modem_itf=%d at_itf=%d name=%s",
                 (int)i,
                 self->modem_id_list[i].match_id.idVendor,
                 self->modem_id_list[i].match_id.idProduct,
                 self->modem_id_list[i].modem_itf_num,
                 self->modem_id_list[i].at_itf_num,
                 self->modem_id_list[i].name ? self->modem_id_list[i].name : "(none)");
    }

    // Null-terminate the list
    memset(&self->modem_id_list[modem_count], 0, sizeof(usb_modem_id_t));

    // Ensure CDC driver is installed first (required by modem component)
    esp_err_t err = ensure_cdc_driver_installed();
    if (err != ESP_OK) {
        m_del(usb_modem_id_t, self->modem_id_list, modem_count + 1);
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("CDC driver install failed: %s"), esp_err_to_name(err));
    }

    // Configure and install the USB modem
    usbh_modem_config_t config = {
        .modem_id_list = self->modem_id_list,
        .at_tx_buffer_size = 1024,
        .at_rx_buffer_size = 2048,
    };

    err = usbh_modem_install(&config);
    if (err != ESP_OK) {
        m_del(usb_modem_id_t, self->modem_id_list, modem_count + 1);
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("usbh_modem_install failed: %s"), esp_err_to_name(err));
    }

    self->installed = true;
    usbh_modem_singleton = self;

    ESP_LOGI(TAG, "USB modem installed successfully");
    return MP_OBJ_FROM_PTR(self);
}

// Destructor
static mp_obj_t network_usbh_modem___del__(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->installed) {
        esp_err_t err = usbh_modem_uninstall();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "usbh_modem_uninstall failed: %s", esp_err_to_name(err));
        }
        self->installed = false;

        if (self->modem_id_list != NULL) {
            m_del(usb_modem_id_t, self->modem_id_list, self->modem_count + 1);
            self->modem_id_list = NULL;
        }

        if (usbh_modem_singleton == self) {
            usbh_modem_singleton = NULL;
        }

        ESP_LOGI(TAG, "USB modem uninstalled");
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem___del___obj, network_usbh_modem___del__);

// deinit() - explicit cleanup
static mp_obj_t network_usbh_modem_deinit(mp_obj_t self_in) {
    return network_usbh_modem___del__(self_in);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_deinit_obj, network_usbh_modem_deinit);

// connect(timeout=30000) - Start PPP connection
static mp_obj_t network_usbh_modem_connect(size_t n_args, const mp_obj_t *args) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    // Default timeout 30 seconds
    TickType_t timeout_ms = 30000;
    if (n_args > 1) {
        timeout_ms = mp_obj_get_int(args[1]);
    }

    ESP_LOGI(TAG, "Starting PPP connection (timeout=%lu ms)", (unsigned long)timeout_ms);

    esp_err_t err = usbh_modem_ppp_start(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            mp_raise_OSError(MP_ETIMEDOUT);
        } else {
            mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("ppp_start failed: %s"), esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "PPP connected");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_usbh_modem_connect_obj, 1, 2, network_usbh_modem_connect);

// disconnect() - Stop PPP connection
static mp_obj_t network_usbh_modem_disconnect(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    ESP_LOGI(TAG, "Stopping PPP connection");

    esp_err_t err = usbh_modem_ppp_stop();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("ppp_stop failed: %s"), esp_err_to_name(err));
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_disconnect_obj, network_usbh_modem_disconnect);

// isconnected() - Check if PPP is connected
static mp_obj_t network_usbh_modem_isconnected(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        return mp_const_false;
    }

    esp_netif_t *netif = usbh_modem_get_netif();
    if (netif == NULL) {
        return mp_const_false;
    }

    return mp_obj_new_bool(esp_netif_is_netif_up(netif));
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_isconnected_obj, network_usbh_modem_isconnected);

// ifconfig() - Get IP configuration
static mp_obj_t network_usbh_modem_ifconfig(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    esp_netif_t *netif = usbh_modem_get_netif();
    if (netif == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        mp_raise_OSError(MP_EIO);
    }

    // Get DNS server
    const ip_addr_t *dns = dns_getserver(0);

    // Return (ip, netmask, gateway, dns) tuple
    mp_obj_t tuple[4] = {
        netutils_format_ipv4_addr((uint8_t *)&ip_info.ip, NETUTILS_BIG),
        netutils_format_ipv4_addr((uint8_t *)&ip_info.netmask, NETUTILS_BIG),
        netutils_format_ipv4_addr((uint8_t *)&ip_info.gw, NETUTILS_BIG),
        netutils_format_ipv4_addr((uint8_t *)&dns->u_addr.ip4, NETUTILS_BIG),
    };

    return mp_obj_new_tuple(4, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_ifconfig_obj, network_usbh_modem_ifconfig);

// status() - Get current state
static mp_obj_t network_usbh_modem_status(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        return MP_OBJ_NEW_SMALL_INT(USBH_MODEM_STATE_INACTIVE);
    }

    esp_netif_t *netif = usbh_modem_get_netif();
    if (netif == NULL) {
        return MP_OBJ_NEW_SMALL_INT(USBH_MODEM_STATE_INSTALLED);
    }

    if (esp_netif_is_netif_up(netif)) {
        return MP_OBJ_NEW_SMALL_INT(USBH_MODEM_STATE_CONNECTED);
    }

    return MP_OBJ_NEW_SMALL_INT(USBH_MODEM_STATE_INSTALLED);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_status_obj, network_usbh_modem_status);

// scan() - Install CDC driver and wait for device enumeration (static method)
// Returns after a short delay to allow USB enumeration to complete
static mp_obj_t network_usbh_modem_scan(void) {
    esp_err_t err = ensure_cdc_driver_installed();
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("CDC driver install failed: %s"), esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Scanning for USB devices... (check serial output for device info)");

    // Wait a bit for USB enumeration
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Scan complete. Any connected devices have been logged above.");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(network_usbh_modem_scan_obj, network_usbh_modem_scan);

// config(auto_connect=True/False) - Configure modem behavior
static mp_obj_t network_usbh_modem_config(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    enum { ARG_auto_connect };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_auto_connect, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_auto_connect].u_obj != MP_OBJ_NULL) {
        bool auto_connect = mp_obj_is_true(args[ARG_auto_connect].u_obj);
        esp_err_t err = usbh_modem_ppp_auto_connect(auto_connect);
        if (err != ESP_OK) {
            mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("ppp_auto_connect failed: %s"), esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Auto-connect set to %s", auto_connect ? "enabled" : "disabled");
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_usbh_modem_config_obj, 1, network_usbh_modem_config);

// at(command) - Send raw AT command and check for OK response
static mp_obj_t network_usbh_modem_at(mp_obj_t self_in, mp_obj_t cmd_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    at_handle_t at = usbh_modem_get_atparser();
    if (at == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("AT parser not available (modem not connected?)"));
    }

    const char *cmd = mp_obj_str_get_str(cmd_in);
    ESP_LOGI(TAG, "Sending AT command: %s", cmd);

    esp_err_t err = at_send_command_response_ok(at, cmd);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("AT command failed: %s"), esp_err_to_name(err));
    }

    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_2(network_usbh_modem_at_obj, network_usbh_modem_at);

// signal() - Get signal quality (RSSI, BER)
static mp_obj_t network_usbh_modem_signal(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    at_handle_t at = usbh_modem_get_atparser();
    if (at == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("AT parser not available (modem not connected?)"));
    }

    esp_modem_at_csq_t csq;
    esp_err_t err = at_cmd_get_signal_quality(at, &csq);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("Failed to get signal: %s"), esp_err_to_name(err));
    }

    // Convert RSSI to dBm: dBm = -113 + (rssi * 2), 99 = unknown
    int rssi_dbm = (csq.rssi == 99) ? 0 : (-113 + (csq.rssi * 2));

    ESP_LOGI(TAG, "Signal: RSSI=%d (%d dBm), BER=%d", csq.rssi, rssi_dbm, csq.ber);

    // Return dict with signal info
    mp_obj_t dict = mp_obj_new_dict(3);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(csq.rssi));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rssi_dbm), MP_OBJ_NEW_SMALL_INT(rssi_dbm));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ber), MP_OBJ_NEW_SMALL_INT(csq.ber));

    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_signal_obj, network_usbh_modem_signal);

// info() - Get modem identification info
static mp_obj_t network_usbh_modem_info(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    at_handle_t at = usbh_modem_get_atparser();
    if (at == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("AT parser not available (modem not connected?)"));
    }

    mp_obj_t dict = mp_obj_new_dict(5);
    char buffer[128];

    // Manufacturer
    if (at_cmd_get_manufacturer_id(at, buffer, sizeof(buffer)) == ESP_OK) {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_manufacturer), mp_obj_new_str(buffer, strlen(buffer)));
    }

    // Model
    if (at_cmd_get_module_id(at, buffer, sizeof(buffer)) == ESP_OK) {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_model), mp_obj_new_str(buffer, strlen(buffer)));
    }

    // Revision
    if (at_cmd_get_revision_id(at, buffer, sizeof(buffer)) == ESP_OK) {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_revision), mp_obj_new_str(buffer, strlen(buffer)));
    }

    // IMEI
    if (at_cmd_get_imei_number(at, buffer, sizeof(buffer)) == ESP_OK) {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_imei), mp_obj_new_str(buffer, strlen(buffer)));
    }

    // Operator
    if (at_cmd_get_operator_name(at, buffer, sizeof(buffer)) == ESP_OK) {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_operator), mp_obj_new_str(buffer, strlen(buffer)));
    }

    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_info_obj, network_usbh_modem_info);

// sim_status() - Get SIM card status
static mp_obj_t network_usbh_modem_sim_status(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    at_handle_t at = usbh_modem_get_atparser();
    if (at == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("AT parser not available (modem not connected?)"));
    }

    esp_modem_pin_state_t pin_state;
    esp_err_t err = at_cmd_read_pin(at, &pin_state);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("Failed to get SIM status: %s"), esp_err_to_name(err));
    }

    const char *status_str;
    switch (pin_state) {
        case PIN_READY: status_str = "READY"; break;
        case PIN_SIM_PIN: status_str = "SIM PIN"; break;
        case PIN_SIM_PIN2: status_str = "SIM PIN2"; break;
        case PIN_SIM_PUK: status_str = "SIM PUK"; break;
        case PIN_SIM_PUK2: status_str = "SIM PUK2"; break;
        default: status_str = "UNKNOWN"; break;
    }

    ESP_LOGI(TAG, "SIM status: %s", status_str);
    return mp_obj_new_str(status_str, strlen(status_str));
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_sim_status_obj, network_usbh_modem_sim_status);

// network_status() - Get network registration status
static mp_obj_t network_usbh_modem_network_status(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    at_handle_t at = usbh_modem_get_atparser();
    if (at == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("AT parser not available (modem not connected?)"));
    }

    esp_modem_at_cereg_t cereg;
    esp_err_t err = at_cmd_get_network_reg_status(at, &cereg);
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("Failed to get network status: %s"), esp_err_to_name(err));
    }

    const char *status_str;
    switch (cereg.stat) {
        case 0: status_str = "NOT_REGISTERED"; break;
        case 1: status_str = "REGISTERED_HOME"; break;
        case 2: status_str = "SEARCHING"; break;
        case 3: status_str = "DENIED"; break;
        case 4: status_str = "UNKNOWN"; break;
        case 5: status_str = "REGISTERED_ROAMING"; break;
        default: status_str = "INVALID"; break;
    }

    ESP_LOGI(TAG, "Network registration: n=%d stat=%d (%s)", cereg.n, cereg.stat, status_str);

    mp_obj_t dict = mp_obj_new_dict(2);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_stat), MP_OBJ_NEW_SMALL_INT(cereg.stat));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_status), mp_obj_new_str(status_str, strlen(status_str)));

    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_network_status_obj, network_usbh_modem_network_status);

// dns_info() - Get DNS server information
static mp_obj_t network_usbh_modem_dns_info(mp_obj_t self_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    mp_obj_t dict = mp_obj_new_dict(4);

    // Get DNS servers (up to 3)
    for (int i = 0; i < 3; i++) {
        const ip_addr_t *dns = dns_getserver(i);
        char key[8];
        snprintf(key, sizeof(key), "dns%d", i);

        if (dns != NULL && !ip_addr_isany(dns)) {
            char buf[IP4ADDR_STRLEN_MAX];
            ip4addr_ntoa_r(&dns->u_addr.ip4, buf, sizeof(buf));
            mp_obj_dict_store(dict, mp_obj_new_str(key, strlen(key)), mp_obj_new_str(buf, strlen(buf)));
            ESP_LOGI(TAG, "DNS[%d]: %s", i, buf);
        } else {
            mp_obj_dict_store(dict, mp_obj_new_str(key, strlen(key)), mp_const_none);
            ESP_LOGI(TAG, "DNS[%d]: (not set)", i);
        }
    }

    // Check if netif is up and get its info
    esp_netif_t *netif = usbh_modem_get_netif();
    if (netif != NULL) {
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_netif_up),
                          mp_obj_new_bool(esp_netif_is_netif_up(netif)));
    }

    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_usbh_modem_dns_info_obj, network_usbh_modem_dns_info);

// resolve(hostname) - Test DNS resolution
static mp_obj_t network_usbh_modem_resolve(mp_obj_t self_in, mp_obj_t hostname_in) {
    network_usbh_modem_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (!self->installed) {
        mp_raise_OSError(MP_ENODEV);
    }

    const char *hostname = mp_obj_str_get_str(hostname_in);
    ESP_LOGI(TAG, "Resolving hostname: %s", hostname);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    MP_THREAD_GIL_EXIT();
    int err = lwip_getaddrinfo(hostname, "80", &hints, &res);
    MP_THREAD_GIL_ENTER();

    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS resolution failed: err=%d", err);
        if (res != NULL) {
            lwip_freeaddrinfo(res);
        }
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("DNS resolution failed: %d"), err);
    }

    // Extract IP address
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    char ip_str[IP4ADDR_STRLEN_MAX];
    inet_ntoa_r(addr->sin_addr, ip_str, sizeof(ip_str));

    ESP_LOGI(TAG, "Resolved %s to %s", hostname, ip_str);

    lwip_freeaddrinfo(res);

    return mp_obj_new_str(ip_str, strlen(ip_str));
}
static MP_DEFINE_CONST_FUN_OBJ_2(network_usbh_modem_resolve_obj, network_usbh_modem_resolve);

// Method table
static const mp_rom_map_elem_t network_usbh_modem_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&network_usbh_modem___del___obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&network_usbh_modem_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&network_usbh_modem_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&network_usbh_modem_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&network_usbh_modem_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&network_usbh_modem_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&network_usbh_modem_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&network_usbh_modem_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_scan), MP_ROM_PTR(&network_usbh_modem_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_at), MP_ROM_PTR(&network_usbh_modem_at_obj) },
    { MP_ROM_QSTR(MP_QSTR_signal), MP_ROM_PTR(&network_usbh_modem_signal_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&network_usbh_modem_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_sim_status), MP_ROM_PTR(&network_usbh_modem_sim_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_network_status), MP_ROM_PTR(&network_usbh_modem_network_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_dns_info), MP_ROM_PTR(&network_usbh_modem_dns_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_resolve), MP_ROM_PTR(&network_usbh_modem_resolve_obj) },

    // State constants
    { MP_ROM_QSTR(MP_QSTR_STATE_INACTIVE), MP_ROM_INT(USBH_MODEM_STATE_INACTIVE) },
    { MP_ROM_QSTR(MP_QSTR_STATE_INSTALLED), MP_ROM_INT(USBH_MODEM_STATE_INSTALLED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_CONNECTING), MP_ROM_INT(USBH_MODEM_STATE_CONNECTING) },
    { MP_ROM_QSTR(MP_QSTR_STATE_CONNECTED), MP_ROM_INT(USBH_MODEM_STATE_CONNECTED) },
    { MP_ROM_QSTR(MP_QSTR_STATE_ERROR), MP_ROM_INT(USBH_MODEM_STATE_ERROR) },
};
static MP_DEFINE_CONST_DICT(network_usbh_modem_locals_dict, network_usbh_modem_locals_dict_table);

// Type definition
MP_DEFINE_CONST_OBJ_TYPE(
    esp_network_usbh_modem_type,
    MP_QSTR_USBModem,
    MP_TYPE_FLAG_NONE,
    make_new, network_usbh_modem_make_new,
    print, network_usbh_modem_print,
    locals_dict, &network_usbh_modem_locals_dict
);

#endif // MICROPY_HW_USBH_MODEM
