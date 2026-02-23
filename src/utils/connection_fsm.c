#include "connection_fsm.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/**
 * FSM instance structure
 */
struct ConnectionFsm {
    char *connection_name;       /* Connection name for logging */
    ConnectionState current_state;
};

/**
 * State transition rule
 */
typedef struct {
    ConnectionState from_state;
    ConnectionFsmEvent event;
    ConnectionState to_state;
} StateTransition;

/**
 * Transition table - defines ALL valid state transitions
 */
static const StateTransition transition_table[] = {
    /* From DISCONNECTED */
    {CONN_STATE_DISCONNECTED, FSM_EVENT_CONNECT_REQUESTED,     CONN_STATE_CONNECTING},
    {CONN_STATE_DISCONNECTED, FSM_EVENT_SESSION_CONNECTING,    CONN_STATE_CONNECTING},
    {CONN_STATE_DISCONNECTED, FSM_EVENT_SESSION_DISCONNECTED,  CONN_STATE_DISCONNECTED},  /* self (poll no-op) */
    {CONN_STATE_DISCONNECTED, FSM_EVENT_SESSION_CONNECTED,     CONN_STATE_CONNECTED},     /* app restart: VPN already up */
    {CONN_STATE_DISCONNECTED, FSM_EVENT_SESSION_AUTH_REQUIRED,  CONN_STATE_AUTH_REQUIRED}, /* app restart: VPN waiting for auth */
    {CONN_STATE_DISCONNECTED, FSM_EVENT_SESSION_PAUSED,        CONN_STATE_PAUSED},        /* app restart: VPN paused */
    {CONN_STATE_DISCONNECTED, FSM_EVENT_SESSION_ERROR,         CONN_STATE_ERROR},          /* app restart: VPN in error */

    /* From CONNECTING */
    {CONN_STATE_CONNECTING, FSM_EVENT_SESSION_CONNECTED,       CONN_STATE_CONNECTED},
    {CONN_STATE_CONNECTING, FSM_EVENT_SESSION_AUTH_REQUIRED,   CONN_STATE_AUTH_REQUIRED},
    {CONN_STATE_CONNECTING, FSM_EVENT_SESSION_ERROR,           CONN_STATE_ERROR},
    {CONN_STATE_CONNECTING, FSM_EVENT_SESSION_DISCONNECTED,    CONN_STATE_DISCONNECTED},
    {CONN_STATE_CONNECTING, FSM_EVENT_DISCONNECT_REQUESTED,    CONN_STATE_DISCONNECTED},
    {CONN_STATE_CONNECTING, FSM_EVENT_SESSION_CONNECTING,      CONN_STATE_CONNECTING},    /* self (poll no-op) */

    /* From CONNECTED */
    {CONN_STATE_CONNECTED, FSM_EVENT_SESSION_PAUSED,           CONN_STATE_PAUSED},
    {CONN_STATE_CONNECTED, FSM_EVENT_SESSION_RECONNECTING,     CONN_STATE_RECONNECTING},
    {CONN_STATE_CONNECTED, FSM_EVENT_SESSION_DISCONNECTED,     CONN_STATE_DISCONNECTED},
    {CONN_STATE_CONNECTED, FSM_EVENT_DISCONNECT_REQUESTED,     CONN_STATE_DISCONNECTED},
    {CONN_STATE_CONNECTED, FSM_EVENT_SESSION_ERROR,            CONN_STATE_ERROR},
    {CONN_STATE_CONNECTED, FSM_EVENT_SESSION_CONNECTED,        CONN_STATE_CONNECTED},     /* self (poll no-op) */

    /* From PAUSED */
    {CONN_STATE_PAUSED, FSM_EVENT_SESSION_RESUMED,             CONN_STATE_CONNECTED},
    {CONN_STATE_PAUSED, FSM_EVENT_SESSION_CONNECTED,           CONN_STATE_CONNECTED},
    {CONN_STATE_PAUSED, FSM_EVENT_SESSION_DISCONNECTED,        CONN_STATE_DISCONNECTED},
    {CONN_STATE_PAUSED, FSM_EVENT_DISCONNECT_REQUESTED,        CONN_STATE_DISCONNECTED},
    {CONN_STATE_PAUSED, FSM_EVENT_SESSION_ERROR,               CONN_STATE_ERROR},
    {CONN_STATE_PAUSED, FSM_EVENT_SESSION_PAUSED,              CONN_STATE_PAUSED},        /* self (poll no-op) */

    /* From AUTH_REQUIRED */
    {CONN_STATE_AUTH_REQUIRED, FSM_EVENT_SESSION_CONNECTED,    CONN_STATE_CONNECTED},
    {CONN_STATE_AUTH_REQUIRED, FSM_EVENT_SESSION_CONNECTING,   CONN_STATE_CONNECTING},
    {CONN_STATE_AUTH_REQUIRED, FSM_EVENT_SESSION_DISCONNECTED, CONN_STATE_DISCONNECTED},
    {CONN_STATE_AUTH_REQUIRED, FSM_EVENT_DISCONNECT_REQUESTED, CONN_STATE_DISCONNECTED},
    {CONN_STATE_AUTH_REQUIRED, FSM_EVENT_SESSION_ERROR,        CONN_STATE_ERROR},
    {CONN_STATE_AUTH_REQUIRED, FSM_EVENT_SESSION_AUTH_REQUIRED, CONN_STATE_AUTH_REQUIRED}, /* self (poll no-op) */

    /* From ERROR */
    {CONN_STATE_ERROR, FSM_EVENT_SESSION_DISCONNECTED,         CONN_STATE_DISCONNECTED},
    {CONN_STATE_ERROR, FSM_EVENT_DISCONNECT_REQUESTED,         CONN_STATE_DISCONNECTED},
    {CONN_STATE_ERROR, FSM_EVENT_CONNECT_REQUESTED,            CONN_STATE_CONNECTING},
    {CONN_STATE_ERROR, FSM_EVENT_SESSION_CONNECTING,           CONN_STATE_CONNECTING},
    {CONN_STATE_ERROR, FSM_EVENT_SESSION_CONNECTED,            CONN_STATE_CONNECTED},
    {CONN_STATE_ERROR, FSM_EVENT_SESSION_ERROR,                CONN_STATE_ERROR},          /* self (poll no-op) */

    /* From RECONNECTING */
    {CONN_STATE_RECONNECTING, FSM_EVENT_SESSION_CONNECTED,     CONN_STATE_CONNECTED},
    {CONN_STATE_RECONNECTING, FSM_EVENT_SESSION_AUTH_REQUIRED, CONN_STATE_AUTH_REQUIRED},
    {CONN_STATE_RECONNECTING, FSM_EVENT_SESSION_DISCONNECTED,  CONN_STATE_DISCONNECTED},
    {CONN_STATE_RECONNECTING, FSM_EVENT_DISCONNECT_REQUESTED,  CONN_STATE_DISCONNECTED},
    {CONN_STATE_RECONNECTING, FSM_EVENT_SESSION_ERROR,         CONN_STATE_ERROR},
    {CONN_STATE_RECONNECTING, FSM_EVENT_SESSION_RECONNECTING,  CONN_STATE_RECONNECTING},  /* self (poll no-op) */
};

