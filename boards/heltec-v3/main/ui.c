#include "ui.h"
#include "machine.h"
#include "garmin_rsc.h"
#include "serial_ctrl.h"
#include "ctrl_svc.h"
#include "button.h"
#include "display.h"
#include "display_format.h"
#include "ftms_devlist.h"
#include "model.h"
#include "power.h"
#include "battery.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

typedef enum { MODE_HOME, MODE_PAIRING } ui_mode_t;

static ui_mode_t       s_mode = MODE_HOME;
static int             s_sel;
static int             s_nsnap;
static ftms_device_t   s_snap[FTMS_MAX_DEVICES];
static treadmill_state_t s_last_state;
static TaskHandle_t    s_render;

/* Called from the button task on press-down: wake the render loop immediately
 * so the loading bar appears even when the idle screen is refreshing at ~1 Hz. */
static void on_press_start(void) {
    if (s_render) xTaskNotifyGive(s_render);
}

static void on_state(const treadmill_state_t *s) {
    s_last_state = *s;
    garmin_rsc_update(s);
    serial_ctrl_push_state(s);
}

static void on_link(bool connected) {
    (void)connected;
    ctrl_svc_notify_status();   /* push 'S' frame to the watch */
}

static void on_button(button_event_t e) {
    if (e == BTN_VERYLONG) {     /* double-long hold (bar filled then reversed to 0) */
        power_off();             /* does not return */
        return;
    }
    if (s_mode == MODE_HOME) {
        if (e == BTN_LONG) {                       /* open pairing menu */
            machine_start_scan();             /* cancel any reconnect, scan fresh */
            s_nsnap = machine_get_devices(s_snap, FTMS_MAX_DEVICES);
            s_sel = 0;
            s_mode = MODE_PAIRING;
        }
    } else { /* MODE_PAIRING */
        if (e == BTN_SHORT) {
            if (s_nsnap > 0) s_sel = (s_sel + 1) % s_nsnap;
        } else if (e == BTN_LONG) {
            if (s_nsnap > 0) machine_connect(&s_snap[s_sel]);
            s_mode = MODE_HOME;
        }
    }
}

/* Battery changes slowly and a read briefly toggles the divider; cache it and
 * refresh every ~5 s rather than every frame. The reading bounces (BLE TX sags
 * VBAT, then it recovers), so smooth the 5 s samples with an EMA (alpha 1/8,
 * ~40 s settle) before anything sees them. */
static uint16_t batt_mv_cached(void) {
    static uint16_t avg;
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (last == 0 || now - last > 5000000) {
        uint16_t mv = battery_read_mv();
        avg = (last == 0) ? mv
                          : (uint16_t)((int)avg + ((int)mv - (int)avg) / 8);
        last = now;
    }
    return avg;
}

static void render_task(void *arg) {
    (void)arg;
    char lines[DISP_LINES][DISP_COLS];
    for (;;) {
        /* Push battery % to the watch (standard Battery Service); dedups on
         * its own, so calling every frame is cheap. */
        garmin_rsc_update_battery(battery_pct_from_mv(batt_mv_cached()));
        if (s_mode == MODE_PAIRING) {
            s_nsnap = machine_get_devices(s_snap, FTMS_MAX_DEVICES);
            if (s_nsnap > 0 && s_sel >= s_nsnap) s_sel = s_nsnap - 1;
            display_format_menu(s_snap, s_nsnap, s_sel, lines);
        } else {
            /* s_last_state and the machine_ftms/garmin_rsc getters are written
             * by the NimBLE host task; these reads are intentionally
             * unsynchronized — worst case is one ~150 ms frame with a torn
             * status value. Benign for a status display. */
            display_model_t m = {
                .mill_connected   = machine_connected(),
                .mill_connecting  = machine_connecting(),
                .watch_subscribed = garmin_rsc_subscribed(),
                .advertising      = garmin_rsc_advertising(),
#ifdef CONFIG_DISPLAY_IMPERIAL
                .imperial         = true,
#endif
                .rssi             = machine_conn_rssi(),
                .battery_mv       = batt_mv_cached(),
                .state            = s_last_state,
            };
            const ftms_device_t *cd = machine_connected_device();
            if (cd) device_label(cd, m.device_name, FTMS_NAME_LEN); else m.device_name[0] = '\0';
            display_format_home(&m, lines);
        }
        /* Long-press loading bar: fills, then reverses toward power-off. */
        uint32_t held = button_held_ms();
        int bar = (held >= 60) ? button_bar_pct(held) : -1;
        display_draw(lines, bar);
        /* Refresh fast while the bar animates or the menu is open; idle home
         * screen only needs ~1 Hz (opt #1). A press-down notification wakes us
         * early so the bar shows immediately rather than waiting out the idle
         * period. */
        /* Use physical button state, not elapsed ms: right after the press-down
         * wake, held_ms is still 0, so `held > 0` would wrongly idle for 1 s and
         * the bar wouldn't draw until the reverse phase. */
        bool active = (s_mode == MODE_PAIRING) || button_is_down();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(active ? 80 : 1000));
    }
}

void ui_start(void) {
    battery_init();
    garmin_rsc_start();
    ctrl_svc_start();
    serial_ctrl_start();
    machine_set_link_cb(on_link);
    machine_set_data_cb(on_state);
    machine_try_last();   /* reconnect to last device, else scan */
    xTaskCreate(render_task, "render", 4096, NULL, 4, &s_render);
    button_init(on_button);
    button_set_press_cb(on_press_start);   /* wake render loop on press-down */
}
