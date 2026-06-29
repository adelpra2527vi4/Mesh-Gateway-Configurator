// Rendering: nessun framework, solo DOM diretto. Tiene una copia locale
// dell'ultimo CFG:STATE ricevuto (lastState) cosi' i push spontanei
// (ONOFF;/LEVEL;/SENSOR;) possono aggiornarla e ridisegnare senza aspettare
// il prossimo poll.

let api = null; // { sendCmd, afterCmdRefresh, gw }
let lastState = { busy: false, oob: false, nodes: [], discovered: [] };
let logLines = 0;
const LOG_MAX = 500;

export function init(a) {
  api = a;
  document.getElementById('btn-adddev').addEventListener('click', () => {
    const qr = document.getElementById('qr-adddev').value.trim();
    if (!qr) return;
    api.sendCmd('CFG:ADDDEV;qr=' + qr);
  });
  document.getElementById('btn-scanstart').addEventListener('click', () => {
    api.sendCmd('CFG:SCANSTART');
  });
  document.getElementById('btn-clearoob').addEventListener('click', () => {
    api.sendCmd('CFG:CLEAROOB');
    api.afterCmdRefresh();
  });
  document.getElementById('btn-reset').addEventListener('click', () => {
    if (!confirm('Questa operazione cancella tutti i nodi. Sei sicuro?')) return;
    api.sendCmd('CFG:RESET');
    startResetCountdown();
  });
}

