import type { Envelope } from '$lib/gen/karshipta/v1/envelope';
import type { Event, WardInfo, WardState } from '$lib/gen/karshipta/v1/telemetry';
import { CommandStatus, type Command } from '$lib/gen/karshipta/v1/command';
import { WardConfigStatus, type AddWard } from '$lib/gen/karshipta/v1/fleet';
import { Severity, type WardClass } from '$lib/gen/karshipta/v1/common';
import {
	ParamAckStatus,
	ParamListStatus,
	type Param,
	type ParamAck,
	type ParamList
} from '$lib/gen/karshipta/v1/parameter';
import {
	LogDownloadStatus,
	LogEntryListStatus,
	type DownloadLogFileAck,
	type LogEntryList,
	type LogFileChunk
} from '$lib/gen/karshipta/v1/log_file';
import {
	CalibrationStatus,
	CalibrationType,
	type CalibrationProgress
} from '$lib/gen/karshipta/v1/calibration';
import { WebSocketTransport, type FleetTransport } from '$lib/transport';
import { RelayTransport } from '$lib/transport/relay-transport';
import { FAKE_FLEET_CENTER, type DemoEngine } from '$lib/fake/fleet-sim';
import type { LatLon } from '$lib/geolocation';
import { fleetGroups } from '$lib/fleet-groups/fleet-groups-store.svelte';
import { zoneStore } from '$lib/zones/zone-store.svelte';

/** where a ward's data comes from: the local demo engine or a connected gateway */
export type WardSource = 'demo' | 'gateway';

export interface Ward {
	info: WardInfo | undefined;
	state: WardState | undefined;
	source: WardSource;
}

// Shared with fleet-groups-store.svelte.ts's own route-planning draft
// (FleetMission's per-ward routes) - the per-ward draft this type
// originally backed here is gone, but the shape itself is still genuinely
// shared, not exclusive to either store.
export interface DraftWaypoint {
	latitudeDeg: number;
	longitudeDeg: number;
	altitudeRelM: number;
}

export interface CommandTracker {
	commandId: string;
	wardId: string;
	/** the action oneof case, e.g. "takeoff" */
	kind: string;
	status: CommandStatus;
	message: string;
	sentAtMs: number;
}

export interface WardConfigTracker {
	requestId: string;
	kind: 'add' | 'remove';
	wardId: string;
	status: WardConfigStatus;
	message: string;
	sentAtMs: number;
}

export interface ParamWriteTracker {
	requestId: string;
	wardId: string;
	paramName: string;
	status: ParamAckStatus;
	message: string;
	sentAtMs: number;
}

/** A DownloadLogFile request's progress (karshipta#249): 'pending' until
 * the DownloadLogFileAck arrives, 'downloading' while LogFileChunk
 * messages are still coming in, then 'success' (triggers the browser
 * download) or 'rejected'. Unlike ParamWriteTracker/CommandTracker, this
 * is not a single request/response - see fleet-store.svelte.ts's own
 * applyDownloadLogFileAck()/applyLogFileChunk(). */
export interface LogDownloadTracker {
	requestId: string;
	wardId: string;
	logId: number;
	status: 'pending' | 'downloading' | 'success' | 'rejected';
	message: string;
	totalSizeBytes: number;
	chunkCount: number;
	receivedBytes: number;
	sentAtMs: number;
}

/** State of the one calibration run active (or just-settled) for a ward
 * (karshipta#248) - at most one at a time, gateway-enforced
 * (CalibrationExecutor rejects a second StartCalibration outright), so
 * this is keyed by wardId directly, not by requestId like
 * LogDownloadTracker above (which supports several concurrent downloads
 * per ward). 'running' covers every CalibrationProgress the gateway
 * sends before its one terminal message; hasProgress/progress and
 * hasInstruction/instruction mirror CalibrationProgress's own
 * independently-optional fields. */
export interface CalibrationRunState {
	requestId: string;
	wardId: string;
	calibrationType: CalibrationType;
	status: 'running' | 'success' | 'failed' | 'cancelled' | 'rejected';
	message: string;
	hasProgress: boolean;
	progress: number;
	hasInstruction: boolean;
	instruction: string;
}

const MAX_EVENTS = 50;
const REQUEST_TIMEOUT_MS = 10_000;
/** trackers in a terminal state linger briefly so the operator sees the outcome */
const TRACKER_LINGER_MS = 6_000;
const GATEWAY_URL_STORAGE_KEY = 'karshipta.gatewayUrl';
const RELAY_URL_STORAGE_KEY = 'karshipta.relayUrl';
const RELAY_DEVICE_ID_STORAGE_KEY = 'karshipta.relayDeviceId';
// Same tier of protection as the gateway's own relay_credentials.yaml
// (gitignored, plaintext on disk, see gateway/docs/relay-transport.md) -
// stored locally, never committed anywhere, not hidden from the browser
// environment itself. There is no server-side session store this console
// could use instead (self-hosted, static app).
const RELAY_DEVICE_TOKEN_STORAGE_KEY = 'karshipta.relayDeviceToken';
/**
 * Each demo ward's home location, not just how many there were: a plain
 * count (this key's previous shape) loses exactly the thing spawnWard
 * now needs to reproduce a ward where the operator actually put it -
 * respawning N wards with no location silently drops them all back to
 * FAKE_FLEET_CENTER, which is what happened before this fix (deploy in
 * Amsterdam, refresh, ward is back in Zurich).
 */
const DEMO_WARD_HOMES_STORAGE_KEY = 'karshipta.demoWardHomes';
const SELECTED_WARD_STORAGE_KEY = 'karshipta.selectedWardId';

function readStoredSelectedWardId(): string | undefined {
	if (typeof localStorage === 'undefined') return undefined;
	return localStorage.getItem(SELECTED_WARD_STORAGE_KEY) ?? undefined;
}

export function isTerminal(status: CommandStatus): boolean {
	return (
		status === CommandStatus.COMMAND_STATUS_SUCCESS ||
		status === CommandStatus.COMMAND_STATUS_REJECTED ||
		status === CommandStatus.COMMAND_STATUS_TIMEOUT
	);
}

export function isConfigTerminal(status: WardConfigStatus): boolean {
	return (
		status === WardConfigStatus.WARD_CONFIG_STATUS_ACCEPTED ||
		status === WardConfigStatus.WARD_CONFIG_STATUS_REJECTED
	);
}

export function isParamWriteTerminal(status: ParamAckStatus): boolean {
	return (
		status === ParamAckStatus.PARAM_ACK_STATUS_SUCCESS ||
		status === ParamAckStatus.PARAM_ACK_STATUS_REJECTED
	);
}

export function isLogDownloadTerminal(status: LogDownloadTracker['status']): boolean {
	return status === 'success' || status === 'rejected';
}

export function isCalibrationTerminal(status: CalibrationRunState['status']): boolean {
	return status !== 'running';
}

function calibrationRunStatus(status: CalibrationStatus): CalibrationRunState['status'] {
	switch (status) {
		case CalibrationStatus.CALIBRATION_STATUS_SUCCESS:
			return 'success';
		case CalibrationStatus.CALIBRATION_STATUS_FAILED:
			return 'failed';
		case CalibrationStatus.CALIBRATION_STATUS_CANCELLED:
			return 'cancelled';
		case CalibrationStatus.CALIBRATION_STATUS_REJECTED:
			return 'rejected';
		// RUNNING and the proto-required UNSPECIFIED/UNRECOGNIZED fallback
		// both read as "still going" here - the gateway never actually
		// sends UNSPECIFIED, but TypeScript needs every CalibrationStatus
		// covered, not just the ones currently defined in the schema.
		case CalibrationStatus.CALIBRATION_STATUS_RUNNING:
		case CalibrationStatus.CALIBRATION_STATUS_UNSPECIFIED:
		case CalibrationStatus.UNRECOGNIZED:
		default:
			return 'running';
	}
}