static const size_t transition_table_size = sizeof(transition_table) / sizeof(transition_table[0]);

/**
 * Button state table - defines which buttons are enabled in each state
 */
typedef struct {
    ConnectionState state;
    ConnectionButtonStates buttons;
} ButtonStateRule;

static const ButtonStateRule button_state_table[] = {
    {CONN_STATE_DISCONNECTED, {
        .connect_enabled = true,
        .disconnect_enabled = false,
        .pause_enabled = false,
        .resume_enabled = false,
        .auth_enabled = false
    }},
    {CONN_STATE_CONNECTING, {
        .connect_enabled = false,
        .disconnect_enabled = true,
        .pause_enabled = false,
        .resume_enabled = false,
        .auth_enabled = false
    }},
    {CONN_STATE_CONNECTED, {
        .connect_enabled = false,
        .disconnect_enabled = true,
        .pause_enabled = true,
        .resume_enabled = false,
        .auth_enabled = false
    }},
    {CONN_STATE_PAUSED, {
        .connect_enabled = false,
        .disconnect_enabled = true,
        .pause_enabled = false,
        .resume_enabled = true,
        .auth_enabled = false
    }},
    {CONN_STATE_AUTH_REQUIRED, {
        .connect_enabled = false,
        .disconnect_enabled = true,
        .pause_enabled = false,
        .resume_enabled = false,
        .auth_enabled = true
    }},
    {CONN_STATE_ERROR, {
        .connect_enabled = true,  /* Allow retry */
        .disconnect_enabled = true,
        .pause_enabled = false,
        .resume_enabled = false,
        .auth_enabled = false
    }},
    {CONN_STATE_RECONNECTING, {
        .connect_enabled = false,
        .disconnect_enabled = true,
        .pause_enabled = false,
        .resume_enabled = false,
        .auth_enabled = false
    }},
};

