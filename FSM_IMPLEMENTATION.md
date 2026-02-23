# Connection State Machine Implementation

## Overview
Implemented a lightweight Finite State Machine (FSM) to manage VPN connection states and UI button states. This replaces the previous ad-hoc state management with a formal, validated state machine.

## Problem Solved
Previously, the CONNECT button was not consistently being enabled when VPN disconnected. The state management was scattered and implicit, making it difficult to track and debug state transitions.

## Implementation

### Files Created
1. **src/utils/connection_fsm.h** - FSM interface and state/event definitions
2. **src/utils/connection_fsm.c** - FSM implementation with transition tables

### Key Components

#### States (ConnectionState enum)
- `CONN_STATE_DISCONNECTED` - Config available, no active session
- `CONN_STATE_CONNECTING` - Session is connecting
- `CONN_STATE_CONNECTED` - Session is connected
- `CONN_STATE_PAUSED` - Session is paused
- `CONN_STATE_AUTH_REQUIRED` - Session needs authentication
- `CONN_STATE_ERROR` - Session encountered an error
- `CONN_STATE_RECONNECTING` - Session is reconnecting

#### Events (ConnectionFsmEvent enum)
- `FSM_EVENT_CONNECT_REQUESTED` - User clicked Connect
- `FSM_EVENT_SESSION_CONNECTING` - D-Bus reports connecting
- `FSM_EVENT_SESSION_CONNECTED` - D-Bus reports connected
- `FSM_EVENT_SESSION_PAUSED` - D-Bus reports paused
- `FSM_EVENT_SESSION_RESUMED` - D-Bus reports resumed
- `FSM_EVENT_SESSION_AUTH_REQUIRED` - D-Bus reports auth needed
- `FSM_EVENT_SESSION_ERROR` - D-Bus reports error
- `FSM_EVENT_SESSION_DISCONNECTED` - D-Bus reports disconnected
- `FSM_EVENT_DISCONNECT_REQUESTED` - User clicked Disconnect
- `FSM_EVENT_SESSION_RECONNECTING` - D-Bus reports reconnecting

#### Transition Table
The FSM validates all state transitions through an explicit transition table. Invalid transitions are logged as warnings and ignored, keeping the system in a valid state.

Example transitions:
```c
DISCONNECTED + CONNECT_REQUESTED     -> CONNECTING
CONNECTING   + SESSION_CONNECTED     -> CONNECTED
CONNECTED    + DISCONNECT_REQUESTED  -> DISCONNECTED
CONNECTED    + SESSION_PAUSED        -> PAUSED
PAUSED       + SESSION_RESUMED       -> CONNECTED
```

#### Button State Table
Each state has an associated button configuration that defines which action buttons should be enabled:

```c
DISCONNECTED -> Connect: YES, Others: NO
CONNECTING   -> Disconnect: YES, Others: NO
CONNECTED    -> Disconnect: YES, Pause: YES, Others: NO
PAUSED       -> Disconnect: YES, Resume: YES, Others: NO
ERROR        -> Connect: YES, Disconnect: YES, Others: NO (allow retry)
AUTH_REQUIRED -> Disconnect: YES, Auth: YES, Others: NO
RECONNECTING -> Disconnect: YES, Others: NO
```

### Integration Points

#### tray.c Changes
1. **Added FSM instance** to `ConnectionMenuItem` structure
2. **Updated `create_connection_menu_item()`** to create FSM and initialize state
3. **Updated `update_connection_menu_item()`** to process state changes as FSM events
4. **Updated `set_action_states()`** to get button states from FSM
5. **Updated `free_connection_menu_item()`** to destroy FSM

#### Key Functions
- `connection_fsm_create()` - Create FSM instance for a connection
- `connection_fsm_process_event()` - Process state transition event
- `connection_fsm_get_button_states()` - Get button enable/disable states for current state
- `connection_fsm_get_state()` - Get current FSM state
- `connection_fsm_state_name()` - Get state name for logging
- `connection_fsm_event_name()` - Get event name for logging

## Benefits

### 1. Correctness
- All state transitions are validated against the transition table
- Invalid transitions are rejected and logged
- Button states are guaranteed to match the current FSM state

### 2. Debuggability
- All state transitions are logged with human-readable state and event names
- FSM logs show: `FSM 'dev': CONNECTED + SESSION_DISCONNECTED -> DISCONNECTED`
- Invalid transitions are logged as warnings

### 3. Maintainability
- State logic is centralized in FSM tables
- Adding new states/transitions is straightforward
- No scattered switch statements for state management

### 4. Testability
- FSM can be tested independently
- Transition table can be validated for completeness
- Button states can be verified for each state

## Example State Transition Log

```
2026-01-06 12:55:46 [INFO] FSM created for connection 'dev' in state DISCONNECTED
2026-01-06 12:55:46 [INFO] FSM 'dev': DISCONNECTED + SESSION_CONNECTED -> CONNECTED
2026-01-06 12:55:54 [INFO] Connection 'dev' state: CONNECTED -> DISCONNECTED, session_path: /net/.../... -> (null)
2026-01-06 12:55:54 [INFO] FSM 'dev': CONNECTED + SESSION_DISCONNECTED -> DISCONNECTED
2026-01-06 12:55:54 [DEBUG] set_action_states called: config=dev, FSM state=DISCONNECTED
2026-01-06 12:55:54 [DEBUG]   Button states from FSM: connect=1, disconnect=0, pause=0, resume=0, auth=0
```

## Testing Recommendations

1. **Verify all state transitions work correctly** by testing:
   - Connect → Connected → Disconnect → Disconnected
   - Connect → Auth Required → Authenticate → Connected
   - Connected → Pause → Paused → Resume → Connected
   - Connected → Error → Retry (Connect enabled)
   - Connecting → Cancel → Disconnected

2. **Verify button states** at each state:
   - DISCONNECTED: Only CONNECT enabled
   - CONNECTED: DISCONNECT and PAUSE enabled
   - PAUSED: DISCONNECT and RESUME enabled
   - etc.

3. **Verify invalid transitions are rejected**:
   - Try to pause when disconnected
   - Try to resume when connected
   - Check logs for warnings

## Future Enhancements

1. **Add state entry/exit actions**
   - Execute specific code when entering/exiting states
   - e.g., Start timer on CONNECTED entry, clear auth on DISCONNECTED entry

2. **Add FSM visualization**
   - Generate state diagram from transition table
   - Useful for documentation

3. **Add transition guards**
   - Conditional transitions based on additional context
   - e.g., Only allow PAUSED if session supports pausing

4. **Add FSM unit tests**
   - Test all valid transitions
   - Test rejection of invalid transitions
   - Test button states for each state
