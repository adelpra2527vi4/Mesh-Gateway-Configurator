// Service worker: cache-first per gli asset statici, precache hardcoded
// (niente auto-discovery, vedi spec). Web Serial funziona offline (e' una
// API browser, non richiede rete) quindi l'app e' usabile anche senza
// connessione dopo il primo caricamento.
const CACHE_NAME = 'mesh-gateway-pwa-v1';
const ASSETS = [
  './',
  'index.html',
  'app.js',
  'serial.js',
  'ui.js',
  'style.css',
  'manifest.json',
  'icon-192.png',
  'icon-512.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(ASSETS))
  );
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== CACHE_NAME).map((k) => caches.delete(k)))
    )
  );
  self.clients.claim();
});

self.addEventListener('fetch', (event) => {
  event.respondWith(
    caches.match(event.request).then((cached) => {
      if (cached) {
        // Aggiorna in background per la prossima visita (stale-while-revalidate).
        fetch(event.request).then((resp) => {
          if (resp && resp.ok) {
            caches.open(CACHE_NAME).then((cache) => cache.put(event.request, resp));
          }
        }).catch(() => {});
        return cached;
      }
      return fetch(event.request);
    })
  );
});
