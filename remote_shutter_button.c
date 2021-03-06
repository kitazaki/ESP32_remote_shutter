// Remote Shutter for smartphone camera based on the BTstack HID keyboard demo code

#define __BTSTACK_FILE__ "remote_shutter_button.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "btstack.h"

#ifdef HAVE_BTSTACK_STDIN
#include "btstack_stdin.h"
#endif

// added
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static uint8_t hid_service_buffer[250];
static uint8_t device_id_sdp_service_buffer[100];  // add
static const char hid_device_name[] = "BTstack Remote Shutter";
static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint16_t hid_cid;

#define GPIO_INPUT_IO_0     4  // ESP32 DevKitC BOOT button -> GPIO0, NefryBT SW button -> GPIO4
#define GPIO_INPUT_IO_1     18  // Not use
#define GPIO_INPUT_PIN_SEL ((1<<GPIO_INPUT_IO_0) | (1<<GPIO_INPUT_IO_1))
#define ESP_INTR_FLAG_DEFAULT 0

static xQueueHandle gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}


// from USB HID Specification 1.1, Appendix B.1
const uint8_t hid_descriptor_keyboard_boot_mode[] = {
/*
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x06,                    // Usage (Keyboard)
    0xa1, 0x01,                    // Collection (Application)
    0x85, 0x01,                    // Report Id (1)  // added

    // Modifier byte
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x08,                    //   Report Count (8)
    0x05, 0x07,                    //   Usage Page (Key codes)
    0x19, 0xe0,                    //   Usage Minimum (Keyboard LeftControl)
    0x29, 0xe7,                    //   Usage Maxium (Keyboard Right GUI)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x81, 0x02,                    //   Input (Data, Variable, Absolute)

    // Reserved byte
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x08,                    //   Report Count (8)
    0x81, 0x03,                    //   Input (Constant, Variable, Absolute)

    // LED report + padding
    0x95, 0x05,                    //   Report Count (5)
    0x75, 0x01,                    //   Report Size (1)
    0x05, 0x08,                    //   Usage Page (LEDs)
    0x19, 0x01,                    //   Usage Minimum (Num Lock)
    0x29, 0x05,                    //   Usage Maxium (Kana)
    0x91, 0x02,                    //   Output (Data, Variable, Absolute)

    0x95, 0x01,                    //   Report Count (1)
    0x75, 0x03,                    //   Report Size (3)
    0x91, 0x03,                    //   Output (Constant, Variable, Absolute)

    // Keycodes
    0x95, 0x06,                    //   Report Count (6)
    0x75, 0x08,                    //   Report Size (8)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0xff,                    //   Logical Maximum (1)
    0x05, 0x07,                    //   Usage Page (Key codes)
    0x19, 0x00,                    //   Usage Minimum (Reserved (no event indicated))
    0x29, 0xff,                    //   Usage Maxium (Reserved)
    0x81, 0x00,                    //   Input (Data, Array)

    0xc0,                          // End collection  
*/
    0x05, 0x0c,       // (GLOBAL) USAGE_PAGE         0x000C Consumer Device Page  // added
    0x09, 0x01,       // (LOCAL)  USAGE              0x000C0001 Consumer Control  // added
    0xa1, 0x01,       // (MAIN)   COLLECTION         0x01 Application  // added
    0x85, 0x02,       //   (GLOBAL) REPORT_ID          2  // added
    0x19, 0x00,       //   (LOCAL)  USAGE_MINIMUM  // added
    0x2a, 0x9c, 0x02, //   (LOCAL)  USAGE_MAXIMUM  // added
    0x15, 0x00,       //   (GLOBAL) LOGICAL_MINIMUM   // added
    0x26, 0x9c, 0x02, //   (GLOBAL) LOGICAL_MAXIMUM    0x029C (668)  // added
    0x95, 0x01,       //   (GLOBAL) REPORT_COUNT       0x01 (1) Number of fields  // added
    0x75, 0x10,       //   (GLOBAL) REPORT_SIZE        0x10 (16) Number of bits per field  // added
    0x81, 0x00,       //   (MAIN)   INPUT   // added
    0xc0,              // (MAIN)   END_COLLECTION     Application  // added
};

