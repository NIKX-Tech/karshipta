/**
 * App-wide light/dark theme, shared by any consuming app (this console and
 * karshipta-cloud both import theme.css directly, see its own top comment)
 * rather than each maintaining its own toggle and palette. Applies
 * data-theme to the document root, which is what the CSS custom-property
 * overrides in theme.css key off; FleetMap reads .current reactively to
 * pick a matching basemap rather than exposing its own separate light/dark
 * choice.
 */
export type Theme = 'dark' | 'light';

const STORAGE_KEY = 'karshipta:theme';

class ThemeStore {
	current = $state<Theme>('dark');

	/** Reads any previously saved choice and applies it; call once, client-side only. */
	init(): void {
		if (typeof window === 'undefined') return;
		const stored = window.localStorage.getItem(STORAGE_KEY);
		if (stored === 'dark' || stored === 'light') this.current = stored;
		this.apply();
	}

	set(theme: Theme): void {
		this.current = theme;
		if (typeof window === 'undefined') return;
		window.localStorage.setItem(STORAGE_KEY, theme);
		this.apply();
	}

	toggle(): void {
		this.set(this.current === 'dark' ? 'light' : 'dark');
	}

	private apply(): void {
		document.documentElement.dataset.theme = this.current;
	}
}

export const themeStore = new ThemeStore();
