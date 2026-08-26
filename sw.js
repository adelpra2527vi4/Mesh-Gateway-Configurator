// Service worker: cache-first per gli asset statici, precache hardcoded
// (niente auto-discovery, vedi spec). Web Serial funziona offline (e' una
// API browser, non richiede rete) quindi l'app e' usabile anche senza
// connessione dopo il primo caricamento.
const CACHE_NAME = 'mesh-gateway-pwa-v103';
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

// cache.addAll() userebbe fetch() di default, che PUO' essere soddisfatto
// dalla cache HTTP del browser invece che dalla rete (stesso identico
// problema gia' risolto per sw.js stesso con updateViaCache:'none' - qui
// pero' riguarda gli ASSET precaricati, style.css/ui.js/ecc.): un nuovo SW
// installava correttamente (byte diversi rilevati -> updatefound) ma
// finiva comunque per mettere in cache uno style.css VECCHIO preso dalla
// cache HTTP, quindi un aggiornamento CSS/JS poteva non vedersi affatto
// anche dopo aver confermato il popup "Aggiorna" - vedi conversazione
// ("identico a prima ed e' la v102 e ho refreshato tutto"). {cache:
// 'reload'} forza ogni asset precaricato a passare sempre dalla rete.
self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) =>
      Promise.all(ASSETS.map((url) =>
        fetch(url, { cache: 'reload' }).then((resp) => cache.put(url, resp))
      ))
    )
  );
  // NIENTE self.skipWaiting() automatico qui: un nuovo service worker deve
  // restare "waiting" finche' l'utente non conferma dal popup "Aggiorna"
  // (vedi index.html/SKIP_WAITING sotto) - altrimenti la pagina gia' aperta
  // si ritrova codice nuovo sotto i piedi senza preavviso a meta' sessione.
});

// Il popup "Aggiorna disponibile" (index.html) manda questo messaggio al
// click dell'utente: solo allora il SW in attesa prende il controllo.
// GET_VERSION invece risponde con CACHE_NAME (unica fonte di verita' della
// versione, niente numero duplicato a mano in index.html) - usato dal
// popup versione al long-press sul titolo, vedi conversazione.
self.addEventListener('message', (event) => {
  if (event.data === 'SKIP_WAITING') self.skipWaiting();
  else if (event.data === 'GET_VERSION' && event.source) {
    event.source.postMessage({ type: 'VERSION', version: CACHE_NAME });
  }
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
        // {cache:'reload'} stesso motivo dell'install sopra: senza, questo
        // fetch potrebbe a sua volta pescare dalla cache HTTP del browser
        // invece che dalla rete, vanificando l'aggiornamento in background.
        fetch(event.request, { cache: 'reload' }).then((resp) => {
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
