/**
 * Serializes every OpenAIP request across every layer that reads from it
 * (airspaces/geozones, obstacles, airports - see each's own openaip.ts),
 * because OpenAIP's free-tier key is rate-limited per key, not per endpoint:
 * confirmed directly that 4 requests inside ~6s starts returning 429s
 * (geozone-store.svelte.ts's own original comment), and a 429 carries no
 * Access-Control-Allow-Origin header, so the browser can't even see it as a
 * 429 - fetch() just throws a generic "Failed to fetch" TypeError,
 * indistinguishable from a real network failure. With three independent
 * layers each debouncing and firing on their own schedule, enabling more
 * than one at once could trivially exceed that combined budget even though
 * each layer alone stays well under it. Every layer's own store still owns
 * its own moveend debounce and cache (this gate has no opinion on either) -
 * this only decides *when it's safe to actually start* a request relative
 * to every other layer's own requests, and backs every layer off together
 * after any one of them fails.
 */

// Minimum gap between the start of two OpenAIP requests, regardless of which
// layer they're for.
const MIN_REQUEST_GAP_MS = 1500;
// Shared with geozone-store.svelte.ts's own value: confirmed directly that
// OpenAIP's rate-limit window clears in roughly 5-10s.
const FAILURE_COOLDOWN_MS = 10_000;

type FetchTask = () => Promise<void>;

class OpenAipRequestGate {
	private queue: FetchTask[] = [];
	private cooldownUntilMs = 0;
	private lastRequestStartMs = 0;
	private draining = false;

	/** Queues a layer's fetch to run as soon as the shared gap/cooldown
	 * allows, resolving once it actually runs; call sites are expected to
	 * already be debounced on their own (see each store's own moveend
	 * debounce) - this only orders and spaces out whatever actually gets
	 * enqueued, it does not debounce itself. */
	enqueue(task: FetchTask): Promise<void> {
		return new Promise((resolve) => {
			this.queue.push(async () => {
				await task();
				resolve();
			});
			void this.drain();
		});
	}

	/** Backs every queued and future request off together, not just the
	 * layer whose request actually failed - the two are indistinguishable
	 * from here (see this file's own header comment), so treating one
	 * layer's failure as "the key is currently throttled" for all of them
	 * is the safe assumption. */
	noteFailure(): void {
		this.cooldownUntilMs = Date.now() + FAILURE_COOLDOWN_MS;
	}

	private async drain(): Promise<void> {
		if (this.draining) return;
		this.draining = true;
		while (this.queue.length > 0) {
			const now = Date.now();
			const waitMs = Math.max(
				0,
				this.cooldownUntilMs - now,
				this.lastRequestStartMs + MIN_REQUEST_GAP_MS - now
			);
			if (waitMs > 0) await new Promise((resolve) => setTimeout(resolve, waitMs));
			const task = this.queue.shift();
			if (!task) break;
			this.lastRequestStartMs = Date.now();
			await task();
		}
		this.draining = false;
	}
}

export const openAipRequestGate = new OpenAipRequestGate();
