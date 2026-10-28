export type MobileSurface = 'drawer' | 'sheet' | null;

/**
 * Tracks which mobile-only overlay (the left drawer or the bottom sheet)
 * is open below the sm breakpoint, so opening one closes the other.
 * Mirrors left-rail-ui.svelte.ts/right-panel-ui.svelte.ts's singleton
 * shape and, like them, is app-shell state - not exported from this
 * package's barrel (see index.ts's own comment on EdgeSheet). A
 * consuming app building its own mobile shell with EdgeSheet gets its
 * own copy of this exact pattern, the same way karshipta-cloud already
 * has its own rail-ui.svelte.ts rather than importing this console's.
 * Purely ephemeral (no localStorage): "what's open right now" has no
 * reason to survive a reload the way a collapsed preference does.
 */
class MobileSurfaceUi {
	open = $state<MobileSurface>(null);

	openDrawer(): void {
		this.open = 'drawer';
	}

	openSheet(): void {
		this.open = 'sheet';
	}

	close(): void {
		this.open = null;
	}
}

export const mobileSurfaceUi = new MobileSurfaceUi();