export function setConnected(connected) {
  const btn = document.getElementById('btn-connect');
  const dot = document.getElementById('conn-dot');
  btn.textContent = connected ? 'Disconnetti' : 'Connetti';
  btn.classList.toggle('danger', connected);
  dot.classList.toggle('on', connected);
  document.getElementById('main-content').classList.toggle('hidden', !connected);
  if (!connected) {
    lastState = { busy: false, oob: false, nodes: [], discovered: [] };
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

export function clearLog() {
  document.getElementById('log-panel').innerHTML = '';
  logLines = 0;
}

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

export function renderState(state) {
  lastState = state;
  render();
}

// Applica un push spontaneo (ONOFF;/LEVEL;/SENSOR;) allo stato locale e
// ridisegna - senza aspettare il prossimo CFG:STATE.
export function applyPush(detail) {
  if (detail.type === 'DUMPEND') return;
  const addr = detail.addr ? '0x' + detail.addr.toUpperCase() : null;
  if (!addr) return;
  for (const node of lastState.nodes) {
    if (detail.type === 'ONOFF') {
      const el = node.elems.find(e => e.addr.toLowerCase() === ('0x' + detail.addr).toLowerCase());
      if (el) { el.on = detail.val === 1; render(); return; }
    } else if (detail.type === 'LEVEL') {
      const lv = node.lvls.find(l => l.addr.toLowerCase() === ('0x' + detail.addr).toLowerCase());
      if (lv) { lv.pct = detail.pct; render(); return; }
    } else if (detail.type === 'SENSOR') {
      if (node.base && node.base.toLowerCase() === ('0x' + detail.addr).toLowerCase() && node.sensor) {
        node.sensor.pres = detail.presence ? 1 : 0;
        node.sensor.light = detail.lux >= 0 ? Math.round(detail.lux * 100) : -1;
        render();
        return;
      }
    }
  }
}

function render() {
  document.getElementById('badge-busy').classList.toggle('show', lastState.busy);
  document.getElementById('badge-oob').classList.toggle('show', lastState.oob);
  document.getElementById('btn-clearoob').disabled = !lastState.oob;

  renderDiscovered();
  renderNodes();
}

function shortUuid(u) {
  return u.slice(0, 8) + '...' + u.slice(-4);
}

function renderDiscovered() {
  const box = document.getElementById('discovered-box');
  const list = lastState.discovered || [];
  if (list.length === 0) { box.innerHTML = ''; box.classList.add('hidden'); return; }
  box.classList.remove('hidden');
  box.innerHTML = '<h2>Dispositivi rilevati</h2>' + list.map(d => {
    const oobTag = d.oob ? `<span class="badge warn">OOB richiesto</span>` : '';
    const canProv = !d.oob || lastState.oob;
    const btn = canProv
      ? `<button class="btn primary sm" data-uuid="${d.uuid}">Provisiona</button>`
      : `<span class="muted" title="Registra prima il QR code dello switch">Provisiona</span>`;
    return `<div class="dev-card">
      <div class="grow">
        <div class="mono">${shortUuid(d.uuid)}</div>
        <div class="muted">${d.addr} &middot; ${d.rssi} dBm ${oobTag}</div>
      </div>
      ${btn}
    </div>`;
  }).join('');
  box.querySelectorAll('button[data-uuid]').forEach(b => {
    b.addEventListener('click', () => api.sendCmd('CFG:PROVISION;uuid=' + b.dataset.uuid));
  });
}

function renderNodes() {
  const box = document.getElementById('nodes-box');
  const nodes = lastState.nodes || [];
  if (nodes.length === 0) { box.innerHTML = '<p class="muted">Nessun nodo configurato.</p>'; return; }
  box.innerHTML = nodes.map(renderNode).join('');
  wireNodeEvents(box);
}

function statusBadge(n) {
  if (n.fail) return `<span class="badge err">&#10060; ERRORE</span>`;
  if (!n.cfg) return `<span class="badge warn">&#9888; conf...</span>`;
  return `<span class="badge ok">&#9989; OK</span>`;
}

function renderNode(n) {
  const head = `<div class="node-head">
    <b>Dispositivo #${n.i}</b> <span class="mono muted">${n.base}</span> ${statusBadge(n)}
  </div>`;

  if (n.sw) {
    return `<div class="card switch-card" data-node="${n.i}">
      ${head}
      <div class="row">
        <span class="mono muted">&#127918; Switch</span>
        <button class="btn danger sm" data-act="forget" data-node="${n.i}">&#128465; Rimuovi</button>
      </div>
    </div>`;
  }

  const kindSel = `<select data-act="setkind" data-node="${n.i}">
      <option value="0" ${n.kind === 0 ? 'selected' : ''}>Lampada</option>
      <option value="1" ${n.kind === 1 ? 'selected' : ''}>Sensore</option>
    </select>`;
  const grpBadge = n.grp
    ? `<span class="badge ok">Gruppo OK</span>`
    : `<span class="badge warn">non rebindato</span>`;
  const actions = `<div class="row">
      ${kindSel} ${grpBadge}
      <button class="btn sm" data-act="rebind" data-node="${n.i}">&#128260; Rebind</button>
      <button class="btn danger sm" data-act="forget" data-node="${n.i}">&#128465; Rimuovi</button>
    </div>`;

  let body;
  if (n.kind === 1) {
    const s = n.sensor;
    const presTxt = !s || s.pres < 0 ? '&mdash;' : (s.pres ? 'PRESENZA' : 'assente');
    const presCls = s && s.pres > 0 ? 'on' : 'off';
    const luxTxt = !s || s.light < 0 ? '&mdash;' : (s.light / 100).toFixed(2) + ' lux';
    const warn = !s || !s.hassens ? `<div class="muted">&#9888; nessun Sensor Server</div>` : '';
    body = `<div class="elem-card sensor-card">
      <span class="badge ${presCls}">${presTxt}</span>
      <span class="lux">${luxTxt}</span>
      ${warn}
    </div>`;
  } else {
    const elems = n.elems.map(e => `<div class="elem-card">
        <span class="mono muted">e${e.e}</span>
        <span class="badge ${e.on ? 'on' : 'off'}">${e.on ? 'ON' : 'OFF'}</span>
        <button class="btn sm" data-act="cmd" data-node="${n.i}" data-elem="${e.e}" data-val="1">ACCENDI</button>
        <button class="btn sm" data-act="cmd" data-node="${n.i}" data-elem="${e.e}" data-val="0">SPEGNI</button>
      </div>`).join('');
    const lvls = n.lvls.map(l => `<div class="elem-card">
        <span class="mono muted">e${l.e}</span>
        <input type="range" min="0" max="100" value="${l.pct}" class="slider"
               data-act="level-input" data-node="${n.i}" data-li="${l.li}">
        <span class="pct-label" data-li-label="${n.i}-${l.li}">${l.pct}%</span>
      </div>`).join('');
    const pairBox = n.paired
      ? `<div class="row"><span class="badge ok">&#9989; Abbinato</span>
           <button class="btn danger sm" data-act="unpair" data-node="${n.i}">Scollega</button></div>`
      : `<div class="row">
           <input type="text" placeholder="QR companion" data-act="pair-qr" data-node="${n.i}" class="grow-input">
           <button class="btn sm" data-act="pair" data-node="${n.i}">Abbina</button></div>`;
    body = elems + lvls + `<div class="companion">${pairBox}</div>`;
  }

  return `<div class="card lamp-card" data-node="${n.i}">${head}${actions}${body}</div>`;
}

function wireNodeEvents(box) {
  box.querySelectorAll('[data-act="setkind"]').forEach(el => {
    el.addEventListener('change', () => {
      api.sendCmd(`CFG:SETKIND;node=${el.dataset.node};kind=${el.value}`);
      api.afterCmdRefresh();
    });
  });
  box.querySelectorAll('[data-act="rebind"]').forEach(el => {
    el.addEventListener('click', () => api.sendCmd(`CFG:REBIND;node=${el.dataset.node}`));
  });
  box.querySelectorAll('[data-act="forget"]').forEach(el => {
    el.addEventListener('click', () => {
      if (!confirm('Rimuovere questo nodo?')) return;
      api.sendCmd(`CFG:FORGET;node=${el.dataset.node}`);
    });
  });
  box.querySelectorAll('[data-act="cmd"]').forEach(el => {
    el.addEventListener('click', () => {
      api.sendCmd(`CFG:CMD;node=${el.dataset.node};elem=${el.dataset.elem};val=${el.dataset.val}`);
      api.afterCmdRefresh();
    });
  });
  box.querySelectorAll('[data-act="level-input"]').forEach(el => {
    const label = box.querySelector(`[data-li-label="${el.dataset.node}-${el.dataset.li}"]`);
    el.addEventListener('input', () => { if (label) label.textContent = el.value + '%'; });
    el.addEventListener('change', () => {
      api.sendCmd(`CFG:LEVEL;node=${el.dataset.node};elem=${el.dataset.li};val=${el.value}`);
      api.afterCmdRefresh();
    });
  });
  box.querySelectorAll('[data-act="pair"]').forEach(el => {
    el.addEventListener('click', () => {
      const input = box.querySelector(`[data-act="pair-qr"][data-node="${el.dataset.node}"]`);
      const qr = input ? input.value.trim() : '';
      if (!qr) return;
      api.sendCmd(`CFG:PAIR;node=${el.dataset.node};qr=${qr}`);
    });
  });
  box.querySelectorAll('[data-act="unpair"]').forEach(el => {
    el.addEventListener('click', () => {
      if (!confirm('Scollegare il companion?')) return;
      api.sendCmd(`CFG:UNPAIR;node=${el.dataset.node}`);
    });
  });
}
