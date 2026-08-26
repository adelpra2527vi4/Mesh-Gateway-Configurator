// Rendering: nessun framework, solo DOM diretto. Porta la stessa logica
// della vecchia pagina HTML del gateway (render(d) per la mesh, poll() per
// relè/sensori BLE classici, lo sniffer con evidenziazione byte) sul
// protocollo a righe CFG: invece che su fetch+JSON.

let api = null; // { sendCmd, afterCmdRefresh, afterStatusRefresh, startSnifferPoll, stopSnifferPoll, gw }
let lastState  = { busy: false, oob: false, usbMode: false, nodes: [], discovered: [], discActive: false };

// Scanner QR companion: decodifica via jsQR (vendor/jsQR.min.js, libreria
// locale nel progetto, MIT). jsQR lavora
// su un frame video catturato in canvas e funziona in qualsiasi browser
// moderno, restando comunque un file locale (nessuna CDN a runtime, ok per
// GitHub Pages offline).
function openQrScanner(targetInputId) {
  if (!targetInputId) return;
  const overlay = document.createElement('div');
  overlay.className = 'qr-modal';
  overlay.innerHTML = `
    <div class="qr-modal-box">
      <div class="qr-modal-head"><span>Inquadra il QR code</span><button type="button" class="iconbtn" id="qr-close">&times;</button></div>
      <div class="qr-video-wrap">
        <video id="qr-video" autoplay playsinline muted></video>
        <button type="button" class="iconbtn qr-switch-btn" id="qr-switch" style="display:none" title="Cambia fotocamera"><svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 12a10 10 0 0 1 15-8.66"/><path d="M22 12a10 10 0 0 1-15 8.66"/><path d="M17 2v5h-5"/><path d="M7 22v-5h5"/></svg></button>
        <div class="qr-guide">
          <div class="qr-guide-grid"></div>
          <div class="qr-guide-laser"></div>
          <span class="qr-guide-corner tl"></span><span class="qr-guide-corner tr"></span>
          <span class="qr-guide-corner bl"></span><span class="qr-guide-corner br"></span>
        </div>
      </div>
      <div class="muted" id="qr-status">Avvio fotocamera...</div>
      <div class="qr-adjust-row">
        <label>Contrasto <span id="qr-contrast-val"></span></label>
        <input type="range" id="qr-contrast" min="0" max="250" step="5">
      </div>
      <div class="qr-adjust-row">
        <label>Luminosit&agrave; <span id="qr-brightness-val"></span></label>
        <input type="range" id="qr-brightness" min="0" max="220" step="5">
      </div>
      <label class="qr-invert-row"><input type="checkbox" id="qr-invert"> Inverti colori (negativo)</label>
      <div class="qr-adjust-row" id="qr-zoom-row" style="display:none">
        <label>Zoom <span id="qr-zoom-val"></span></label>
        <input type="range" id="qr-zoom">
      </div>
      <label class="qr-invert-row" id="qr-manualfocus-row" style="display:none"><input type="checkbox" id="qr-manualfocus"> Fuoco manuale</label>
      <div class="qr-adjust-row" id="qr-focus-row" style="display:none">
        <label>Fuoco <span id="qr-focus-val"></span></label>
        <input type="range" id="qr-focus">
      </div>
    </div>`;
  document.body.appendChild(overlay);

  const video = overlay.querySelector('#qr-video');
  const status = overlay.querySelector('#qr-status');
  const switchBtn = overlay.querySelector('#qr-switch');
  const zoomRow = overlay.querySelector('#qr-zoom-row');
  const zoomEl = overlay.querySelector('#qr-zoom');
  const zoomVal = overlay.querySelector('#qr-zoom-val');
  const manualFocusRow = overlay.querySelector('#qr-manualfocus-row');
  const manualFocusEl = overlay.querySelector('#qr-manualfocus');
  const focusRow = overlay.querySelector('#qr-focus-row');
  const focusEl = overlay.querySelector('#qr-focus');
  const focusVal = overlay.querySelector('#qr-focus-val');
  let stream = null, rafId = null, stopped = false;
  let devices = [], devIdx = 0;

  // Contrasto/luminosita' regolabili a mano (prima erano fissi) e ricordati
  // tra una scansione e l'altra - un QR piccolo/stampato male puo' aver
  // bisogno di valori diversi da uno grande e ben illuminato.
  const contrastEl = overlay.querySelector('#qr-contrast');
  const brightnessEl = overlay.querySelector('#qr-brightness');
  const contrastVal = overlay.querySelector('#qr-contrast-val');
  const brightnessVal = overlay.querySelector('#qr-brightness-val');
  const invertEl = overlay.querySelector('#qr-invert');
  let qrAdjust = { contrast: 140, brightness: 120, invert: false };
  try { qrAdjust = Object.assign(qrAdjust, JSON.parse(localStorage.getItem('qr_adjust') || '{}')); } catch {}
  contrastEl.value = qrAdjust.contrast;
  brightnessEl.value = qrAdjust.brightness;
  contrastVal.textContent = qrAdjust.contrast + '%';
  brightnessVal.textContent = qrAdjust.brightness + '%';
  invertEl.checked = qrAdjust.invert;
  // Applicato sia al canvas invisibile (quello che legge jsQR) sia al
  // <video> a schermo: prima solo il canvas veniva filtrato, quindi
  // muovere gli slider non produceva alcun cambiamento visibile - vedi
  // conversazione. Il negativo (invert) aiuta sia a occhio con QR poco
  // leggibili sia jsQR con QR a colori invertiti (es. generati in dark
  // mode, moduli bianchi su fondo nero).
  const applyAdjust = () => {
    const f = `contrast(${qrAdjust.contrast}%) brightness(${qrAdjust.brightness}%)${qrAdjust.invert ? ' invert(1)' : ''}`;
    ctx.filter = f;
    video.style.filter = f;
  };
  const persist = () => localStorage.setItem('qr_adjust', JSON.stringify(qrAdjust));
  contrastEl.addEventListener('input', () => {
    qrAdjust.contrast = parseInt(contrastEl.value, 10);
    contrastVal.textContent = qrAdjust.contrast + '%';
    applyAdjust(); persist();
  });
  brightnessEl.addEventListener('input', () => {
    qrAdjust.brightness = parseInt(brightnessEl.value, 10);
    brightnessVal.textContent = qrAdjust.brightness + '%';
    applyAdjust(); persist();
  });
  invertEl.addEventListener('change', () => {
    qrAdjust.invert = invertEl.checked;
    applyAdjust(); persist();
  });

  const cleanup = () => {
    stopped = true;
    if (rafId) cancelAnimationFrame(rafId);
    if (stream) stream.getTracks().forEach(t => t.stop());
    // Esce con la stessa "grazia" con cui e' entrato (fade+scorrimento verso
    // il basso), invece di sparire di scatto - vedi conversazione.
    overlay.classList.add('qr-modal-closing');
    overlay.querySelector('.qr-modal-box').addEventListener('animationend', () => overlay.remove(), { once: true });
  };
  overlay.querySelector('#qr-close').addEventListener('click', cleanup);
  overlay.addEventListener('click', (e) => { if (e.target === overlay) cleanup(); });

  if (typeof jsQR !== 'function') {
    video.style.display = 'none';
    status.textContent = 'Libreria di scansione QR non caricata. Usa il campo di testo qui sotto.';
    return;
  }
  if (!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia)) {
    video.style.display = 'none';
    status.textContent = 'Nessuna fotocamera disponibile su questo dispositivo/browser. Usa il campo di testo qui sotto.';
    return;
  }

  const canvas = document.createElement('canvas');
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  // Boost contrasto/luminosita' sul frame usato per la decodifica (non sul
  // video mostrato all'utente): un QR piccolo o generato a bassa risoluzione
  // ha bordi dei moduli meno netti, aumentare il contrasto aiuta jsQR a
  // distinguerli. Regolabile a mano dagli slider sopra invece di un valore
  // fisso - vedi conversazione.
  applyAdjust();

  // Avvia (o riavvia su cambio fotocamera) lo stream video. Al primo giro
  // deviceId e' assente e si usa facingMode:'environment' (retrocamera di
  // default su telefono); dal secondo cambio in poi si punta al deviceId
  // scelto dal pulsante switch.
  function startStream(deviceId) {
    if (stream) stream.getTracks().forEach(t => t.stop());
    // Risoluzione "ideal" alta: il browser sceglie comunque il massimo
    // supportato dalla fotocamera se e' inferiore, ma di default (senza
    // chiederla) molte webcam partono in un modo basso/720p che rende un
    // QR piccolo poco piu' di una macchia di pixel - vedi conversazione.
    const videoConstraints = deviceId
      ? { deviceId: { exact: deviceId }, width: { ideal: 1920 }, height: { ideal: 1080 } }
      : { facingMode: 'environment', width: { ideal: 1920 }, height: { ideal: 1080 } };
    return navigator.mediaDevices.getUserMedia({ video: videoConstraints }).then(s => {
      stream = s;
      video.srcObject = s;
      status.textContent = 'Ricerca QR code...';

      const track = s.getVideoTracks()[0];
      const caps = track && track.getCapabilities ? track.getCapabilities() : {};
      // Se la fotocamera espone il controllo hardware di esposizione, la
      // spingo un po' oltre il default: un QR piccolo/lontano ha moduli
      // sottili che l'auto-esposizione a volte espone troppo debolmente -
      // best-effort, silenzioso se il device non lo supporta.
      if (caps && caps.exposureCompensation && caps.exposureMode) {
        const target = Math.min(caps.exposureCompensation.max, (caps.exposureCompensation.max || 0) * 0.6);
        track.applyConstraints({ advanced: [{ exposureMode: 'manual', exposureCompensation: target }] }).catch(() => {});
      }
      // Zoom ottico/digitale nativo della fotocamera, dove disponibile
      // (soprattutto smartphone): resta nascosto sulle webcam che non lo
      // supportano, stesso pattern gia' usato per torcia/esposizione.
      if (caps && caps.zoom && caps.zoom.max > caps.zoom.min) {
        zoomRow.style.display = '';
        zoomEl.min = caps.zoom.min;
        zoomEl.max = caps.zoom.max;
        zoomEl.step = caps.zoom.step || 1;
        zoomEl.value = track.getSettings().zoom || caps.zoom.min;
        zoomVal.textContent = 'x' + Number(zoomEl.value).toFixed(1);
        zoomEl.oninput = () => {
          track.applyConstraints({ advanced: [{ zoom: Number(zoomEl.value) }] }).catch(() => {});
          zoomVal.textContent = 'x' + Number(zoomEl.value).toFixed(1);
        };
      } else {
        zoomRow.style.display = 'none';
      }
      // Autofocus continuo esplicito: alcune fotocamere di default fanno
      // solo un autofocus "a scatto" iniziale e poi restano ferme, il che
      // e' un problema se durante la scansione ci si avvicina/allontana
      // dal QR - forzare 'continuous' tiene il fuoco sempre aggiornato,
      // best-effort se il device non lo supporta.
      if (caps && Array.isArray(caps.focusMode) && caps.focusMode.includes('continuous')) {
        track.applyConstraints({ advanced: [{ focusMode: 'continuous' }] }).catch(() => {});
      }
      // Fuoco manuale come alternativa (utile se l'autofocus fatica su un
      // QR molto piccolo o molto vicino): disponibile solo su alcuni
      // dispositivi, il checkbox compare solo se la capability c'e'.
      if (caps && Array.isArray(caps.focusMode) && caps.focusMode.includes('manual') && caps.focusDistance) {
        manualFocusRow.style.display = '';
        focusEl.min = caps.focusDistance.min;
        focusEl.max = caps.focusDistance.max;
        focusEl.step = caps.focusDistance.step || 1;
        focusEl.value = track.getSettings().focusDistance || caps.focusDistance.min;
        focusVal.textContent = focusEl.value;
        manualFocusEl.checked = false;
        focusRow.style.display = 'none';
        manualFocusEl.onchange = () => {
          focusRow.style.display = manualFocusEl.checked ? '' : 'none';
          if (manualFocusEl.checked) {
            track.applyConstraints({ advanced: [{ focusMode: 'manual', focusDistance: Number(focusEl.value) }] }).catch(() => {});
          } else if (caps.focusMode.includes('continuous')) {
            track.applyConstraints({ advanced: [{ focusMode: 'continuous' }] }).catch(() => {});
          }
        };
        focusEl.oninput = () => {
          focusVal.textContent = focusEl.value;
          track.applyConstraints({ advanced: [{ focusMode: 'manual', focusDistance: Number(focusEl.value) }] }).catch(() => {});
        };
      } else {
        manualFocusRow.style.display = 'none';
        focusRow.style.display = 'none';
      }
    });
  }

  startStream()
    .then(() => {
      // Elenco fotocamere disponibili, per il pulsante "cambia fotocamera":
      // va fatto DOPO il primo getUserMedia perche' prima di aver dato il
      // permesso enumerateDevices() restituisce label/deviceId vuoti.
      return navigator.mediaDevices.enumerateDevices().then(list => {
        devices = list.filter(d => d.kind === 'videoinput');
        if (devices.length > 1) switchBtn.style.display = '';
      });
    })
    .then(() => {
      switchBtn.addEventListener('click', () => {
        if (!devices.length) return;
        devIdx = (devIdx + 1) % devices.length;
        status.textContent = 'Cambio fotocamera...';
        startStream(devices[devIdx].deviceId).catch(() => { status.textContent = 'Impossibile passare a questa fotocamera.'; });
      });

      const tick = () => {
        if (stopped) return;
        if (video.readyState === video.HAVE_ENOUGH_DATA) {
          canvas.width = video.videoWidth;
          canvas.height = video.videoHeight;
          ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
          const frame = ctx.getImageData(0, 0, canvas.width, canvas.height);
          const code = jsQR(frame.data, frame.width, frame.height, { inversionAttempts: 'dontInvert' });
          if (code && code.data) {
            // Rilettura per id, non riferimento catturato all'apertura: un
            // refresh periodico nel frattempo puo' aver ridisegnato
            // nodes-box e reso l'elemento vecchio "orfano" - scrivendoci
            // sopra il valore sparirebbe in silenzio - vedi conversazione.
            const liveInput = document.getElementById(targetInputId);
            if (liveInput) {
              liveInput.value = code.data;
              liveInput.dispatchEvent(new Event('input', { bubbles: true }));
              liveInput.focus();
            }
            cleanup();
            return;
          }
        }
        rafId = requestAnimationFrame(tick);
      };
      rafId = requestAnimationFrame(tick);
    })
    .catch(() => { status.textContent = 'Fotocamera non disponibile o permesso negato.'; });
}

