import type { City } from './types';

// Bundled from GeoNames' cities15000 dump (CC BY 4.0 - https://www.geonames.org),
// filtered to population >= 100,000 (6,234 of the original ~34,000 entries
// with population >= 15,000). A static, offline dataset rather than a
// live-fetched layer like obstacles/airports: city locations don't change,
// so there's no reason to pay a network round trip - or share OpenAIP's
// rate-limited key - for data that's the same today as it'll be next year.
// Dynamically imported (see loadCities below) so this ~260KB JSON is
// code-split into its own chunk, not part of every consuming app's initial
// bundle - it only loads once an operator actually turns the Cities layer
// on.
type CityRow = [
	name: string,
	latitudeDeg: number,
	longitudeDeg: number,
	countryCode: string,
	population: number
];

let cached: City[] | undefined;

export async function loadCities(): Promise<City[]> {
	if (cached) return cached;
	const module = await import('./cities-data.json');
	const rows = module.default as CityRow[];
	cached = rows.map(([name, latitudeDeg, longitudeDeg, countryCode, population]) => ({
		name,
		latitudeDeg,
		longitudeDeg,
		countryCode,
		population
	}));
	return cached;
}
