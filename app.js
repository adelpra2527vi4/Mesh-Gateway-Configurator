import { GatewaySerial } from './serial.js';
import * as ui from './ui.js';

const gw = new GatewaySerial();

// Coda comandi: non manda il prossimo CFG: prima che il precedente abbia
// avuto risposta (OK/ERR/BUSY) o sia scaduto un timeout di 8s (vedi spec).
const CMD_TIMEOUT_MS = 8000;
let cmdQueue = [];
let cmdInFlight = null;
let pollTimer = null;
let statePending = false;

function cmdNameOf(line) {
  // "CFG:PROVISION;uuid=..." -> "PROVISION" ; "CFG:STATE" -> "STATE"
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
  if (statePending) return; // gia' una CFG:STATE in volo, non accodarne un'altra
  statePending = true;
  enqueue('CFG:STATE');
}

// API esposta a ui.js per i pulsanti
export function sendCmd(line) {
  enqueue(line);
}

export function afterCmdRefresh(delayMs = 250) {
  setTimeout(requestState, delayMs);
}

gw.addEventListener('connected', () => {
  ui.setConnected(true);
  pollTimer = setInterval(requestState, 2000);
  requestState();
});

gw.addEventListener('disconnected', () => {
  ui.setConnected(false);
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  cmdQueue = [];
  if (cmdInFlight) { clearTimeout(cmdInFlight.timer); cmdInFlight = null; }
  statePending = false;
});

gw.addEventListener('line', (e) => {
  ui.log(e.detail, 'rx');
});
gw.addEventListener('tx', (e) => {
  ui.log(e.detail, 'tx');
});

gw.addEventListener('state', (e) => {
  statePending = false;
  settleInFlight('STATE');
  ui.renderState(e.detail);
});

gw.addEventListener('result', (e) => {
  const { type, cmd, msg } = e.detail;
  settleInFlight(cmd);
  if (type === 'ERR') ui.log(`CFG:ERR;${cmd};${msg}`, 'err');
  if (type === 'BUSY') ui.log(`CFG:BUSY;${cmd}`, 'err');
  // Dopo un OK rilevante, richiedi lo stato aggiornato (vedi spec) - non per
  // STATE stesso (gia' gestito sopra) ne' per RESET (il gateway si riavvia).
  if (type === 'OK' && cmd !== 'STATE' && cmd !== 'RESET') {
    afterCmdRefresh();
  }
});

gw.addEventListener('push', (e) => {
  ui.applyPush(e.detail);
});

document.getElementById('btn-connect').addEventListener('click', async () => {
  if (gw.connected) {
    await gw.disconnect();
    return;
  }
  try {
    await gw.connect();
  } catch (err) {
    ui.log('Errore connessione: ' + err.message, 'err');
  }
});

document.getElementById('btn-refresh').addEventListener('click', () => requestState());
document.getElementById('btn-clearlog').addEventListener('click', () => ui.clearLog());

ui.init({ sendCmd, afterCmdRefresh, gw });

if (!('serial' in navigator)) {
  document.getElementById('banner-nosupport').style.display = 'block';
  document.getElementById('btn-connect').disabled = true;
}
