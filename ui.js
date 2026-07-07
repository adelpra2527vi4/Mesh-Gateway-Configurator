// Rendering: nessun framework, solo DOM diretto. Porta la stessa logica
// della vecchia pagina HTML del gateway (render(d) per la mesh, poll() per
// relè/sensori BLE classici, lo sniffer con evidenziazione byte) sul
// protocollo a righe CFG: invece che su fetch+JSON.

let api = null; // { sendCmd, afterCmdRefresh, afterStatusRefresh, startSnifferPoll, stopSnifferPoll, gw }
let lastState  = { busy: false, oob: false, usbMode: false, nodes: [], discovered: [] };

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

export function init(a) {
  api = a;

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

  document.getElementById('sniffbtn').addEventListener('click', toggleSniffer);
  document.getElementById('sniffclearbtn').addEventListener('click', () => {
    lastSniffDevs = [];
    pinnedMac = null; everChanged = {}; prevHex = {};
    renderSnifferList();
  });
  document.getElementById('sniffsearch').addEventListener('input', () => renderSnifferList());
}

export function showTab(name) {
  document.getElementById('tab-mesh').classList.toggle('hidden', name !== 'mesh');
  document.getElementById('tab-setup').classList.toggle('hidden', name !== 'setup');
  document.getElementById('tb-mesh').classList.toggle('active', name === 'mesh');
  document.getElementById('tb-setup').classList.toggle('active', name === 'setup');
}

export function setConnected(connected) {
  const btn = document.getElementById('btn-connect');
  const dot = document.getElementById('conn-dot');
  btn.textContent = connected ? 'Disconnetti' : 'Connetti';
  dot.classList.toggle('on', connected);
  document.getElementById('main-content').style.display = connected ? '' : 'none';
  if (!connected) {
    lastState = { busy: false, oob: false, usbMode: false, nodes: [], discovered: [] };
    lastStatus = { relays: [], blesensors: [] };
    snifferOn = false;
    provisioningDevice = null;
  }
}

export function log(line, kind) {
  const panel = document.getElementById('log-panel');
  const row = document.createElement('div');
  row.className = 'log-row log-' + (kind || 'rx');
  const ts = new Date();
  const hh = String(ts.getHours()).padStart(2, '0');
  const mm = String(ts.getMinutes()).padStart(2, '0');
  const ss = String(ts.getSeconds()).padStart(2, '0');
  row.textContent = `[${hh}:${mm}:${ss}] ${line}`;
  panel.appendChild(row);
  logLines++;
  while (logLines > LOG_MAX) {
    const first = panel.firstChild;
    if (!first) break;
    panel.removeChild(first);
    logLines--;
  }
  panel.scrollTop = panel.scrollHeight;
}
export function clearLog() { document.getElementById('log-panel').innerHTML = ''; logLines = 0; }

