/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 hathach for Adafruit Industries
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

#include "lib/tinyusb/src/tusb.h"

#if CIRCUITPY_USB_CDC
#include "shared-module/storage/__init__.h"
#endif

#if CIRCUITPY_USB_HID
#include "shared-module/usb_hid/__init__.h"
#endif

#if CIRCUITPY_USB_MIDI
#include "shared-module/usb_midi/__init__.h"
#endif

#if CIRCUITPY_USB_MSC
#include "shared-module/storage/__init__.h"
#endif

#include "shared-module/usb_hid/Device.h"

#include "genhdr/autogen_usb_descriptor.h"

static uint8_t *device_descriptor;
static uint8_t *config_descriptor;
static uint8_t *hid_report_descriptor;

// Table for collecting interface strings (interface names) as descriptor is built.
#define MAX_INTERFACE_STRINGS 16
// slot 0 is not used.
static char * collected_interface_strings[];
static uint16_t current_interface_string;

static const char[] manufacturer_name = USB_MANUFACTURER;
static const char[] product_name = USB_PRODUCT;

// Serial number string is UID length * 2 (2 nibbles per byte) + 1 byte for null termination.
static char serial_number_hex_string[COMMON_HAL_MCU_PROCESSOR_UID_LENGTH * 2 + 1];

static const uint8_t device_descriptor_template[] = {
    0x12,        //  0 bLength
    0x01,        //  1 bDescriptorType (Device)
    0x00, 0x02,  //  2,3 bcdUSB 2.00
    0x00,        //  4 bDeviceClass (Use class information in the Interface Descriptors)
    0x00,        //  5 bDeviceSubClass
    0x00,        //  6 bDeviceProtocol
    0x40,        //  7 bMaxPacketSize0 64
    0x9A, 0x23,  //  8,9 idVendor [SET AT RUNTIME: lo,hi]
#define DEVICE_VID_LO_INDEX (8)
#define DEVICE_VID_HI_INDEX (9)
    0x, 0xFF,  // 10,11 idProduct [SET AT RUNTIME: lo,hi]
#define DEVICE PID_LO_INDEX (10)
#define DEVICE PID_HI_INDEX (11)
    0x00, 0x01,  // 12,13 bcdDevice 2.00
    0x02,        // 14 iManufacturer (String Index) [SET AT RUNTIME]
#define DEVICE_MANUFACTURER_STRING_INDEX (14)
    0x03,        // 15 iProduct (String Index) [SET AT RUNTIME]
#define DEVICE_PRODUCT_STRING_INDEX (15)
    0x01,        // 16 iSerialNumber (String Index)  [SET AT RUNTIME]
#define DEVICE_SERIAL_NUMBER_STRING_INDEX (16)
    0x01,        // 17 bNumConfigurations 1
};

static const uint8_t configuration_descriptor_template[] = {
    0x09,        // 0 bLength
    0x02,        // 1 bDescriptorType (Configuration)
    0xFF, 0xFF,  // 2,3 wTotalLength  [SET AT RUNTIME: lo, hi]
#define CONFIG_TOTAL_LENGTH_LO_INDEX (2)
#define CONFIG_TOTAL_LENGTH_HI_INDEX (3)
    0xFF,        // 4 bNumInterfaces  [SET AT RUNTIME]
#define CONFIG_NUM_INTERFACES_INDEX (4)
    0x01,        // 5 bConfigurationValue
    0x00,        // 6 iConfiguration (String Index)
    0x80,        // 7 bmAttributes
    0x32,        // 8 bMaxPower 100mA
};

void usb_desc_init(void) {
    uint8_t raw_id[COMMON_HAL_MCU_PROCESSOR_UID_LENGTH];
    common_hal_mcu_processor_get_uid(raw_id);

    for (int i = 0; i < COMMON_HAL_MCU_PROCESSOR_UID_LENGTH; i++) {
        for (int j = 0; j < 2; j++) {
            uint8_t nibble = (raw_id[i] >> (j * 4)) & 0xf;
            serial_number_hex_string[i * 2 + (1 - j)] = nibble_to_hex_upper[nibble];
        }
    }

    // Null-terminate the string.
    serial_number_hex_string[sizeof(serial_number_hex_string)] = '\0';

    // Set to zero when allocated; we depend on that.
    collected_interface_strings = m_malloc(MAX_INTERFACE_STRINGS + 1, false);
    current_interface_string = 1;
}


void usb_build_device_descriptor(uint16_t vid, uint16_t pid, uint8_t *current_interface_string) {
    device_descriptor = m_malloc(sizeof(device_descriptor_template), false);
    memcpy(device_descriptor, device_descriptor_template, sizeof(device_descriptor_template));

    device_descriptor[DEVICE_VID_LO_INDEX] = vid & 0xFF;
    device_descriptor[DEVICE_VID_HI_INDEX] = vid >> 8;
    device_descriptor[DEVICE_PID_LO_INDEX] = pid & 0xFF;
    device_descriptor[DEVICE_PID_HI_INDEX] = pid >> 8;

    usb_add_interface_string(*current_interface_string, manufacturer_name);
    device_descriptor[DEVICE_MANUFACTURER_STRING_INDEX] = *current_interface_string;
    (*current_interface_string)++;

    usb_add_interface_string(*current_interface_string, product_name);
    device_descriptor[DEVICE_PRODUCT_STRING_INDEX] = *current_interface_string;
    (*current_interface_string)++;

    usb_add_interface_string(*current_interface_string, serial_number_hex_string);
    device_descriptor[DEVICE_SERIAL_NUMBER_STRING_INDEX] = *current_interface_string;
    (*current_interface_string)++;
}

