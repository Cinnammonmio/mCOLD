import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';

// One self-contained index.html in dist/ so the prototype opens from a file or any static host.
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  base: './',
});