function startResetCountdown() {
  let n = 5;
  const box = document.getElementById('reset-countdown');
  box.style.display = 'block';
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
    if (nd.kind === 1 && nd.sensor && nd.sensor.light >= 0) {
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
const WRITE_BTN_IDS = ['btn-meshsave', 'btn-reset', 'btn-sethubname', 'btn-resetallsensors', 'sniffbtn'];

function renderUsbModeBanner(usbMode) {
  const banner = document.getElementById('banner');
  if (banner) {
    if (!usbMode) {
      banner.textContent = 'Device in modalita\' MQTT: tieni premuto il pulsante BOOT sul gateway per qualche secondo per attivare la modalita\' USB e abilitare scan/provisioning/relè.';
      banner.style.display = 'block'; // #banner ha gia' il suo stile (rosso/border) in style.css
    } else {
      banner.style.display = 'none';
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
  for (const id of ['discovered-box', 'nodes-box', 'tab-setup']) {
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

function renderMesh() {
  const nl = lastState.nodes.filter(n => !n.sw && n.kind === 0).length;
  const ns = lastState.nodes.filter(n => !n.sw && n.kind === 1).length;
  document.getElementById('st-lamps').textContent = nl;
  document.getElementById('st-sens').textContent = ns;
  document.getElementById('st-busy').textContent = lastState.busy ? 'Config...' : 'Pronto';
  document.getElementById('st-busy-box').classList.toggle('busy', !!lastState.busy);
  document.getElementById('badge-busy').style.display = lastState.busy ? '' : 'none';
  document.getElementById('badge-oob').style.display = lastState.oob ? '' : 'none';

  renderDiscovered();
  renderNodes();
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

  if (list.length === 0) { box.innerHTML = ''; return; }

  box.innerHTML = '<div class="section-title">Dispositivi rilevati</div>' + list.map(d => {
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
    const btn = locked
      ? `<span class="pill wait">Provisioning...</span>`
      : (canProv
          ? `<button class="btn primary sm" data-act="provision" data-uuid="${d.uuid}" data-known="${d.known?1:0}" data-knownname="${d.knownName||''}">Provisiona</button>`
          : `<span class="muted">(Registra prima il QR OOB)</span>`);
    return `<div class="dev-card${locked ? ' usb-locked' : ''}"><div class="grow">
        <div style="font-family:ui-monospace,monospace;font-weight:600">${d.addr} <span class="rssi">${d.rssi||0} dBm</span></div>
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
  let editingId = null, editingVal = null;
  if (act && act.id && act.id.indexOf('nm_') === 0) { editingId = act.id; editingVal = act.value; }

  box.innerHTML = lastState.nodes.map(renderNode).join('');
  wireNodeEvents(box);

  if (editingId) {
    const el = document.getElementById(editingId);
    if (el) { el.value = editingVal; el.focus(); }
  }
}

function renderNode(nd) {
  const nameInput = `<input class="name-input" id="nm_${nd.i}" value="${(nd.name||'').replace(/"/g,'')}" placeholder="Nome dispositivo">`
    + `<button class="btn sm" data-act="setname" data-node="${nd.i}">Salva</button> <span class="muted" id="nmsave_${nd.i}"></span>`;

  if (nd.sw) {
    return `<div class="node"><div class="node-head">${nameInput}<span class="idx">#${nd.i}</span><span class="addr">${nd.base}</span>
      <span class="pill ${nd.cfg?'ok':(nd.fail?'err':'wait')}">${nd.cfg?'OK':(nd.fail?'Errore':'Config...')}</span>
      <button class="btn danger sm" style="margin-left:auto" data-act="forget" data-node="${nd.i}">Rimuovi</button></div></div>`;
  }

  const stCls = nd.cfg ? 'ok' : (nd.fail ? 'err' : 'wait');
  const stTxt = nd.cfg ? 'OK' : (nd.fail ? 'Errore' : 'Config...');
  const sel = `<select data-act="setkind" data-node="${nd.i}">
      <option value="0" ${nd.kind===0?'selected':''}>Lampada</option>
      <option value="1" ${nd.kind===1?'selected':''}>Sensore</option></select>`;
  const fbtn = `<button class="btn danger sm" data-act="forget" data-node="${nd.i}">Rimuovi</button>`;
  const grpBadge = nd.kind === 0
    ? (nd.grp ? `<span class="badge good">Gruppo OK</span>` : `<span class="badge warn">non rebindato</span>`)
    : '';
  const rbtn = nd.kind === 0 ? `<button class="btn sm" data-act="rebind" data-node="${nd.i}">Rebind</button>` : '';

  let head = `<div class="node"><div class="node-head">${nameInput}<span class="idx">#${nd.i}</span><span class="addr">${nd.base}</span><span class="pill ${stCls}">${stTxt}</span></div>`
    + `<div class="node-meta">${sel} ${rbtn} ${grpBadge} <span style="margin-left:auto">${fbtn}</span></div>`;

  if (!nd.cfg) return head + `<div class="empty" style="margin-top:10px">Non ancora configurato.</div></div>`;

  if (nd.kind === 1) {
    const s = nd.sensor;
    const presOn = s && s.pres > 0;
    const pres = !s || s.pres < 0 ? '&mdash;' : (s.pres ? 'Presenza' : 'Assente');
    const curLux = luxRawLive[nd.i] !== undefined ? luxRawLive[nd.i]
                   : (s && s.light >= 0 ? s.light / 100 : null);
    const luxStr = curLux !== null ? curLux.toFixed(2) + ' lux' : '&mdash;';
    const warn = !s || !s.hassens ? `<div class="addr" style="margin-top:8px">(nessun Sensor Server su questo device)</div>` : '';

    const calib = getNodeCalib(nd.i);
    const calibSummary = calib && calib.factor_1000 > 0
      ? `<span class="badge good">Calibrato</span> &times;${(calib.factor_1000/1000).toFixed(3)}, zero ${(calib.dark_cl/100).toFixed(2)} lux (rif: ${calib.ref_lux||'?'} lux)`
      : `<span class="badge warn">Non calibrato</span>`;
    const darkAcq = calib && calib.dark_cl !== undefined
      ? `<span class="muted" style="font-size:0.84em">Zero: ${(calib.dark_cl/100).toFixed(2)} lux</span>`
      : '';
    const usbLock = !lastState.usbMode ? ' usb-locked' : '';
    // Preserva il valore che l'utente sta digitando nel campo lux di riferimento
    const refLuxCurrentVal = document.getElementById(`cref-${nd.i}`)?.value ?? (calib?.ref_lux || '');

    const calibCard = `<div class="card${usbLock}" style="margin-top:10px">
      <div class="elem-title">Calibrazione Lux &nbsp; ${calibSummary}</div>
      ${calib && calib.factor_1000 > 0 ? `<p class="muted" style="font-size:0.84em;margin:4px 0 8px">
        Se vuoi ri-calibrare, azzera prima (il firmware torner&agrave; a valori grezzi).</p>` : ''}
      <div style="margin:6px 0">
        <button class="btn sm danger" data-act="calib-zero" data-node="${nd.i}">Azzera calibrazione</button>
        <span class="muted" style="font-size:0.84em">(passo obbligatorio prima di ri-calibrare)</span>
      </div>
      <hr style="margin:10px 0;opacity:.3">
      <div style="margin:6px 0">
        <strong>1.</strong> Copri il sensore (buio completo), attendi qualche secondo, poi premi:
        <button class="btn sm" data-act="calib-dark" data-node="${nd.i}">Cattura zero</button>
        &nbsp; ${darkAcq}
      </div>
      <div style="margin:8px 0">
        <strong>2.</strong> Scopri il sensore sotto illuminazione nota &mdash;
        inserisci i lux del sensore di riferimento:
        <input type="number" id="cref-${nd.i}" min="1" step="1" style="width:80px;margin:0 4px" value="${refLuxCurrentVal}"> lux
        <button class="btn sm primary" data-act="calib-save" data-node="${nd.i}">Calibra e invia</button>
        <span id="csm-${nd.i}" class="muted" style="font-size:0.84em"></span>
      </div>
    </div>`;

    return head + `<div class="cards">
        <div class="card"><div class="elem-title">Presenza <span class="pill ${presOn?'on':'off'}">${pres}</span></div></div>
        <div class="card"><div class="elem-title">Luce ambiente</div><div class="pctlbl" style="margin-top:6px">${luxStr}</div></div>
      </div>${warn}${calibCard}</div>`;
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
    cards += `<div class="card"><div class="elem-title">Luminosit&agrave; #${lv.e}<span class="pctlbl" data-li-label="${nd.i}-${lv.li}">${lv.pct}%</span></div>
      <span class="addr">${lv.addr}</span>
      <input type="range" min="0" max="100" value="${lv.pct}" class="slider" style="--p:${lv.pct}%"
             data-act="level-input" data-node="${nd.i}" data-li="${lv.li}"></div>`;
  }
  cards += `</div>`;

  const pairBox = nd.paired
    ? `<div style="margin:8px 0"><span class="badge good">Abbinato</span></div>
       <button class="btn danger sm" data-act="unpair" data-node="${nd.i}">Scollega companion</button> <span class="muted" id="pm_${nd.i}"></span>`
    : `<input type="text" id="pq_${nd.i}" placeholder="Contenuto QR interruttore..." style="width:100%;margin:8px 0">
       <button class="btn sm" data-act="pair" data-node="${nd.i}">Abbina</button> <span class="muted" id="pm_${nd.i}"></span>`;
  const companion = `<div class="card" style="margin-top:10px"><div class="elem-title">Companion switch</div>${pairBox}</div>`;

  return head + cards + companion + `</div>`;
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
  box.querySelectorAll('[data-act="setkind"]').forEach(el => {
    el.addEventListener('change', () => { api.sendCmd(`CFG:SETKIND;node=${el.dataset.node};kind=${el.value}`); api.afterCmdRefresh(); });
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
    el.addEventListener('input', () => { if (label) label.textContent = el.value + '%'; el.style.setProperty('--p', el.value + '%'); });
    el.addEventListener('change', () => { api.sendCmd(`CFG:LEVEL;node=${el.dataset.node};elem=${el.dataset.li};val=${el.value}`); api.afterCmdRefresh(); });
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

  // Calibrazione Lux — passo 0: azzera (factor=0 dark=0 → firmware torna a valori grezzi)
  box.querySelectorAll('[data-act="calib-zero"]').forEach(el => {
    el.addEventListener('click', () => {
      const ni = parseInt(el.dataset.node);
      if (!confirm('Azzerare la calibrazione lux per questo sensore?')) return;
      api.sendCmd(`CFG:SETLUXCALIB;node=${ni};factor=0;dark=0`);
      clearNodeCalib(ni);
      renderMesh();
    });
  });

  // Calibrazione Lux — passo 1: cattura zero (lettura attuale = dark)
  box.querySelectorAll('[data-act="calib-dark"]').forEach(el => {
    el.addEventListener('click', () => {
      const ni = parseInt(el.dataset.node);
      const cur = luxRawLive[ni];
      if (cur === undefined) { alert('Nessuna lettura lux disponibile. Attendi il prossimo aggiornamento dal sensore.'); return; }
      const dark_cl = Math.round(cur * 100);
      const all = loadLuxCalib();
      all[String(ni)] = { ...(all[String(ni)] || {}), dark_cl };
      saveLuxCalib(all);
      renderMesh();
    });
  });

  // Calibrazione Lux — passo 2: calcola factor e invia CFG:SETLUXCALIB
  box.querySelectorAll('[data-act="calib-save"]').forEach(el => {
    el.addEventListener('click', () => {
      const ni = parseInt(el.dataset.node);
      const refInput = document.getElementById(`cref-${ni}`);
      const ref_lux = parseFloat(refInput?.value);
      if (isNaN(ref_lux) || ref_lux <= 0) { alert('Inserisci un valore lux valido (> 0).'); return; }
      const cur = luxRawLive[ni];
      if (cur === undefined) { alert('Nessuna lettura lux disponibile. Attendi il prossimo aggiornamento dal sensore.'); return; }
      const all = loadLuxCalib();
      const dark_cl = all[String(ni)]?.dark_cl ?? 0;
      const dark_lux = dark_cl / 100;
      const net = cur - dark_lux;
      if (net <= 0) {
        alert(`La lettura attuale (${cur.toFixed(2)} lux) non supera il valore al buio (${dark_lux.toFixed(2)} lux).\nAssicurati di essere sotto illuminazione sufficiente e che lo zero sia stato acquisito correttamente.`);
        return;
      }
      const factor = ref_lux / net;
      const factor_1000 = Math.round(factor * 1000);
      all[String(ni)] = { dark_cl, factor_1000, ref_lux };
      saveLuxCalib(all);
      api.sendCmd(`CFG:SETLUXCALIB;node=${ni};factor=${factor_1000};dark=${dark_cl}`);
      const msg = document.getElementById(`csm-${ni}`);
      if (msg) msg.textContent = `Inviato: ×${(factor_1000/1000).toFixed(3)}, zero ${dark_lux.toFixed(2)} lux`;
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

function renderRelays() {
  const relays = lastStatus.relays || [];
  const liveBox = document.getElementById('relays');
  const liveHtml = relays.filter(r => r.enabled).map(r =>
    `<button class="relay-btn ${r.on?'on':''}" data-act="relayset" data-n="${r.n}" data-on="${r.on?1:0}">Rel&egrave; ${r.n+1}</button>`
  ).join('');
  liveBox.innerHTML = liveHtml || '<i class="muted">Nessun rel&egrave; abilitato</i>';
  liveBox.querySelectorAll('[data-act="relayset"]').forEach(b => {
    b.addEventListener('click', () => {
      const newVal = b.dataset.on === '1' ? 0 : 1;
      api.sendCmd(`CFG:RELAYSET;n=${b.dataset.n};val=${newVal}`);
      api.afterStatusRefresh();
    });
  });

  const checksBox = document.getElementById('relay-checks');
  checksBox.innerHTML = relays.map(r =>
    `<label><input type="checkbox" data-act="relaycfg" data-n="${r.n}" ${r.enabled?'checked':''}> Rel&egrave; ${r.n+1}</label>`
  ).join('');
  checksBox.querySelectorAll('[data-act="relaycfg"]').forEach(c => {
    c.addEventListener('change', () => api.sendCmd(`CFG:RELAYCFG;n=${c.dataset.n};enabled=${c.checked?1:0}`));
  });
}

function renderSensorSlots() {
  const liveBox = document.getElementById('sensors-live');
  const list = lastStatus.blesensors || [];
  const liveHtml = list.filter(s => s.configured).map(s =>
    `<div class="sensor-card ${s.last?'':'offline'}">Slot ${s.slot+1}${s.name?(' ('+s.name+')'):''}: ${s.last || 'in attesa beacon...'}</div>`
  ).join('');
  liveBox.innerHTML = liveHtml || '<i class="muted">Nessun sensore configurato</i>';

  // Non ridisegnare i campi se l'utente li ha modificati ma non ancora salvato:
  // il ridisegno sovrascrive i valori appena scritti (da "Usa MAC", "Usa regola"
  // o digitazione diretta). Il flag viene azzerato solo su Salva/Elimina.
  if (sensorSlotsDirty) return;

  const box = document.getElementById('sensor-slots');
  let h = '';
  for (let i = 0; i < 8; i++) {
    const s = list.find(x => x.slot === i) || { slot: i, name: '', mac: '', rules: '' };
    h += `<div class="slotrow"><span class="slotn">Slot ${i+1}</span>
      <span class="lab">nome</span> <input id="bn_${i}" value="${s.name||''}" placeholder="es. Cucina" size="12">
      <span class="lab">mac</span> <input id="bm_${i}" value="${s.mac||''}" placeholder="aabbccddeeff" size="18">
      <span class="lab">regole</span> <input id="bg_${i}" value="${s.rules||''}" placeholder="SVC,181A,0,2,S100,temp" size="32">
      <button type="button" class="btn sm" data-act="sensorsave" data-slot="${i}">Salva</button>
      <button type="button" class="btn danger sm" data-act="sensorreset" data-slot="${i}">Elimina</button></div>`;
  }
  box.innerHTML = h;
  // Qualsiasi digitazione nei campi attiva il flag "dirty"
  box.querySelectorAll('input').forEach(inp => inp.addEventListener('input', () => { sensorSlotsDirty = true; }));
  box.querySelectorAll('[data-act="sensorsave"]').forEach(b => {
    b.addEventListener('click', () => {
      sensorSlotsDirty = false; // permette il ridisegno con i nuovi dati dall'ESP
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
    btn.textContent = 'Ferma sniffer';
    api.startSnifferPoll();
  } else {
    api.sendCmd('CFG:SNIFFERSTOP');
    snifferOn = false;
    btn.textContent = 'Avvia sniffer';
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
    if (prevBytes[i] !== undefined && prevBytes[i] !== b) everChanged[offKey] = true;
    const cls = everChanged[offKey] ? 'byte changed' : 'byte';
    row += `<span class="${cls}" title="offset ${i}" data-act="fillrule" data-type="${type}" data-id="${id}" data-off="${i}">${b}</span>`;
  });
  prevHex[key] = hex;
  return row + '</div>';
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
  const base = pinnedMac ? lastSniffDevs.filter(d => d.mac === pinnedMac) : lastSniffDevs;
  const filtered = q ? base.filter(d => d.mac.toUpperCase().includes(q) || (d.name && d.name.toUpperCase().includes(q))) : base;

  const total = lastSniffDevs.length;
  const statusLine = snifferOn
    ? `<span style="font-size:.85em;opacity:.7">${total} dispositivi in memoria${q || pinnedMac ? ` (${filtered.length} visibili)` : ''}</span>`
    : total > 0
      ? `<span style="font-size:.85em;opacity:.6"><i>Sniffer fermo — ${total} dispositivi congelati. "Pulisci lista" per azzerare.</i></span>`
      : '';
  let h = statusLine ? `<p style="margin:0 0 8px">${statusLine}</p>` : '';
  const sorted = [...filtered].sort((a, b) => b.rssi - a.rssi); // dal più vicino al più lontano
  sorted.forEach(d => {
    h += `<div class="dev-card"><div><b>${d.mac}</b> ${d.name?('('+d.name+')'):''} - ${d.rssi} dBm
        <button type="button" data-act="usemac" data-mac="${d.mac}">Usa MAC</button>
        <button type="button" data-act="copymac" data-mac="${d.mac}">Copia MAC</button>
        <button type="button" data-act="togglepin" data-mac="${d.mac}">${pinnedMac===d.mac?'Mostra tutti':'Isola sensore'}</button></div>`;
    if (d.svc_hex) h += renderByteRow(d.mac, 'SVC', d.svc_uuid, d.svc_hex);
    if (d.mfr_hex) h += renderByteRow(d.mac, 'MFR', '0', d.mfr_hex);
    h += '</div>';
  });
  const list = document.getElementById('snifflist');
  list.innerHTML = h || (q ? '<i>Nessun dispositivo trovato con questo filtro.</i>' : '<i>Nessun dispositivo rilevato finora...</i>');

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
