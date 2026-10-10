// @ts-check
// The Melange documentation site (https://jaminb.github.io/melange/). See README.md in this folder.
// `npm run build` / `npm run dev` run scripts/prebuild.mjs first, which generates src/content/docs/** and src/data/**.
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import { sidebar } from './scripts/sidebar.mjs';

export default defineConfig({
	site: 'https://jaminb.github.io',
	base: '/melange',
	trailingSlash: 'always',
	integrations: [
		starlight({
			title: 'Melange',
			// Fallback <meta name="description"> for pages without their own (every generated page has one).
			description:
				'Melange is an open-source modding framework for Worms Ultimate Mayhem: multiplayer fixes, a plugin store, graphics tools and a Lua API.',
			logo: { src: './src/assets/logo-mark.svg', replacesTitle: false },
			favicon: '/favicon.svg',
			social: [{ icon: 'github', label: 'GitHub', href: 'https://github.com/JaminB/melange' }],
			// Generated pages carry their own editUrl (their docs/ source file); this is the fallback.
			editLink: { baseUrl: 'https://github.com/JaminB/melange/edit/main/' },
			lastUpdated: true,
			pagination: true,
			tableOfContents: { minHeadingLevel: 2, maxHeadingLevel: 3 },
			customCss: [
				'@fontsource/fredoka/600.css',
				'@fontsource/fredoka/700.css',
				'@fontsource-variable/inter',
				'@fontsource-variable/jetbrains-mono',
				'./src/styles/theme.css', // the shared theme, verbatim
				'./src/styles/site.css', // Starlight wiring for this site's overrides (header, landing, footer)
			],
			expressiveCode: {
				styleOverrides: {
					borderRadius: '8px',
					borderWidth: '1px',
					borderColor: 'var(--sl-color-hairline)',
					frames: { frameBoxShadowCssValue: 'none' },
				},
			},
			// The latest docs groups, then one collapsed group per versioned snapshot ({label: 'v0.8.0',
			// autogenerate: {directory: 'v0.8.0'}}); src/route-data.ts keeps only the page's own version.
			sidebar,
			routeMiddleware: './src/route-data.ts',
			components: {
				ThemeProvider: './src/components/ThemeProvider.astro',
				Header: './src/components/Header.astro',
				Footer: './src/components/Footer.astro',
				Hero: './src/components/Hero.astro',
				MobileMenuFooter: './src/components/MobileMenuFooter.astro',
			},
		}),
	],
});
