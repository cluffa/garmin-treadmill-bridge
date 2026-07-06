#include "platform_ant_sdm.h"
#include "ant_sdm_encode.h"
#include "ant_network_key.h"

#include "app_error.h"
#include "app_util_platform.h"
#include "ant_interface.h"
#include "ant_parameters.h"
#include "nrf.h"
#include "nrf_log.h"
#include "nrf_sdh_ant.h"

#include <string.h>

#define SDM_CHANNEL        0
#define SDM_NETWORK        0
#define SDM_DEVICE_TYPE    124     /* ANT+ SDM (foot pod) */
#define SDM_TRANS_TYPE     0x05
#define SDM_RF_FREQ        57      /* 2457 MHz — ANT+ */
#define SDM_CHANNEL_PERIOD 8134    /* ~4.03 Hz, per the SDM device profile */

/* Broadcast pattern, per the SDM profile's page cadence: the main data page
 * with periodic background pages. Common pages 80/81 (manufacturer/product
 * info) are required at least once every 65 messages for the receiver to
 * identify the sensor. Cycle of 68 TX slots:
 *   0..63  page 1 (distance/speed)
 *   64..65 page 2 (cadence/status — zero cadence, treadmill has no stride)
 *   66     common page 80, 67 common page 81 */
#define CYCLE_LEN     68
#define P2_FIRST      64
#define P80_SLOT      66
#define P81_SLOT      67

/* Common page 80: HW revision 1, manufacturer 0x00FF (development),
 * model 1. Page 81: SW revision 1, serial number 0xFFFFFFFF (none). */
static const uint8_t PAGE_80[8] = {0x50,0xFF,0xFF,0x01,0xFF,0x00,0x01,0x00};
static const uint8_t PAGE_81[8] = {0x51,0xFF,0xFF,0x01,0xFF,0xFF,0xFF,0xFF};

static treadmill_state_t s_state;   /* latched by platform_ant_sdm_set_state */
static uint8_t           s_slot;    /* position in the CYCLE_LEN pattern */

void platform_ant_sdm_set_state(const treadmill_state_t *s)
{
    CRITICAL_REGION_ENTER();
    s_state = *s;
    CRITICAL_REGION_EXIT();
}

static void sdm_broadcast_next(void)
{
    uint8_t pg[8];
    treadmill_state_t st;

    CRITICAL_REGION_ENTER();
    st = s_state;
    CRITICAL_REGION_EXIT();

    if (s_slot == P80_SLOT) {
        memcpy(pg, PAGE_80, sizeof pg);
    } else if (s_slot == P81_SLOT) {
        memcpy(pg, PAGE_81, sizeof pg);
    } else if (s_slot >= P2_FIRST) {
        ant_sdm_encode_page2(&st, pg);
    } else {
        ant_sdm_encode_page1(&st, pg);
    }
    s_slot = (uint8_t)((s_slot + 1) % CYCLE_LEN);

    uint32_t err = sd_ant_broadcast_message_tx(SDM_CHANNEL, sizeof pg, pg);
    if (err != NRF_SUCCESS) {
        NRF_LOG_WARNING("sdm tx err %u", err);
    }
}

static void ant_evt_handler(ant_evt_t *p_evt, void *p_context)
{
    (void)p_context;
    if (p_evt->channel != SDM_CHANNEL) {
        return;
    }
    switch (p_evt->event) {
    case EVENT_TX:
        sdm_broadcast_next();
        break;
    default:
        break;
    }
}

NRF_SDH_ANT_OBSERVER(m_sdm_observer, 1 /* prio */, ant_evt_handler, NULL);

void platform_ant_sdm_init(void)
{
    /* Device number from the chip id — stable across boots, unique enough. */
    uint16_t dev_num = (uint16_t)(NRF_FICR->DEVICEID[0] & 0xFFFF);
    if (dev_num == 0) {
        dev_num = 1;   /* 0 is the ANT wildcard; never broadcast it */
    }

    APP_ERROR_CHECK(sd_ant_network_address_set(SDM_NETWORK,
                                               ANT_PLUS_NETWORK_KEY));
    APP_ERROR_CHECK(sd_ant_channel_assign(SDM_CHANNEL,
                                          CHANNEL_TYPE_MASTER,
                                          SDM_NETWORK, 0));
    APP_ERROR_CHECK(sd_ant_channel_id_set(SDM_CHANNEL, dev_num,
                                          SDM_DEVICE_TYPE, SDM_TRANS_TYPE));
    APP_ERROR_CHECK(sd_ant_channel_radio_freq_set(SDM_CHANNEL, SDM_RF_FREQ));
    APP_ERROR_CHECK(sd_ant_channel_period_set(SDM_CHANNEL, SDM_CHANNEL_PERIOD));

    /* Prime the first broadcast, then open; subsequent pages go out from
     * EVENT_TX. */
    sdm_broadcast_next();
    APP_ERROR_CHECK(sd_ant_channel_open(SDM_CHANNEL));

    NRF_LOG_INFO("ANT+ SDM master open, dev_num=%u", dev_num);
}
