# Architecture

All the host applications in this repository share one modular, event-driven architecture, the same one used by the [Asset Tracker Template](https://github.com/nrfconnect/Asset-Tracker-Template). The modules interact through messages that are processed as events by the modules' state machines.

The architecture is implemented using [Zephyr bus (zbus)](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) for inter-module communication and the [State Machine Framework](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html) (SMF) for managing module behavior.

This document describes the pattern that every application follows, with a focus on the zbus message passing and the modules' state machines. The code examples are taken from the [PPP Host Application](applications/ppp/README.md), and the nRF93M1 AT and nRF91 AT Host Applications implement the same pattern. Which modules each application has is listed in [Modules per application](#modules-per-application).

## Diagram notation

The diagrams in this guide and in the module guides are generated from the PlantUML sources in each application's `diagrams/` folder, for example [`docs/applications/ppp/diagrams/`](https://github.com/nrfconnect/ncs-serial-modem-host-applications/tree/main/docs/applications/ppp/diagrams), which describe the implementation as it is. They use the following conventions:

| Notation | Meaning |
|----------|---------|
| `entry / action` and `exit / action` | An action performed by the state's entry or exit function |
| `EVENT / action` | An action performed when the state handles `EVENT`, without leaving the state |
| `EVENT / ;` | An event the state deliberately handles by doing nothing, so that it does not propagate to the parent state |
| `EVENT [guard]` on a transition | A transition taken only when the guard holds |
| Dashed state or transition | Present only in some build configurations. The Kconfig symbol is named in the state |

To change a diagram, edit its `.puml` file and regenerate the `.svg` with [`scripts/render_diagrams.py`](https://github.com/nrfconnect/ncs-serial-modem-host-applications/blob/main/scripts/render_diagrams.py), as described in [CI and contribution](ci-and-contribution.md#documentation-diagrams).

## System overview

Each application runs on the host MCU and uses a Serial Modem for cellular connectivity. The application is split into modules that each own one zbus channel. Both the requests sent to a module and the notifications it publishes go on that channel. All communication passes through the Main module, which implements the business logic and is the only module that knows about the others.

```mermaid
flowchart LR
    Main["Main module: business logic"]
    Network["Network module"] <-->|network_chan| Main
    Cloud["Cloud module"] <-->|cloud_chan| Main
    Location["Location module"] <-->|location_chan| Main
    Other["Other modules: FOTA, battery, buttons"] <-->|"one channel each"| Main
```

The following steps show the simplified flow of a typical operation:

1. The Network module brings up the cellular link on startup and publishes `NETWORK_CONNECTED` on `network_chan` once it is usable.
1. The Main module responds by starting a periodic **cloud synchronization**. Depending on the application, it connects to nRF Cloud, sends data, requests a location fix, and polls for firmware updates, one step at a time, by publishing requests on the other modules' channels and waiting for their replies.
1. Each module does its work in its own thread and publishes the result on its channel, which the Main module uses to move to the next step.

See the module overview of each application for its modules and the diagram of the channels between them, for example the [PPP modules](applications/ppp/README.md#modules).

## Modules per application

The applications share the pattern but not the code. Each application has its own copy of its modules, adapted to its modem and cloud connection. The PPP Host Application builds the same modules for every board and selects board-specific behavior through Kconfig and the board files:

| Module | PPP | nRF93M1 AT | nRF91 AT |
|--------|-----|------------|----------|
| Main | State machine | State machine | State machine |
| Network | State machine. PPP link to the Serial Modem | State machine. PPP link used only for connectivity status | State machine. LTE registration from `+CEREG`, no PPP |
| Cloud | State machine. nRF Cloud CoAP from the host | Message loop. Modem nRF Cloud client through AT commands | State machine. Serial Modem provisioning (`AT#XNRFPROV`) and nRF Cloud client through AT commands |
| FOTA | State machine. Host firmware updates through nRF Cloud | Not included | Not included |
| Location | On the nRF54 hosts, state machine. Wi-Fi scan with the nRF7002-EB II, only with the location overlay. On the nRF93M1 DK, message loop. Cell and Wi-Fi measurements from the modem | Message loop. Location fix from the modem nRF Cloud client | Message loop. Cell fix through nRF Cloud, GNSS fallback, both from the Serial Modem |
| Battery | Message loop. Fuel gauge sampling, only on the nRF93M1 DK | Message loop. Fuel gauge sampling | Not included |
| Buttons | No thread. Publishes DK button presses | No thread. Publishes DK button presses | Not included |
| LED | No thread. Listener that drives the host RGB LED, only on the nRF93M1 DK | No thread. Listener that drives the host RGB LED | Not included |
| Modem AT | No channel. Serializes access to the modem AT pipe | No channel. Serializes access to the modem AT pipe | No channel. Owns the Serial Modem UART, serializes AT commands, and routes URCs |
| Modem Power | No channel. Applies PSM and modem specific power saving each time the network connects | No channel. Applies PSM and nRF93M1 module power saving each time the network connects | Not included |

A **state machine** module runs its messages through an SMF state machine, as described in this document. A **message loop** module has its own thread, channel, and watchdog like the others, but handles each message in a plain `switch` statement because it has no state worth modeling. The Buttons module publishes from the button callback and needs no thread. The Modem AT module is a helper that other modules and the shell call directly.

The module guides for the PPP Host Application, linked from its [module overview](applications/ppp/README.md#modules), describe each module's states and messages in detail.

## Module design

Each module follows a similar design:

- **Message channel**: Each module defines its own zbus channel. A single channel is used for all messages that are specific to a module.
- **Message types**: Each module exposes a set of input and output message types. These can be considered events in the state machine sense and might have associated data.
- **State machine**: Modules with state implement a state machine using SMF to manage their internal state and behavior. Simpler modules handle messages directly, as listed in [Modules per application](#modules-per-application).
- **Thread**: Each module has its own thread, so that blocking calls in one module cannot stall another.
- **Watchdog**: Each module thread is monitored by a task watchdog. Each thread periodically calls `task_wdt_feed()`. If a thread fails to feed its watchdog within its configured timeout, the system resets.
- **Initialization**: Modules are initialized in their dedicated thread, which is started with `K_THREAD_DEFINE()`.

The modules are designed as loosely coupled units with well-defined message-based interfaces. They communicate exclusively through their zbus channels, without reference to other modules' internals. This means you can develop, test, and maintain each module independently, and every module except the Main module can be reused in other applications.

Modules often handle state transitions based on messages they themselves publish. For example, when the Network module publishes a `NETWORK_CONNECTED` message, it also receives this message in its own state machine, allowing it to transition to the connected state with consistent handling.

The Main module deviates from the pattern in the following two ways:

- It runs in the `main()` thread instead of a thread of its own.
- It is the only module that calls `task_wdt_init()` to set up the hardware watchdog (`DT_ALIAS(watchdog0)`) that all the other modules add their tasks to.

### Module threads

Each module's thread follows the same pattern of waiting for new messages and handling them by executing a state machine. Message loop modules use the same loop, with a `switch` on the message type in place of `smf_run_state()`. For example, a simplified version of the Network module's thread looks like this:

```c
static void network_module(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_NETWORK_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_NETWORK_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);

	task_wdt_id = task_wdt_add(wdt_timeout_ms, network_wdt_callback, (void *)k_current_get());

	/* Initialize the state machine to the initial state */
	smf_set_initial(SMF_CTX(&network_state), &states[STATE_DISCONNECTED]);

	while (true) {
		/* Feed the watchdog before blocking on the next message */
		err = task_wdt_feed(task_wdt_id);

		/* Wait for a message on any subscribed channel */
		err = zbus_sub_wait_msg(&network, &network_state.chan,
					network_state.msg_buf, zbus_wait);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
		}

		/* Run the state machine with the received message */
		err = smf_run_state(SMF_CTX(&network_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			FATAL_ERROR();
		}
	}
}
```

In this pattern:

1. The thread feeds its task watchdog, then waits for a message using `zbus_sub_wait_msg()`.
1. When a message arrives, it is stored in the state object's buffer along with the channel it was received on.
1. The state machine is then executed with `smf_run_state()`, which calls the appropriate handler for the current state.
1. The handler processes the message based on its type and the current state, potentially triggering state transitions.
1. The loop continues, waiting for the next message.

The wait is bounded rather than `K_FOREVER` so that the watchdog is fed even when no messages arrive. The timeout is the watchdog timeout minus the time budgeted for processing a single message, which leaves room for the longest possible message handling before the watchdog expires. A timeout returns `-ENOMSG`, which simply restarts the loop.

### Watchdog timeouts

Each module configures its own watchdog timeout and message processing budget with the `CONFIG_APP_<MODULE>_WATCHDOG_TIMEOUT_SECONDS` and `CONFIG_APP_<MODULE>_MSG_PROCESSING_TIMEOUT_SECONDS` options, sized after the longest operation the module performs while handling a message. The defaults, as watchdog timeout / processing budget, are:

| Module | PPP | nRF93M1 AT | nRF91 AT |
|--------|----:|-----------:|---------:|
| Main | 30 s / 5 s | 300 s / 290 s | 300 s / 290 s |
| Network | 120 s / 110 s | 300 s / 290 s | 300 s / 290 s |
| Cloud | 300 s / 290 s | 220 s / 190 s | 300 s / 290 s |
| FOTA | 300 s / 290 s | Not included | Not included |
| Location | 120 s / 100 s | 150 s / 130 s | 300 s / 290 s |
| Battery | 60 s / 30 s | 30 s / 10 s | Not included |

The PPP Host Application uses the same timeouts on every board. Modules whose handlers make blocking calls get long timeouts. In the PPP Host Application, the Cloud and FOTA handlers make blocking CoAP calls. In the Cloud module a connect attempt performs a DTLS handshake, and the shadow, message, location, and remote debugging requests are blocking CoAP exchanges. In the FOTA module the poll makes reliable CoAP calls to check for and report a job; the image download itself does not block, because the module uses the nRF Cloud FOTA poll library in non-blocking mode and drives the download from its callbacks. The AT host applications size their timeouts the same way, after the operations their own modules perform. In the nRF91 AT Host Application, each provisioning step and each location attempt waits for a Serial Modem result URC, which can take minutes.

Modules that never block in their handlers only publish messages, drive the connection manager, or start asynchronous work, all of which return immediately while results arrive later as messages or callbacks. Their timeouts are short, sized only to catch a genuinely stuck thread rather than to cover a long operation. In the PPP Host Application, the Main module is such a module. Two others have longer budgets for one blocking case each:

- The Network module drives the connection manager, whose calls return immediately while results arrive later as events. The exception is bringing the interface down, where Zephyr's `modem_cellular` driver blocks the thread for up to 30 seconds while it suspends the modem.
- The Location module has two backends. The Location library backend, used on the nRF54 hosts, starts an asynchronous Wi-Fi scan that returns immediately, with the result arriving later through a callback. The modem AT backend, used on the nRF93M1 DK, blocks on its cell and Wi-Fi scans. The timeouts cover the blocking backend, so they are the same on every board.

## Message passing with zbus

The zbus library is part of Zephyr and implements channel-based message passing between threads. This section covers how zbus is used in the applications. See the [zbus documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/zbus/index.html) for an introduction to zbus.

### Channels

Each module declares and defines a channel using zbus macros. For example, in the `src/modules/network/network.h` file, the Network module declares the `network_chan` channel:

```c
ZBUS_CHAN_DECLARE(network_chan);
```

The channel is defined in `src/modules/network/network.c`:

```c
ZBUS_CHAN_DEFINE(network_chan,          /* Channel name, derived from module name */
		 struct network_msg,    /* Message data type */
		 NULL,                  /* Optional validator function */
		 NULL,                  /* Optional pointer to user data */
		 ZBUS_OBSERVERS_EMPTY,  /* Initial observers */
		 ZBUS_MSG_INIT(.type = NETWORK_DISCONNECTED)); /* Message initialization */
```

The fields of the `ZBUS_CHAN_DEFINE` macro are populated in a similar way across modules:

- **Channel name**: The name of the channel, derived from the module name.
- **Message data type**: The data type used to hold message data. The name comes from the channel name.
- **Validator function** and **User data**: Not used.
- **Initial observers**: The initial observer list is empty. Observers are added in the relevant modules later.
- **Message initialization**: The initial value stored in the channel. Not used, and therefore set either to the module's disconnected message type or to `ZBUS_MSG_INIT(0)`.

> [!IMPORTANT]
> In the context of zbus, you can use the term "message type" to refer to the data type or structure containing the message data for a given channel, in this case `struct network_msg`. In this architecture, "message type" is used to mean the enumerated value that distinguishes different kinds of messages sent on the same channel, in this case the `type` field of `struct network_msg`.

### Message types and the message structure

Each module exposes a set of message types through an enumeration. The message types can be considered events in the state machine sense. The message type is typically used within a switch-case statement by the subscriber to determine what actions to take and whether to trigger a state transition.

The message types can be divided into two categories:

- **Input message types**: Commands or requests sent by other modules to the defining module to trigger actions (for example, `CLOUD_CONNECT` to request the Cloud module to connect to nRF Cloud).
- **Output message types**: Responses or notifications sent by the defining module to other modules to report status, data, or events (for example, `CLOUD_CONNECTED` when the cloud connection has been established).

Each module's message types are defined in its public header file located at `modules/<module_name>/<module_name>.h`, under the application's `src/` folder. For example, the Cloud module's messages are defined in `src/modules/cloud/cloud.h` in the `enum cloud_msg_type` enumeration:

```c
enum cloud_msg_type {
	/* Output message types */

	/* The cloud connection is down. */
	CLOUD_DISCONNECTED = 0x1,
	/* The cloud connection is established. */
	CLOUD_CONNECTED,
	/* The device shadow has been polled. */
	CLOUD_SHADOW_POLLED,
	/* The device message in cloud_msg::payload has been sent. */
	CLOUD_MESSAGE_SENT,
	/* Pending remote debugging data has been posted. */
	CLOUD_MEMFAULT_POSTED,

	/* Input message types */

	/* Request to establish the cloud connection. */
	CLOUD_CONNECT,

	/* ... */
};
```

The message type forms the first part of the message structure. For some message types, associated data is present in the message, and the message type determines which fields are valid. For optional features, `#if defined()` guards are used to include the fields only when the feature is enabled:

```c
struct cloud_msg {
	enum cloud_msg_type type;
	char payload[CONFIG_APP_CLOUD_MSG_MAX_LEN];
	size_t payload_len;
#if defined(CONFIG_APP_LOCATION)
	/** Valid for @ref CLOUD_LOCATION_REQUEST messages. */
	struct location_search_wifi_cell_result location_request;
#endif /* CONFIG_APP_LOCATION */
};
```

In the above example, the Cloud module receiving a `CLOUD_LOCATION_REQUEST` message reads the scanned Wi-Fi access points from the `location_request` field of the message.

### Sending messages

Messages are sent on a channel using `zbus_chan_pub()`. For example, to request a cloud disconnect on the `cloud_chan` channel:

```c
	struct cloud_msg msg = {
		.type = CLOUD_DISCONNECT
	};

	err = zbus_chan_pub(&cloud_chan, &msg, PUB_TIMEOUT);
```

Zbus copies the message, so the original message struct is no longer needed after calling `zbus_chan_pub()`. `PUB_TIMEOUT` is a common publish timeout defined in `lib/include/app_common.h`.

### Receiving messages

In zbus, structures called _observers_ are used to receive messages on one or more zbus channels. There are multiple types of observers. All modules with a thread use **message subscribers**, which receive messages asynchronously in the module's own thread.

A message subscriber queues up messages that are received while the module is busy processing another message. The module then processes the messages in the order they were received. An incoming message can never interrupt the processing of another message.

A message subscriber is defined using `ZBUS_MSG_SUBSCRIBER_DEFINE`, and the subscriber is added to a channel using `ZBUS_CHAN_ADD_OBS`. For example, in the Network module:

```c
ZBUS_MSG_SUBSCRIBER_DEFINE(network);
ZBUS_CHAN_ADD_OBS(network_chan, network, 0);
```

The messages are received in the module's thread loop by calling `zbus_sub_wait_msg()`:

```c
err = zbus_sub_wait_msg(&network, &network_state.chan,
			network_state.msg_buf, zbus_wait);
```

The channel and the message contents are stored in the module's [state object](#state-object) in preparation to run the state handler, where the message will be processed.

In modules that subscribe to multiple channels, a channel list macro is used to simplify subscribing to all the channels. The same macro is used to determine the size of a message buffer to handle the largest possible message. For example, in `src/main.c`:

```c
#define CHANNEL_LIST(X) \
	X(network_chan, struct network_msg) \
	X(cloud_chan, struct cloud_msg) \
	IF_ENABLED(CONFIG_APP_FOTA, (X(fota_chan, struct fota_msg))) \
	IF_ENABLED(CONFIG_APP_LOCATION, (X(location_chan, struct location_msg))) \
	X(main_priv_chan, struct priv_main_msg)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE		    MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

/* Add main_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, main_subscriber, 0);

CHANNEL_LIST(ADD_OBSERVERS)
```

### Private channels

When a module needs internal state handling that should not be exposed to other modules, it uses a private channel. Private channels are reserved exclusively for the respective module and are not intended for external use. Otherwise, they are defined, published to, and subscribed to just like public channels. In the PPP Host Application, three modules use one:

- The Cloud module uses `priv_cloud_chan` to turn every attempt at connecting into a message, so that it can retry without blocking its state machine.
- The FOTA module uses `priv_fota_chan` to move the nRF Cloud FOTA library callbacks, which run in the library's own context, into the module's state machine.
- The Main module uses `main_priv_chan` to move the periodic cloud synchronization from the workqueue that runs the timer into its state machine.

Private channels are defined in the module's `.c` file and are never declared in a public header, so no other module can publish to them.

## State machines

The State Machine Framework (SMF) is a Zephyr library that provides a way to implement hierarchical state machines in a structured manner. In a hierarchical state machine, you can implement behavior common to multiple states in a shared parent state. In the PPP Host Application, the Main and FOTA modules and the Location library backend of the Location module use hierarchical state machines, while the Network and Cloud modules are simple enough to use flat ones.

The state machines follow a run-to-completion model where:

- Message processing and state machine execution, including transitions, are completed before processing any new messages.
- Entry and exit functions are called in the correct order when transitioning states.
- Parent state transitions are handled automatically when transitioning between child states.

This model ensures predictable behavior and proper state cleanup during transitions, as there is no mechanism for interrupting or changing the state machine execution from the outside.

This section covers how SMF is used in the modules. See the [SMF documentation](https://docs.nordicsemi.com/bundle/ncs-latest/page/zephyr/services/smf/index.html) for an introduction to SMF.

### State machine definition

SMF supports defining a hierarchy of states. For example, the FOTA module's states can be graphically described as follows:

![Simplified FOTA module state hierarchy](applications/ppp/diagrams/fota-hierarchy.svg)

In the diagram, the black dots with arrows indicate initial transitions. In this case, the initial state of the machine is set to the top-level `STATE_RUNNING` state. In the state definitions, an initial transition is configured such that the state machine ends up in `STATE_WAITING_FOR_POLL_REQUEST` when first initialized. See [FOTA module](applications/ppp/modules/fota.md) for the complete set of transitions.

In SMF, a single state is defined using the `SMF_CREATE_STATE` macro. You can specify the following parameters:

- **Entry function:** Called when entering the state.
- **Run function:** Called when processing a message while in the state.
- **Exit function:** Called when leaving the state.
- **Parent state:** Another state to which this state is subordinate.
- **Initial state transition:** Another state to transition to immediately after entry.

The following shows the definitions of two of the states in the FOTA module from `src/modules/fota/fota.c`, with the parent state and initial transition as shown in the diagram:

```c
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry,
				 state_running_run,
				 NULL,				/* No exit function */
				 NULL,				/* No parent state */
				 &states[STATE_WAITING_FOR_POLL_REQUEST]),
	[STATE_WAITING_FOR_POLL_REQUEST] =
		SMF_CREATE_STATE(NULL,				/* No entry function */
				 state_waiting_for_poll_request_run,
				 NULL,				/* No exit function */
				 &states[STATE_RUNNING],
				 NULL),				/* No initial transition */
	/* ... */
};
```

### State object

SMF uses a data structure of type `struct smf_ctx` to track the current state of the state machine. This structure is embedded within a larger structure called the _state object_ that holds the following:

- SMF's context structure.
- The zbus channel of the latest message received.
- The contents of the latest message received.
- Any extended state variables needed by the module.

For example, the FOTA module defines its state object structure:

```c
struct fota_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Buffer for last zbus message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Extended state variable: nRF Cloud FOTA poll context */
	struct nrf_cloud_fota_poll_ctx fota_ctx;
};
```

The structure is allocated in the module's thread and passed to all SMF functions.

### State machine initialization

State machines are initialized to an initial state using `smf_set_initial()`:

```c
smf_set_initial(SMF_CTX(&fota_state), &states[STATE_RUNNING]);
```

This has to be done before the state machine is executed for the first time. Entry functions of the initial state, and of any state reached through initial transitions, run at this point. This is where modules that need it perform their initialization, such as the FOTA module initializing the nRF Cloud FOTA poll library in `state_running_entry()`.

### Run functions

The run function for each state handles incoming messages and follows a similar pattern:

- The incoming message is identified by its channel and message type.
- If the module should handle the message in its current state, any relevant actions are performed.
- To enforce the run-to-completion model, control flow must end in one of three ways:

    - `return SMF_EVENT_HANDLED;` to end the message handling without transitioning to a new state.
    - `smf_set_state()` followed directly by `return SMF_EVENT_HANDLED;` to end the message handling and trigger a state transition.
    - `return SMF_EVENT_PROPAGATE;` to signal that the message was **not** handled in the current state.

> [!IMPORTANT]
> In SMF, the run function of the current state is executed first, and then the run function of the parent state is executed, unless the child state returns `SMF_EVENT_HANDLED` to indicate that the event has been handled.

The FOTA module shows why this matters: `FOTA_DOWNLOAD_CANCEL` is handled once in the parent `STATE_RUNNING`, so a cancel request is honored from any substate that does not handle it itself.

### State transitions

Transitions between states are handled using `smf_set_state()`:

```c
smf_set_state(SMF_CTX(state_object), &states[NEW_STATE]);
```

A transition to another state must be the last thing happening in a state handler. This is to ensure the correct order of execution of parent state handlers. SMF automatically handles the execution of exit and entry functions for all states along the path to the new state.

### State machine execution

The state machine is run using `smf_run_state()`. SMF executes the run function defined for the current state, and then:

- If the run function returns `SMF_EVENT_PROPAGATE`, the process is repeated for the parent state, if there is one.
- If the run function triggers a state transition, SMF runs any relevant exit and entry functions.

`smf_run_state()` is run from the module threads to process incoming messages, as described in [Module threads](#module-threads).
