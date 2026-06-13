import { fileURLToPath, URL } from 'node:url';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

export default defineConfig({
  base: '/',
  plugins: [vue()],
  resolve: {
    alias: {
      vue: 'vue/dist/vue.esm-bundler.js'
    }
  },
  build: {
    outDir: '../static',
    emptyOutDir: false,
    rollupOptions: {
      input: {
        index: fileURLToPath(new URL('./index.html', import.meta.url)),
        upload: fileURLToPath(new URL('./upload.html', import.meta.url)),
        backup: fileURLToPath(new URL('./backup.html', import.meta.url)),
        record: fileURLToPath(new URL('./record.html', import.meta.url)),
        momoda: fileURLToPath(new URL('./momoda.html', import.meta.url)),
        lequ: fileURLToPath(new URL('./lequ.html', import.meta.url))
      },
      output: {
        entryFileNames: 'assets/[name]-[hash].js',
        chunkFileNames: 'assets/[name]-[hash].js',
        assetFileNames: 'assets/[name]-[hash][extname]'
      }
    }
  }
});
