import {
	RelaylyClient,
	generateKey,
	encodeBase64,
	keyPairFromPrivateKey,
	type KeyPair,
	type Peer
} from 'relayly';
import { Envelope } from '$lib/gen/karshipta/v1/envelope';
import type { FleetTransport, TransportHandlers, TransportStatus } from './index';

const DEVICE_KEY_STORAGE_KEY = 'karshipta.relayDeviceKey';

/**
 * Wraps relayly's browser SDK as a FleetTransport, the console-side half of
 * gateway/docs/relay-transport.md. Unlike WebSocketTransport, this class
 * does not implement its own reconnect/backoff loop: RelaylyClient already
 * reconnects on its own (capped exponential backoff, re-authenticates), so
 * this class only translates its events into the FleetTransport shape.
 *
 * Pairing (requestPairCode/acceptPair) is deliberately not part of start():
 * it is a one-time setup action, the same split as request_pair_code/
 * accept_pair not being part of Transport on the gateway side. Call
 * pairWithGateway() once, from the pairing UI, after start() has connected.
 *
 * Built against relayly@0.5.0 (protocol v1: Noise XX, device tokens, peer
 * pinning) - deviceToken is a real required option now, unlike 0.3.0's
 * published types. 'open' tracks the 'ready' event (fired once a peer's
 * Noise session is actually usable for send(), including after a
 * reconnect-triggered re-handshake), not 'connected' or 'paired' alone -
 * mirrors RelayTransport::handle_peer_ready on the gateway side, which
 * makes the identical distinction (on_ready vs. the peer_status online
 * transition) for the same reason: a peer can be online before its session
 * is ready to send to.
 */
export class RelayTransport implements FleetTransport {
	private readonly client: RelaylyClient;
	private peer: Peer | undefined;
	private peerReady = false;
	private stopped = true;

	constructor(
		relayUrl: string,
		deviceId: string,
		deviceToken: string,
		private readonly handlers: TransportHandlers
	) {
		this.client = new RelaylyClient(relayUrl, {
			deviceId,
			deviceToken,
			keyPair: loadOrCreateDeviceKey()
			// peerStore intentionally omitted: defaults to relayly's in-memory
			// store (pinning does not survive a reload). A persistent browser
			// store (IndexedDB-backed) is future work, not needed for v1 - the
			// gateway side already pins durably to disk.
		});

		this.client.on('connected', () => {
			if (!this.peerReady) this.setStatus('connecting');
		});
		this.client.on('disconnected', () => {
			this.peerReady = false;
			this.setStatus('closed');
		});
		this.client.on('reconnecting', () => {
			this.peerReady = false;
			this.setStatus('connecting');
		});
		this.client.on('paired', (peer) => {
			this.peer = peer;
		});
		this.client.on('ready', () => {
			this.peerReady = true;
			this.setStatus('open');
		});
		this.client.on('peerStatus', (_peerId, online) => {
			if (!online) {
				this.peerReady = false;
				this.setStatus('connecting');
			}
		});
		this.client.on('message', (msg) => {
			try {
				this.handlers.onEnvelope(Envelope.decode(msg.rawPayload));
			} catch (error) {
				console.error('relay transport: failed to decode Envelope frame', error);
			}
		});
		this.client.on('error', (error) => {
			console.error(`relay transport: server error (${error.code})`, error.message);
		});
	}

	start(): void {
		if (!this.stopped) return;
		this.stopped = false;
		this.setStatus('connecting');
		this.client.connect().catch((error) => {
			console.error('relay transport: failed to connect', error);
			this.setStatus('closed');
		});
	}

	stop(): void {
		this.stopped = true;
		this.peerReady = false;
		this.client.disconnect();
		this.peer = undefined;
		this.setStatus('closed');
	}

	send(envelope: Envelope): void {
		if (!this.peer || !this.peerReady) {
			throw new Error('relay transport not paired, cannot send envelope');
		}
		this.client
			.send(this.peer.id, Envelope.encode(envelope).finish())
			.catch((error) => console.error('relay transport: failed to send envelope', error));
	}

	/**
	 * One-time pairing against a code the gateway's pairing tool displayed
	 * (gateway/docs/relay-transport.md). Requires start() to have connected
	 * first. Not part of FleetTransport, same reasoning as accept_pair not
	 * being part of Transport on the gateway side. acceptPair() itself
	 * already blocks until the Noise handshake completes, so 'ready' has
	 * already fired by the time this resolves.
	 */
	async pairWithGateway(code: string): Promise<Peer> {
		return await this.client.acceptPair(code);
	}

	isPaired(): boolean {
		return this.peer !== undefined && this.peerReady;
	}

	private setStatus(status: TransportStatus): void {
		this.handlers.onStatus?.(status);
	}
}

function loadOrCreateDeviceKey(): KeyPair {
	if (typeof localStorage === 'undefined') return generateKey();
	const saved = localStorage.getItem(DEVICE_KEY_STORAGE_KEY);
	if (saved) {
		try {
			return keyPairFromPrivateKey(saved);
		} catch (error) {
			console.warn('relay transport: saved device key was invalid, generating a new one', error);
		}
	}
	const key = generateKey();
	localStorage.setItem(DEVICE_KEY_STORAGE_KEY, encodeBase64(key.privateKey));
	return key;
}
