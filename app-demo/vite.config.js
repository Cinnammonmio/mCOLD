import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { readFileSync } from 'node:fs';

// The demo's version (package.json) and build time, shown on screen so a stale copy is easy to spot.
const pkg = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf-8'));
const built = new Date(Date.now() + 7 * 3600e3).toISOString().slice(0, 16).replace('T', ' '); // UTC+7

// One self-contained HTML file in docs/app/demo/, next to the app docs: it opens from a file or any static host.
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  define: { __DEMO_VERSION__: JSON.stringify(pkg.version), __DEMO_BUILT__: JSON.stringify(built) },
  base: './',
  build: { outDir: '../docs/app/demo', emptyOutDir: true },
});