static const size_t button_state_table_size = sizeof(button_state_table) / sizeof(button_state_table[0]);

/**
 * Get state name as string
 */
const char* connection_fsm_state_name(ConnectionState state) {
    switch (state) {
        case CONN_STATE_DISCONNECTED:   return "DISCONNECTED";
        case CONN_STATE_CONNECTING:     return "CONNECTING";
        case CONN_STATE_CONNECTED:      return "CONNECTED";
        case CONN_STATE_PAUSED:         return "PAUSED";
        case CONN_STATE_AUTH_REQUIRED:  return "AUTH_REQUIRED";
        case CONN_STATE_ERROR:          return "ERROR";
        case CONN_STATE_RECONNECTING:   return "RECONNECTING";
        default:                        return "UNKNOWN";
    }
}

/**
 * Get event name as string
 */
const char* connection_fsm_event_name(ConnectionFsmEvent event) {
    switch (event) {
        case FSM_EVENT_CONNECT_REQUESTED:     return "CONNECT_REQUESTED";
        case FSM_EVENT_SESSION_CONNECTING:    return "SESSION_CONNECTING";
        case FSM_EVENT_SESSION_CONNECTED:     return "SESSION_CONNECTED";
        case FSM_EVENT_SESSION_PAUSED:        return "SESSION_PAUSED";
        case FSM_EVENT_SESSION_RESUMED:       return "SESSION_RESUMED";
        case FSM_EVENT_SESSION_AUTH_REQUIRED: return "SESSION_AUTH_REQUIRED";
        case FSM_EVENT_SESSION_ERROR:         return "SESSION_ERROR";
        case FSM_EVENT_SESSION_DISCONNECTED:  return "SESSION_DISCONNECTED";
        case FSM_EVENT_DISCONNECT_REQUESTED:  return "DISCONNECT_REQUESTED";
        case FSM_EVENT_SESSION_RECONNECTING:  return "SESSION_RECONNECTING";
        default:                              return "UNKNOWN_EVENT";
    }
}

/**
 * Create a new FSM instance
 */
ConnectionFsm* connection_fsm_create(const char *connection_name) {
    ConnectionFsm *fsm = malloc(sizeof(ConnectionFsm));
    if (!fsm) {
        logger_error("Failed to allocate FSM for connection '%s'",
                     connection_name ? connection_name : "unknown");
        return NULL;
    }

    fsm->connection_name = connection_name ? strdup(connection_name) : NULL;
    fsm->current_state = CONN_STATE_DISCONNECTED;  /* Initial state */

    logger_debug("FSM created for connection '%s' in state %s",
                 fsm->connection_name ? fsm->connection_name : "unknown",
                 connection_fsm_state_name(fsm->current_state));

    return fsm;
}