// Calibrazione lux: rawLux live per nodo (aggiornato dai push SENSOR)
let luxRawLive = {}; // { nodeI: float }

function loadLuxCalib() { try { return JSON.parse(localStorage.getItem('lux_calib') || '{}'); } catch { return {}; } }
function saveLuxCalib(d) { localStorage.setItem('lux_calib', JSON.stringify(d)); }
function getNodeCalib(nodeI) { return loadLuxCalib()[String(nodeI)] || null; }
function clearNodeCalib(nodeI) { const a = loadLuxCalib(); delete a[String(nodeI)]; saveLuxCalib(a); }

// Usato da app.js per rallentare il polling durante il provisioning.
export function getLastStateBusy() { return !!lastState.busy; }
let lastStatus = { relays: [], blesensors: [] };
let logLines = 0;
const LOG_MAX = 500;

// --- Slot sensori: flag "modificato dall'utente, non ridisegnare" ---
let sensorSlotsDirty = false;

// --- Sniffer: stato lato client (byte evidenziati, dispositivo isolato) ---
let snifferOn = false;
let lastSniffDevs = [];
let pinnedMac = null;
let everChanged = {}; // key mac|TYPE|id|offset -> true una volta cambiato, resta evidenziato
let prevHex = {};     // key mac|TYPE|id -> ultimo hex visto, per il confronto
// Mac gia' visti almeno una volta: la card riceve l'animazione di comparsa
// (fadeInUp) solo la prima volta che appare, non ad ogni refresh successivo
// della lista - vedi conversazione ("migliora il pannello sniffing").
let seenSniffMacs = new Set();

// Header "congelato" (topbar+tabbar+stats mesh, vedi style.css): gli offset
// --topbar-h/--tabbar-h usati per impilare gli sticky vanno misurati a
// runtime (l'altezza reale dipende da font/zoom/tema, un valore fisso in
// CSS si disallineerebbe) - richiamata al resize e ogni volta che il
// contenuto sopra puo' essere cambiato di dimensione (connessione, cambio
// tab, refresh dei dati) - vedi conversazione ("questo blocchetto possiamo
// renderlo fisso?").
function syncStickyOffsets() {
  const topbar = document.querySelector('.topbar');
  const tabbar = document.querySelector('.tabbar');
  if (!topbar) return;
  const topbarH = topbar.offsetHeight + parseFloat(getComputedStyle(topbar).marginBottom || 0);
  document.documentElement.style.setProperty('--topbar-h', topbarH + 'px');
  if (tabbar) {
    const tabbarH = tabbar.offsetHeight + parseFloat(getComputedStyle(tabbar).marginBottom || 0);
    document.documentElement.style.setProperty('--tabbar-h', tabbarH + 'px');
  }
}

export function init(a) {
  api = a;

  window.addEventListener('resize', syncStickyOffsets);
  // Il primo giro va rimandato di un frame: al momento di init() il tabbar
  // e' ancora dentro #main-content con display:none (non connesso), quindi
  // offsetHeight leggerebbe 0 - richiamata di nuovo in setConnected() non
  // appena il layout vero diventa visibile.
  requestAnimationFrame(syncStickyOffsets);

  document.getElementById('btn-meshsave').addEventListener('click', () => {
    const m = document.getElementById('savemsg'); if (m) m.textContent = '...';
    api.sendCmd('CFG:MESHSAVE');
  });
  document.getElementById('btn-reset').addEventListener('click', () => {
    if (!confirm('Resettare tutta la rete mesh?')) return;
    api.sendCmd('CFG:RESET');
    startResetCountdown();
  });

  document.getElementById('btn-sethubname').addEventListener('click', () => {
    const v = document.getElementById('hubname-input').value.trim();
    if (!v) return;
    const m = document.getElementById('hubname-msg'); if (m) m.textContent = '...';
    api.sendCmd('CFG:SETHUBNAME;name=' + v);
  });
  document.getElementById('btn-resetallsensors').addEventListener('click', () => {
    if (!confirm('Cancellare tutti i sensori BLE classici configurati?')) return;
    api.sendCmd('CFG:RESETSENSORS');
  });

  document.getElementById('discbtn').addEventListener('click', toggleDiscovery);

  document.getElementById('sniffbtn').addEventListener('click', toggleSniffer);
  document.getElementById('sniffclearbtn').addEventListener('click', () => {
    lastSniffDevs = [];
    pinnedMac = null; everChanged = {}; prevHex = {}; seenSniffMacs = new Set();
    renderSnifferList();
  });
  document.getElementById('sniffsearch').addEventListener('input', () => renderSnifferList());

  // Chip "Solo con nome" / "Solo senza nome (solo MAC)": mutuamente esclusive
  // (attivarne una spegne l'altra), cosi' il filtro resta sempre univoco
  // invece di poter combaciare a un risultato vuoto - vedi conversazione
  // ("metti delle spunte per filtrare i dispositivi ble").
  const filterNamedEl = document.getElementById('sniff-filter-named');
  const filterUnnamedEl = document.getElementById('sniff-filter-unnamed');
  filterNamedEl.addEventListener('click', () => {
    const nowActive = !filterNamedEl.classList.contains('active');
    filterNamedEl.classList.toggle('active', nowActive);
    if (nowActive) filterUnnamedEl.classList.remove('active');
    renderSnifferList();
  });
  filterUnnamedEl.addEventListener('click', () => {
    const nowActive = !filterUnnamedEl.classList.contains('active');
    filterUnnamedEl.classList.toggle('active', nowActive);
    if (nowActive) filterNamedEl.classList.remove('active');
    renderSnifferList();
  });
}