void usb_build_configuration_descriptor(uint16_t total_length, uint8_t num_interfaces) {
    size_t total_descriptor_length = sizeof(configuration_descriptor_template);

    // CDC should be first, for compatibility with Adafruit Windows 7 drivers.
    // In the past, the order has been CDC, MSC, MIDI, HID, so preserve
    // that order.
#if CIRCUITPY_USB_CDC
    if (usb_cdc_repl_enabled) {
        total_descriptor_length += usb_cdc_descriptor_length();
    }
    if (usb_cdc_data_enabled) {
        total_descriptor_length += usb_cdc_descriptor_length();
    }
#endif

#if CIRCUITPY_USB_MSC
    if (storage_usb_enabled) {
        total_descriptor_length += storage_usb_descriptor_length();
    }
#endif

#if CIRCUITPY_USB_MIDI
    if (usb_midi_enabled) {
        total_descriptor_length += usb_midi_descriptor_length();
    }
#endif

#if CIRCUITPY_USB_HID
    if (usb_hid_enabled) {
        total_descriptor_length += usb_hid_descriptor_length();
    }
#endif

    // Now we now how big the configuration descriptor will be.
    configuration_descriptor = m_malloc(total_descriptor_length, false);

    // Copy the top-level template, and fix up its length.
    memcpy(config_descriptor, configuration_descriptor_template, sizeof(configuration_descriptor_template));
    configuration_descriptor[CONFIG_TOTAL_LENGTH_LO_INDEX] = total_descriptor_length & 0xFF;
    configuration_descriptor[CONFIG_TOTAL_LENGTH_HI_INDEX] = (total_descriptor_length >> 8) & 0xFF;

    // Number interfaces and endpoints.
    // Endpoint 0 is already used for USB control, so start with 1.
    uint8_t current_interface = 0;
    uint8_t current_endpoint = 1;

    uint8_t *descriptor_buf_remaining = configuration_descriptor + sizeof(configuration_descriptor_template);

#if CIRCUITPY_USB_CDC
    if (usb_cdc_repl_enabled) {
        // Concatenate and fix up the CDC REPL descriptor.
        descriptor_buf_remaining += usb_cdc_add_descriptor(
            descriptor_buf_remaining, *current_interface, *current_endpoint, *current_interface_string, true);
    }
    if (usb_cdc_data_enabled) {
        // Concatenate and fix up the CDC data descriptor.
        descriptor_buf_remaining += usb_cdc_add_descriptor(
            descriptor_buf_remaining, *current_interface, *current_endpoint, *current_interface_string, false);
    }
#endif

#if CIRCUITPY_USB_MSC
    if (storage_usb_enabled) {
        // Concatenate and fix up the MSC descriptor.
        descriptor_buf_remaining += storage_usb_add_descriptor(
            descriptor_buf_remaining, *current_interface, *current_endpoint, *current_interface_string);
    }
#endif

#if CIRCUITPY_USB_MIDI
    if (usb_midi_enabled) {
        // Concatenate and fix up the MIDI descriptor.
        descriptor_buf_remaining += usb_midi_add_descriptor(
            descriptor_buf_remaining, *current_interface, *current_endpoint, *current_interface_string);
    }
#endif

#if CIRCUITPY_USB_HID
    if (usb_hid_enabled) {
        descriptor_buf_remaining += usb_hid_add_descriptor(
            descriptor_buf_remaining, *current_interface, *current_endpoint, *current_interface_string);
    }
#endif

    // Now we know how many interfaces have been used.
    configuration_descriptor[CONFIG_NUM_INTERFACES_INDEX] = current_interface - 1;

    // Did we run out of endpoints?
    if (current_endpoint - 1 > USB_NUM_EP) {
        mp_raise_SystemError("Not enough USB endpoints");
    }

}

void usb_add_interface_string(uint8_t interface_string_index, const char[] str) {
    if (interface_string_index > MAX_INTERFACE_STRINGS) {
        mp_raise_SystemError("Too many USB interface names");
    }
    // 2 bytes for String Descriptor header, then 2 bytes for each character
    const size_t str_len = strlen(str);
    uint8_t descriptor_size  = 2 + (str_len * 2);
    uint16_t *string_descriptor = (uint16_t *) m_malloc(descriptor_size, false);
    string_descriptor[0] = 0x0300 | descriptor_size;
    // Convert to le16
    for (i = 0; i <= str_len; i++) {
        string_descriptor[i + 1] = str[i];
    }

    collected_interface_strings[interface_string_index] = string_descriptor;
}


void usb_desc_gc_collect(void) {
    // Once tud_mounted() is true, we're done with the constructed descriptors.
    if (tud_mounted()) {
        // GC will pick up the inaccessible blocks.
        device_descriptor = NULL;
        configuration_descriptor = NULL;
        hid_report_descriptors = NULL;
    } else {
        gc_collect_ptr(device_descriptor);
        gc_collect_ptr(configuration_descriptor);
        gc_collect_ptr(hid_report_descriptors);  // Collects children too.
    }
}


// Invoked when GET DEVICE DESCRIPTOR is received.
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void) {
    return usb_descriptor_dev;
}

// Invoked when GET CONFIGURATION DESCRIPTOR is received.
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;  // for multiple configurations
    return config_desc;
}

#if CIRCUITPY_USB_HID
// Invoked when GET HID REPORT DESCRIPTOR is received.
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) {
    return hid_report_descriptor;
}
#endif

// Invoked when GET STRING DESCRIPTOR request is received.
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    if (index > MAX_INTERFACE_STRINGS) {
        return NULL;
    }
    return collected_interface_strings[index];
}