/**
 * PX4's own log format is ULog (.ulg); ArduPilot's is a binary dataflash
 * log, whose own native extension is .bin. Guessing the wrong one for the
 * connected firmware would be worse than a plain fallback - same "know
 * your client" rule configuration-tab.svelte's per-firmware fields
 * already follow - so this only ever claims the firmware-specific
 * extension when WardInfo.autopilot actually says which firmware
 * produced the bytes; anything else (unknown, empty) gets the generic
 * fallback.
 */
function logFileName(wardId: string, logId: number, autopilot: string | undefined): string {
	const extension = autopilot === 'PX4' ? 'ulg' : 'bin';
	return `${wardId}-log-${logId}.${extension}`;
}

/** Hands the reassembled file to the browser the only way a page can
 * trigger a save without a server endpoint: an object URL plus a
 * transient, never-appended <a download> click. Safe here specifically
 * because this is the real console app, not a sandboxed preview - not a
 * pattern to reuse inside an Artifact. */
function triggerBrowserDownload(filename: string, bytes: Uint8Array): void {
	// bytes.buffer is typed ArrayBufferLike (could in principle be a
	// SharedArrayBuffer), which Blob's constructor type doesn't accept;
	// copying into a fresh Uint8Array (always backed by a plain
	// ArrayBuffer) satisfies that without changing the actual bytes sent.
	const blob = new Blob([new Uint8Array(bytes)], { type: 'application/octet-stream' });
	const url = URL.createObjectURL(blob);
	const link = document.createElement('a');
	link.href = url;
	link.download = filename;
	link.click();
	URL.revokeObjectURL(url);
}

function readStoredGatewayUrl(): string | undefined {
	if (typeof localStorage === 'undefined') return undefined;
	return localStorage.getItem(GATEWAY_URL_STORAGE_KEY) ?? undefined;
}

function readStoredRelayUrl(): string | undefined {
	if (typeof localStorage === 'undefined') return undefined;
	return localStorage.getItem(RELAY_URL_STORAGE_KEY) ?? undefined;
}

/**
 * The relay device id only needs to stay stable across sessions once
 * chosen - generated once, then persisted, the same way the device keypair
 * itself is persisted inside relay-transport.ts. The matching device_token
 * (see readStoredRelayDeviceToken below) is provisioned out of band against
 * a real relayly server (POST /api/v1/devices or its CLI, same as the
 * gateway side - gateway/docs/relay-transport.md), not minted here.
 */
function readOrCreateRelayDeviceId(): string {
	if (typeof localStorage === 'undefined') return crypto.randomUUID();
	const saved = localStorage.getItem(RELAY_DEVICE_ID_STORAGE_KEY);
	if (saved) return saved;
	const id = crypto.randomUUID();
	localStorage.setItem(RELAY_DEVICE_ID_STORAGE_KEY, id);
	return id;
}

function readStoredRelayDeviceToken(): string {
	if (typeof localStorage === 'undefined') return '';
	return localStorage.getItem(RELAY_DEVICE_TOKEN_STORAGE_KEY) ?? '';
}

function readStoredDemoWardHomes(): LatLon[] {
	if (typeof localStorage === 'undefined') return [];
	const raw = localStorage.getItem(DEMO_WARD_HOMES_STORAGE_KEY);
	if (!raw) return [];
	try {
		const parsed: unknown = JSON.parse(raw);
		if (!Array.isArray(parsed)) return [];
		return parsed.filter(
			(point): point is LatLon =>
				typeof point === 'object' &&
				point !== null &&
				typeof (point as LatLon).lat === 'number' &&
				typeof (point as LatLon).lon === 'number'
		);
	} catch {
		// stale/corrupt data (e.g. the old plain-count shape this key replaced) - start empty, not crash
		return [];
	}
}

/**
 * Single owner of all live fleet state, keyed by ward_id. Two independent
 * channels feed it: the local demo engine (always available, instant, never
 * touches a network) and an optionally connected gateway (real hardware or
 * simulated wards behind a real gateway process). Every ward carries
 * which channel it came from, so commands and mission uploads route to the
 * right one and disconnecting the gateway never touches demo wards.
 * Components only read; connection and ward lifecycle are explicit
 * actions, never automatic.
 */
class FleetStore {
	wards = $state<Record<string, Ward>>({});
	events = $state<Event[]>([]);
	commands = $state<Record<string, CommandTracker>>({});
	wardConfigRequests = $state<Record<string, WardConfigTracker>>({});
	/** last GetParams response per ward (karshipta#212), keyed by ward_id */
	paramLists = $state<Record<string, ParamList>>({});
	paramListLoading = $state<Record<string, boolean>>({});
	/** the requestId of the most recently sent GetParams per ward, so a
	 * stale response from an earlier, superseded request can't clobber a
	 * newer one still in flight */
	private paramListRequestIds: Record<string, string> = {};
	paramWrites = $state<Record<string, ParamWriteTracker>>({});
	/** last GetLogEntries response per ward (karshipta#249), keyed by ward_id - same shape as paramLists above */
	logEntries = $state<Record<string, LogEntryList>>({});
	logEntriesLoading = $state<Record<string, boolean>>({});
	/** same stale-response guard as paramListRequestIds above */
	private logEntriesRequestIds: Record<string, string> = {};
	logDownloads = $state<Record<string, LogDownloadTracker>>({});
	/** Chunks received so far per in-flight download, indexed by
	 * chunk_index, sparse until every slot fills. Deliberately NOT $state:
	 * Uint8Array chunks are write-once binary payloads no component reads
	 * reactively (logDownloads above is what the UI watches), and routing
	 * multi-MB transfers through Svelte's reactivity proxy on every chunk
	 * would be pure overhead. */
	private logDownloadChunks: Record<string, (Uint8Array | undefined)[]> = {};
	/** the one active (or just-settled) calibration run per ward (karshipta#248), keyed by ward_id */
	calibrationRuns = $state<Record<string, CalibrationRunState>>({});
	selectedWardId = $state<string | undefined>(undefined);
	/**
	 * A ward ID restored from a previous session (requested directly, for a
	 * smoother feel across a refresh) that hasn't actually reappeared in
	 * `wards` yet - not selectedWardId itself, to avoid a brief or
	 * permanent phantom-selected state (WardStatusStrip etc. rendering
	 * against an ID with no corresponding ward data) if the stored ward
	 * never comes back this session (e.g. a gateway/simulated ward whose
	 * gateway isn't connected here). upsert() below is the one place a new
	 * ward ID actually lands in `wards`, for either a demo respawn (near-
	 * instant, at bindDemoEngine time) or a slower gateway sync - checking
	 * there, not just once at construction, is what makes the restore work
	 * for both. Cleared the moment anything calls select() explicitly (see
	 * that method), so a user who's since picked something else never gets
	 * silently knocked back to an old selection if the stored ward happens
	 * to reappear later in the same session.
	 */
	private pendingSelectedWardId: string | undefined = readStoredSelectedWardId();
	/** goto targeting: the panel arms it, the map's next click supplies the point */
	gotoArming = $state(false);
	pendingGoto = $state<{ latitudeDeg: number; longitudeDeg: number } | undefined>(undefined);
	/** set-home targeting: identical shape to goto targeting above, same map-click handoff */
	setHomeArming = $state(false);
	pendingSetHome = $state<{ latitudeDeg: number; longitudeDeg: number } | undefined>(undefined);
	/** gateway connection as the operator should read it */
	link = $state<'live' | 'connecting' | 'down'>('down');
	gatewayUrl = $state(readStoredGatewayUrl() ?? '');
	relayUrl = $state(readStoredRelayUrl() ?? '');
	relayDeviceId = $state(readOrCreateRelayDeviceId());
	relayDeviceToken = $state(readStoredRelayDeviceToken());
	/** connected to the relay but pairWithGateway() hasn't completed yet */
	relayAwaitingPair = $state(false);
	/** read-only session: telemetry flows, commanding is refused observably */
	readonly = $state(false);
	/** how many simulated-ward adds this session has requested; drives the resource warning at C7's UI layer */
	simulatedWardAddCount = $state(0);