export function showTab(name) {
  document.getElementById('tab-mesh').classList.toggle('hidden', name !== 'mesh');
  document.getElementById('tab-beacon').classList.toggle('hidden', name !== 'beacon');
  document.getElementById('tab-device').classList.toggle('hidden', name !== 'device');
  document.getElementById('tb-mesh').classList.toggle('active', name === 'mesh');
  document.getElementById('tb-beacon').classList.toggle('active', name === 'beacon');
  document.getElementById('tb-device').classList.toggle('active', name === 'device');
  // Solo il tab Mesh ha le .stats sticky sotto la tabbar - un cambio tab non
  // altera l'altezza di topbar/tabbar in se', ma ricalcolare qui e' innocuo
  // e copre eventuali a-capo diversi del testo dei pulsanti tab.
  requestAnimationFrame(syncStickyOffsets);
}

export function setConnected(connected) {
  const btn = document.getElementById('btn-connect');
  const label = btn.querySelector('.btn-connect-label');
  const dot = document.getElementById('conn-dot');
  const newText = connected ? 'Disconnetti' : 'Connetti';
  // Il testo scorre via e il nuovo entra dall'altro lato, invece di cambiare
  // di scatto - stessa idea del pulsante notte/giorno ma orizzontale, dato
  // che qui e' testo non un'icona - vedi conversazione ("un'animazione
  // anche per il pulsante connetti/disconnetti").
  if (label && label.textContent !== newText) {
    label.classList.remove('btn-label-out');
    void label.offsetWidth; // riavvia l'animazione anche se già in corso
    label.classList.add('btn-label-out');
    label.addEventListener('animationend', () => {
      label.textContent = newText;
      label.classList.remove('btn-label-out');
      label.classList.add('btn-label-in');
      label.addEventListener('animationend', () => label.classList.remove('btn-label-in'), { once: true });
    }, { once: true });
  } else if (label) {
    label.textContent = newText;
  }
  btn.classList.toggle('primary', !connected);
  btn.classList.toggle('connected', connected);
  dot.classList.toggle('on', connected);
  if (!connected) dot.classList.remove('usbmode');
  document.getElementById('main-content').style.display = connected ? '' : 'none';
  // La tabbar era a display:none finche' non connessi (offsetHeight=0):
  // solo ora che #main-content torna visibile la sua altezza vera e'
  // misurabile - vedi syncStickyOffsets sopra.
  if (connected) requestAnimationFrame(syncStickyOffsets);
  if (!connected) {
    lastState = { busy: false, oob: false, usbMode: false, nodes: [], discovered: [], discActive: false };
    lastStatus = { relays: [], blesensors: [] };
    snifferOn = false;
    provisioningDevice = null;
    lastStatsShown = { lamps: null, sens: null };
    lastNodeVals = {};
    kindPending = {};
  }
}

// Log seriale unico (stessa connessione USB) ma mostrato in due tab diversi
// (Beacon e Dispositivo, vedi conversazione): non e' possibile spostare lo
// stesso nodo DOM in due posti, quindi ogni riga viene clonata su tutti gli
// elementi .log-panel presenti - restano sempre sincronizzati tra loro.
export function log(line, kind) {
  const panels = document.querySelectorAll('.log-panel');
  if (!panels.length) return;
  const ts = new Date();
  const hh = String(ts.getHours()).padStart(2, '0');
  const mm = String(ts.getMinutes()).padStart(2, '0');
  const ss = String(ts.getSeconds()).padStart(2, '0');
  const text = `[${hh}:${mm}:${ss}] ${line}`;
  panels.forEach(panel => {
    const row = document.createElement('div');
    row.className = 'log-row log-' + (kind || 'rx');
    row.textContent = text;
    panel.appendChild(row);
    panel.scrollTop = panel.scrollHeight;
  });
  logLines++;
  while (logLines > LOG_MAX) {
    panels.forEach(panel => { if (panel.firstChild) panel.removeChild(panel.firstChild); });
    logLines--;
  }
}
export function clearLog() {
  document.querySelectorAll('.log-panel').forEach(p => { p.innerHTML = ''; });
  logLines = 0;
}

function startResetCountdown() {
  let n = 5;
  const box = document.getElementById('reset-countdown');
  box.style.display = 'block';
  box.classList.add('animate-fade-in-up');
  box.textContent = `Riavvio in corso... riconnettiti tra ${n} secondi`;
  const t = setInterval(() => {
    n--;
    if (n <= 0) { clearInterval(t); box.style.display = 'none'; return; }
    box.textContent = `Riavvio in corso... riconnettiti tra ${n} secondi`;
  }, 1000);
  api.gw.disconnect();
}

// ============================================================
// TAB MESH
// ============================================================
export function renderState(state) {
  // Popola luxRawLive dai dati SENSOR_DATA nell'aggiornamento di stato
  // (backup per quando non sono ancora arrivati push SENSOR)
  for (const nd of (state.nodes || [])) {
    if ((nd.kind & 2) && nd.sensor && nd.sensor.light >= 0) {
      if (luxRawLive[nd.i] === undefined) luxRawLive[nd.i] = nd.sensor.light / 100;
    }
  }
  lastState = state; renderUsbModeBanner(state.usbMode); renderMesh();
}

// Punto 1 della richiesta: il device di default e' in modalita' "normale"
// (solo MQTT) - i comandi di scrittura/scan/provisioning sono rifiutati dal
// firmware finche' non si tiene premuto BOOT per qualche secondo (vedi
// CFG:ERR;.../"modalita' USB non attiva" in main.c). Qui mostriamo un
// banner e disabilitiamo i pulsanti che lancerebbero comandi di scrittura,
// cosi' l'utente capisce perche' non succede nulla invece di vedere solo
// errori in log.
const WRITE_BTN_IDS = ['btn-meshsave', 'btn-reset', 'btn-sethubname', 'btn-resetallsensors', 'sniffbtn', 'discbtn'];

function renderUsbModeBanner(usbMode) {
  // Il pallino accanto al titolo rispecchia il colore del LED fisico
  // sull'ESP (rgb_led_set in main.c): verde in modalita' normale, blu in
  // modalita' USB/config (BOOT tenuto premuto) - prima era sempre verde a
  // connessione avvenuta, senza distinguere le due modalita' - vedi
  // conversazione.
  const dot = document.getElementById('conn-dot');
  if (dot) dot.classList.toggle('usbmode', !!usbMode);

  const banner = document.getElementById('banner');
  if (banner) {
    if (!usbMode) {
      banner.textContent = 'Modalità MQTT attiva: tieni premuto BOOT sul gateway per passare a USB e sbloccare scan/provisioning/relè.';
      // fade-in-up solo alla vera comparsa (era gia' nascosto), non ad ogni
      // poll mentre resta visibile - altrimenti l'animazione "tremolerebbe".
      if (banner.style.display !== 'block') banner.classList.add('animate-fade-in-up');
      banner.style.display = 'block'; // #banner ha gia' il suo stile (rosso/border) in style.css
    } else {
      banner.style.display = 'none';
      banner.classList.remove('animate-fade-in-up');
    }
  }
  for (const id of WRITE_BTN_IDS) {
    const el = document.getElementById(id);
    if (el) el.disabled = !usbMode;
  }
  // I controlli per-nodo (provisiona/rebind/forget/onoff/livello/pair/relè/
  // sniffer) sono rigenerati a ogni renderMesh()/renderNodes()/render* e non
  // hanno un id fisso da disabilitare uno per uno: la classe "usb-locked"
  // sui contenitori principali li disattiva tutti via CSS (pointer-events
  // none + opacita' ridotta), il firmware li rifiuta comunque con CFG:ERR
  // se per qualche motivo arrivano lo stesso (vedi usb_cfg_handle_line).
  for (const id of ['discovered-box', 'nodes-box', 'tab-beacon', 'tab-device']) {
    const el = document.getElementById(id);
    if (el) el.classList.toggle('usb-locked', !usbMode);
  }
}

export function applyPush(detail) {
  if (detail.type === 'DUMPEND') return;
  if (!detail.addr) return;
  const addrHex = ('0x' + detail.addr).toLowerCase();
  for (const node of lastState.nodes) {
    if (detail.type === 'ONOFF') {
      const el = node.elems.find(e => e.addr.toLowerCase() === addrHex);
      if (el) { el.on = detail.val === 1; renderMesh(); return; }
    } else if (detail.type === 'LEVEL') {
      const lv = node.lvls.find(l => l.addr.toLowerCase() === addrHex);
      if (lv) { lv.pct = detail.pct; renderMesh(); return; }
    } else if (detail.type === 'SENSOR') {
      if (node.base && node.base.toLowerCase() === addrHex && node.sensor) {
        node.sensor.pres = detail.presence ? 1 : 0;
        node.sensor.light = detail.lux >= 0 ? Math.round(detail.lux * 100) : -1;
        if (detail.lux >= 0) luxRawLive[node.i] = detail.lux;  // per il wizard di calibrazione
        renderMesh();
        return;
      }
    }
  }
}

// Ultimi valori mostrati nei contatori in alto, per far scattare l'animazione
// "value-bump" (style.css) solo quando il numero cambia davvero - renderMesh
// gira ad ogni poll (ogni pochi secondi) quindi senza questo confronto
// l'animazione ripartirebbe di continuo anche a valori invariati.
let lastStatsShown = { lamps: null, sens: null };
// Stessa idea per-nodo (lettura lux dei sensori BLE mesh): { [nd.i]: { lux } }
let lastNodeVals = {};
// Stato ottimistico delle spunte Lampada/Sensore appena cambiate dall'utente,
// finche' il firmware non conferma (o scade come rete di sicurezza) - vedi
// il commento in renderNode() sul bug "non fa deselezionare Lampada...".
let kindPending = {}; // { [nd.i]: { combined, until } }

