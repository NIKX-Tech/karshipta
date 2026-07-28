<script lang="ts">
	import Dialog from '$lib/components/ui/dialog.svelte';
	import logoMark from '$lib/assets/logo-mark.svg';

	interface Props {
		onclose: () => void;
	}

	const { onclose }: Props = $props();
</script>

<Dialog label="About Karshipta" {onclose}>
	<div class="flex items-center gap-2">
		<img src={logoMark} alt="" class="h-5 w-auto" aria-hidden="true" />
		<h3 class="font-display text-sm font-semibold">Karshipta</h3>
	</div>
	<p class="mt-1 font-mono text-[11px] text-fg-muted tabular-nums">console v{__APP_VERSION__}</p>

	<p class="mt-3 text-xs text-fg-muted">
		Open-source, self-hosted command and control for unmanned vehicle and tracker fleets - from
		flight vehicles over MAVLink to any other tracked entity, GPS tags and generic trackers
		included.
	</p>
	<p class="mt-2 text-xs text-accent">
		Simulation-first. Not certified for real flight operations yet.
	</p>

	<dl class="mt-3 space-y-1 text-[11px] text-fg-muted">
		<div class="flex justify-between gap-2">
			<dt>License</dt>
			<dd>AGPL-3.0</dd>
		</div>
		<div class="flex justify-between gap-2">
			<dt>Copyright</dt>
			<dd>&copy; NIKX Technologies B.V.</dd>
		</div>
	</dl>

	<div class="mt-4 flex justify-between gap-2">
		<a
			href="https://github.com/NIKX-Tech/karshipta"
			target="_blank"
			rel="noreferrer"
			class="rounded border border-edge px-2 py-1 text-[11px] text-fg-muted hover:text-fg"
		>
			GitHub
		</a>
		<button
			data-autofocus
			onclick={onclose}
			class="rounded border border-accent/60 bg-accent/15 px-3 py-1.5 text-xs font-medium text-accent hover:bg-accent/25"
		>
			Close
		</button>
	</div>
</Dialog>
