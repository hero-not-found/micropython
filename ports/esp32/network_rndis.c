/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 HNF
 */

#include "py/runtime.h"

#if MICROPY_PY_NETWORK_RNDIS

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "extmod/modnetwork.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_rndis.h"
#include "modnetwork.h"

typedef struct _rndis_if_obj_t {
    base_if_obj_t base;
    bool initialized;
    bool cdc_driver_installed;
    iot_eth_driver_t *rndis_driver;
    iot_eth_handle_t eth_handle;
    iot_eth_netif_glue_handle_t glue;
} rndis_if_obj_t;

static const char *TAG = "rndis";
const mp_obj_type_t rndis_if_type;
static rndis_if_obj_t rndis_obj = {{{&rndis_if_type}, ESP_IF_ETH, NULL, false}, false, false, NULL, NULL, NULL};
static uint8_t rndis_status = ETH_STOPPED;

static void rndis_raise(esp_err_t err, const char *msg) {
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s failed: %s"), msg, esp_err_to_name(err));
    }
}

static void rndis_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;

    if (event_base == IOT_ETH_EVENT) {
        switch (event_id) {
            case IOT_ETH_EVENT_START:
                rndis_status = ETH_STARTED;
                ESP_LOGI(TAG, "RNDIS Ethernet Started");
                break;
            case IOT_ETH_EVENT_STOP:
                rndis_status = ETH_STOPPED;
                ESP_LOGI(TAG, "RNDIS Ethernet Stopped");
                break;
            case IOT_ETH_EVENT_CONNECTED:
                rndis_status = ETH_CONNECTED;
                ESP_LOGI(TAG, "RNDIS Link Up");
                break;
            case IOT_ETH_EVENT_DISCONNECTED:
                rndis_status = ETH_DISCONNECTED;
                ESP_LOGI(TAG, "RNDIS Link Down");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        rndis_status = ETH_GOT_IP;
        ESP_LOGI(TAG, "RNDIS Got IP");
    }
}

static void rndis_install_cdc_driver(rndis_if_obj_t *self) {
    const usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_coreid = 0,
        .skip_init_usb_host_driver = false,
    };
    esp_err_t err = usbh_cdc_driver_install(&cdc_config);
    if (err == ESP_ERR_INVALID_STATE) {
        self->cdc_driver_installed = false;
    } else {
        rndis_raise(err, "usbh_cdc_driver_install");
        self->cdc_driver_installed = true;
    }
}

static mp_obj_t get_rndis(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    rndis_if_obj_t *self = &rndis_obj;

    enum { ARG_id };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_id].u_obj != mp_const_none && mp_obj_get_int(args[ARG_id].u_obj) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid RNDIS interface identifier"));
    }

    if (self->initialized) {
        return MP_OBJ_FROM_PTR(self);
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        rndis_raise(err, "esp_netif_init");
    }

    rndis_install_cdc_driver(self);

    static const usb_device_match_id_t match_ids[] = {
        {
            .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT,
            .idVendor = USB_DEVICE_VENDOR_ANY,
            .idProduct = USB_DEVICE_PRODUCT_ANY,
        },
        {0},
    };

    const iot_usbh_rndis_config_t rndis_config = {
        .match_id_list = match_ids,
    };
    rndis_raise(iot_eth_new_usb_rndis(&rndis_config, &self->rndis_driver), "iot_eth_new_usb_rndis");

    const iot_eth_config_t eth_config = {
        .driver = self->rndis_driver,
        .stack_input = NULL,
    };
    rndis_raise(iot_eth_install(&eth_config, &self->eth_handle), "iot_eth_install");

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    self->base.netif = esp_netif_new(&netif_config);
    if (self->base.netif == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("esp_netif_new failed"));
    }

    self->glue = iot_eth_new_netif_glue(self->eth_handle);
    if (self->glue == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("iot_eth_new_netif_glue failed"));
    }
    rndis_raise(esp_netif_attach(self->base.netif, self->glue), "esp_netif_attach");

    rndis_raise(esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, &rndis_event_handler, NULL), "esp_event_handler_register");
    rndis_raise(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &rndis_event_handler, NULL), "esp_event_handler_register");

    self->base.active = false;
    self->initialized = true;
    rndis_status = ETH_INITIALIZED;

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(esp_network_get_rndis_obj, 0, get_rndis);

static mp_obj_t rndis_active(size_t n_args, const mp_obj_t *args) {
    rndis_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (n_args > 1) {
        bool make_active = mp_obj_is_true(args[1]);
        if (make_active && !self->base.active) {
            rndis_raise(esp_netif_set_hostname(self->base.netif, mod_network_hostname_data), "esp_netif_set_hostname");
            rndis_raise(esp_netif_set_default_netif(self->base.netif), "esp_netif_set_default_netif");
            rndis_raise(iot_eth_start(self->eth_handle), "iot_eth_start");
            self->base.active = true;
        } else if (!make_active && self->base.active) {
            rndis_raise(iot_eth_stop(self->eth_handle), "iot_eth_stop");
            self->base.active = false;
        }
    }

    return mp_obj_new_bool(self->base.active);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(rndis_active_obj, 1, 2, rndis_active);

static mp_obj_t rndis_status_fn(mp_obj_t self_in) {
    (void)self_in;
    return MP_OBJ_NEW_SMALL_INT(rndis_status);
}
static MP_DEFINE_CONST_FUN_OBJ_1(rndis_status_obj, rndis_status_fn);

static mp_obj_t rndis_isconnected(mp_obj_t self_in) {
    rndis_if_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->base.active && (rndis_status == ETH_GOT_IP));
}
static MP_DEFINE_CONST_FUN_OBJ_1(rndis_isconnected_obj, rndis_isconnected);

static mp_obj_t rndis_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (kwargs->used != 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("RNDIS config is read-only"));
    }
    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("can query only one param"));
    }

    rndis_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    switch (mp_obj_str_get_qstr(args[1])) {
        case MP_QSTR_mac: {
            uint8_t mac[6];
            rndis_raise(iot_eth_get_addr(self->eth_handle, mac), "iot_eth_get_addr");
            return mp_obj_new_bytes(mac, sizeof(mac));
        }
        case MP_QSTR_ifname:
            return esp_ifname(self->base.netif);
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
    }
}
static MP_DEFINE_CONST_FUN_OBJ_KW(rndis_config_obj, 1, rndis_config);

static const mp_rom_map_elem_t rndis_if_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&rndis_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&rndis_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&rndis_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&rndis_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&esp_network_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipconfig), MP_ROM_PTR(&esp_nic_ipconfig_obj) },
};

static MP_DEFINE_CONST_DICT(rndis_if_locals_dict, rndis_if_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    rndis_if_type,
    MP_QSTR_RNDIS,
    MP_TYPE_FLAG_NONE,
    locals_dict, &rndis_if_locals_dict
    );

#endif