function bumpIfChanged(el, newVal, key) {
  el.textContent = newVal;
  if (lastStatsShown[key] !== null && lastStatsShown[key] !== newVal) {
    el.classList.remove('animate-value-bump');
    void el.offsetWidth; // riavvia l'animazione anche se già in corso
    el.classList.add('animate-value-bump');
  }
  lastStatsShown[key] = newVal;
}

function renderMesh() {
  const nl = lastState.nodes.filter(n => !n.sw && (n.kind & 1)).length;
  const ns = lastState.nodes.filter(n => !n.sw && (n.kind & 2)).length;
  bumpIfChanged(document.getElementById('st-lamps'), nl, 'lamps');
  bumpIfChanged(document.getElementById('st-sens'), ns, 'sens');
  document.getElementById('st-busy').textContent = lastState.busy ? 'Config...' : 'Pronto';
  document.getElementById('st-busy-box').classList.toggle('busy', !!lastState.busy);
  document.getElementById('badge-busy').style.display = lastState.busy ? '' : 'none';
  document.getElementById('badge-oob').style.display = lastState.oob ? '' : 'none';

  renderDiscButton();
  renderDiscovered();
  renderNodes();
}

// Ricerca nuovi nodi ora e' su richiesta esplicita (CFG:DISCSTART/DISCSTOP,
// vedi main.c): il pulsante riflette lo stato reale del firmware
// (lastState.discActive), cosi' mostra correttamente anche lo spegnimento
// automatico per timeout (60s) senza bisogno di un timer lato client.
function toggleDiscovery() {
  api.sendCmd(lastState.discActive ? 'CFG:DISCSTOP' : 'CFG:DISCSTART');
}

// Icona radar (pallino + due anelli concentrici che si espandono e sfumano
// in loop): usata ovunque nella UI compaia "ricerca/sniffer in corso", al
// posto di un testo statico - vedi conversazione ("un simbolo con una bella
// animazione, fagli fare qualcosa").
const RADAR_HTML = '<span class="radar-ping"><span class="radar-dot"></span><span class="radar-ring"></span><span class="radar-ring r2"></span></span>';

function renderDiscButton() {
  const btn = document.getElementById('discbtn');
  if (!btn) return;
  btn.innerHTML = lastState.discActive ? (RADAR_HTML + 'Ferma ricerca') : 'Cerca nuovi dispositivi';
  btn.classList.toggle('searching', !!lastState.discActive);
}

// Il dispositivo che si sta provisionando viene tolto SUBITO da discovered[]
// lato firmware (cfg_provision) ma entra in nodes[] solo a provisioning
// completato: per qualche secondo non e' in nessuna delle due liste. Lo
// teniamo in memoria qui lato client cosi' possiamo continuare a mostrarlo
// (in grigio, non cliccabile) invece di farlo sparire del tutto.
let provisioningDevice = null;

function renderDiscovered() {
  const box = document.getElementById('discovered-box');
  let list = lastState.discovered || [];

  if (!lastState.busy) {
    provisioningDevice = null;
  } else if (provisioningDevice && !list.some(d => d.uuid === provisioningDevice.uuid)) {
    list = [...list, provisioningDevice];
  }

  if (list.length === 0) {
    box.innerHTML = '';
    return;
  }

  box.innerHTML = list.map(d => {
    const locked = lastState.busy; // provisioning di un altro nodo in corso: tutta la lista in grigio, non cliccabile
    const canProv = !locked && (!d.oob || lastState.oob);
    const oobTag = d.oob ? `<span class="badge warn">OOB</span>` : `<span class="badge">No OOB</span>`;
    // Il firmware riconosce l'UUID (fisso di fabbrica) confrontandolo con
    // quelli dei nodi gia' provisionati: se combacia, questo "nuovo"
    // dispositivo scoperto e' in realta' un nodo gia' configurato che e'
    // caduto dalla mesh (es. si e' resettato) - vedi conversazione, prima
    // sembrava che un nodo sparisse/si sostituisse a sorpresa.
    const knownTag = d.known
      ? `<span class="badge warn">Gia' noto${d.knownName ? (': ' + d.knownName) : ''}</span>`
      : '';
    const isThisOneProvisioning = locked && provisioningDevice && d.uuid === provisioningDevice.uuid;
    const btn = isThisOneProvisioning
      ? `<span class="pill wait">Provisioning...</span>`
      : (locked
        ? `<span class="muted">In attesa...</span>`
        : (canProv
          ? `<button class="btn primary sm" data-act="provision" data-uuid="${d.uuid}" data-known="${d.known?1:0}" data-knownname="${d.knownName||''}">Provisiona</button>`
          : `<span class="muted">(Registra prima il QR OOB)</span>`));
    // addr/name vuoti = il firmware non ha ancora risolto MAC/nome veri del
    // beacon (vedi ble_classic_lookup_mesh_beacon in main.c, richiede una
    // finestra di scan classico in piu' per intercettare il pacchetto giusto):
    // pillola animata invece del dato mancante, cosi' si capisce che sta
    // arrivando e non che manca per sempre.
    const pendingPill = (label) =>
      `<span class="pill-pending"><span class="pill-pending-dot"></span>${label}</span>`;
    const macFmt = d.addr
      ? d.addr.replace(/(.{2})(?=.)/g, '$1:').toUpperCase()
      : pendingPill('MAC in verifica...');
    const nameStr = d.name
      ? `<span class="dev-name">${d.name}</span>`
      : pendingPill('Nome in verifica...');
    return `<div class="dev-card${locked ? ' usb-locked' : ''}"><div class="grow">
        <div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap">${nameStr} <span class="rssi">${renderSigBars(d.rssi||0)}${d.rssi||0} dBm</span></div>
        <div style="margin-top:3px;font-family:ui-monospace,monospace">${macFmt}</div>
        <div style="margin-top:3px">${oobTag} ${knownTag} <span class="addr">UUID ${d.uuid.slice(0,8)}...</span></div>
      </div>${btn}</div>`;
  }).join('');
  box.querySelectorAll('[data-act="provision"]').forEach(b => {
    b.addEventListener('click', () => {
      if (b.dataset.known === '1') {
        const name = b.dataset.knownname || '(senza nome)';
        if (!confirm(`Questo dispositivo sembra gia' configurato come "${name}" ed e' caduto dalla rete mesh.\n\nRiprovisionarlo lo riconfigurera' da zero (gruppo/nome restano, ma andra' ribindato). Continuare?`)) return;
      }
      const d = list.find(x => x.uuid === b.dataset.uuid);
      if (d) provisioningDevice = d;
      api.sendCmd('CFG:PROVISION;uuid=' + b.dataset.uuid);
    });
  });
}

function renderNodes() {
  const box = document.getElementById('nodes-box');
  if (!lastState.nodes.length) { box.innerHTML = "<div class='empty'>In attesa di dispositivi...</div>"; return; }

  // Preserva il focus/valore di un eventuale input nome in modifica, come
  // faceva la vecchia pagina (altrimenti il refresh ogni 2s cancella quello
  // che si sta scrivendo).
  const act = document.activeElement;
  let editingId = null, editingVal = null, editingSel = null;
  if (act && act.id && box.contains(act)) {
    // Checkbox Lampada/Sensore in corso di click: un re-render qui
    // ricostruisce il DOM sotto al dito/mouse esattamente mentre il
    // browser sta processando il toggle - stessa protezione che aveva il
    // vecchio <select> unico, ora su entrambe le checkbox indipendenti.
    if (act.id.startsWith('kindlamp_') || act.id.startsWith('kindsens_')) return;
    // Slider luminosita' in trascinamento: un re-render qui sostituisce il
    // range sotto al dito/mouse e il pallino "scatta" indietro alla vecchia
    // posizione - stessa protezione degli altri controlli interattivi.
    if (act.id.startsWith('lvl_')) return;
    // Se l'utente sta scrivendo in un campo che non sia il nome nodo,
    // salta il re-render per non disturbare il cursore.
    if (!act.id.startsWith('nm_')) return;
    editingId = act.id; editingVal = act.value;
    try { editingSel = [act.selectionStart, act.selectionEnd]; } catch {}
  }

  box.innerHTML = lastState.nodes.map(renderNode).join('');
  wireNodeEvents(box);

  if (editingId) {
    const el = document.getElementById(editingId);
    if (el) {
      el.value = editingVal;
      el.focus();
      try {
        const s = editingSel ? editingSel[0] : editingVal.length;
        const e = editingSel ? editingSel[1] : editingVal.length;
        el.setSelectionRange(s, e);
      } catch {}
    }
  }
}