// HID Report sending
// added
static xQueueHandle sendQueue=0;
typedef struct keyReport {
    int reportId;
    int modifier;
    int keycode;
} keyReport_t;

static void send_key(int reportID, int modifier, int keycode){
    keyReport_t rep={reportID, modifier, keycode};
    if (xQueueSend(sendQueue, &rep, 0)==pdTRUE) {
        hid_device_request_can_send_now_event(hid_cid);
    }
}


// added for gpio
static void gpio_task(void* arg) {
    uint8_t io_num;
    while (true) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
          int io_level = gpio_get_level(io_num);
          printf("GPIO[%d] val: %d\n", io_num, io_level);
//          ble_indicate(io_level);
            // for iOS
          if (!(io_level)) {
            printf("BTStack: send Volume Increment key\n");
            send_key(2, 0, 0xe9); // Volume Increment
            vTaskDelay(100);
          } else {
            printf("BTStack: send Release\n");
            send_key(2, 0, 0); // Release
            vTaskDelay(100);
          }
        }
    }
}

// added for gpio
static void init_switch() {
    gpio_config_t io_conf;
    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //configure
    gpio_config(&io_conf);

    //change gpio intrrupt type for one pin
    gpio_set_intr_type(GPIO_INPUT_IO_0, GPIO_INTR_ANYEDGE);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task, "gpio_task", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);
}