	readonly wardIds = $derived(Object.keys(this.wards).sort());
	readonly selectedWard = $derived(
		this.selectedWardId !== undefined ? this.wards[this.selectedWardId] : undefined
	);
	readonly gatewayConnected = $derived(this.link === 'live');

	private demoEngine: DemoEngine | undefined;
	private gatewayTransport: FleetTransport | undefined;
	private relayTransport: RelayTransport | undefined;
	// timers are bookkeeping, not state
	private timeoutTimers = new Map<string, ReturnType<typeof setTimeout>>();
	// each live demo ward's home location, keyed by id - bookkeeping for
	// persistDemoWardHomes(), not state components read directly
	private demoWardHomes: Record<string, LatLon> = {};

	/**
	 * Wires the always-available local demo engine; call once at app start.
	 * Respawns whatever demo fleet was persisted from a previous session
	 * (see addDemoWard/removeDemoWard), each at its original home
	 * location, so a browser refresh doesn't wipe a locally-run operator's
	 * fleet back to empty or move it back to FAKE_FLEET_CENTER - the
	 * wards come back on a fresh patrol (armed, not mid-mission), not
	 * with their exact prior flight state restored.
	 */
	bindDemoEngine(engine: DemoEngine): void {
		this.demoEngine = engine;
		engine.start();
		for (const home of readStoredDemoWardHomes()) {
			const wardId = engine.spawnWard(home);
			this.demoWardHomes[wardId] = home;
		}
	}

	connectGateway(url: string): void {
		this.disconnectGateway();
		// disconnectGateway() just resynced fleetGroups/zoneStore from the demo
		// engine (see its own comment); clear that back out immediately, since
		// this new gateway's own on-connect snapshot is about to become
		// authoritative for these three resource types instead.
		zoneStore.teardown();
		fleetGroups.teardown();
		this.gatewayUrl = url;
		if (typeof localStorage !== 'undefined') {
			localStorage.setItem(GATEWAY_URL_STORAGE_KEY, url);
		}
		this.link = 'connecting';
		const transport = new WebSocketTransport(url, {
			onEnvelope: (envelope) => this.applyEnvelope(envelope, 'gateway'),
			onStatus: (status) => this.setGatewayStatus(status)
		});
		this.gatewayTransport = transport;
		transport.start();
	}

	/**
	 * Connects through relayly instead of a direct WebSocket - the console
	 * half of gateway/docs/relay-transport.md, for operators whose gateway
	 * sits behind NAT/CGNAT with no reachable inbound port. Reuses the same
	 * gatewayTransport slot and wards/link plumbing as connectGateway();
	 * only the wire underneath differs, per the Transport abstraction rule
	 * in CLAUDE.md ("nothing outside the transport layer may know which is
	 * active"). relayTransport is kept separately only because pairing
	 * (pairRelay below) is relay-specific and not part of FleetTransport.
	 */
	connectRelay(relayUrl: string, deviceId: string, deviceToken: string): void {
		this.disconnectGateway();
		// See connectGateway()'s matching comment.
		zoneStore.teardown();
		fleetGroups.teardown();
		this.relayUrl = relayUrl;
		this.relayDeviceId = deviceId;
		this.relayDeviceToken = deviceToken;
		if (typeof localStorage !== 'undefined') {
			localStorage.setItem(RELAY_URL_STORAGE_KEY, relayUrl);
			localStorage.setItem(RELAY_DEVICE_ID_STORAGE_KEY, deviceId);
			localStorage.setItem(RELAY_DEVICE_TOKEN_STORAGE_KEY, deviceToken);
		}
		this.link = 'connecting';
		const transport = new RelayTransport(relayUrl, deviceId, deviceToken, {
			onEnvelope: (envelope) => this.applyEnvelope(envelope, 'gateway'),
			onStatus: (status) => this.setGatewayStatus(status)
		});
		this.gatewayTransport = transport;
		this.relayTransport = transport;
		this.relayAwaitingPair = true;
		transport.start();
	}

	/**
	 * One-time pairing against the code the gateway's pairing tool displayed
	 * (see gateway/docs/relay-transport.md). Only meaningful right after
	 * connectRelay(); once paired, the pairing persists server-side and
	 * future connectRelay() calls with the same deviceId reconnect straight
	 * to 'open' without this step.
	 */
	async pairRelay(code: string): Promise<void> {
		if (!this.relayTransport) throw new Error('relay transport not connected');
		await this.relayTransport.pairWithGateway(code);
		this.relayAwaitingPair = false;
	}

	disconnectGateway(): void {
		this.gatewayTransport?.stop();
		this.gatewayTransport = undefined;
		this.relayTransport = undefined;
		this.relayAwaitingPair = false;
		this.link = 'down';
		for (const [id, ward] of Object.entries(this.wards)) {
			if (ward.source === 'gateway') delete this.wards[id];
		}
		if (this.selectedWardId && !(this.selectedWardId in this.wards)) {
			this.select(undefined);
		}
		// Unlike wards (tagged per-entity with source: 'demo' | 'gateway' and
		// shown from both at once), Fleet/Zone/FleetMission have no such tag -
		// they're gateway-owned config, not something meant to show a demo
		// and a real gateway's copies merged together. A disconnect drops
		// whatever the just-closed gateway had synced, and the demo engine
		// (still running the whole time; only wards ever got source-filtered)
		// becomes authoritative again, so its own data needs pushing back in
		// - it was never re-sent after the initial start(), and nothing else
		// would otherwise repopulate these three now-empty stores.
		zoneStore.teardown();
		fleetGroups.teardown();
		this.demoEngine?.resync();
	}