function renderNode(nd) {
  const nameInput = `<input class="name-input" id="nm_${nd.i}" value="${(nd.name||'').replace(/"/g,'')}" placeholder="Nome dispositivo">`
    + `<button class="btn sm" data-act="setname" data-node="${nd.i}">Salva</button> <span class="muted" id="nmsave_${nd.i}"></span>`;

  // Nodo raggiungibile davvero adesso, non solo "configurato una volta" -
  // basato sull'ultima risposta reale vista dal firmware (main.c
  // node_is_online/s_node_last_seen_us). Prima era un badge "Offline" a se'
  // stante accanto al pill "Connesso" (i due si contraddicevano a vista) -
  // ora sostituisce direttamente il pill stesso, in rosso - vedi conversazione.
  const swOffline = nd.sw && nd.cfg && !nd.online;
  if (nd.sw) {
    const swCls = swOffline ? 'err' : (nd.cfg ? 'ok' : (nd.fail ? 'err' : 'wait'));
    const swTxt = swOffline ? 'Disconnesso' : (nd.cfg ? 'Connesso' : (nd.fail ? 'Errore' : 'Config...'));
    return `<div class="node"><div class="node-head">${nameInput}
      <span class="node-id"><span class="idx">#${nd.i}</span><span class="addr">${nd.base}</span></span>
      <span class="pill ${swCls}">${swTxt}</span>
      <button class="btn danger sm" style="margin-left:auto" data-act="forget" data-node="${nd.i}">Rimuovi</button></div></div>`;
  }

  const offline = nd.cfg && !nd.online;
  const stCls = offline ? 'err' : (nd.cfg ? 'ok' : (nd.fail ? 'err' : 'wait'));
  const stTxt = offline ? 'Disconnesso' : (nd.cfg ? 'Connesso' : (nd.fail ? 'Errore' : 'Config...'));
  // "kind" e' una bitmask (1=lampada, 2=sensore, 3=entrambi), non piu' una
  // scelta esclusiva: un device combo (es. dongle SR con 2 LED + PIR/LUX)
  // puo' avere entrambe le capacita' gestite insieme, ognuna con le sue
  // card e il suo traffico MQTT indipendente - vedi CFG:SETKIND lato
  // firmware. Due checkbox indipendenti al posto del vecchio <select>
  // Lampada/Sensore esclusivo.
  //
  // BUG FIX ("non fa deselezionare Lampada mantenendo Sensore"): il valore
  // mostrato non viene piu' letto SOLO da nd.kind (che riflette l'ULTIMO
  // stato confermato dal firmware). Se un poll periodico arriva PRIMA che
  // il firmware abbia processato/ripubblicato il CFG:SETKIND appena
  // inviato, renderNode veniva richiamato con l'nd.kind ancora VECCHIO e
  // "resuscitava" la spunta appena tolta dall'utente. kindPending tiene lo
  // stato ottimistico appena scelto per qualche secondo, finche' nd.kind
  // non lo conferma davvero (o scade, come rete di sicurezza) - vedi
  // kindPending piu' sotto e il listener "change" in wireNodeEvents.
  const pending = kindPending[nd.i];
  const pendingLive = pending && pending.until > Date.now() && pending.combined !== nd.kind;
  const hasLampKind = pendingLive ? (pending.combined & 1) !== 0 : (nd.kind & 1) !== 0;
  const hasSensorKind = pendingLive ? (pending.combined & 2) !== 0 : (nd.kind & 2) !== 0;
  if (pending && !pendingLive) delete kindPending[nd.i]; // confermato dal firmware o scaduto
  const bulbIcon = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18h6"/><path d="M10 22h4"/><path d="M12 2a7 7 0 0 0-4 12.7c.5.4.8 1 .8 1.7v.1h6.4v-.1c0-.7.3-1.3.8-1.7A7 7 0 0 0 12 2Z"/></svg>`;
  const sensorIcon = `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 12h3l2-7 4 14 3-9 2 5h4"/></svg>`;
  const kindPicker = `<span class="kind-picker">
      <label class="kind-toggle lamp"><input type="checkbox" id="kindlamp_${nd.i}" data-act="setkind" data-node="${nd.i}" data-bit="1" ${hasLampKind ? 'checked' : ''}>${bulbIcon}Lampada</label>
      <label class="kind-toggle sensor"><input type="checkbox" id="kindsens_${nd.i}" data-act="setkind" data-node="${nd.i}" data-bit="2" ${hasSensorKind ? 'checked' : ''}>${sensorIcon}Sensore</label>
    </span>`;
  const fbtn = `<button class="btn danger sm" data-act="forget" data-node="${nd.i}">Rimuovi</button>`;
  const grpBadge = hasLampKind
    ? (nd.grp ? `<span class="badge good">Gruppo OK</span>` : `<span class="badge warn">non rebindato</span>`)
    : '';
  const rbtn = hasLampKind ? `<button class="btn sm" data-act="rebind" data-node="${nd.i}">Rebind</button>` : '';

  let head = `<div class="node${offline ? ' node-offline' : ''}"><div class="node-head">${nameInput}${fbtn}</div>`
    + `<div class="node-meta">${kindPicker} <span style="margin-left:auto;display:flex;align-items:center;gap:8px"><span class="node-id"><span class="idx">#${nd.i}</span><span class="addr">${nd.base}</span></span><span class="pill ${stCls}">${stTxt}</span></span></div>`
    + (rbtn || grpBadge ? `<div class="node-meta" style="margin-top:-2px">${rbtn} ${grpBadge}</div>` : '');

  if (!nd.cfg) return head + `<div class="empty" style="margin-top:10px">Non ancora configurato.</div></div>`;

  // Un device combo con entrambi i bit mostra sia le card sensore sia
  // quelle lampada, una sotto l'altra (prima erano mutuamente esclusive:
  // if/else su nd.kind === 1, che per un device con 2 LED + PIR/LUX
  // scartava sempre uno dei due set di dati gia' presenti in nd - vedi
  // conversazione: "esistono dispositivi che possono avere PIR e Led
  // assieme").
  let sensorBlock = '';
  if (hasSensorKind) {
    const s = nd.sensor;
    const presOn = s && s.pres > 0;
    const pres = !s || s.pres < 0 ? '&mdash;' : (s.pres ? 'Presenza' : 'Assente');
    const curLux = luxRawLive[nd.i] !== undefined ? luxRawLive[nd.i]
                   : (s && s.light >= 0 ? s.light / 100 : null);
    const luxStr = curLux !== null ? curLux.toFixed(2) + ' lux' : '&mdash;';
    // "Scatto" (value-bump, style.css) sulla lettura lux solo quando il
    // valore mostrato cambia davvero rispetto all'ultimo giro - il nodo
    // viene comunque riscritto per intero ad ogni poll, quindi la classe va
    // decisa qui in fase di template invece che via classList più sotto.
    const lastLux = lastNodeVals[nd.i]?.lux;
    const luxBump = lastLux !== undefined && lastLux !== luxStr ? ' animate-value-bump' : '';
    lastNodeVals[nd.i] = Object.assign(lastNodeVals[nd.i] || {}, { lux: luxStr });
    const warn = !s || !s.hassens ? `<div class="addr" style="margin-top:8px">(nessun Sensor Server su questo device)</div>` : '';

    // Il firmware applica un fattore moltiplicativo per nodo (sensor_light_cal
    // in main.c: calibrato = grezzo * fattore / 1000) - un offset additivo
    // provato prima lasciava il buio (grezzo=0) diverso da 0 dopo calibrazione,
    // sbagliato per definizione - vedi conversazione. Vedi CFG:SETLUXCALIB.
    const calib = getNodeCalib(nd.i);
    const calibSummary = calib && calib.factor_1000
      ? `<span class="badge good">Calibrato</span> &times;${(calib.factor_1000/1000).toFixed(3)} (rif: ${calib.ref_lux||'?'} lux)`
      : `<span class="badge warn">Non calibrato</span>`;
    const usbLock = !lastState.usbMode ? ' usb-locked' : '';
    // Preserva il valore che l'utente sta digitando nel campo lux di riferimento
    const refLuxCurrentVal = document.getElementById(`cref-${nd.i}`)?.value ?? (calib?.ref_lux || '');

    const calibCard = `<div class="card${usbLock}" style="margin-top:10px">
      <div class="elem-title">Calibrazione Lux &nbsp; ${calibSummary}</div>
      <div style="margin:6px 0">
        <button class="btn sm danger" data-act="calib-zero" data-node="${nd.i}">Azzera calibrazione</button>
        <span class="muted" style="font-size:0.84em">(passo obbligatorio prima di ri-calibrare, cosi' la lettura attuale torna grezza)</span>
      </div>
      <hr style="margin:10px 0;opacity:.3">
      <div style="margin:8px 0">
        Dopo aver azzerato e atteso una nuova lettura, confronta con un luxmetro di riferimento
        e inserisci qui i lux letti dallo strumento:
        <input type="text" inputmode="numeric" id="cref-${nd.i}" style="width:80px;margin:0 4px" value="${refLuxCurrentVal}"> lux
        <button class="btn sm primary" data-act="calib-save" data-node="${nd.i}">Calibra e invia</button>
        <span id="csm-${nd.i}" class="muted" style="font-size:0.84em"></span>
      </div>
    </div>`;

    sensorBlock = `<div class="cards">
        <div class="card"><div class="elem-title">Presenza <span class="pill ${presOn?'on':'off'}">${pres}</span></div></div>
        <div class="card"><div class="elem-title">Luce ambiente</div><div class="pctlbl${luxBump}" style="margin-top:6px">${luxStr}</div></div>
      </div>${warn}${calibCard}`;
  }

  let lampBlock = '';
  if (!hasLampKind) {
    return head + sensorBlock + `</div>`;
  }

  // Mentre lastState.busy e' true (provisioning/config di un nodo in corso,
  // vedi gateway_is_provisioning() lato firmware che ora blocca anche
  // CFG:CMD/CFG:LEVEL) blocchiamo qui i controlli sugli ALTRI nodi gia'
  // configurati, cosi' l'utente non manda comandi che il firmware
  // rifiuterebbe comunque con CFG:ERR.
  let cards = `<div class="cards${lastState.busy ? ' usb-locked' : ''}">`;
  for (const el of nd.elems) {
    cards += `<div class="card"><div class="elem-title">Elemento #${el.e}<span class="pill ${el.on?'on':'off'}">${el.on?'Acceso':'Spento'}</span></div>
      <span class="addr">${el.addr}</span>
      <div class="row-btns">
        <button class="btn ${el.on?'':'primary'} sm" data-act="cmd" data-node="${nd.i}" data-elem="${el.e}" data-val="1">Accendi</button>
        <button class="btn sm" data-act="cmd" data-node="${nd.i}" data-elem="${el.e}" data-val="0">Spegni</button>
        <button class="btn sm" data-act="cmd" data-node="${nd.i}" data-elem="${el.e}" data-val="2">Leggi</button>
      </div></div>`;
  }
  for (const lv of nd.lvls) {
    // Bump solo su un cambio "esterno" (push/poll dal firmware): mentre
    // l'utente trascina lo slider, renderNodes() salta il rerender per
    // intero (protezione anti-refresh sopra), quindi questo ramo non viene
    // mai raggiunto durante il drag - niente conflitto con quell'animazione.
    const lvKey = `${nd.i}-${lv.li}`;
    const lastLv = lastNodeVals[lvKey]?.pct;
    const lvBump = lastLv !== undefined && lastLv !== lv.pct ? ' animate-value-bump' : '';
    lastNodeVals[lvKey] = { pct: lv.pct };
    cards += `<div class="card"><div class="elem-title">Luminosit&agrave; #${lv.e}<span class="pctlbl${lvBump}" data-li-label="${nd.i}-${lv.li}">${lv.pct}%</span></div>
      <span class="addr">${lv.addr}</span>
      <input type="range" min="0" max="100" value="${lv.pct}" class="slider" style="--p:${lv.pct}"
             id="lvl_${nd.i}_${lv.li}" data-act="level-input" data-node="${nd.i}" data-li="${lv.li}"></div>`;
  }
  cards += `</div>`;

  // Il pulsante e' sempre visibile (invece di sparire in silenzio se manca
  // il supporto): se BarcodeDetector/fotocamera non sono disponibili lo
  // scanner lo dice esplicitamente nel modal, il campo testo resta comunque
  // sempre utilizzabile come richiesto.
  const qrBtn = `<button type="button" class="iconbtn qr-scan-btn" data-act="qrscan" data-node="${nd.i}" title="Scansiona QR con la fotocamera"><svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 7V5a2 2 0 0 1 2-2h2"/><path d="M17 3h2a2 2 0 0 1 2 2v2"/><path d="M21 17v2a2 2 0 0 1-2 2h-2"/><path d="M7 21H5a2 2 0 0 1-2-2v-2"/><rect x="7" y="7" width="10" height="10" rx="1"/></svg></button>`;
  const pairBox = nd.paired
    ? `<div style="margin:8px 0"><span class="badge good">Abbinato</span></div>
       <button class="btn danger sm" data-act="unpair" data-node="${nd.i}">Scollega companion</button> <span class="muted" id="pm_${nd.i}"></span>`
    : `<div class="qr-row">
         <input type="text" id="pq_${nd.i}" placeholder="Contenuto QR interruttore..." style="flex:1;margin:8px 0">
         ${qrBtn}
       </div>
       <button class="btn sm" data-act="pair" data-node="${nd.i}">Abbina</button> <span class="muted" id="pm_${nd.i}"></span>`;
  const companion = `<div class="card" style="margin-top:10px"><div class="elem-title">Companion switch</div>${pairBox}</div>`;
  lampBlock = cards + companion;

  // Un nodo combo mostra prima le card sensore (presenza/lux/calibrazione)
  // e poi quelle lampada (elementi on/off, livelli, companion switch), una
  // sotto l'altra invece che scartare l'una o l'altra come prima.
  return head + sensorBlock + lampBlock + `</div>`;
}

