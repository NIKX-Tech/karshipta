import { Envelope } from '$lib/gen/karshipta/v1/envelope';

export type TransportStatus = 'connecting' | 'open' | 'closed';

export interface TransportHandlers {
	onEnvelope: (envelope: Envelope) => void;
	onStatus?: (status: TransportStatus) => void;
}

/**
 * What the rest of the console sees. The real WebSocket transport and the
 * dev FakeGateway both implement it; nothing outside may depend on which
 * one is active.
 */
export interface FleetTransport {
	start(): void;
	stop(): void;
	send(envelope: Envelope): void;
}

const INITIAL_BACKOFF_MS = 500;
const MAX_BACKOFF_MS = 10_000;
const BACKOFF_FACTOR = 2;

/**
 * Owns the WebSocket to the gateway. One binary protobuf Envelope per frame.
 * Reconnects forever with capped exponential backoff; components never see
 * this class directly, they read the FleetStore it feeds.
 */
export class WebSocketTransport implements FleetTransport {
	private socket: WebSocket | undefined;
	private reconnectTimer: ReturnType<typeof setTimeout> | undefined;
	private backoffMs = INITIAL_BACKOFF_MS;
	private stopped = true;

	constructor(
		private readonly url: string,
		private readonly handlers: TransportHandlers
	) {}

	start(): void {
		if (!this.stopped) return;
		this.stopped = false;
		this.connect();
	}

	stop(): void {
		this.stopped = true;
		if (this.reconnectTimer !== undefined) {
			clearTimeout(this.reconnectTimer);
			this.reconnectTimer = undefined;
		}
		if (this.socket) {
			this.socket.close();
			this.socket = undefined;
		}
		this.setStatus('closed');
	}

	send(envelope: Envelope): void {
		if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
			throw new Error(`transport not open, cannot send envelope to ${this.url}`);
		}
		this.socket.send(Envelope.encode(envelope).finish());
	}

	private connect(): void {
		this.setStatus('connecting');
		const socket = new WebSocket(this.url);
		socket.binaryType = 'arraybuffer';
		this.socket = socket;

		socket.onopen = () => {
			this.backoffMs = INITIAL_BACKOFF_MS;
			this.setStatus('open');
		};

		socket.onmessage = (message: MessageEvent) => {
			if (!(message.data instanceof ArrayBuffer)) {
				console.error('transport: expected binary frame, got', typeof message.data);
				return;
			}
			try {
				this.handlers.onEnvelope(Envelope.decode(new Uint8Array(message.data)));
			} catch (error) {
				console.error('transport: failed to decode Envelope frame', error);
			}
		};

		socket.onclose = () => {
			if (this.socket === socket) this.socket = undefined;
			this.scheduleReconnect();
		};

		// onclose always follows onerror, so reconnect is scheduled there
		socket.onerror = () => {
			console.error(`transport: websocket error on ${this.url}`);
		};
	}

	private scheduleReconnect(): void {
		if (this.stopped || this.reconnectTimer !== undefined) return;
		this.setStatus('closed');
		const jitter = Math.random() * 0.3 * this.backoffMs;
		const delay = this.backoffMs + jitter;
		this.backoffMs = Math.min(this.backoffMs * BACKOFF_FACTOR, MAX_BACKOFF_MS);
		this.reconnectTimer = setTimeout(() => {
			this.reconnectTimer = undefined;
			if (!this.stopped) this.connect();
		}, delay);
	}

	private setStatus(status: TransportStatus): void {
		this.handlers.onStatus?.(status);
	}
}
