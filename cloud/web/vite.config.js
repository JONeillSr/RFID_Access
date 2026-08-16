import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';

export default defineConfig({
  plugins: [preact()],
  build: {
    outDir: 'dist',
    // Fail the build rather than silently shipping something enormous: this is
    // a small admin app and a jump in size means a dependency crept in.
    chunkSizeWarningLimit: 600,
  },
  server: {
    // Must match a redirect URI registered on the app registration, or local
    // sign-in fails with a reply-URL mismatch.
    port: 5173,
  },
});
