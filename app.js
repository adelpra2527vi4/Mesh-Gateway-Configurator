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
          // reset dei pending in caso di timeout
          if (name === 'STATE') statePending = false;
          if (name === 'STATUS') statusPending = false;
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
    if (statePending) return; // bloccato!
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
    statePollTimer = setInterval(requestState, 2000);
    statusPollTimer = setInterval(requestStatus, 2000);
    requestState();    // <-- subito!
    requestStatus();   // <-- subito!
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

document.getElementById('btn-connect').addEventListener('click', async () => {
  if (gw.connected) { await gw.disconnect(); return; }
  try { await gw.connect(); } catch (err) { ui.log('Errore connessione: ' + err.message, 'err'); }
});
document.getElementById('btn-refresh').addEventListener('click', () => { requestState(); requestStatus(); });
document.getElementById('btn-clearlog').addEventListener('click', () => ui.clearLog());
document.getElementById('btn-theme').addEventListener('click', () => {
  document.documentElement.classList.toggle('light');
  localStorage.setItem('theme', document.documentElement.classList.contains('light') ? 'light' : 'dark');
});

document.getElementById('tb-mesh').addEventListener('click', () => ui.showTab('mesh'));
document.getElementById('tb-setup').addEventListener('click', () => ui.showTab('setup'));

ui.init({ sendCmd, afterCmdRefresh, afterStatusRefresh, startSnifferPoll, stopSnifferPoll, gw });

if (!('serial' in navigator)) {
  document.getElementById('banner-nosupport').style.display = 'block';
  document.getElementById('btn-connect').disabled = true;
}
