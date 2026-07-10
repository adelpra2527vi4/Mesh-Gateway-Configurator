import { GatewaySerial } from './serial.js';
import * as ui from './ui.js';

const gw = new GatewaySerial();

// Coda comandi: non manda il prossimo CFG: prima che il precedente abbia
// avuto risposta (OK/ERR/BUSY) o sia scaduto un timeout di 8s.
const CMD_TIMEOUT_MS = 8000;
let cmdQueue = [];
let cmdInFlight = null;
let statePollTimer = null;
let statusPollTimer = null;
let sniffPollTimer = null;
let statePending = false;
let statusPending = false;

function cmdNameOf(line) {
  const body = line.slice(4);
  const semi = body.indexOf(';');
  return semi >= 0 ? body.slice(0, semi) : body;
}

function enqueue(line) {
  cmdQueue.push(line);
  pump();
}

function pump() {
  if (cmdInFlight || cmdQueue.length === 0) return;
  const line = cmdQueue.shift();
  const name = cmdNameOf(line);
  cmdInFlight = {
    name,
    timer: setTimeout(() => {
      ui.log(`(timeout: nessuna risposta per ${name})`, 'err');
      ui.onCmdResult('TIMEOUT', name);
      cmdInFlight = null;
      pump();
    }, CMD_TIMEOUT_MS),
  };
  gw.send(line);
}

function settleInFlight(name) {
  if (cmdInFlight && cmdInFlight.name === name) {
    clearTimeout(cmdInFlight.timer);
    cmdInFlight = null;
    pump();
  }
}

function requestState() {
  if (statePending) return;
  statePending = true;
  enqueue('CFG:STATE');
}
function requestStatus() {
  if (statusPending) return;
  statusPending = true;
  enqueue('CFG:STATUS');
}

export function sendCmd(line) { enqueue(line); }
export function afterCmdRefresh(delayMs = 250) { setTimeout(requestState, delayMs); }
export function afterStatusRefresh(delayMs = 250) { setTimeout(requestStatus, delayMs); }

export function startSnifferPoll() {
  stopSnifferPoll();
  enqueue('CFG:SNIFFERDATA');
  sniffPollTimer = setInterval(() => enqueue('CFG:SNIFFERDATA'), 1000);
}
export function stopSnifferPoll() {
  if (sniffPollTimer) { clearInterval(sniffPollTimer); sniffPollTimer = null; }
}

gw.addEventListener('connected', () => {
  ui.setConnected(true);
  // Durante il provisioning (lastState.busy) il radio BLE e' interamente
  // dedicato alla config del nuovo nodo: inviare CFG:STATE/CFG:STATUS ogni 2s
  // aggiunge traffico USB e porta il firmware a rispondere con decine di righe
  // che competono con i log di debug e i retry di config. Si rallenta a 10s
  // quando busy, si torna a 2s appena il gateway torna libero.
  let stateTick = 0;
  statePollTimer = setInterval(() => {
    const isBusy = ui.getLastStateBusy();
    // Durante il busy si rallenta a 10s (1 tick ogni 5) invece di fermarsi
    // del tutto: altrimenti il client non richiede mai piu' CFG:STATE e non
    // si accorge quando il firmware torna libero (bisognava usare il
    // refresh manuale per "sbloccare" la UI a fine provisioning).
    stateTick++;
    if (!isBusy) { stateTick = 0; requestState(); }
    else if (stateTick % 5 === 0) requestState();
  }, 2000);
  let statusTick = 0;
  statusPollTimer = setInterval(() => {
    const isBusy = ui.getLastStateBusy();
    statusTick++;
    if (!isBusy) { statusTick = 0; requestStatus(); }
    else if (statusTick % 5 === 0) requestStatus();
  }, 2000);
  // Poll iniziale sempre, poi si adatta
  requestState();
  requestStatus();
});

gw.addEventListener('disconnected', () => {
  ui.setConnected(false);
  if (statePollTimer) { clearInterval(statePollTimer); statePollTimer = null; }
  if (statusPollTimer) { clearInterval(statusPollTimer); statusPollTimer = null; }
  stopSnifferPoll();
  cmdQueue = [];
  if (cmdInFlight) { clearTimeout(cmdInFlight.timer); cmdInFlight = null; }
  statePending = false;
  statusPending = false;
});

gw.addEventListener('line', (e) => ui.log(e.detail, 'rx'));
gw.addEventListener('tx', (e) => ui.log(e.detail, 'tx'));

gw.addEventListener('state', (e) => {
  statePending = false;
  settleInFlight('STATE');
  ui.renderState(e.detail);
});

gw.addEventListener('status', (e) => {
  statusPending = false;
  settleInFlight('STATUS');
  ui.renderStatus(e.detail);
});

gw.addEventListener('sniffer', (e) => {
  settleInFlight('SNIFFERDATA');
  ui.renderSniffer(e.detail);
});

