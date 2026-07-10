// Service worker: cache-first per gli asset statici, precache hardcoded
// (niente auto-discovery, vedi spec). Web Serial funziona offline (e' una
// API browser, non richiede rete) quindi l'app e' usabile anche senza
// connessione dopo il primo caricamento.
const CACHE_NAME = 'mesh-gateway-pwa-v36';
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
  'vendor/jsQR.min.js',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(ASSETS))
  );
  // NIENTE self.skipWaiting() automatico qui: un nuovo service worker deve
  // restare "waiting" finche' l'utente non conferma dal popup "Aggiorna"
  // (vedi index.html/SKIP_WAITING sotto) - altrimenti la pagina gia' aperta
  // si ritrova codice nuovo sotto i piedi senza preavviso a meta' sessione.
});

// Il popup "Aggiorna disponibile" (index.html) manda questo messaggio al
// click dell'utente: solo allora il SW in attesa prende il controllo.
self.addEventListener('message', (event) => {
  if (event.data === 'SKIP_WAITING') self.skipWaiting();
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
