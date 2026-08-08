export interface WeatherConditions {
	temperatureC: number;
	/** WMO weather interpretation code (the standard used by Open-Meteo and
	 * most other providers) - see condition.ts for the code-to-icon/label
	 * mapping. */
	weatherCode: number;
	isDay: boolean;
	feelsLikeC: number | undefined;
	humidityPct: number | undefined;
	cloudCoverPct: number | undefined;
	pressureHpa: number | undefined;
	windSpeedMS: number;
	windDirectionDeg: number;
	windGustMS: number | undefined;
	precipitationMm: number;
	/** mm, converted from Open-Meteo's own cm at the source (see
	 * open-meteo.ts) so every precipitation-depth field here shares one
	 * unit and can share formatPrecipitation regardless of what physically
	 * fell. */
	rainMm: number | undefined;
	showersMm: number | undefined;
	snowfallMm: number | undefined;
	visibilityM: number | undefined;
}

export interface WeatherSource {
	fetchConditions(latitudeDeg: number, longitudeDeg: number): Promise<WeatherConditions>;
}