	setGatewayStatus(status: 'connecting' | 'open' | 'closed'): void {
		this.link = status === 'open' ? 'live' : status === 'connecting' ? 'connecting' : 'down';
		// covers relay reconnects that resume an already-established pairing
		// (RelayTransport finds it via getPeers() and reports 'open' directly,
		// without pairRelay() ever being called) - the pairing prompt should
		// not linger once telemetry is actually flowing
		if (status === 'open') this.relayAwaitingPair = false;
		// A dropped or reconnecting link must not leave already-known wards
		// looking fully live forever: fleet-map's fade-on-disconnect keys
		// off each ward's own state.connected, which telemetry only ever
		// sets true - nothing else clears it when the whole link goes down.
		if (status !== 'open') {
			for (const ward of Object.values(this.wards)) {
				if (ward.source === 'gateway' && ward.state) {
					ward.state.connected = false;
				}
			}
			// A dropped console<->gateway link means no further
			// CalibrationProgress/DownloadLogFileAck/LogFileChunk for a
			// gateway-sourced ward will ever arrive - settle any in-flight
			// one immediately rather than leaving its dialog stuck for up
			// to REQUEST_TIMEOUT_MS until its own stall timer would
			// eventually catch this. Both dialogs are deliberately
			// non-dismissible while a run is active (karshipta#248/#249 -
			// "calibrate state is sensitive") specifically so the operator
			// is never stuck with no way out; this is that same rule
			// applied to the one cause neither tracker's stall timer
			// reacts to on its own. Demo wards are untouched: the demo
			// engine never actually disconnects, so nothing of theirs
			// should be cut short by a real gateway link dropping.
			const isGatewayWard = (wardId: string) => this.wards[wardId]?.source === 'gateway';
			for (const run of Object.values(this.calibrationRuns)) {
				if (run.status === 'running' && isGatewayWard(run.wardId)) {
					this.settleCalibration(
						run.wardId,
						run.requestId,
						'failed',
						'lost connection to the gateway'
					);
				}
			}
			for (const tracker of Object.values(this.logDownloads)) {
				if (!isLogDownloadTerminal(tracker.status) && isGatewayWard(tracker.wardId)) {
					this.settleLogDownload(tracker.requestId, 'lost connection to the gateway');
				}
			}
		}
	}

	select(wardId: string | undefined): void {
		this.selectedWardId = wardId;
		this.pendingSelectedWardId = undefined;
		this.cancelGoto();
		this.cancelSetHome();
		if (typeof localStorage === 'undefined') return;
		if (wardId === undefined) {
			localStorage.removeItem(SELECTED_WARD_STORAGE_KEY);
		} else {
			localStorage.setItem(SELECTED_WARD_STORAGE_KEY, wardId);
		}
	}

	armGoto(): void {
		this.gotoArming = true;
	}

	cancelGoto(): void {
		this.gotoArming = false;
		this.pendingGoto = undefined;
	}

	/** called by the map on click while goto targeting is armed */
	requestGoto(latitudeDeg: number, longitudeDeg: number): void {
		if (!this.gotoArming) return;
		this.gotoArming = false;
		this.pendingGoto = { latitudeDeg, longitudeDeg };
	}

	armSetHome(): void {
		this.setHomeArming = true;
	}

	cancelSetHome(): void {
		this.setHomeArming = false;
		this.pendingSetHome = undefined;
	}

	/** called by the map on click while set-home targeting is armed */
	requestSetHome(latitudeDeg: number, longitudeDeg: number): void {
		if (!this.setHomeArming) return;
		this.setHomeArming = false;
		this.pendingSetHome = { latitudeDeg, longitudeDeg };
	}