function wireNodeEvents(box) {
  box.querySelectorAll('[data-act="setname"]').forEach(el => {
    el.addEventListener('click', () => {
      const inp = document.getElementById('nm_' + el.dataset.node);
      const lbl = document.getElementById('nmsave_' + el.dataset.node);
      if (lbl) lbl.textContent = '...';
      api.sendCmd(`CFG:SETNAME;node=${el.dataset.node};name=${inp ? inp.value : ''}`);
    });
  });
  // Le due checkbox Lampada/Sensore sono indipendenti ma condividono lo
  // stesso comando CFG:SETKIND (bitmask 1|2): al cambio di una delle due si
  // legge anche lo stato attuale dell'altra per comporre il valore
  // combinato da inviare - un device combo puo' avere entrambe spuntate.
  // Se l'utente le spunta via entrambe, non c'e' piu' nessuna capacita' da
  // gestire: si rifiuta l'invio e si ripristina la checkbox appena
  // deselezionata, cosi' il nodo non resta "orfano" di card in PWA (il
  // firmware clamperebbe comunque un kind=0 su NODE_KIND_LAMP, ma qui si
  // evita l'incoerenza visiva di entrambe le checkbox vuote).
  box.querySelectorAll('[data-act="setkind"]').forEach(el => {
    el.addEventListener('change', () => {
      const node = el.dataset.node;
      const lampChecked = document.getElementById(`kindlamp_${node}`)?.checked;
      const sensChecked = document.getElementById(`kindsens_${node}`)?.checked;
      const combined = (lampChecked ? 1 : 0) | (sensChecked ? 2 : 0);
      if (combined === 0) {
        el.checked = true;
        return;
      }
      // Stato ottimistico per qualche secondo: copre la finestra tra
      // l'invio del comando e la conferma del firmware, cosi' un poll che
      // arriva nel mezzo non fa "risorgere" la spunta appena tolta - vedi
      // kindPending e il commento in renderNode().
      kindPending[node] = { combined, until: Date.now() + 4000 };
      api.sendCmd(`CFG:SETKIND;node=${node};kind=${combined}`);
      api.afterCmdRefresh();
    });
  });
  box.querySelectorAll('[data-act="rebind"]').forEach(el => {
    el.addEventListener('click', () => api.sendCmd(`CFG:REBIND;node=${el.dataset.node}`));
  });
  box.querySelectorAll('[data-act="forget"]').forEach(el => {
    el.addEventListener('click', () => { if (confirm('Rimuovere questo nodo?')) api.sendCmd(`CFG:FORGET;node=${el.dataset.node}`); });
  });
  box.querySelectorAll('[data-act="cmd"]').forEach(el => {
    el.addEventListener('click', () => { api.sendCmd(`CFG:CMD;node=${el.dataset.node};elem=${el.dataset.elem};val=${el.dataset.val}`); api.afterCmdRefresh(); });
  });
  box.querySelectorAll('[data-act="level-input"]').forEach(el => {
    const label = box.querySelector(`[data-li-label="${el.dataset.node}-${el.dataset.li}"]`);
    const sendLevel = () => { api.sendCmd(`CFG:LEVEL;node=${el.dataset.node};elem=${el.dataset.li};val=${el.value}`); api.afterCmdRefresh(); };
    el.addEventListener('input', () => { if (label) label.textContent = el.value + '%'; el.style.setProperty('--p', el.value); });
    el.addEventListener('change', sendLevel);
    // Regolazione con la rotellina del mouse: un tick = 1%, come le frecce
    // tastiera del range nativo. preventDefault evita che scrollasse la
    // pagina invece di muovere il valore.
    let wheelDebounce = null;
    el.addEventListener('wheel', (e) => {
      e.preventDefault();
      // Porta il focus sullo slider: e' quello che fa scattare la
      // protezione anti-refresh in renderNodes() (stesso motivo del
      // trascinamento), altrimenti un re-render a meta' scroll/debounce
      // riporta il valore a quello ancora confermato dal firmware.
      if (document.activeElement !== el) el.focus();
      const step = e.deltaY < 0 ? 1 : -1;
      const next = Math.min(100, Math.max(0, parseInt(el.value, 10) + step));
      el.value = next;
      if (label) label.textContent = next + '%';
      el.style.setProperty('--p', next);
      clearTimeout(wheelDebounce);
      wheelDebounce = setTimeout(sendLevel, 250);
    }, { passive: false });
  });
  box.querySelectorAll('[data-act="qrscan"]').forEach(el => {
    el.addEventListener('click', () => openQrScanner('pq_' + el.dataset.node));
  });
  box.querySelectorAll('[data-act="pair"]').forEach(el => {
    el.addEventListener('click', () => {
      const input = document.getElementById('pq_' + el.dataset.node);
      const qr = input ? input.value.trim() : '';
      if (!qr) return;
      api.sendCmd(`CFG:PAIR;node=${el.dataset.node};qr=${qr}`);
    });
  });
  box.querySelectorAll('[data-act="unpair"]').forEach(el => {
    el.addEventListener('click', () => { if (confirm('Scollegare il companion?')) api.sendCmd(`CFG:UNPAIR;node=${el.dataset.node}`); });
  });

  // Calibrazione Lux — azzera (factor=0 → sentinella "non calibrato", il
  // firmware torna a riportare il valore grezzo del sensore)
  box.querySelectorAll('[data-act="calib-zero"]').forEach(el => {
    el.addEventListener('click', () => {
      const ni = parseInt(el.dataset.node);
      if (!confirm('Azzerare la calibrazione lux per questo sensore?')) return;
      api.sendCmd(`CFG:SETLUXCALIB;node=${ni};factor=0`);
      clearNodeCalib(ni);
      renderMesh();
    });
  });

  // Calibrazione Lux — calcola il fattore moltiplicativo e invia
  // CFG:SETLUXCALIB. Va azzerato prima (bottone sopra) cosi' la lettura
  // corrente e' il valore grezzo, non uno gia' calibrato col fattore vecchio.
  box.querySelectorAll('[data-act="calib-save"]').forEach(el => {
    el.addEventListener('click', () => {
      const ni = parseInt(el.dataset.node);
      const refInput = document.getElementById(`cref-${ni}`);
      const ref_lux = parseFloat(refInput?.value);
      if (isNaN(ref_lux) || ref_lux <= 0) { alert('Inserisci un valore lux valido (> 0).'); return; }
      const cur = luxRawLive[ni];
      if (cur === undefined) { alert('Nessuna lettura lux disponibile. Attendi il prossimo aggiornamento dal sensore.'); return; }
      if (cur <= 0) { alert(`La lettura attuale (${cur.toFixed(2)} lux) e' zero o negativa: non posso calcolare un fattore da qui. Assicurati che il sensore sia sotto illuminazione sufficiente e non appena azzerato.`); return; }
      const factor_1000 = Math.round((ref_lux / cur) * 1000);
      const all = loadLuxCalib();
      all[String(ni)] = { factor_1000, ref_lux };
      saveLuxCalib(all);
      api.sendCmd(`CFG:SETLUXCALIB;node=${ni};factor=${factor_1000}`);
      const msg = document.getElementById(`csm-${ni}`);
      if (msg) msg.textContent = `Inviato: ×${(factor_1000/1000).toFixed(3)}`;
      renderMesh();
    });
  });
}

