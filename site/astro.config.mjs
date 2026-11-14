import { defineConfig } from 'astro/config';
export default defineConfig({
  site: 'https://qmqsu.github.io',
  base: '/folio/',
  output: 'static',
  trailingSlash: 'always',
  vite: { build: { sourcemap: false } },
});