	sendCommand(wardId: string, action: NonNullable<Command['action']>): string {
		if (this.readonly) {
			// belt and braces: viewer mode hides the command UI, but any path
			// that still sends must settle observably, never silently
			const refusedId = crypto.randomUUID();
			this.commands[refusedId] = {
				commandId: refusedId,
				wardId,
				kind: action.$case,
				status: CommandStatus.COMMAND_STATUS_REJECTED,
				message: 'read-only session',
				sentAtMs: Date.now()
			};
			return refusedId;
		}
		const command: Command = {
			commandId: crypto.randomUUID(),
			wardId,
			timestampMs: Date.now(),
			action
		};
		this.commands[command.commandId] = {
			commandId: command.commandId,
			wardId,
			kind: action.$case,
			status: CommandStatus.COMMAND_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: command.timestampMs
		};
		const channel = this.channelFor(wardId);
		if (!channel) {
			this.settle(command.commandId, CommandStatus.COMMAND_STATUS_REJECTED, 'no active channel');
			return command.commandId;
		}
		this.timeoutTimers.set(
			command.commandId,
			setTimeout(() => {
				this.settle(
					command.commandId,
					CommandStatus.COMMAND_STATUS_TIMEOUT,
					'no acknowledgment from gateway'
				);
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			channel.send({ payload: { $case: 'command', command } });
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settle(command.commandId, CommandStatus.COMMAND_STATUS_REJECTED, reason);
		}
		return command.commandId;
	}

	/** in-flight and recently settled commands for one ward, newest first */
	commandsFor(wardId: string): CommandTracker[] {
		return Object.values(this.commands)
			.filter((tracker) => tracker.wardId === wardId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/** Sends GetParams for one ward; the response lands in paramLists[wardId]
	 * via applyParamList(). Viewer mode is refused client-side too (not just
	 * by the gateway, see WardManager::reject_viewer_envelope's kGetParams
	 * case) so a read-only session never even sends the request. */
	requestGetParams(wardId: string): void {
		const requestId = crypto.randomUUID();
		this.paramListRequestIds[wardId] = requestId;
		this.paramListLoading[wardId] = true;
		if (this.readonly) {
			this.paramListLoading[wardId] = false;
			this.paramLists[wardId] = {
				requestId,
				wardId,
				status: ParamListStatus.PARAM_LIST_STATUS_REJECTED,
				message: 'read-only session',
				params: []
			};
			return;
		}
		const channel = this.channelFor(wardId);
		if (!channel) {
			this.paramListLoading[wardId] = false;
			this.paramLists[wardId] = {
				requestId,
				wardId,
				status: ParamListStatus.PARAM_LIST_STATUS_REJECTED,
				message: 'no active channel',
				params: []
			};
			return;
		}
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				if (this.paramListRequestIds[wardId] !== requestId) return;
				this.paramListLoading[wardId] = false;
				this.paramLists[wardId] = {
					requestId,
					wardId,
					status: ParamListStatus.PARAM_LIST_STATUS_REJECTED,
					message: 'no response from gateway',
					params: []
				};
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			channel.send({ payload: { $case: 'getParams', getParams: { requestId, wardId } } });
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.paramListLoading[wardId] = false;
			this.paramLists[wardId] = {
				requestId,
				wardId,
				status: ParamListStatus.PARAM_LIST_STATUS_REJECTED,
				message: reason,
				params: []
			};
		}
	}

	/** Sends SetParam for one ward; tracked the same request/ack shape as
	 * sendCommand() above, just against ParamAckStatus instead of
	 * CommandStatus (SetParam isn't Command.action-shaped - see
	 * parameter.proto's own comment on ParamAck). */
	requestSetParam(wardId: string, param: Param): string {
		const requestId = crypto.randomUUID();
		if (this.readonly) {
			this.paramWrites[requestId] = {
				requestId,
				wardId,
				paramName: param.name,
				status: ParamAckStatus.PARAM_ACK_STATUS_REJECTED,
				message: 'read-only session',
				sentAtMs: Date.now()
			};
			return requestId;
		}
		this.paramWrites[requestId] = {
			requestId,
			wardId,
			paramName: param.name,
			status: ParamAckStatus.PARAM_ACK_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: Date.now()
		};
		const channel = this.channelFor(wardId);
		if (!channel) {
			this.settleParamWrite(
				requestId,
				ParamAckStatus.PARAM_ACK_STATUS_REJECTED,
				'no active channel'
			);
			return requestId;
		}
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settleParamWrite(
					requestId,
					ParamAckStatus.PARAM_ACK_STATUS_REJECTED,
					'no acknowledgment from gateway'
				);
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			channel.send({ payload: { $case: 'setParam', setParam: { requestId, wardId, param } } });
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settleParamWrite(requestId, ParamAckStatus.PARAM_ACK_STATUS_REJECTED, reason);
		}
		return requestId;
	}

	/** in-flight and recently settled parameter writes for one ward, newest first */
	paramWritesFor(wardId: string): ParamWriteTracker[] {
		return Object.values(this.paramWrites)
			.filter((tracker) => tracker.wardId === wardId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/** Sends GetLogEntries for one ward (karshipta#249); the response lands
	 * in logEntries[wardId] via applyLogEntryList(). Same request/response
	 * shape as requestGetParams() above, including being refused
	 * client-side in a read-only session before it ever reaches the wire. */
	requestGetLogEntries(wardId: string): void {
		const requestId = crypto.randomUUID();
		this.logEntriesRequestIds[wardId] = requestId;
		this.logEntriesLoading[wardId] = true;
		if (this.readonly) {
			this.logEntriesLoading[wardId] = false;
			this.logEntries[wardId] = {
				requestId,
				wardId,
				status: LogEntryListStatus.LOG_ENTRY_LIST_STATUS_REJECTED,
				message: 'read-only session',
				entries: []
			};
			return;
		}
		const channel = this.channelFor(wardId);
		if (!channel) {
			this.logEntriesLoading[wardId] = false;
			this.logEntries[wardId] = {
				requestId,
				wardId,
				status: LogEntryListStatus.LOG_ENTRY_LIST_STATUS_REJECTED,
				message: 'no active channel',
				entries: []
			};
			return;
		}
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				if (this.logEntriesRequestIds[wardId] !== requestId) return;
				this.logEntriesLoading[wardId] = false;
				this.logEntries[wardId] = {
					requestId,
					wardId,
					status: LogEntryListStatus.LOG_ENTRY_LIST_STATUS_REJECTED,
					message: 'no response from gateway',
					entries: []
				};
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			channel.send({ payload: { $case: 'getLogEntries', getLogEntries: { requestId, wardId } } });
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.logEntriesLoading[wardId] = false;
			this.logEntries[wardId] = {
				requestId,
				wardId,
				status: LogEntryListStatus.LOG_ENTRY_LIST_STATUS_REJECTED,
				message: reason,
				entries: []
			};
		}
	}

	/** Sends DownloadLogFile for one ward (karshipta#249); progress and
	 * completion land in logDownloads[requestId] via
	 * applyDownloadLogFileAck()/applyLogFileChunk(). Unlike GetParams/
	 * SetParam's single request/response, a successful download is a
	 * DownloadLogFileAck followed by chunkCount LogFileChunk messages
	 * (gateway/docs/log-download-executor.md); this store reassembles them
	 * and triggers a real browser download once the last one lands. */
	requestDownloadLogFile(wardId: string, logId: number): string {
		const requestId = crypto.randomUUID();
		if (this.readonly) {
			this.logDownloads[requestId] = {
				requestId,
				wardId,
				logId,
				status: 'rejected',
				message: 'read-only session',
				totalSizeBytes: 0,
				chunkCount: 0,
				receivedBytes: 0,
				sentAtMs: Date.now()
			};
			return requestId;
		}
		this.logDownloads[requestId] = {
			requestId,
			wardId,
			logId,
			status: 'pending',
			message: '',
			totalSizeBytes: 0,
			chunkCount: 0,
			receivedBytes: 0,
			sentAtMs: Date.now()
		};
		const channel = this.channelFor(wardId);
		if (!channel) {
			this.settleLogDownload(requestId, 'no active channel');
			return requestId;
		}
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settleLogDownload(requestId, 'no acknowledgment from gateway');
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			channel.send({
				payload: { $case: 'downloadLogFile', downloadLogFile: { requestId, wardId, logId } }
			});
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settleLogDownload(requestId, reason);
		}
		return requestId;
	}

	/** in-flight and recently settled log downloads for one ward, newest first */
	logDownloadsFor(wardId: string): LogDownloadTracker[] {
		return Object.values(this.logDownloads)
			.filter((tracker) => tracker.wardId === wardId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/** Sends StartCalibration for one ward (karshipta#248); progress lands
	 * in calibrationRuns[wardId] via applyCalibrationProgress(). At most
	 * one run per ward - the gateway itself rejects a second concurrent
	 * start (CalibrationExecutor's own one-job-at-a-time guarantee), so
	 * this store doesn't need to guard against it client-side either;
	 * a stale in-flight run for this ward (if any) is simply overwritten,
	 * matching what the gateway would do anyway. */
	requestStartCalibration(wardId: string, calibrationType: CalibrationType): void {
		const requestId = crypto.randomUUID();
		if (this.readonly) {
			this.calibrationRuns[wardId] = {
				requestId,
				wardId,
				calibrationType,
				status: 'rejected',
				message: 'read-only session',
				hasProgress: false,
				progress: 0,
				hasInstruction: false,
				instruction: ''
			};
			return;
		}
		this.calibrationRuns[wardId] = {
			requestId,
			wardId,
			calibrationType,
			status: 'running',
			message: '',
			hasProgress: false,
			progress: 0,
			hasInstruction: false,
			instruction: ''
		};
		const channel = this.channelFor(wardId);
		if (!channel) {
			this.settleCalibration(wardId, requestId, 'rejected', 'no active channel');
			return;
		}
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settleCalibration(wardId, requestId, 'rejected', 'no response from gateway');
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			channel.send({
				payload: {
					$case: 'startCalibration',
					startCalibration: { requestId, wardId, calibrationType }
				}
			});
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settleCalibration(wardId, requestId, 'rejected', reason);
		}
	}

	/** Sends CancelCalibration for the currently running calibration on one
	 * ward, if any (karshipta#248). No-op if nothing is running. There is
	 * no ack channel for Cancel at all (gateway/docs/calibration-executor.md's
	 * own doc comment) - a successful cancel surfaces as the run's own
	 * terminal CalibrationProgress{CANCELLED} shortly after, via the
	 * normal applyCalibrationProgress() path; a failed or lost cancel
	 * simply leaves the run showing RUNNING, same as the gateway side. */
	requestCancelCalibration(wardId: string): void {
		const run = this.calibrationRuns[wardId];
		if (!run || run.status !== 'running') return;
		const channel = this.channelFor(wardId);
		if (!channel) return;
		try {
			channel.send({
				payload: {
					$case: 'cancelCalibration',
					cancelCalibration: {
						requestId: crypto.randomUUID(),
						wardId,
						calibrationRequestId: run.requestId
					}
				}
			});
		} catch {
			// Best-effort, matching the "no ack channel" contract above - a
			// failed send just means the operator can try again or wait it
			// out; nothing to settle client-side.
		}
	}

	/** Removes a ward's calibration run from local state immediately,
	 * regardless of whether it's still 'running' or already terminal -
	 * the single "go back to the sensor list" action calibration-dialog.svelte's
	 * Cancel button uses (karshipta#248). Deliberately does not wait for a
	 * CANCELLED confirmation: a running calibration's own cancel request
	 * (requestCancelCalibration, called alongside this) is fire-and-forget
	 * with no ack channel, so waiting for confirmation here would mean
	 * "going back" might never actually complete if that message is lost.
	 * Clears any pending stall/settle timer tied to this run so it can't
	 * fire against an already-cleared entry afterward. */
	clearCalibrationRun(wardId: string): void {
		const run = this.calibrationRuns[wardId];
		if (!run) return;
		const timer = this.timeoutTimers.get(run.requestId);
		if (timer) {
			clearTimeout(timer);
			this.timeoutTimers.delete(run.requestId);
		}
		delete this.calibrationRuns[wardId];
	}

	/**
	 * Adds a ward through the connected gateway (covers both a simulated
	 * SITL endpoint and real hardware; the gateway sees no difference). No
	 * gateway connection means nothing to send to, reported immediately.
	 */
	noteSimulatedWardRequested(): void {
		this.simulatedWardAddCount += 1;
	}

	requestAddWard(spec: {
		wardId: string;
		name: string;
		wardClass: WardClass;
		connectionUrl: string;
		mavlinkSystemId: number;
	}): string {
		const requestId = crypto.randomUUID();
		const addWard: AddWard = { requestId, ...spec };
		this.trackConfigRequest(requestId, 'add', spec.wardId, () => ({
			payload: { $case: 'addWard', addWard }
		}));
		return requestId;
	}

	requestRemoveWard(wardId: string): string {
		const requestId = crypto.randomUUID();
		this.trackConfigRequest(requestId, 'remove', wardId, () => ({
			payload: { $case: 'removeWard', removeWard: { requestId, wardId } }
		}));
		return requestId;
	}

	configRequestsFor(wardId: string): WardConfigTracker[] {
		return Object.values(this.wardConfigRequests)
			.filter((tracker) => tracker.wardId === wardId)
			.sort((a, b) => b.sentAtMs - a.sentAtMs);
	}

	/** spawns a new demo ward purely client-side; never touches a network */
	addDemoWard(location?: LatLon): void {
		const wardId = this.demoEngine?.spawnWard(location);
		if (wardId) this.demoWardHomes[wardId] = location ?? FAKE_FLEET_CENTER;
		this.persistDemoWardHomes();
	}

	removeDemoWard(wardId: string): void {
		this.demoEngine?.despawnWard(wardId);
		delete this.wards[wardId];
		delete this.demoWardHomes[wardId];
		if (this.selectedWardId === wardId) this.select(undefined);
		this.persistDemoWardHomes();
	}

	applyEnvelope(envelope: Envelope, source: WardSource): void {
		const payload = envelope.payload;
		if (payload === undefined) {
			console.warn('fleet: dropping Envelope with empty payload');
			return;
		}
		switch (payload.$case) {
			case 'wardInfo':
				this.upsert(payload.wardInfo.wardId, source).info = payload.wardInfo;
				break;
			case 'wardState':
				this.upsert(payload.wardState.wardId, source).state = payload.wardState;
				break;
			case 'event':
				this.events = [payload.event, ...this.events].slice(0, MAX_EVENTS);
				break;
			case 'commandAck': {
				const ack = payload.commandAck;
				const tracker = this.commands[ack.commandId];
				if (!tracker) {
					// Not necessarily a bug: the gateway itself synthesizes and
					// enqueues a StartMissionCommand once a fleet mission's per-ward
					// upload lands (ward_manager.cpp's pending_start), and also
					// synthesizes a Stop dispatch's rtl/pause_mission/land command -
					// both commands this client never sent and so never tracked. A
					// rejection would otherwise vanish with nothing but this
					// console.warn - surfaced as an Event instead, the same channel
					// a fleet mission's upload-phase rejection already uses
					// (MISSION_UPLOAD_REJECTED), so a failure on one ward is never
					// silently invisible. A success needs no separate surfacing: the
					// FleetMission's own ward_states already cover both outcomes -
					// WARD_MISSION_STATUS_ACTIVE on a successful start, STOPPED on a
					// successful stop.
					if (
						ack.status === CommandStatus.COMMAND_STATUS_REJECTED ||
						ack.status === CommandStatus.COMMAND_STATUS_TIMEOUT
					) {
						this.events = [
							{
								wardId: ack.wardId,
								timestampMs: Date.now(),
								severity: Severity.SEVERITY_WARNING,
								code: 'COMMAND_REJECTED',
								message: ack.message
							},
							...this.events
						].slice(0, MAX_EVENTS);
					} else {
						console.warn(`fleet: CommandAck for unknown command_id ${ack.commandId}`);
					}
					break;
				}
				this.settle(ack.commandId, ack.status, ack.message);
				break;
			}
			case 'wardConfigAck': {
				const ack = payload.wardConfigAck;
				const tracker = this.wardConfigRequests[ack.requestId];
				if (!tracker) {
					console.warn(`fleet: WardConfigAck for unknown request_id ${ack.requestId}`);
					break;
				}
				this.settleConfig(ack.requestId, ack.status, ack.message);
				if (
					tracker.kind === 'remove' &&
					ack.status === WardConfigStatus.WARD_CONFIG_STATUS_ACCEPTED
				) {
					delete this.wards[ack.wardId];
					if (this.selectedWardId === ack.wardId) this.select(undefined);
				}
				break;
			}
			case 'command':
			case 'missionUpload':
			case 'addWard':
			case 'removeWard':
			case 'missionFileUpload':
			case 'missionDownloadRequest':
			case 'createFleet':
			case 'renameFleet':
			case 'deleteFleet':
			case 'addWardToFleet':
			case 'removeWardFromFleet':
			case 'createZone':
			case 'updateZone':
			case 'deleteZone':
			case 'createFleetMission':
			case 'stopFleetMission':
			case 'removeFleetMission':
			case 'updateFleetMissionRoutes':
			case 'getParams':
			case 'setParam':
			case 'getLogEntries':
			case 'downloadLogFile':
			case 'startCalibration':
			case 'cancelCalibration':
				console.warn(`fleet: ignoring upstream payload kind ${payload.$case} sent downstream`);
				break;
			case 'fleet':
				fleetGroups.applyFleet(payload.fleet);
				break;
			case 'fleetAck':
				fleetGroups.applyFleetAck(payload.fleetAck);
				break;
			case 'fleetMission':
				fleetGroups.applyFleetMission(payload.fleetMission);
				break;
			case 'fleetMissionAck':
				fleetGroups.applyFleetMissionAck(payload.fleetMissionAck);
				break;
			case 'zone':
				zoneStore.applyZone(payload.zone);
				break;
			case 'zoneAck':
				zoneStore.applyZoneAck(payload.zoneAck);
				break;
			// Still a real, expected wire message - any actively-flying mission
			// keeps emitting these regardless of whether it was started via the
			// old per-ward flow or a FleetMission - just nothing in the console
			// reads them anymore (removed with the per-ward MissionPanel;
			// FleetMission's own ward_states push already covers the operator-
			// visible progress/outcome, see the commandAck case above). Explicit
			// no-op cases, not left to fall into the default's "unknown payload
			// kind" warning, which would be a false alarm for a message this
			// console fully recognizes and is just choosing not to act on.
			case 'missionProgress':
			case 'missionDownload':
				break;
			case 'paramList':
				this.applyParamList(payload.paramList);
				break;
			case 'paramAck':
				this.applyParamAck(payload.paramAck);
				break;
			case 'logEntryList':
				this.applyLogEntryList(payload.logEntryList);
				break;
			case 'downloadLogFileAck':
				this.applyDownloadLogFileAck(payload.downloadLogFileAck);
				break;
			case 'logFileChunk':
				this.applyLogFileChunk(payload.logFileChunk);
				break;
			case 'calibrationProgress':
				this.applyCalibrationProgress(payload.calibrationProgress);
				break;
			default: {
				const unhandled: never = payload;
				console.warn('fleet: ignoring unknown payload kind', unhandled);
			}
		}
	}

	/**
	 * Sends an envelope through whichever channel is currently authoritative
	 * for gateway-owned resources that have no per-ward channel of their own
	 * (Fleet/Zone CRUD, fleet-wide mission assignment): the connected
	 * gateway if there is one, otherwise the always-available demo engine.
	 * Lets fleetGroups/zoneStore reach the wire without either holding its
	 * own transport reference.
	 */
	sendUpstream(envelope: Envelope): void {
		const channel = this.gatewayTransport ?? this.demoEngine;
		if (!channel) throw new Error('no active channel');
		channel.send(envelope);
	}

	/** full app teardown: stops both channels and wipes everything */
	teardown(): void {
		this.demoEngine?.stop();
		this.demoEngine = undefined;
		this.disconnectGateway();
		for (const timer of this.timeoutTimers.values()) clearTimeout(timer);
		this.timeoutTimers.clear();
		this.wards = {};
		this.events = [];
		this.commands = {};
		this.wardConfigRequests = {};
		this.paramLists = {};
		this.paramListLoading = {};
		this.paramListRequestIds = {};
		this.paramWrites = {};
		this.logEntries = {};
		this.logEntriesLoading = {};
		this.logEntriesRequestIds = {};
		this.logDownloads = {};
		this.logDownloadChunks = {};
		this.calibrationRuns = {};
		this.selectedWardId = undefined;
		this.cancelGoto();
		this.cancelSetHome();
		fleetGroups.teardown();
		zoneStore.teardown();
	}

	private channelFor(wardId: string): FleetTransport | undefined {
		const source = this.wards[wardId]?.source;
		if (source === 'demo') return this.demoEngine;
		if (source === 'gateway') return this.gatewayTransport;
		return undefined;
	}

	private trackConfigRequest(
		requestId: string,
		kind: 'add' | 'remove',
		wardId: string,
		buildEnvelope: () => Envelope
	): void {
		this.wardConfigRequests[requestId] = {
			requestId,
			kind,
			wardId,
			status: WardConfigStatus.WARD_CONFIG_STATUS_UNSPECIFIED,
			message: '',
			sentAtMs: Date.now()
		};
		if (!this.gatewayTransport) {
			this.settleConfig(
				requestId,
				WardConfigStatus.WARD_CONFIG_STATUS_REJECTED,
				'no gateway connected'
			);
			return;
		}
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				this.settleConfig(
					requestId,
					WardConfigStatus.WARD_CONFIG_STATUS_REJECTED,
					'no acknowledgment from gateway'
				);
			}, REQUEST_TIMEOUT_MS)
		);
		try {
			this.gatewayTransport.send(buildEnvelope());
		} catch (error) {
			const reason = error instanceof Error ? error.message : String(error);
			this.settleConfig(requestId, WardConfigStatus.WARD_CONFIG_STATUS_REJECTED, reason);
		}
	}

