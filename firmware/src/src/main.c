#include "mgos.h"
#include "mg_bthing_sdk.h"
#include "mgos_bthing_gpio.h"
#include "mgos_bvalve_gpio.h"
#include "mgos_bflowsens_gpio.h"
#include "mgos_balert.h"
#include "mgos_bbutton.h"

struct mg_eddy_flow_context {
  mgos_bflowsens_t flow;
  mgos_balert_t alert;
  int alert_timer_id;
};

struct mg_eddy_context {
  int autoclose_timer_id;
  int64_t autoclose_start;
  mgos_bvalve_t valve;
  mgos_bbutton_t force_btn;
  struct mg_eddy_flow_context pure;
  struct mg_eddy_flow_context tap;
};

static struct mg_eddy_context CTX;


#define EDDY_VALVE_DOMAIN_NAME              "valves"
#define EDDY_FLOWSENS_DOMAIN_NAME           "flow_sensors"
#define EDDY_ALERT_DOMAIN_NAME              "alerts"

#define EDDY_VALVE_PIN1                     4 // D2 on WEMOS D1 Mini shield
#define EDDY_VALVE_PIN1_ACTIVE_HIGH         true
#define EDDY_VALVE_AUTOCLOSE_TICKS          1000 // milliseconds

#define EDDY_VALVE_PIN2                     5 // D1 on WEMOS D1 Mini shield
#define EDDY_VALVE_PIN2_ACTIVE_HIGH         true

#define EDDY_PURE_FLOWSENS_PIN              14 // D5 on WEMOS D1 Mini shield
#define EDDY_PURE_FLOWSENS_GPIO_PULL_TYPE   MGOS_GPIO_PULL_NONE

#define EDDY_TAP_FLOWSENS_PIN               12 // D6 on WEMOS D1 Mini shield
#define EDDY_TAP_FLOWSENS_GPIO_PULL_TYPE    MGOS_GPIO_PULL_NONE

#define EDDY_TAP_FORCEBTN_PIN               0 // D3 on WEMOS D1 Mini shield
#define EDDY_TAP_FORCEBTN_ACTIVE_HIGH       false

#define EDDY_FLOW_ALERT_TIMEOUT             2000 // 2secs.


void mg_eddy_flow_alert_timer_cb(void *param) {
  struct mg_eddy_flow_context *flow = (struct mg_eddy_flow_context *)param;
  mgos_balert_error(flow->alert, 1001);
  flow->alert_timer_id = MGOS_INVALID_TIMER_ID;
}

static void mg_eddy_stop_autoclose_timer() {
  CTX.autoclose_start = 0;
  if (CTX.autoclose_timer_id != MGOS_INVALID_TIMER_ID) {
    mgos_clear_timer(CTX.autoclose_timer_id);
    CTX.autoclose_timer_id = MGOS_INVALID_TIMER_ID;
  }
}

void mg_eddy_autoclose_timer_cb(void *param) {
  if (CTX.autoclose_start != 0) {
    float duration = (mg_bthing_duration_micro(CTX.autoclose_start, mgos_uptime_micros()) / 1000); // ms
    if (duration > (mgos_sys_config_get_eddy_valve_close_timeout() * 1000)) {
      // the auto-close timeout expired... 
      mg_eddy_stop_autoclose_timer();
      // close the purified-water valve
      mgos_bvalve_set_state(CTX.valve, MGOS_BVALVE_STATE_CLOSED);
      LOG(LL_DEBUG, ("The pure water valve has been closed for inactivity."));
    }
  }
}

void mg_eddy_valve_state_changed(int ev, void *ev_data, void *userdata) {
  struct mgos_bthing_state* args = (struct mgos_bthing_state*)ev_data;
  int valve_state = mgos_bvar_get_integer(args->state);
  if ((args->state_flags & MG_BTHING_FLAG_STATE_INITIALIZED) != MG_BTHING_FLAG_STATE_INITIALIZED) {
    if (valve_state == MGOS_BVALVE_STATE_OPEN) {
      // the purified-water valve has been open
      if (CTX.autoclose_timer_id == MGOS_INVALID_TIMER_ID) {
        // start the auto-close timer
        CTX.autoclose_start = mgos_uptime_micros();
        CTX.autoclose_timer_id = mgos_set_timer(EDDY_VALVE_AUTOCLOSE_TICKS, MGOS_TIMER_REPEAT,
          mg_eddy_autoclose_timer_cb, NULL);
      }
    } else if (valve_state == MGOS_BVALVE_STATE_CLOSED) {
      mg_eddy_stop_autoclose_timer();
    }
    LOG(LL_DEBUG, ("The state of pure water valve changed to: %d", valve_state));
  } else {
    LOG(LL_DEBUG, ("The state of pure water valve has been initialized to: %d", mgos_bvar_is_null(args->state) ? -1 : valve_state));
  }
}