// ============================================================
// TAB SETUP — relè + sensori BLE classici (CFG:STATUS)
// ============================================================
export function renderStatus(status) {
  lastStatus = status;

  if (document.activeElement !== document.getElementById('hubname-input')) {
    // niente da riempire: il nome hub non e' nel protocollo CFG:STATUS,
    // l'utente lo digita e basta (placeholder mostra l'ultimo salvato via msg).
  }

  renderRelays();
  renderMqttStrip();
  renderSensorSlots();
}

export function onCmdResult(type, cmd) {
  if (cmd === 'MESHSAVE') {
    const m = document.getElementById('savemsg');
    if (m) m.textContent = type === 'OK' ? 'Salvato' : '';
  } else if (cmd === 'SETHUBNAME') {
    const m = document.getElementById('hubname-msg');
    if (m) m.textContent = type === 'OK' ? 'Salvato' : '';
  } else if (cmd === 'SETLUXCALIB') {
    if (type === 'ERR') {
      // mostra errore nei messaggi attivi nei card calibrazione
      document.querySelectorAll('[id^="csm-"]').forEach(el => {
        if (el.textContent.startsWith('Inviato')) el.textContent = 'Errore firmware: comando rifiutato';
      });
    }
  }
}

// Prima erano due liste separate e slegate (checkbox "abilitato" in una
// sezione, pulsante on/off degli abilitati in un'altra sotto) - per capire
// lo stato di un relè bisognava guardare in due punti diversi della pagina.
// Ora ogni relè e' una card sola, stile coerente col resto dell'app (nodi
// mesh/slot beacon) - vedi conversazione ("rifai la pagina dispositivo").
function renderRelays() {
  const relays = lastStatus.relays || [];
  const box = document.getElementById('relays');
  if (!relays.length) { box.innerHTML = '<i class="muted">Nessun rel&egrave; rilevato</i>'; return; }
  box.innerHTML = relays.map(r => {
    // Pulsante toggle grande (era un .btn sm minuscolo in mezzo alla riga):
    // e' l'azione principale della card, deve pesare visivamente di piu' -
    // vedi conversazione ("pulsantoni piu' grandi").
    // Disabilitato: stesso aspetto del pulsante "Spento" ma non cliccabile
    // e grigio, invece di un dashed-border con testo diverso ("Non
    // presente") che sembrava un elemento a se' - vedi conversazione.
    const toggleBtn = r.enabled
      ? `<button class="relay-toggle-btn${r.on ? ' on' : ''}" data-act="relayset" data-n="${r.n}" data-on="${r.on ? 1 : 0}">${r.on ? 'Acceso' : 'Spento'}</button>`
      : `<div class="relay-toggle-btn disabled">Spento</div>`;
    return `<div class="node relay-card${r.enabled ? '' : ' relay-disabled'}">
      <div class="relay-num">${r.n + 1}</div>
      ${toggleBtn}
      <label class="relay-switch-row">
        <span>${r.enabled ? 'Abilitato' : 'Disabilitato'}</span>
        <span class="switch"><input type="checkbox" data-act="relaycfg" data-n="${r.n}" data-on="${r.on ? 1 : 0}" ${r.enabled ? 'checked' : ''}><span class="switch-track"></span></span>
      </label>
    </div>`;
  }).join('');
  box.querySelectorAll('[data-act="relayset"]').forEach(b => {
    b.addEventListener('click', () => {
      const newVal = b.dataset.on === '1' ? 0 : 1;
      api.sendCmd(`CFG:RELAYSET;n=${b.dataset.n};val=${newVal}`);
      api.afterStatusRefresh();
    });
  });
  box.querySelectorAll('[data-act="relaycfg"]').forEach(c => {
    c.addEventListener('change', () => {
      // Disabilitare un relè che era acceso non deve lasciarlo alimentato:
      // manda anche lo spegnimento fisico, non solo il flag "abilitato" -
      // vedi conversazione.
      if (!c.checked && c.dataset.on === '1') {
        api.sendCmd(`CFG:RELAYSET;n=${c.dataset.n};val=0`);
      }
      api.sendCmd(`CFG:RELAYCFG;n=${c.dataset.n};enabled=${c.checked?1:0}`);
      api.afterStatusRefresh();
    });
  });
}

// Striscia di stato del modem MQTT (tab Dispositivo, tra i relè e il log):
// stato sintetico del firmware (modem_mqtt.c) + ultimo payload ricevuto,
// utile per capire se il gateway sta parlando col server senza dover aprire
// il log seriale - vedi conversazione.
const MQTT_STATE_INFO = {
  0: { cls: 'bad',  label: 'Non risponde' },
  1: { cls: 'warn', label: 'Riconnessione in corso...' },
  2: { cls: 'orange', label: 'Riavvio modem in corso...' },
  3: { cls: 'good', label: 'Connesso' },
};
function mqttEsc(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}
function renderMqttStrip() {
  const box = document.getElementById('mqtt-strip');
  if (!box) return;
  const state = lastStatus.mqttState;
  const info = MQTT_STATE_INFO[state];
  if (!info) { box.innerHTML = ''; return; }
  const last = lastStatus.mqttLast || '';
  box.innerHTML = `<span class="mqtt-dot ${info.cls}"></span>
    <span class="mqtt-state-label">${info.label}</span>
    <span class="mqtt-last">${last ? mqttEsc(last) : '<i class="muted">nessun messaggio ricevuto</i>'}</span>`;
}

// "last" arriva dal firmware come "chiave=valore;chiave2=valore2;...;rssi=N"
// (ble_classic_handler.c, un pezzo per ogni NOME della regola di decodifica
// piu' l'rssi in coda): prima veniva mostrato cosi' com'era, illeggibile.
// Qui diventa una fila di chip valore/etichetta, l'RSSI evidenziato diverso
// dagli altri (e' un dato di contesto, non una lettura del sensore) - vedi
// conversazione.
function renderSensorReadings(lastStr) {
  const pairs = (lastStr || '').split(';').filter(Boolean).map(p => {
    const eq = p.indexOf('=');
    return eq < 0 ? null : [p.slice(0, eq), p.slice(eq + 1)];
  }).filter(Boolean);
  if (!pairs.length) return '';
  return `<div class="reading-row">` + pairs.map(([k, v]) => {
    const isRssi = k.toLowerCase() === 'rssi';
    return `<div class="reading-chip${isRssi ? ' reading-chip-rssi' : ''}">`
      + `<span class="reading-key">${isRssi ? 'RSSI' : k}</span>`
      + `<span class="reading-val">${v}${isRssi ? ' dBm' : ''}</span></div>`;
  }).join('') + `</div>`;
}

function renderSensorSlots() {
  const list = lastStatus.blesensors || [];

  // Non ridisegnare i campi se l'utente li ha modificati ma non ancora salvato:
  // il ridisegno sovrascrive i valori appena scritti (da "Usa MAC", "Usa regola"
  // o digitazione diretta). Il flag viene azzerato solo su Salva/Elimina.
  if (sensorSlotsDirty) return;

  const box = document.getElementById('sensor-slots');
  // Stesso "gioco di velocita'" gia' visto col menu Lampada/Sensore: il
  // refresh periodico (ogni ~2s, CFG:STATUS) ridisegna la card e un
  // <details> aperto da meno di un secondo si ritrova chiuso di scatto,
  // prima ancora che l'utente riesca a leggerne il contenuto - vedi
  // conversazione. Se un pannello "Configurazione" e' aperto, salta il
  // render finche' l'utente non lo chiude da solo.
  if (box && box.querySelector('.slot-edit[open]')) return;
  let h = '';
  for (let i = 0; i < 8; i++) {
    const s = list.find(x => x.slot === i) || { slot: i, name: '', mac: '', rules: '' };
    const hasData = !!(s.mac);
    // Stato live integrato nella card (prima era una lista separata sopra,
    // scollegata visivamente dai campi dello stesso slot) - vedi
    // conversazione ("card come nella mesh" anche per i beacon).
    const pill = !hasData
      ? `<span class="pill wait">Vuoto</span>`
      : (s.last ? `<span class="pill ok">Attivo</span>` : `<span class="pill err">Offline</span>`);
    const liveLine = hasData
      ? (s.last ? renderSensorReadings(s.last) : `<div class="sensor-live-line offline">in attesa beacon...</div>`)
      : `<div class="sensor-live-line offline">Slot non configurato</div>`;
    // Il dato letto e' la cosa che interessa a colpo d'occhio, la
    // configurazione (nome/mac/regole) e' roba da toccare una volta e poi
    // dimenticare - demota a pannello richiudibile invece di stare sempre
    // in vista quanto i valori letti - vedi conversazione ("dai piu'
    // risalto ai dati").
    h += `<div class="node">
      <div class="node-head"><span class="idx">Slot ${i+1}${s.name?(': '+s.name):''}</span>${pill}</div>
      ${liveLine}
      <details class="slot-edit">
        <summary>Configurazione</summary>
        <div class="slot-field"><label>Nome</label><input id="bn_${i}" value="${s.name||''}" placeholder="es. Cucina"></div>
        <div class="slot-field"><label>MAC</label><input id="bm_${i}" value="${s.mac||''}" placeholder="aabbccddeeff"></div>
        <div class="slot-field"><label>Regole</label><input id="bg_${i}" value="${s.rules||''}" placeholder="SVC,181A,0,2,S100,temp"></div>
        <div class="row-btns">
          <button type="button" class="btn sm" data-act="sensorsave" data-slot="${i}">Salva</button>
          <button type="button" class="btn danger sm" data-act="sensorreset" data-slot="${i}">Elimina</button>
        </div>
      </details>
    </div>`;
  }
  box.innerHTML = h;
  // Qualsiasi digitazione nei campi attiva il flag "dirty"
  box.querySelectorAll('input').forEach(inp => inp.addEventListener('input', () => { sensorSlotsDirty = true; }));
  box.querySelectorAll('[data-act="sensorsave"]').forEach(b => {
    b.addEventListener('click', () => {
      sensorSlotsDirty = false; // permette il ridisegno con i nuovi dati dall'ESP
      // Chiude il pannello "Configurazione": resterebbe aperto altrimenti,
      // e la protezione anti-refresh aggiunta per non farlo richiudere da
      // solo (vedi conversazione) bloccherebbe ANCHE il render con la
      // conferma del salvataggio, dando l'impressione che "Salva" non
      // faccia nulla.
      const details = b.closest('.slot-edit');
      if (details) details.open = false;
      const i = b.dataset.slot;
      const mac = document.getElementById('bm_' + i).value.trim();
      const rules = document.getElementById('bg_' + i).value.trim();
      const name = document.getElementById('bn_' + i).value.trim();
      api.sendCmd(`CFG:SENSORCFG;slot=${i};mac=${mac};rules=${rules};name=${name}`);
      api.afterStatusRefresh();
    });
  });
  box.querySelectorAll('[data-act="sensorreset"]').forEach(b => {
    b.addEventListener('click', () => {
      if (!confirm(`Svuotare lo slot ${parseInt(b.dataset.slot)+1}?`)) return;
      sensorSlotsDirty = false;
      const details = b.closest('.slot-edit');
      if (details) details.open = false;
      api.sendCmd(`CFG:RESETSLOT;slot=${b.dataset.slot}`);
      api.afterStatusRefresh();
    });
  });
}

