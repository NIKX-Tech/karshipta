// See https://svelte.dev/docs/kit/types#app.d.ts
// for information about these interfaces
declare global {
	namespace App {
		// interface Error {}
		// interface Locals {}
		// interface PageData {}
		// interface PageState {}
		// interface Platform {}
	}

	// Injected by vite.config.ts's define; console-core's package.json
	// version, for about-dialog.svelte.
	const __APP_VERSION__: string;
	// Injected by vite.config.ts's define; the whole product's own release
	// version (../VERSION), distinct from __APP_VERSION__ above.
	const __PRODUCT_VERSION__: string;
}

export {};
