#ifndef CONNECTION_FSM_H
#define CONNECTION_FSM_H

#include <stdbool.h>

/**
 * Connection states (shared with tray.c)
 */
typedef enum {
    CONN_STATE_DISCONNECTED,     /* Config available, no session */
    CONN_STATE_CONNECTING,       /* Session connecting */
    CONN_STATE_CONNECTED,        /* Session connected */
    CONN_STATE_PAUSED,           /* Session paused */
    CONN_STATE_AUTH_REQUIRED,    /* Session needs auth */
    CONN_STATE_ERROR,            /* Session in error state */
    CONN_STATE_RECONNECTING      /* Session reconnecting */
} ConnectionState;

/**
 * Events that trigger state transitions
 */
typedef enum {
    FSM_EVENT_CONNECT_REQUESTED,     /* User clicked Connect */
    FSM_EVENT_SESSION_CONNECTING,    /* D-Bus reports connecting */
    FSM_EVENT_SESSION_CONNECTED,     /* D-Bus reports connected */
    FSM_EVENT_SESSION_PAUSED,        /* D-Bus reports paused */
    FSM_EVENT_SESSION_RESUMED,       /* D-Bus reports resumed */
    FSM_EVENT_SESSION_AUTH_REQUIRED, /* D-Bus reports auth needed */
    FSM_EVENT_SESSION_ERROR,         /* D-Bus reports error */
    FSM_EVENT_SESSION_DISCONNECTED,  /* D-Bus reports disconnected */
    FSM_EVENT_DISCONNECT_REQUESTED,  /* User clicked Disconnect */
    FSM_EVENT_SESSION_RECONNECTING,  /* D-Bus reports reconnecting */
} ConnectionFsmEvent;

/**
 * Button states derived from connection state
 */
typedef struct {
    bool connect_enabled;
    bool disconnect_enabled;
    bool pause_enabled;
    bool resume_enabled;
    bool auth_enabled;
} ConnectionButtonStates;

/**
 * Opaque FSM instance (one per connection)
 */
typedef struct ConnectionFsm ConnectionFsm;

/**
 * Create a new FSM instance for a connection
 * @param connection_name Name for logging/debugging
 * @return New FSM instance (must be freed with connection_fsm_destroy)
 */
ConnectionFsm* connection_fsm_create(const char *connection_name);

/**
 * Destroy an FSM instance
 * @param fsm FSM instance to destroy
 */
void connection_fsm_destroy(ConnectionFsm *fsm);

/**
 * Process an event and transition to new state
 * @param fsm FSM instance
 * @param event Event to process
 * @return New state after transition (may be same as before if transition invalid)
 */
ConnectionState connection_fsm_process_event(ConnectionFsm *fsm, ConnectionFsmEvent event);

/**
 * Get current state
 * @param fsm FSM instance
 * @return Current connection state
 */
ConnectionState connection_fsm_get_state(const ConnectionFsm *fsm);

/**
 * Get button states for current FSM state
 * @param fsm FSM instance
 * @return Button states (which buttons should be enabled)
 */
ConnectionButtonStates connection_fsm_get_button_states(const ConnectionFsm *fsm);

/**
 * Get state name as string (for logging)
 * @param state Connection state
 * @return Human-readable state name
 */
const char* connection_fsm_state_name(ConnectionState state);

/**
 * Get event name as string (for logging)
 * @param event FSM event
 * @return Human-readable event name
 */
const char* connection_fsm_event_name(ConnectionFsmEvent event);

/**
 * Force FSM to a specific state, bypassing transition rules.
 * Used to re-sync when FSM state diverges from observed D-Bus state.
 * Logs a warning when called.
 * @param fsm FSM instance
 * @param state Target state to force
 */
void connection_fsm_force_state(ConnectionFsm *fsm, ConnectionState state);

#endif /* CONNECTION_FSM_H */