	private settle(commandId: string, status: CommandStatus, message: string): void {
		const tracker = this.commands[commandId];
		if (!tracker || isTerminal(tracker.status)) return;
		tracker.status = status;
		tracker.message = message;
		if (isTerminal(status)) {
			const timer = this.timeoutTimers.get(commandId);
			if (timer) {
				clearTimeout(timer);
				this.timeoutTimers.delete(commandId);
			}
			setTimeout(() => {
				delete this.commands[commandId];
			}, TRACKER_LINGER_MS);
		}
	}

	private settleConfig(requestId: string, status: WardConfigStatus, message: string): void {
		const tracker = this.wardConfigRequests[requestId];
		if (!tracker || isConfigTerminal(tracker.status)) return;
		tracker.status = status;
		tracker.message = message;
		if (isConfigTerminal(status)) {
			const timer = this.timeoutTimers.get(requestId);
			if (timer) {
				clearTimeout(timer);
				this.timeoutTimers.delete(requestId);
			}
			setTimeout(() => {
				delete this.wardConfigRequests[requestId];
			}, TRACKER_LINGER_MS);
		}
	}

	/** Resolves a GetParams response. Ignores a response whose request_id no
	 * longer matches the latest one sent for that ward - a stale answer to a
	 * superseded "refresh" click. */
	applyParamList(response: ParamList): void {
		if (this.paramListRequestIds[response.wardId] !== response.requestId) return;
		const timer = this.timeoutTimers.get(response.requestId);
		if (timer) {
			clearTimeout(timer);
			this.timeoutTimers.delete(response.requestId);
		}
		this.paramListLoading[response.wardId] = false;
		this.paramLists[response.wardId] = response;
	}

