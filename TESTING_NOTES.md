# Testing Notes - FSM and Enhanced Logging

## Changes Made for Testing

### 1. File Logging Enabled
- **Location**: `~/.local/share/ovpn-manager/app.log`
- **Mode**: Append (logs persist across restarts)
- Logs are written to both file and syslog

### 2. Enhanced Logging Added
All logging requires verbosity level 2+ (use `-v 2` or `-v 3` flag)

#### New Logging Points:
1. **D-Bus Session State Mapping**:
   - Shows when D-Bus session state is mapped to connection state
   - Format: `D-Bus session state: SESSION_STATE_XXX (N) -> Connection state: XXX, session_path=..., config_name=...`

2. **Connection Merging**:
   - Shows total configs and sessions being merged
   - Format: `Merging connections: N configs, M active sessions`

3. **Config-Session Matching**:
   - When a config has an active session: `Config 'name' matched to session (state=XXX, session_path=...)`
   - When a config has no session: `Config 'name' has no active session (state=DISCONNECTED)`

4. **FSM State Transitions**:
   - All state transitions: `FSM 'name': OLD_STATE + EVENT -> NEW_STATE`
   - Invalid transitions: `FSM 'name': Invalid transition from STATE with event EVENT (ignored)`

5. **Button State Updates**:
   - Shows which buttons are enabled: `Button states from FSM: connect=1, disconnect=0, pause=0, resume=0, auth=0`

## How to Test

### 1. Start the Application with Verbose Logging
```bash
./builddir/src/ovpn-manager -v 2
```

Or for maximum verbosity:
```bash
./builddir/src/ovpn-manager -v 3
```

### 2. Reproduce the Timeout Issue
1. Connect to a VPN
2. Wait for connection timeout (or force disconnect from server side)
3. Observe the menu behavior

### 3. Check the Logs
```bash
tail -f ~/.local/share/ovpn-manager/app.log
```

Or to view full log:
```bash
less ~/.local/share/ovpn-manager/app.log
```

### 4. What to Look For in Logs

#### When Connection Times Out:
Look for this sequence:
```
[INFO] D-Bus session state: SESSION_STATE_CONNECTED (2) -> Connection state: CONNECTED, session_path=/net/..., config_name=dev
[INFO] Merging connections: 3 configs, 1 active sessions
[INFO]   Config 'dev' matched to session (state=CONNECTED, session_path=/net/...)
[INFO] Connection 'dev' state: CONNECTED (no change)
```

Then when timeout occurs, you should see:
```
[INFO] D-Bus session state: SESSION_STATE_DISCONNECTED (0) -> Connection state: DISCONNECTED, session_path=/net/..., config_name=dev
[INFO] Merging connections: 3 configs, 0 active sessions
[INFO]   Config 'dev' has no active session (state=DISCONNECTED)
[INFO] Connection 'dev' state: CONNECTED -> DISCONNECTED, session_path: /net/... -> (null)
[INFO] FSM 'dev': CONNECTED + SESSION_DISCONNECTED -> DISCONNECTED
[DEBUG] set_action_states called: config=dev, FSM state=DISCONNECTED
[DEBUG]   Button states from FSM: connect=1, disconnect=0, pause=0, resume=0, auth=0
```

#### Missing Transition Scenario:
If the CONNECT button doesn't get enabled, look for:
- Did the FSM state change to DISCONNECTED?
- Was the SESSION_DISCONNECTED event processed?
- Were the button states updated?

#### Invalid State Transitions:
If you see warnings like:
```
[WARN] FSM 'dev': Invalid transition from CONNECTED with event SESSION_PAUSED (ignored)
```
This indicates the FSM rejected an invalid transition.

### 5. Clear Logs Between Tests
```bash
rm ~/.local/share/ovpn-manager/app.log
```

## Expected FSM State Flows

### Normal Connection Flow:
```
DISCONNECTED + CONNECT_REQUESTED -> CONNECTING
CONNECTING + SESSION_CONNECTED -> CONNECTED
CONNECTED + DISCONNECT_REQUESTED -> DISCONNECTED
```

### Timeout/Error Flow:
```
CONNECTED + SESSION_DISCONNECTED -> DISCONNECTED
CONNECTED + SESSION_ERROR -> ERROR
ERROR + SESSION_DISCONNECTED -> DISCONNECTED
```

### Auth Flow:
```
CONNECTING + SESSION_AUTH_REQUIRED -> AUTH_REQUIRED
AUTH_REQUIRED + SESSION_CONNECTED -> CONNECTED
```

### Pause/Resume Flow:
```
CONNECTED + SESSION_PAUSED -> PAUSED
PAUSED + SESSION_RESUMED -> CONNECTED
```

## Debugging Tips

### High-Level View (Verbosity 2):
```bash
./builddir/src/ovpn-manager -v 2 2>&1 | grep -E "FSM|Connection.*state|Merging"
```

### Focus on Specific Connection:
```bash
tail -f ~/.local/share/ovpn-manager/app.log | grep "dev"
```

### Watch Button States:
```bash
tail -f ~/.local/share/ovpn-manager/app.log | grep "Button states"
```

### See All State Transitions:
```bash
tail -f ~/.local/share/ovpn-manager/app.log | grep "FSM.*->"
```

## Known Issues to Verify

1. **CONNECT button not enabled after timeout**
   - Check: Does FSM transition to DISCONNECTED?
   - Check: Are button states showing `connect=1`?
   - Check: Is `set_action_states()` being called after state change?

2. **Session state not detected**
   - Check: Does D-Bus report SESSION_STATE_DISCONNECTED?
   - Check: Is merge_connections_data finding 0 active sessions?
   - Check: Is config matched to "no active session"?

## After Testing

Once you've identified the issue from the logs, share the relevant log section that shows the problem. The detailed logging should help us understand exactly where the state tracking is failing.

## Reverting File Logging

If you want to disable file logging after testing, edit `src/main.c` line 269:
```c
// Change this:
logger_init(true, NULL, log_level, true);

// To this:
logger_init(false, NULL, log_level, true);
```

Then recompile:
```bash
PATH=/home/renny/.local/bin:/usr/bin:/bin /home/renny/.local/bin/ninja -C builddir
```
