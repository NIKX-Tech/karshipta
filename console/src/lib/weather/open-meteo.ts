import type { WeatherConditions, WeatherSource } from './types';

const OPEN_METEO_API_URL = 'https://api.open-meteo.com/v1/forecast';
const KMH_TO_MS = 1 / 3.6;
const CM_TO_MM = 10;

const CURRENT_FIELDS = [
	'temperature_2m',
	'relative_humidity_2m',
	'apparent_temperature',
	'is_day',
	'weather_code',
	'cloud_cover',
	'pressure_msl',
	'precipitation',
	'rain',
	'showers',
	'snowfall',
	'wind_speed_10m',
	'wind_direction_10m',
	'wind_gusts_10m',
	'visibility'
].join(',');

function numberField(data: Record<string, unknown>, key: string): number | undefined {
	const value = data[key];
	return typeof value === 'number' ? value : undefined;
}

export class OpenMeteoWeatherSource implements WeatherSource {
	async fetchConditions(latitudeDeg: number, longitudeDeg: number): Promise<WeatherConditions> {
		const url =
			`${OPEN_METEO_API_URL}?latitude=${latitudeDeg}&longitude=${longitudeDeg}` +
			`&current=${CURRENT_FIELDS}`;
		const response = await fetch(url);
		if (!response.ok) {
			throw new Error(`Open-Meteo request failed: ${response.status} ${response.statusText}`);
		}
		const body: unknown = await response.json();
		const current =
			typeof body === 'object' && body !== null
				? (body as { current?: unknown }).current
				: undefined;
		if (typeof current !== 'object' || current === null) {
			throw new Error('Open-Meteo: unexpected response shape');
		}
		const data = current as Record<string, unknown>;
		const temperatureC = numberField(data, 'temperature_2m');
		const weatherCode = numberField(data, 'weather_code');
		// Confirmed live: wind_speed_10m/wind_gusts_10m are km/h by default
		// (see current_units in the same response), not m/s - converted here
		// so every speed in this app is m/s internally, same as everywhere
		// else (formatSpeed only ever takes m/s).
		const windSpeedKmh = numberField(data, 'wind_speed_10m');
		const windDirectionDeg = numberField(data, 'wind_direction_10m');
		const windGustKmh = numberField(data, 'wind_gusts_10m');
		const precipitationMm = numberField(data, 'precipitation');
		// snowfall is cm from Open-Meteo, converted here so every
		// precipitation-depth field on WeatherConditions shares one unit -
		// see types.ts's own comment on snowfallMm.
		const snowfallCm = numberField(data, 'snowfall');
		if (
			temperatureC === undefined ||
			weatherCode === undefined ||
			windSpeedKmh === undefined ||
			windDirectionDeg === undefined ||
			precipitationMm === undefined
		) {
			throw new Error('Open-Meteo: missing expected fields');
		}
		return {
			temperatureC,
			weatherCode,
			isDay: data.is_day === 1,
			feelsLikeC: numberField(data, 'apparent_temperature'),
			humidityPct: numberField(data, 'relative_humidity_2m'),
			cloudCoverPct: numberField(data, 'cloud_cover'),
			pressureHpa: numberField(data, 'pressure_msl'),
			windSpeedMS: windSpeedKmh * KMH_TO_MS,
			windDirectionDeg,
			windGustMS: windGustKmh !== undefined ? windGustKmh * KMH_TO_MS : undefined,
			precipitationMm,
			rainMm: numberField(data, 'rain'),
			showersMm: numberField(data, 'showers'),
			snowfallMm: snowfallCm !== undefined ? snowfallCm * CM_TO_MM : undefined,
			visibilityM: numberField(data, 'visibility')
		};
	}
}