void mg_eddy_flowsens_state_changed(int ev, void *ev_data, void *userdata) {
  struct mgos_bthing_state* args = (struct mgos_bthing_state*)ev_data;
  double flow_rate = mgos_bvar_get_decimal(mgos_bvarc_get_key(args->state, MGOS_BFLOWSENS_STATE_FLOW_RATE));
  LOG(LL_DEBUG, ("%s water flow %f", (((void *)args->thing) == ((void *)CTX.pure.flow) ? "Pure" : "Tap"), flow_rate));

  if (flow_rate == 0.0) {
    // Water flow stopped...

    if (((void *)args->thing) == (void *)CTX.pure.flow) {
      // Purified-water flow stopped

      // 1. Stop the alert timeout
      if (CTX.pure.alert_timer_id != MGOS_INVALID_TIMER_ID) {
        mgos_clear_timer(CTX.pure.alert_timer_id);
        CTX.pure.alert_timer_id = MGOS_INVALID_TIMER_ID;
      }
      // 2. Enable the auto-close countdown if not yet started
      if (CTX.autoclose_timer_id != MGOS_INVALID_TIMER_ID) {
        CTX.autoclose_start = mgos_uptime_micros(); // enable the countdown
      }

    } else if (((void *)args->thing) == (void *)CTX.tap.flow) {
      // Tap-water flow stopped

      // 1. Stop the alert timeout
      if (CTX.tap.alert_timer_id != MGOS_INVALID_TIMER_ID) {
        mgos_clear_timer(CTX.tap.alert_timer_id);
        CTX.tap.alert_timer_id = MGOS_INVALID_TIMER_ID;
      }
    }

  } else {
    // Water flow detected...
  
    enum mgos_bvalve_state valve_state = mgos_bvalve_get_state(CTX.valve);
    if (((void *)args->thing) == (void *)CTX.pure.flow) {
      // Purified-water flow detected

      // 1. Prevent the auto-close timer countdown
      CTX.autoclose_start = 0; 
      // 2. Start alert timeout
      if (valve_state == MGOS_BVALVE_STATE_CLOSED && CTX.pure.alert_timer_id == MGOS_INVALID_TIMER_ID) {
        // ERROR: "Unexpected flow of purified water has been detected."
        CTX.pure.alert_timer_id = mgos_set_timer(EDDY_FLOW_ALERT_TIMEOUT, 0,
          mg_eddy_flow_alert_timer_cb, &CTX.pure);
      }

    } else if (((void *)args->thing) == (void *)CTX.tap.flow) {
      // Tap-water flow detected

      // 1. Start alert timeout
      if (valve_state == MGOS_BVALVE_STATE_OPEN && CTX.tap.alert_timer_id == MGOS_INVALID_TIMER_ID) {
        // ERROR: ""Unexpected flow of tap water has been detected."
        CTX.tap.alert_timer_id = mgos_set_timer(EDDY_FLOW_ALERT_TIMEOUT, 0,
          mg_eddy_flow_alert_timer_cb, &CTX.tap);
      }

    }
  }
}

static void mg_eddy_forcebtn_on_event(mgos_bbutton_t btn, enum mgos_bbutton_event ev, void *userdata) {
  switch (ev) {
    case MGOS_EV_BBUTTON_ON_CLICK:
      // open the purified-water valve
      mgos_bvalve_set_state(CTX.valve, MGOS_BVALVE_STATE_OPEN);
      break;
    case MGOS_EV_BBUTTON_ON_DBLCLICK:
      break;
    case MGOS_EV_BBUTTON_ON_PRESS:
      // close the purified-water valve
      mgos_bvalve_set_state(CTX.valve, MGOS_BVALVE_STATE_CLOSED);
      // clear alerts
      mgos_balert_clear(CTX.pure.alert);
      mgos_balert_clear(CTX.tap.alert);
      break;
    case MGOS_EV_BBUTTON_ON_RELEASE:
      break;
    case MGOS_EV_BBUTTON_ON_IDLE:
      break;
    default:
      break;
  }
} 

enum mgos_app_init_result mgos_app_init(void) {
  // Initialize context
  CTX.pure.alert_timer_id = MGOS_INVALID_TIMER_ID;
  CTX.tap.alert_timer_id = MGOS_INVALID_TIMER_ID;
  CTX.autoclose_timer_id = MGOS_INVALID_TIMER_ID;
  CTX.autoclose_start = 0;