	/** Resolves a SetParam ack. On success, patches the cached ParamList for
	 * this ward with the vehicle-confirmed value (ParamAck.param - see its
	 * own doc comment: never an echo of the request) so the panel reflects
	 * it without a full re-fetch. */
	applyParamAck(ack: ParamAck): void {
		this.settleParamWrite(ack.requestId, ack.status, ack.message);
		if (ack.status !== ParamAckStatus.PARAM_ACK_STATUS_SUCCESS || !ack.param) return;
		const list = this.paramLists[ack.wardId];
		if (!list) return;
		const confirmed = ack.param;
		const index = list.params.findIndex((candidate) => candidate.name === confirmed.name);
		if (index >= 0) {
			list.params[index] = confirmed;
		} else {
			list.params.push(confirmed);
		}
	}

	private settleParamWrite(requestId: string, status: ParamAckStatus, message: string): void {
		const tracker = this.paramWrites[requestId];
		if (!tracker || isParamWriteTerminal(tracker.status)) return;
		tracker.status = status;
		tracker.message = message;
		if (isParamWriteTerminal(status)) {
			const timer = this.timeoutTimers.get(requestId);
			if (timer) {
				clearTimeout(timer);
				this.timeoutTimers.delete(requestId);
			}
			setTimeout(() => {
				delete this.paramWrites[requestId];
			}, TRACKER_LINGER_MS);
		}
	}

	/** Resolves a GetLogEntries response. Same stale-response guard as
	 * applyParamList() above. */
	applyLogEntryList(response: LogEntryList): void {
		if (this.logEntriesRequestIds[response.wardId] !== response.requestId) return;
		const timer = this.timeoutTimers.get(response.requestId);
		if (timer) {
			clearTimeout(timer);
			this.timeoutTimers.delete(response.requestId);
		}
		this.logEntriesLoading[response.wardId] = false;
		this.logEntries[response.wardId] = response;
	}

	/** Resolves the immediate accept/reject for a DownloadLogFile request.
	 * REJECTED ends the download outright; SUCCESS arms this store to
	 * receive exactly chunk_count LogFileChunk messages next (0 completes
	 * immediately - an empty file has nothing to wait for). */
	applyDownloadLogFileAck(ack: DownloadLogFileAck): void {
		const tracker = this.logDownloads[ack.requestId];
		if (!tracker || tracker.status !== 'pending') return;
		const timer = this.timeoutTimers.get(ack.requestId);
		if (timer) {
			clearTimeout(timer);
			this.timeoutTimers.delete(ack.requestId);
		}
		if (ack.status === LogDownloadStatus.LOG_DOWNLOAD_STATUS_REJECTED) {
			this.settleLogDownload(ack.requestId, ack.message);
			return;
		}
		tracker.totalSizeBytes = ack.totalSizeBytes;
		tracker.chunkCount = ack.chunkCount;
		if (ack.chunkCount === 0) {
			this.completeLogDownload(ack.requestId, new Uint8Array(0));
			return;
		}
		tracker.status = 'downloading';
		// .fill(undefined), not a bare `new Array(n)`: the latter is sparse
		// (holes, not real undefined values), and every/some/forEach/filter
		// all SKIP holes per spec - chunks.every(piece => piece !== undefined)
		// below would then vacuously return true after just one chunk filled
		// one slot, since the remaining holes are never visited at all. Only
		// for...of (used to reassemble the bytes) actually visits holes,
		// yielding literal undefined there - which crashed Uint8Array.set().
		this.logDownloadChunks[ack.requestId] = new Array(ack.chunkCount).fill(undefined);
		this.armLogChunkStallTimer(ack.requestId);
	}