// ============================================================
// SNIFFER
// ============================================================
function toggleSniffer() {
  const btn = document.getElementById('sniffbtn');
  if (!snifferOn) {
    pinnedMac = null; everChanged = {}; prevHex = {};
    api.sendCmd('CFG:SNIFFERSTART');
    snifferOn = true;
    btn.innerHTML = RADAR_HTML + 'Ferma sniffer';
    btn.classList.add('searching');
    api.startSnifferPoll();
  } else {
    api.sendCmd('CFG:SNIFFERSTOP');
    snifferOn = false;
    btn.innerHTML = 'Avvia sniffer';
    btn.classList.remove('searching');
    api.stopSnifferPoll();
    renderSnifferList(); // mantiene la lista, aggiunge indicatore "congelata"
  }
}

function hexToBytes(hex) { const a = []; for (let i = 0; i < hex.length; i += 2) a.push(hex.substr(i, 2)); return a; }

function renderByteRow(mac, type, id, hex) {
  if (!hex) return '';
  const key = mac + '|' + type + '|' + id;
  const bytes = hexToBytes(hex);
  const prevBytes = hexToBytes(prevHex[key] || '');
  const label = (type === 'SVC' ? ('SVC ' + id + ': ') : 'MFR: ');
  let row = `<div style="font-size:.85em;margin-top:4px">${label}`;
  bytes.forEach((b, i) => {
    const offKey = key + '|' + i;
    // "Appena cambiato" solo su QUESTA chiamata (confronto con l'hex della
    // volta precedente, sotto sovrascritto subito dopo): fa scattare il
    // lampo byte-flash una volta sola, non ad ogni ridisegno successivo
    // della stessa riga - vedi conversazione ("migliora il pannello
    // sniffing a livello... animativo").
    const justChanged = prevBytes[i] !== undefined && prevBytes[i] !== b;
    if (justChanged) everChanged[offKey] = true;
    let cls = everChanged[offKey] ? 'byte changed' : 'byte';
    if (justChanged) cls += ' byte-flash';
    row += `<span class="${cls}" title="offset ${i}" data-act="fillrule" data-type="${type}" data-id="${id}" data-off="${i}">${b}</span>`;
  });
  prevHex[key] = hex;
  return row + '</div>';
}

// Barre di segnale RSSI (4 tacche), colorate per soglia: verde/oro/rosso -
// vedi .sig-bars in style.css.
function renderSigBars(rssi) {
  const lvl = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -80 ? 2 : 1;
  return `<span class="sig-bars lvl-${lvl}"><i></i><i></i><i></i><i></i></span>`;
}

export function renderSniffer(devs) {
  // Merge: aggiorna i device già visti, aggiunge quelli nuovi,
  // non rimuove mai nulla — la lista cresce solo, non si azzera tra un poll e l'altro.
  devs.forEach(d => {
    const idx = lastSniffDevs.findIndex(x => x.mac === d.mac);
    if (idx >= 0) {
      lastSniffDevs[idx] = d; // aggiorna rssi e payload se cambiano
    } else {
      lastSniffDevs.push(d);
    }
  });
  renderSnifferList();
}

function renderSnifferList() {
  const q = (document.getElementById('sniffsearch').value || '').toUpperCase();
  const onlyNamed = document.getElementById('sniff-filter-named').classList.contains('active');
  const onlyUnnamed = document.getElementById('sniff-filter-unnamed').classList.contains('active');
  let base = pinnedMac ? lastSniffDevs.filter(d => d.mac === pinnedMac) : lastSniffDevs;
  if (onlyNamed) base = base.filter(d => !!d.name);
  else if (onlyUnnamed) base = base.filter(d => !d.name);
  const filtered = q ? base.filter(d => d.mac.toUpperCase().includes(q) || (d.name && d.name.toUpperCase().includes(q))) : base;

  const anyFilter = !!(q || pinnedMac || onlyNamed || onlyUnnamed);
  const total = lastSniffDevs.length;
  const statusEl = document.getElementById('sniff-status');
  statusEl.classList.toggle('live', snifferOn);
  statusEl.innerHTML = snifferOn
    ? `<span class="dot on"></span>${total} dispositivi in memoria${anyFilter ? ` (${filtered.length} visibili)` : ''}`
    : total > 0
      ? `${total} dispositivi congelati &mdash; "Pulisci lista" per azzerare`
      : 'Sniffer fermo';

  let h = '';
  const sorted = [...filtered].sort((a, b) => b.rssi - a.rssi); // dal più vicino al più lontano
  sorted.forEach(d => {
    // Stessa gerarchia visiva della lista "dispositivi rilevati" del tab
    // Mesh: nome in evidenza (.dev-name) sopra, MAC monospace sotto, invece
    // di MAC-poi-nome-tra-parentesi - vedi conversazione.
    const nameStr = d.name
      ? `<span class="dev-name">${d.name}</span>`
      : `<span class="muted">(nome sconosciuto)</span>`;
    const isPinned = pinnedMac === d.mac;
    // fadeInUp solo la primissima volta che un mac compare in lista, non ad
    // ogni refresh - vedi seenSniffMacs sopra.
    const isNew = !seenSniffMacs.has(d.mac);
    seenSniffMacs.add(d.mac);
    const cardCls = 'dev-card sniff-card' + (isPinned ? ' sniff-card-pinned' : '') + (isNew ? ' animate-fade-in-up' : '');
    h += `<div class="${cardCls}">
      <div class="sniff-card-head">${nameStr}<span class="rssi">${renderSigBars(d.rssi)}${d.rssi} dBm</span></div>
      <div class="addr" style="font-family:ui-monospace,Menlo,monospace">${d.mac}</div>
      <div class="sniff-card-actions">
        <button type="button" class="btn sm" data-act="usemac" data-mac="${d.mac}">Usa MAC</button>
        <button type="button" class="btn sm" data-act="copymac" data-mac="${d.mac}">Copia MAC</button>
        <button type="button" class="btn sm${isPinned ? ' primary' : ''}" data-act="togglepin" data-mac="${d.mac}">${isPinned ? 'Mostra tutti' : 'Isola sensore'}</button>
      </div>`;
    if (d.svc_hex) h += renderByteRow(d.mac, 'SVC', d.svc_uuid, d.svc_hex);
    if (d.mfr_hex) h += renderByteRow(d.mac, 'MFR', '0', d.mfr_hex);
    h += '</div>';
  });
  const list = document.getElementById('snifflist');
  list.innerHTML = h || `<div class="empty" style="grid-column:1/-1">${anyFilter ? 'Nessun dispositivo trovato con questo filtro.' : 'Nessun dispositivo rilevato finora...'}</div>`;

  list.querySelectorAll('[data-act="usemac"]').forEach(b => {
    b.addEventListener('click', () => {
      const slot = document.getElementById('sniffslot').value;
      const m = document.getElementById('bm_' + slot);
      if (m) { sensorSlotsDirty = true; m.value = b.dataset.mac; m.focus(); }
    });
  });
  list.querySelectorAll('[data-act="copymac"]').forEach(b => {
    b.addEventListener('click', () => {
      navigator.clipboard.writeText(b.dataset.mac).then(() => {
        const orig = b.textContent; b.textContent = 'Copiato!';
        setTimeout(() => { b.textContent = orig; }, 1000);
      }).catch(() => {});
    });
  });
  list.querySelectorAll('[data-act="togglepin"]').forEach(b => {
    b.addEventListener('click', () => { pinnedMac = (pinnedMac === b.dataset.mac) ? null : b.dataset.mac; renderSnifferList(); });
  });
  list.querySelectorAll('[data-act="fillrule"]').forEach(b => {
    b.addEventListener('click', () => {
      const label = prompt('Nome del dato (es. temp, hum, pir):', 'dato');
      if (!label) return;
      const rule = `${b.dataset.type},${b.dataset.id},${b.dataset.off},1,1,${label}`;
      const slot = document.getElementById('sniffslot').value;
      const g = document.getElementById('bg_' + slot);
      if (g) { sensorSlotsDirty = true; g.value = g.value ? g.value + ';' + rule : rule; g.focus(); }
    });
  });
}
