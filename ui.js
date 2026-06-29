// Rendering: nessun framework, solo DOM diretto. Porta la stessa logica
// della vecchia pagina HTML del gateway (render(d) per la mesh, poll() per
// relè/sensori BLE classici, lo sniffer con evidenziazione byte) sul
// protocollo a righe CFG: invece che su fetch+JSON.

let api = null; // { sendCmd, afterCmdRefresh, afterStatusRefresh, startSnifferPoll, stopSnifferPoll, gw }
let lastState  = { busy: false, oob: false, nodes: [], discovered: [] };
let lastStatus = { relays: [], blesensors: [] };
let logLines = 0;
const LOG_MAX = 500;

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
    lastState = { busy: false, oob: false, nodes: [], discovered: [] };
    lastStatus = { relays: [], blesensors: [] };
    snifferOn = false;
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
export function renderState(state) { lastState = state; renderMesh(); }

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

function renderDiscovered() {
  const box = document.getElementById('discovered-box');
  const list = lastState.discovered || [];
  if (list.length === 0) { box.innerHTML = ''; return; }
  box.innerHTML = '<div class="section-title">Dispositivi rilevati</div>' + list.map(d => {
    const canProv = !d.oob || lastState.oob;
    const oobTag = d.oob ? `<span class="badge warn">OOB</span>` : `<span class="badge">No OOB</span>`;
    const btn = canProv
      ? `<button class="btn primary sm" data-act="provision" data-uuid="${d.uuid}">Provisiona</button>`
      : `<span class="muted">(Registra prima il QR OOB)</span>`;
    return `<div class="dev-card"><div class="grow">
        <div style="font-family:ui-monospace,monospace;font-weight:600">${d.addr} <span class="rssi">${d.rssi||0} dBm</span></div>
        <div style="margin-top:3px">${oobTag} <span class="addr">UUID ${d.uuid.slice(0,8)}...</span></div>
      </div>${btn}</div>`;
  }).join('');
  box.querySelectorAll('[data-act="provision"]').forEach(b => {
    b.addEventListener('click', () => api.sendCmd('CFG:PROVISION;uuid=' + b.dataset.uuid));
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
    const lux = !s || s.light < 0 ? '&mdash;' : (s.light/100).toFixed(2) + ' lux';
    const warn = !s || !s.hassens ? `<div class="addr" style="margin-top:8px">(nessun Sensor Server su questo device)</div>` : '';
    return head + `<div class="cards">
        <div class="card"><div class="elem-title">Presenza <span class="pill ${presOn?'on':'off'}">${pres}</span></div></div>
        <div class="card"><div class="elem-title">Luce ambiente</div><div class="pctlbl" style="margin-top:6px">${lux}</div></div>
      </div>${warn}</div>`;
  }

  let cards = `<div class="cards">`;
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

  // I campi di editing (mac/regole/nome): non li ridisegno se l'utente ci sta
  // scrivendo dentro (stesso problema/fix del nome nodo nella mesh).
  if (document.activeElement && document.activeElement.closest && document.activeElement.closest('#sensor-slots')) return;

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
  box.querySelectorAll('[data-act="sensorsave"]').forEach(b => {
    b.addEventListener('click', () => {
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
    document.getElementById('snifflist').innerHTML = '<i>Sniffer fermo.</i>';
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
  lastSniffDevs = devs;
  renderSnifferList();
}

function renderSnifferList() {
  const q = (document.getElementById('sniffsearch').value || '').toUpperCase();
  const base = pinnedMac ? lastSniffDevs.filter(d => d.mac === pinnedMac) : lastSniffDevs;
  const filtered = q ? base.filter(d => d.mac.toUpperCase().includes(q) || (d.name && d.name.toUpperCase().includes(q))) : base;

  let h = '';
  filtered.forEach(d => {
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
      if (m) m.value = b.dataset.mac;
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
      if (g) g.value = g.value ? g.value + ';' + rule : rule;
    });
  });
}
