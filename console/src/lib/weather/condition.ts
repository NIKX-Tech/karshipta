/** Shapes actually drawn by WeatherIcon - collapsed from the much larger
 * WMO code table below into the handful of icons that are visually
 * distinct at a glance. */
export type WeatherIconShape =
	| 'clear-day'
	| 'clear-night'
	| 'partly-cloudy-day'
	| 'partly-cloudy-night'
	| 'cloudy'
	| 'fog'
	| 'rain'
	| 'snow'
	| 'thunderstorm';

// WMO weather interpretation codes (WMO code table 4677), the standard
// Open-Meteo and most other providers use for `weather_code` - confirmed
// live against real locations (0 = clear sky in London, 1 = mainly clear
// in Singapore, 53 = moderate drizzle in Reykjavik, each matching real
// conditions at the time).
const CONDITION_LABELS: Record<number, string> = {
	0: 'Clear sky',
	1: 'Mainly clear',
	2: 'Partly cloudy',
	3: 'Overcast',
	45: 'Fog',
	48: 'Rime fog',
	51: 'Light drizzle',
	53: 'Drizzle',
	55: 'Dense drizzle',
	56: 'Light freezing drizzle',
	57: 'Freezing drizzle',
	61: 'Light rain',
	63: 'Rain',
	65: 'Heavy rain',
	66: 'Light freezing rain',
	67: 'Freezing rain',
	71: 'Light snow',
	73: 'Snow',
	75: 'Heavy snow',
	77: 'Snow grains',
	80: 'Light rain showers',
	81: 'Rain showers',
	82: 'Violent rain showers',
	85: 'Snow showers',
	86: 'Heavy snow showers',
	95: 'Thunderstorm',
	96: 'Thunderstorm with hail',
	99: 'Severe thunderstorm with hail'
};

const RAIN_CODES = new Set([51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 80, 81, 82]);
const SNOW_CODES = new Set([71, 73, 75, 77, 85, 86]);
const THUNDERSTORM_CODES = new Set([95, 96, 99]);
const FOG_CODES = new Set([45, 48]);

export function weatherConditionLabel(weatherCode: number): string {
	return CONDITION_LABELS[weatherCode] ?? 'Unknown conditions';
}

export function weatherIconShape(weatherCode: number, isDay: boolean): WeatherIconShape {
	if (THUNDERSTORM_CODES.has(weatherCode)) return 'thunderstorm';
	if (SNOW_CODES.has(weatherCode)) return 'snow';
	if (RAIN_CODES.has(weatherCode)) return 'rain';
	if (FOG_CODES.has(weatherCode)) return 'fog';
	if (weatherCode === 0) return isDay ? 'clear-day' : 'clear-night';
	if (weatherCode === 1 || weatherCode === 2)
		return isDay ? 'partly-cloudy-day' : 'partly-cloudy-night';
	if (weatherCode === 3) return 'cloudy';
	return 'cloudy';
}