gw.addEventListener('result', (e) => {
  const { type, cmd, msg } = e.detail;
  settleInFlight(cmd);
  ui.onCmdResult(type, cmd);
  if (type === 'ERR') ui.log(`CFG:ERR;${cmd};${msg}`, 'err');
  if (type === 'BUSY') ui.log(`CFG:BUSY;${cmd}`, 'err');
  if (type === 'OK' && cmd !== 'STATE' && cmd !== 'STATUS' && cmd !== 'SNIFFERDATA' && cmd !== 'RESET') {
    // I comandi relè/sensori/hubname/sniffer aggiornano CFG:STATUS, quelli
    // mesh aggiornano CFG:STATE - per semplicita' aggiorniamo entrambi.
    afterCmdRefresh();
    afterStatusRefresh();
  }
});

gw.addEventListener('push', (e) => ui.applyPush(e.detail));

document.getElementById('btn-connect').addEventListener('click', async (e) => {
  // Feedback immediato al click (anello che si dilata da sotto il punto
  // premuto), a prescindere da quanto ci mette la vera connessione/
  // disconnessione USB a rispondere - vedi conversazione.
  const btn = e.currentTarget;
  btn.classList.remove('pressed');
  void btn.offsetWidth;
  btn.classList.add('pressed');
  setTimeout(() => btn.classList.remove('pressed'), 500);

  if (gw.connected) { await gw.disconnect(); return; }
  try { await gw.connect(); } catch (err) { ui.log('Errore connessione: ' + err.message, 'err'); }
});
document.getElementById('btn-refresh').addEventListener('click', () => {
  const btn = document.getElementById('btn-refresh');
  btn.classList.remove('spinning');
  void btn.offsetWidth; // forza reflow: riavvia l'animazione anche se già attiva
  btn.classList.add('spinning');
  setTimeout(() => btn.classList.remove('spinning'), 1400);
  requestState(); requestStatus();
});
document.querySelectorAll('.btn-clearlog').forEach(btn => btn.addEventListener('click', () => ui.clearLog()));
document.querySelectorAll('.btn-copylog').forEach(btn => btn.addEventListener('click', async () => {
  // I pannelli .log-panel sono mirror dello stesso stream (vedi ui.js log()):
  // basta leggere il primo che c'e' nella pagina, contengono lo stesso testo.
  const panel = document.querySelector('.log-panel');
  const text = panel ? panel.innerText : '';
  const flash = (ok) => {
    const old = btn.textContent;
    btn.textContent = ok ? 'Copiato!' : 'Errore copia';
    setTimeout(() => { btn.textContent = old; }, 1200);
  };
  // navigator.clipboard richiede un contesto sicuro (HTTPS o localhost):
  // stesso problema gia' risolto nel pannello debug di Manager.py - fallback
  // con textarea nascosta + execCommand('copy') se manca l'API.
  if (navigator.clipboard && navigator.clipboard.writeText && window.isSecureContext) {
    try { await navigator.clipboard.writeText(text); flash(true); } catch { flash(false); }
    return;
  }
  try {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.focus();
    ta.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    flash(ok);
  } catch { flash(false); }
}));
// Pulsante notte/giorno: l'icona mostrata e' quella su cui si sta per
// cliccare (sole di notte = "passa al giorno", luna di giorno = "passa alla
// notte"), non lo stato attuale - vedi conversazione. sun/moon.icon-appear e
// .icon-disappear sono animazioni CSS distinte (vedi style.css); al termine
// di ciascuna si passa a icon-idle (entrata/riposo) o si toglie ogni classe
// (uscita, resta invisibile) cosi' l'animazione non si "ripete" da ferma.
(() => {
  const btn  = document.getElementById('btn-theme');
  const sun  = btn.querySelector('.sun');
  const moon = btn.querySelector('.moon');

  function setIcon(el, cls) {
    el.classList.remove('icon-appear', 'icon-disappear', 'icon-idle');
    if (cls) el.classList.add(cls);
  }

  // showSun=true -> modalita' notturna attiva (mostra il sole, per passare al giorno).
  function applyIcons(showSun, animate) {
    const enter = showSun ? sun : moon;
    const exit  = showSun ? moon : sun;
    if (!animate) { setIcon(enter, 'icon-idle'); setIcon(exit, null); return; }
    setIcon(enter, 'icon-appear');
    setIcon(exit, 'icon-disappear');
    enter.addEventListener('animationend', () => setIcon(enter, 'icon-idle'), { once: true });
    exit.addEventListener('animationend', () => setIcon(exit, null), { once: true });
  }

  applyIcons(!document.documentElement.classList.contains('light'), false); // stato iniziale, senza animare

  btn.addEventListener('click', () => {
    document.documentElement.classList.toggle('light');
    const isLight = document.documentElement.classList.contains('light');
    localStorage.setItem('theme', isLight ? 'light' : 'dark');
    applyIcons(!isLight, true);
  });
})();

document.getElementById('tb-mesh').addEventListener('click', () => ui.showTab('mesh'));
document.getElementById('tb-beacon').addEventListener('click', () => ui.showTab('beacon'));
document.getElementById('tb-device').addEventListener('click', () => ui.showTab('device'));

ui.init({ sendCmd, afterCmdRefresh, afterStatusRefresh, startSnifferPoll, stopSnifferPoll, gw });

if (!('serial' in navigator)) {
  document.getElementById('banner-nosupport').style.display = 'block';
  document.getElementById('btn-connect').disabled = true;
}
