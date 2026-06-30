// Modulo Web Serial: apre la porta USB CDC del gateway, accumula i byte in
// arrivo in righe complete (i chunk USB non corrispondono 1:1 alle righe),
// e smista ogni riga negli eventi 'line' / 'state' / 'status' / 'sniffer' /
// 'push' / 'result'. Nessuna dipendenza esterna - EventTarget nativo del browser.

const BLOCK_TIMEOUT_MS = 5000;

export class GatewaySerial extends EventTarget {
  constructor() {
    super();
    this.port = null;
    this.reader = null;
    this.connected = false;
    this.rxBuf = '';
    this._stateAcc = null;   // CFG:STATE_START..END
    this._statusAcc = null;  // CFG:STATUS_START..END (relè + sensori BLE classici)
    this._sniffAcc = null;   // CFG:SNIFF_START..END (dispositivi sniffer)
    this._blockTimer = null;
  }

  async connect() {
    if (!('serial' in navigator)) {
      throw new Error('Web Serial non disponibile in questo browser.');
    }
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200 });
    this.connected = true;
    this.dispatchEvent(new CustomEvent('connected'));
    this._readLoop();
  }

  async disconnect() {
    this.connected = false;
    try { if (this.reader) await this.reader.cancel(); } catch (_) {}
    try { if (this.port) await this.port.close(); } catch (_) {}
    this.reader = null;
    this.port = null;
    this._clearBlockTimer();
    this.dispatchEvent(new CustomEvent('disconnected'));
  }

  async send(line) {
    if (!this.connected || !this.port || !this.port.writable) return;
    const encoder = new TextEncoder();
    const writer = this.port.writable.getWriter();
    try {
      await writer.write(encoder.encode(line + '\n'));
    } finally {
      writer.releaseLock();
    }
    this.dispatchEvent(new CustomEvent('tx', { detail: line }));
  }

  async _readLoop() {
    const decoder = new TextDecoderStream();
    const readableClosed = this.port.readable.pipeTo(decoder.writable);
    this.reader = decoder.readable.getReader();
    try {
      while (this.connected) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (value) this._onChunk(value);
      }
    } catch (e) {
      // porta scollegata/errore - normale durante un CFG:RESET o stacco USB
    } finally {
      try { await readableClosed.catch(() => {}); } catch (_) {}
      if (this.connected) {
        this.connected = false;
        this.dispatchEvent(new CustomEvent('disconnected'));
      }
    }
  }

  _onChunk(text) {
    this.rxBuf += text;
    let idx;
    while ((idx = this.rxBuf.indexOf('\n')) >= 0) {
      let line = this.rxBuf.slice(0, idx);
      this.rxBuf = this.rxBuf.slice(idx + 1);
      if (line.endsWith('\r')) line = line.slice(0, -1);
      if (line.length === 0) continue;
      this._onLine(line);
    }
  }

  static parseFields(rest) {
    // rest = "k=v;k2=v2..." (senza il "CFG:TYPE;" iniziale)
    const parts = rest.split(';');
    const fields = {};
    for (const p of parts) {
      const eq = p.indexOf('=');
      if (eq < 0) continue;
      fields[p.slice(0, eq)] = p.slice(eq + 1);
    }
    return fields;
  }

  _onLine(line) {
    // Le righe di CORPO dei blocchi CFG:STATE/STATUS/SNIFF (decine ogni 2s,
    // poll automatico) e i loro marcatori START/END NON vengono piu' mandate
    // sull'evento 'line' (quindi non finiscono nel pannello "Log seriale"):
    // sono gia' interpretate strutturalmente (eventi 'state'/'status'/
    // 'sniffer') e il rumore rendeva impossibile vedere a occhio le righe che
    // contano davvero (DBG;, CFG:OK/ERR, comandi mandati, push) durante un
    // provisioning che richiede minuti - vedi conversazione.
    if (line === 'CFG:STATE_START') { this._stateAcc = { busy: false, oob: false, usbMode: false, nodes: [], discovered: [] }; this._armTimer(); return; }
    if (line === 'CFG:STATE_END') {
      this._clearBlockTimer();
      const st = this._stateAcc; this._stateAcc = null;
      if (st) this.dispatchEvent(new CustomEvent('state', { detail: st }));
      return;
    }
    if (line === 'CFG:STATUS_START') { this._statusAcc = { relays: [], blesensors: [] }; this._armTimer(); return; }
    if (line === 'CFG:STATUS_END') {
      this._clearBlockTimer();
      const st = this._statusAcc; this._statusAcc = null;
      if (st) this.dispatchEvent(new CustomEvent('status', { detail: st }));
      return;
    }
    if (line === 'CFG:SNIFF_START') { this._sniffAcc = []; this._armTimer(); return; }
    if (line === 'CFG:SNIFF_END') {
      this._clearBlockTimer();
      const devs = this._sniffAcc; this._sniffAcc = null;
      if (devs) this.dispatchEvent(new CustomEvent('sniffer', { detail: devs }));
      return;
    }

    if (this._stateAcc && line.startsWith('CFG:')) { this._accumulateState(line); return; }
    if (this._statusAcc && line.startsWith('CFG:')) { this._accumulateStatus(line); return; }
    if (this._sniffAcc && line.startsWith('CFG:SNIFFDEV;')) { this._accumulateSniff(line); return; }

    // Da qui in poi: righe "interessanti" (non corpo di un blocco) - queste
    // SI vedono nel log.
    this.dispatchEvent(new CustomEvent('line', { detail: line }));

    if (line.startsWith('CFG:OK;') || line.startsWith('CFG:ERR;') || line.startsWith('CFG:BUSY;')) {
      this._onResult(line);
      return;
    }
    if (line.startsWith('ONOFF;') || line.startsWith('LEVEL;') || line.startsWith('SENSOR;') || line === 'DUMPEND') {
      this._onPush(line);
      return;
    }
  }

  _armTimer() {
    this._clearBlockTimer();
    this._blockTimer = setTimeout(() => {
      this.dispatchEvent(new CustomEvent('line', { detail: '(timeout: blocco CFG: non terminato entro 5s)' }));
      this._stateAcc = null; this._statusAcc = null; this._sniffAcc = null;
    }, BLOCK_TIMEOUT_MS);
  }
  _clearBlockTimer() {
    if (this._blockTimer) { clearTimeout(this._blockTimer); this._blockTimer = null; }
  }

  _accumulateState(line) {
    const semi = line.indexOf(';');
    const type = semi >= 0 ? line.slice(4, semi) : line.slice(4);
    const fields = GatewaySerial.parseFields(semi >= 0 ? line.slice(semi + 1) : '');
    const st = this._stateAcc;
    switch (type) {
      case 'BUSY': st.busy = line === 'CFG:BUSY;true'; break;
      case 'OOB':  st.oob  = line === 'CFG:OOB;true'; break;
      case 'USBMODE': st.usbMode = fields.active === 'true'; break;
      case 'NODE': {
        st.nodes.push({
          i: parseInt(fields.i, 10), base: fields.base,
          cfg: fields.cfg === '1', fail: fields.fail === '1', sw: fields.sw === '1',
          kind: parseInt(fields.kind, 10), grp: fields.grp === '1', paired: fields.paired === '1',
          lamp_elem: parseInt(fields.lamp_elem, 10), name: fields.name || '',
          elems: [], lvls: [], sensor: null,
        });
        break;
      }
      case 'ELEM': {
        const node = st.nodes.find(x => x.i === parseInt(fields.node, 10));
        if (node) node.elems.push({ e: parseInt(fields.e, 10), addr: fields.addr, on: fields.on === '1' });
        break;
      }
      case 'LVL': {
        const node = st.nodes.find(x => x.i === parseInt(fields.node, 10));
        if (node) node.lvls.push({ li: parseInt(fields.li, 10), e: parseInt(fields.e, 10), addr: fields.addr, pct: parseInt(fields.pct, 10) });
        break;
      }
      case 'SENSOR_DATA': {
        const node = st.nodes.find(x => x.i === parseInt(fields.node, 10));
        if (node) node.sensor = { pres: parseInt(fields.pres, 10), light: parseInt(fields.light, 10), hassens: fields.hassens === '1' };
        break;
      }
      case 'DISCOVERED':
        st.discovered.push({ uuid: fields.uuid, addr: fields.addr, rssi: parseInt(fields.rssi, 10), oob: fields.oob === '1' });
        break;
      default: break;
    }
  }

  static getBlesensor(st, slot) {
    let b = st.blesensors.find(x => x.slot === slot);
    if (!b) { b = { slot, configured: false, name: '', mac: '', rules: '', last: '' }; st.blesensors.push(b); }
    return b;
  }

  _accumulateStatus(line) {
    const semi = line.indexOf(';');
    const type = semi >= 0 ? line.slice(4, semi) : line.slice(4);
    const st = this._statusAcc;
    if (type === 'RELAY') {
      const fields = GatewaySerial.parseFields(line.slice(semi + 1));
      st.relays.push({ n: parseInt(fields.n, 10), enabled: fields.enabled === '1', on: fields.on === '1' });
    } else if (type === 'BLESENSOR') {
      const fields = GatewaySerial.parseFields(line.slice(semi + 1));
      const b = GatewaySerial.getBlesensor(st, parseInt(fields.slot, 10));
      b.configured = fields.configured === '1'; b.name = fields.name || ''; b.mac = fields.mac || '';
    } else if (type === 'BLERULES') {
      // "rules=" e' sempre l'ultimo campo (puo' contenere ';'): prendo tutto
      // cio' che segue, non mi fermo al primo ';'.
      const m = line.match(/;slot=(\d+);rules=([\s\S]*)$/);
      if (m) { const b = GatewaySerial.getBlesensor(st, parseInt(m[1], 10)); b.rules = m[2]; }
    } else if (type === 'BLELAST') {
      const m = line.match(/;slot=(\d+);last=([\s\S]*)$/);
      if (m) { const b = GatewaySerial.getBlesensor(st, parseInt(m[1], 10)); b.last = m[2]; }
    }
  }

  _accumulateSniff(line) {
    const fields = GatewaySerial.parseFields(line.slice('CFG:SNIFFDEV;'.length));
    this._sniffAcc.push({
      mac: fields.mac || '', name: fields.name || '', rssi: parseInt(fields.rssi, 10),
      svc_uuid: fields.svc_uuid || '', svc_hex: fields.svc_hex || '', mfr_hex: fields.mfr_hex || '',
    });
  }

  _onResult(line) {
    const semi1 = line.indexOf(';');
    const type = line.slice(4, semi1);
    const rest = line.slice(semi1 + 1);
    const semi2 = rest.indexOf(';');
    const cmd = semi2 >= 0 ? rest.slice(0, semi2) : rest;
    const msg = semi2 >= 0 ? rest.slice(semi2 + 1) : '';
    this.dispatchEvent(new CustomEvent('result', { detail: { type, cmd, msg } }));
  }

  _onPush(line) {
    if (line === 'DUMPEND') { this.dispatchEvent(new CustomEvent('push', { detail: { type: 'DUMPEND' } })); return; }
    const parts = line.split(';');
    const type = parts[0];
    const fields = {};
    for (let i = 1; i < parts.length; i++) {
      const eq = parts[i].indexOf('=');
      if (eq < 0) continue;
      fields[parts[i].slice(0, eq)] = parts[i].slice(eq + 1);
    }
    const detail = { type };
    if (fields.addr !== undefined) detail.addr = fields.addr;
    if (fields.val !== undefined) detail.val = parseInt(fields.val, 10);
    if (fields.pct !== undefined) detail.pct = parseInt(fields.pct, 10);
    if (fields.presence !== undefined) detail.presence = fields.presence === '1';
    if (fields.lux !== undefined) detail.lux = parseFloat(fields.lux);
    this.dispatchEvent(new CustomEvent('push', { detail }));
  }
}
