<script lang="ts">
	import type { WeatherIconShape } from '$lib/weather/condition';

	interface Props {
		shape: WeatherIconShape;
		size?: number;
	}
	let { shape, size = 20 }: Props = $props();
</script>

<svg
	width={size}
	height={size}
	viewBox="0 0 24 24"
	fill="none"
	stroke="currentColor"
	stroke-width="1.75"
	stroke-linecap="round"
	stroke-linejoin="round"
	aria-hidden="true"
>
	{#if shape === 'clear-day'}
		<circle cx="12" cy="12" r="5" />
		<path
			d="M12 2.5v2.5M12 19v2.5M2.5 12H5M19 12h2.5M4.9 4.9l1.8 1.8M17.3 17.3l1.8 1.8M4.9 19.1l1.8-1.8M17.3 6.7l1.8-1.8"
		/>
	{:else if shape === 'clear-night'}
		<path d="M20.5 14.2A8.4 8.4 0 1 1 9.8 3.5a6.7 6.7 0 0 0 10.7 10.7Z" />
	{:else if shape === 'partly-cloudy-day'}
		<circle cx="8" cy="8" r="3.2" />
		<path d="M8 3v1.6M8 12.4V14M3 8h1.6M12.4 8H14M4.7 4.7l1.1 1.1M10.2 4.7l-1.1 1.1" />
		<path d="M8.5 20h9a4 4 0 0 0 .4-8 5.5 5.5 0 0 0-10.4-1.3A4.3 4.3 0 0 0 8.5 20Z" />
	{:else if shape === 'partly-cloudy-night'}
		<path d="M15 8.3a5.3 5.3 0 1 1-6.6-6.6 4.2 4.2 0 0 0 6.6 6.6Z" />
		<path d="M6.5 20h9a4 4 0 0 0 .4-8 5.5 5.5 0 0 0-10.4-1.3A4.3 4.3 0 0 0 6.5 20Z" />
	{:else if shape === 'cloudy'}
		<path d="M5 18h12.5a4 4 0 0 0 .5-8 6 6 0 0 0-11.4-1.8A4.5 4.5 0 0 0 5 18Z" />
	{:else if shape === 'fog'}
		<path d="M4 8.5h13M3 12.5h16.5M5.5 16.5h13M8 20.5h9" />
	{:else if shape === 'rain'}
		<path d="M5 14h12.5a4 4 0 0 0 .5-8 6 6 0 0 0-11.4-1.8A4.5 4.5 0 0 0 5 14Z" />
		<path d="M8 17.5l-1.2 3M12.5 17.5l-1.2 3M17 17.5l-1.2 3" />
	{:else if shape === 'snow'}
		<path d="M5 14h12.5a4 4 0 0 0 .5-8 6 6 0 0 0-11.4-1.8A4.5 4.5 0 0 0 5 14Z" />
		<path d="M8 17v4M6 18.2l4 1.6M10 18.2l-4 1.6M16 17v4M14 18.2l4 1.6M18 18.2l-4 1.6" />
	{:else if shape === 'thunderstorm'}
		<path d="M5 12.5h12.5a4 4 0 0 0 .5-8 6 6 0 0 0-11.4-1.8A4.5 4.5 0 0 0 5 12.5Z" />
		<path d="M13.5 13l-3.5 5.5h3.2l-2 4.5" />
	{/if}
</svg>