/**
 * Destroy an FSM instance
 */
void connection_fsm_destroy(ConnectionFsm *fsm) {
    if (!fsm) {
        return;
    }

    if (fsm->connection_name) {
        free(fsm->connection_name);
    }

    free(fsm);
}

/**
 * Find transition in table
 */
static const StateTransition* find_transition(ConnectionState from_state, ConnectionFsmEvent event) {
    for (size_t i = 0; i < transition_table_size; i++) {
        if (transition_table[i].from_state == from_state &&
            transition_table[i].event == event) {
            return &transition_table[i];
        }
    }
    return NULL;
}

/**
 * Process an event and transition to new state
 */
ConnectionState connection_fsm_process_event(ConnectionFsm *fsm, ConnectionFsmEvent event) {
    if (!fsm) {
        logger_error("NULL FSM in process_event");
        return CONN_STATE_DISCONNECTED;
    }

    ConnectionState old_state = fsm->current_state;
    const StateTransition *transition = find_transition(old_state, event);

    if (transition) {
        /* Valid transition found */
        fsm->current_state = transition->to_state;

        if (old_state != fsm->current_state) {
            logger_info("FSM '%s': %s + %s -> %s",
                       fsm->connection_name ? fsm->connection_name : "unknown",
                       connection_fsm_state_name(old_state),
                       connection_fsm_event_name(event),
                       connection_fsm_state_name(fsm->current_state));
        } else {
            logger_debug("FSM '%s': %s + %s -> %s (no change)",
                        fsm->connection_name ? fsm->connection_name : "unknown",
                        connection_fsm_state_name(old_state),
                        connection_fsm_event_name(event),
                        connection_fsm_state_name(fsm->current_state));
        }
    } else {
        /* Invalid transition - log warning and stay in current state */
        logger_warn("FSM '%s': Invalid transition from %s with event %s (ignored)",
                   fsm->connection_name ? fsm->connection_name : "unknown",
                   connection_fsm_state_name(old_state),
                   connection_fsm_event_name(event));
    }

    return fsm->current_state;
}

/**
 * Force FSM to a specific state (bypassing transition rules)
 */
void connection_fsm_force_state(ConnectionFsm *fsm, ConnectionState state) {
    if (!fsm) {
        logger_error("NULL FSM in force_state");
        return;
    }

    ConnectionState old_state = fsm->current_state;
    fsm->current_state = state;

    logger_warn("FSM '%s': Force-syncing state %s -> %s (D-Bus reality override)",
                fsm->connection_name ? fsm->connection_name : "unknown",
                connection_fsm_state_name(old_state),
                connection_fsm_state_name(state));
}

/**
 * Get current state
 */
ConnectionState connection_fsm_get_state(const ConnectionFsm *fsm) {
    if (!fsm) {
        logger_error("NULL FSM in get_state");
        return CONN_STATE_DISCONNECTED;
    }
    return fsm->current_state;
}

/**
 * Get button states for current state
 */
ConnectionButtonStates connection_fsm_get_button_states(const ConnectionFsm *fsm) {
    ConnectionButtonStates default_states = {
        .connect_enabled = false,
        .disconnect_enabled = false,
        .pause_enabled = false,
        .resume_enabled = false,
        .auth_enabled = false
    };

    if (!fsm) {
        logger_error("NULL FSM in get_button_states");
        return default_states;
    }

    /* Find button states for current state */
    for (size_t i = 0; i < button_state_table_size; i++) {
        if (button_state_table[i].state == fsm->current_state) {
            return button_state_table[i].buttons;
        }
    }

    /* Should never happen if table is complete */
    logger_error("FSM '%s': No button states defined for state %s",
                 fsm->connection_name ? fsm->connection_name : "unknown",
                 connection_fsm_state_name(fsm->current_state));

    return default_states;
}