  /* Create and initialize the valve */
  CTX.valve = mgos_bvalve_create(mgos_sys_config_get_eddy_valve_id(), MGOS_BVALVE_TYPE_BISTABLE, EDDY_VALVE_DOMAIN_NAME);
  if (!CTX.valve) {
    return MGOS_APP_INIT_ERROR;
  }
  // attach the valve to pin
  if (!mgos_bvalve_gpio_attach(CTX.valve, EDDY_VALVE_PIN1, EDDY_VALVE_PIN1_ACTIVE_HIGH,
                               EDDY_VALVE_PIN2, EDDY_VALVE_PIN1_ACTIVE_HIGH,
                               mgos_sys_config_get_eddy_valve_pulse_duration())) {
    return MGOS_APP_INIT_ERROR;
  }
  mgos_bthing_on_event(MGOS_BVALVE_THINGCAST(CTX.valve), MGOS_EV_BTHING_STATE_CHANGED,
                       mg_eddy_valve_state_changed, NULL);

  /* Create and initialize the flow sensor #1 */
  CTX.pure.flow = mgos_bflowsens_create(mgos_sys_config_get_eddy_pure_flowsens_id(), EDDY_FLOWSENS_DOMAIN_NAME);
  if (!CTX.pure.flow) {
    return MGOS_APP_INIT_ERROR;
  }
  // attach the flow sensor #1 to the pin
  if (!mgos_bflowsens_gpio_attach(CTX.pure.flow, EDDY_PURE_FLOWSENS_PIN, EDDY_PURE_FLOWSENS_GPIO_PULL_TYPE,
                                  mgos_sys_config_get_eddy_pure_flowsens_pulse_high(),
                                  mgos_sys_config_get_eddy_pure_flowsens_flow_ratio())) {
    return MGOS_APP_INIT_ERROR;
  }
  mgos_bthing_on_event(MGOS_BFLOWSENS_THINGCAST(CTX.pure.flow), MGOS_EV_BTHING_STATE_CHANGED,
                       mg_eddy_flowsens_state_changed, NULL);


  /* Create and initialize the flow sensor #2 */
  CTX.tap.flow = mgos_bflowsens_create(mgos_sys_config_get_eddy_tap_flowsens_id(), EDDY_FLOWSENS_DOMAIN_NAME);
  if (!CTX.tap.flow) {
    return MGOS_APP_INIT_ERROR;
  }
  // attach the flow sensor #2 to the pin
  if (!mgos_bflowsens_gpio_attach(CTX.tap.flow, EDDY_TAP_FLOWSENS_PIN, EDDY_TAP_FLOWSENS_GPIO_PULL_TYPE,
                                  mgos_sys_config_get_eddy_tap_flowsens_pulse_high(),
                                  mgos_sys_config_get_eddy_tap_flowsens_flow_ratio())) {
    return MGOS_APP_INIT_ERROR;
  }
  mgos_bthing_on_event(MGOS_BFLOWSENS_THINGCAST(CTX.tap.flow), MGOS_EV_BTHING_STATE_CHANGED,
                       mg_eddy_flowsens_state_changed, NULL);

  /* Create and initialize the purified-water flow alert sensor */
  CTX.pure.alert = mgos_balert_create(mgos_sys_config_get_eddy_pure_alert_id(), EDDY_ALERT_DOMAIN_NAME);
  if (!CTX.pure.alert) {
    return MGOS_APP_INIT_ERROR;
  }

  /* Create and initialize the tap-water flow alert sensor */
  CTX.tap.alert = mgos_balert_create(mgos_sys_config_get_eddy_tap_alert_id(), EDDY_ALERT_DOMAIN_NAME);
  if (!CTX.tap.alert) {
    return MGOS_APP_INIT_ERROR;
  }

  /* Create and initialize the force-button */
  CTX.force_btn = mgos_bbutton_create("force-btn", NULL);
  if (!mgos_bthing_gpio_attach(MGOS_BBUTTON_THINGCAST(CTX.force_btn), EDDY_TAP_FORCEBTN_PIN, EDDY_TAP_FORCEBTN_ACTIVE_HIGH, MGOS_BTHING_GPIO_PULL_AUTO)) {
    LOG(LL_WARN, ("The force button won't work because an error."));
  } else {
    mgos_bbutton_on_event(CTX.force_btn, mg_eddy_forcebtn_on_event, NULL);
  }

  mgos_bvalve_set_state(CTX.valve, MGOS_BVALVE_STATE_CLOSED);

  return MGOS_APP_INIT_SUCCESS;
}