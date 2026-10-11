import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';

// One self-contained HTML file in docs/app/demo/, next to the app docs: it opens from a file or any static host.
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  base: './',
  build: { outDir: '../docs/app/demo', emptyOutDir: true },
});