	/** Reassembles LogFileChunk messages for one accepted download. A
	 * REJECTED chunk means the gateway aborted mid-stream (see
	 * log_file.proto's own comment on LogFileChunk) - no further chunks
	 * will follow, so this settles immediately rather than waiting out the
	 * stall timer. Out-of-range or duplicate indices are ignored
	 * defensively; a single ordered WebSocket/relay connection should never
	 * produce either, but silently dropping an impossible frame is safer
	 * than indexing past the end of the array. */
	applyLogFileChunk(chunk: LogFileChunk): void {
		const tracker = this.logDownloads[chunk.requestId];
		if (!tracker || tracker.status !== 'downloading') return;
		if (chunk.status === LogDownloadStatus.LOG_DOWNLOAD_STATUS_REJECTED) {
			delete this.logDownloadChunks[chunk.requestId];
			this.settleLogDownload(chunk.requestId, chunk.message || 'download aborted mid-stream');
			return;
		}
		const chunks = this.logDownloadChunks[chunk.requestId];
		if (!chunks || chunk.chunkIndex >= chunks.length || chunks[chunk.chunkIndex] !== undefined) {
			return;
		}
		chunks[chunk.chunkIndex] = chunk.data;
		tracker.receivedBytes += chunk.data.length;
		if (chunks.every((piece) => piece !== undefined)) {
			const bytes = new Uint8Array(tracker.receivedBytes);
			let offset = 0;
			for (const piece of chunks) {
				bytes.set(piece!, offset);
				offset += piece!.length;
			}
			delete this.logDownloadChunks[chunk.requestId];
			this.completeLogDownload(chunk.requestId, bytes);
			return;
		}
		this.armLogChunkStallTimer(chunk.requestId);
	}

	/** (Re)arms the "no further chunks" stall timer, reset on every chunk
	 * received so a large transfer isn't penalized for simply taking a
	 * while - only silence between chunks is a failure, not total elapsed
	 * time. */
	private armLogChunkStallTimer(requestId: string): void {
		const existing = this.timeoutTimers.get(requestId);
		if (existing) clearTimeout(existing);
		this.timeoutTimers.set(
			requestId,
			setTimeout(() => {
				delete this.logDownloadChunks[requestId];
				this.settleLogDownload(requestId, 'no further chunks received from gateway');
			}, REQUEST_TIMEOUT_MS)
		);
	}

	private completeLogDownload(requestId: string, bytes: Uint8Array): void {
		const timer = this.timeoutTimers.get(requestId);
		if (timer) {
			clearTimeout(timer);
			this.timeoutTimers.delete(requestId);
		}
		const tracker = this.logDownloads[requestId];
		if (!tracker) return;
		tracker.status = 'success';
		tracker.message = '';
		triggerBrowserDownload(
			logFileName(tracker.wardId, tracker.logId, this.wards[tracker.wardId]?.info?.autopilot),
			bytes
		);
		setTimeout(() => {
			delete this.logDownloads[requestId];
		}, TRACKER_LINGER_MS);
	}

	private settleLogDownload(requestId: string, message: string): void {
		const tracker = this.logDownloads[requestId];
		if (!tracker || isLogDownloadTerminal(tracker.status)) return;
		const timer = this.timeoutTimers.get(requestId);
		if (timer) {
			clearTimeout(timer);
			this.timeoutTimers.delete(requestId);
		}
		delete this.logDownloadChunks[requestId];
		tracker.status = 'rejected';
		tracker.message = message;
		setTimeout(() => {
			delete this.logDownloads[requestId];
		}, TRACKER_LINGER_MS);
	}

	/** Resolves a CalibrationProgress update: RUNNING (re-arms the stall
	 * timer - a calibration is operator-paced, so only silence between
	 * updates is a failure, not total elapsed time, same reasoning as
	 * armLogChunkStallTimer()) or one of the terminal statuses (settles
	 * and lingers before clearing, same TRACKER_LINGER_MS every other
	 * tracker uses). Ignores a message whose request_id no longer matches
	 * this ward's current run (stale/superseded) or that arrives after
	 * the run already settled. */
	applyCalibrationProgress(progress: CalibrationProgress): void {
		const run = this.calibrationRuns[progress.wardId];
		if (!run || run.requestId !== progress.requestId || isCalibrationTerminal(run.status)) return;
		const timer = this.timeoutTimers.get(progress.requestId);
		if (timer) {
			clearTimeout(timer);
			this.timeoutTimers.delete(progress.requestId);
		}

		const status = calibrationRunStatus(progress.status);
		run.status = status;
		run.message = progress.message;
		run.hasProgress = progress.hasProgress;
		run.progress = progress.progress;
		run.hasInstruction = progress.hasInstruction;
		run.instruction = progress.instruction;

		if (status === 'running') {
			this.timeoutTimers.set(
				progress.requestId,
				setTimeout(() => {
					this.settleCalibration(
						progress.wardId,
						progress.requestId,
						'failed',
						'no further progress from gateway'
					);
				}, REQUEST_TIMEOUT_MS)
			);
			return;
		}
		setTimeout(() => {
			if (this.calibrationRuns[progress.wardId]?.requestId === progress.requestId) {
				delete this.calibrationRuns[progress.wardId];
			}
		}, TRACKER_LINGER_MS);
	}

	private settleCalibration(
		wardId: string,
		requestId: string,
		status: 'failed' | 'rejected',
		message: string
	): void {
		const run = this.calibrationRuns[wardId];
		if (!run || run.requestId !== requestId || isCalibrationTerminal(run.status)) return;
		const timer = this.timeoutTimers.get(requestId);
		if (timer) {
			clearTimeout(timer);
			this.timeoutTimers.delete(requestId);
		}
		run.status = status;
		run.message = message;
		setTimeout(() => {
			if (this.calibrationRuns[wardId]?.requestId === requestId) {
				delete this.calibrationRuns[wardId];
			}
		}, TRACKER_LINGER_MS);
	}

	private persistDemoWardHomes(): void {
		if (typeof localStorage === 'undefined') return;
		const homes = Object.values(this.demoWardHomes);
		if (homes.length > 0) {
			localStorage.setItem(DEMO_WARD_HOMES_STORAGE_KEY, JSON.stringify(homes));
		} else {
			localStorage.removeItem(DEMO_WARD_HOMES_STORAGE_KEY);
		}
	}

	private upsert(wardId: string, source: WardSource): Ward {
		if (!(wardId in this.wards)) {
			this.wards[wardId] = { info: undefined, state: undefined, source };
			// Restoring a selection from a previous session (see
			// pendingSelectedWardId's own comment) - this is the one place
			// a ward ID actually starts existing, for a demo respawn or a
			// gateway sync alike.
			if (this.pendingSelectedWardId === wardId) this.select(wardId);
		}
		return this.wards[wardId];
	}
}

export const fleet = new FleetStore();