static void send_report() {
    keyReport_t rep;
    if (xQueueReceive(sendQueue, &rep, 0)==pdTRUE) {
        switch (rep.reportId) {
            case 1:
                printf("Send key press [0x%02X]\n",rep.keycode);
                uint8_t report_key[] = { 0xa1, 1, rep.modifier, 0, 0, rep.keycode, 0, 0, 0, 0, 0};
                hid_device_send_interrupt_message(hid_cid, &report_key[0], sizeof(report_key));
                break;
            case 2:
                printf("Send consumer control [0x%03X]\n",rep.keycode);
                uint8_t report_cc[] = { 0xa1, 2, rep.keycode & 0xff, (rep.keycode>>8) & 0xff};
                hid_device_send_interrupt_message(hid_cid, &report_cc[0], sizeof(report_cc));
                break;
            default:
                ;
        }
        hid_device_request_can_send_now_event(hid_cid);
    } else {
        printf("Send nothing\n");
        uint8_t report[] = { 0xa1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        hid_device_send_interrupt_message(hid_cid, &report[0], sizeof(report));
    }
}

// Demo Application

#ifdef HAVE_BTSTACK_STDIN

// On systems with STDIN, we can directly type on the console

static void stdin_process(char character){
    printf("BTstack: Put shutter keycodes into the FIFO\n");
    // for Android
//    send_key(1, 0, 0x40); // Return
//    send_key(1, 0, 0x80); // Volume Up
    // for iOS
    send_key(2, 0, 0xe9); // Volume Increment
    send_key(2, 0, 0); // Release
}
#else

// On embedded systems, send constant demo text with fixed period

#define TYPING_PERIOD_MS 100
static const char * demo_text = "\n\nHello World!\n\nThis is the BTstack HID Keyboard Demo running on an Embedded Device.\n\n";

static int demo_pos;
static btstack_timer_source_t typing_timer;

static void typing_timer_handler(btstack_timer_source_t * ts){

    // abort if not connected
    if (!hid_cid) return;

    // get next character
    uint8_t character = demo_text[demo_pos++];
    if (demo_text[demo_pos] == 0){
        demo_pos = 0;
    }

    // get keycodeand send
    uint8_t modifier;
    uint8_t keycode;
    int found = keycode_and_modifer_us_for_character(character, &keycode, &modifier);
    if (found){
        send_key(1, modifier, keycode);
    }

    // set next timer
    btstack_run_loop_set_timer(ts, TYPING_PERIOD_MS);
    btstack_run_loop_add_timer(ts);
}

static void hid_embedded_start_typing(void){
    demo_pos = 0;
    // set one-shot timer
    typing_timer.process = &typing_timer_handler;
    btstack_run_loop_set_timer(&typing_timer, TYPING_PERIOD_MS);
    btstack_run_loop_add_timer(&typing_timer);
}

#endif

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t packet_size){
    UNUSED(channel);
    UNUSED(packet_size);
    switch (packet_type){
        case HCI_EVENT_PACKET:
            switch (packet[0]){
                case HCI_EVENT_USER_CONFIRMATION_REQUEST:
                    // ssp: inform about user confirmation request
                    log_info("SSP User Confirmation Request with numeric value '%06"PRIu32"'\n", hci_event_user_confirmation_request_get_numeric_value(packet));
                    log_info("SSP User Confirmation Auto accept\n");                   
                    break; 

                case HCI_EVENT_HID_META:
                    switch (hci_event_hid_meta_get_subevent_code(packet)){
                        case HID_SUBEVENT_CONNECTION_OPENED:
                            if (hid_subevent_connection_opened_get_status(packet)) return;
                            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
#ifdef HAVE_BTSTACK_STDIN                        
                            printf("HID Connected, please start typing...\n");
#else                        
                            printf("HID Connected, sending demo text...\n");
                            hid_embedded_start_typing();
#endif
                            break;
                        case HID_SUBEVENT_CONNECTION_CLOSED:
                            printf("HID Disconnected\n");
                            hid_cid = 0;
                            break;
                        case HID_SUBEVENT_CAN_SEND_NOW:
                            printf("HID Send Report\n");
                            send_report();
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

/* @section Main Application Setup
 *
 * @text Listing MainConfiguration shows main application code. 
 * To run a HID Device service you need to initialize the SDP, and to create and register HID Device record with it. 
 * At the end the Bluetooth stack is started.
 */

/* LISTING_START(MainConfiguration): Setup HID Device */

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    (void)argc;
    (void)argv;

    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hci_register_sco_packet_handler(&packet_handler);

    gap_discoverable_control(1);
    gap_set_class_of_device(0x2540);
    gap_set_local_name("ESP32 Remote Shutter 00:00:00:00:00:00");
    
    // L2CAP
    l2cap_init();

    // SDP Server
    sdp_init();
    memset(hid_service_buffer, 0, sizeof(hid_service_buffer));
    // hid sevice subclass 2540 Keyboard, hid counntry code 33 US, hid virtual cable off, hid reconnect initiate off, hid boot device off 
    hid_create_sdp_record(hid_service_buffer, 0x10001, 0x2540, 33, 0, 0, 0, hid_descriptor_keyboard_boot_mode, sizeof(hid_descriptor_keyboard_boot_mode), hid_device_name);
    printf("HID service record size: %u\n", de_get_len( hid_service_buffer));
    sdp_register_service(hid_service_buffer);

    // See https://www.bluetooth.com/specifications/assigned-numbers/company-identifiers if you don't have a USB Vendor ID and need a Bluetooth Vendor ID
    // device info: BlueKitchen GmbH, product 1, version 1
    device_id_create_sdp_record(device_id_sdp_service_buffer, 0x10003, DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    printf("Device ID SDP service record size: %u\n", de_get_len((uint8_t*)device_id_sdp_service_buffer));
    sdp_register_service(device_id_sdp_service_buffer);

 
    // HID Device
    hid_device_init();
    hid_device_register_packet_handler(&packet_handler);

    // Init send queue  // added
//    sendQueue = xQueueCreate(10,sizeof(keyReport_t));

#ifdef HAVE_BTSTACK_STDIN
    btstack_stdin_setup(stdin_process);
#endif  
    // turn on!
    hci_power_control(HCI_POWER_ON);
    sendQueue = xQueueCreate(10,sizeof(keyReport_t));

    // start GPIO
    init_switch();

    return 0;
}
/* LISTING_END */
/* EXAMPLE_END */
