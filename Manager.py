from flask import Flask, Response, render_template_string, jsonify, redirect, url_for, request
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import io
import datetime
from datetime import date
import threading
import paho.mqtt.client as mqtt_client
import json
import os
import time
import socket
import subprocess

def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Non deve per forza connettersi davvero, serve solo per determinare l'interfaccia di rete
        s.connect(('10.255.255.255', 1))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

app = Flask(__name__)

# --- VARIABILI GLOBALI E CONFIGURAZIONI ---
DT_FORMAT = "%d/%m/%Y %H:%M:%S"
MQTT_BROKER = "91.241.86.224"
MQTT_PORT   = 1883
MQTT_TOPIC_RX = "modem/display"
RELAY_TOPIC = "modem/invio"
CONFIG_FILE = "config.json"
NAMES_CACHE_FILE = "names_cache.json"  # vedi load_names_cache()/save_names_cache()
MESH_PRESENCE_MAX_AGE_SEC = 90  # oltre questa eta' un presence=1 mesh viene ignorato (link UART/mesh caduto)
MESH_GHOST_TIMEOUT_SEC = 660  # voci MESH_STATUS non aggiornate da piu' di cosi' vengono rimosse (lampade fantasma dopo un reset/riprovisioning della mesh).
# Il gateway rinfresca "time" per ogni addr ancora valido solo quando manda MESHLIST (ogni 5 min, vedi
# mqtt_periodic_publish in main.c): deve restare > 300s con un margine per un eventuale ciclo perso,
# altrimenti una lampada ferma (nessun cambio onoff/pct, quindi nessun MESH| diff-pubblicato) viene
# cancellata per "inattivita'" anche se il gateway la confirma viva ad ogni manifest.

DATA_STORE = []
LAST_RELAY_STATUS = {}
ENABLED_RELAYS = {}
LAST_MOTION_TIMES = {}
ENABLED_RELAYS = {}
MESH_STATUS = {}  # { hubName: { addrHex: {campo: valore, "time": str} } }
MESH_LIST_MISSES = {}  # { hubName: { addrHex: int } } - manifest consecutivi senza vedere l'addr
MESH_LAMP_NAMES = {}  # { hubName: { addrHex: nome } } - da MESHCONFIG (vedi handle_mesh_config)
SENSOR_NAMES = {}  # { hubName: { slotId: nome } } - da SENSORCFG (vedi handle_sensor_config)
MESH_SENSOR_NAMES = {}  # { hubName: { addrHex: nome } } - da MESHSENSORCFG (vedi handle_mesh_sensor_config)
MESH_LIST_MISS_THRESHOLD = 3  # tolleranza a dump MESHLIST incompleti prima di considerare la lampada fantasma
CONFIGREQ_RESEND_SEC = 120  # vedi maybe_resend_configreq()

# Override manuale per lampada: { (hub, addr_upper): datetime_scadenza }
# Popolato da MESHOVERRIDE (companion button rilevato dall'ESP32).
# Svuotato automaticamente alla scadenza; l'automazione lo controlla
# prima di inviare MESHCMD per saltare le lampade in override manuale.
MANUAL_OVERRIDE_UNTIL = {}

# Hold provvisorio per lampada: { (hub, addr_upper): datetime_scadenza }
# Scatta quando arriva un MESH con stato inatteso (companion ha agito)
# prima che MESHOVERRIDE raggiunga il server via cellulare (latenza 2-4s).
# Dura MESH_PROVISIONAL_HOLD_SEC: tempo sufficiente al MESHOVERRIDE di
# arrivare e prendere il controllo. Se MESHOVERRIDE non arriva (falso
# allarme), l'automazione riprende automaticamente alla scadenza.
MESH_PROVISIONAL_OVERRIDE = {}
MESH_PROVISIONAL_HOLD_SEC = 8  # > latenza cellulare tipica (2-4s)

# Ultimo stato comandato dall'automazione per lampada (non dalla UI manuale).
# Serve a distinguere uno stato "attesi" da uno "inatteso" per il provisional hold.
LAST_AUTO_MESH_STATE = {}  # { (hub, addr_upper): {"onoff": "0"|"1", "pct": str} }

# Override a livello hub: { hub: datetime_scadenza }
# Quando arriva MESHOVERRIDE per qualsiasi lampada di un hub, tutta
# l'automazione di quel hub viene sospesa per la stessa durata dell'override.
# Risolve il problema delle MESHOVERRIDE scaglionate via cellulare: la prima
# che arriva blocca subito tutto il hub, le successive aggiornano solo il
# timer per-lampada senza che l'automazione abbia già re-inviato ON.
HUB_MESH_OVERRIDE_UNTIL = {}  # in-memory, non persistito su disco

# Badge companion: { hub: datetime_scadenza } — come HUB_MESH_OVERRIDE_UNTIL ma impostato
# SOLO quando il cambio di stato è significativo (onoff cambia, o pct varia > 10 punti).
# Filtra i falsi positivi da arrotondamento DALI (mismatch di 1-2% che fanno scattare
# MESHOVERRIDE senza che nessun companion sia stato premuto).
COMPANION_BADGE_UNTIL = {}


# --- TEMPLATE HTML ---
HTML_TEMPLATE = """
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{% if current_area == 'TUTTE' %}Panoramica{% else %}{{ current_area }}{% endif %} · Monitor</title>
  
  <script>
    // Applica subito il tema per evitare il flash bianco/scuro
    if (localStorage.getItem('theme') === 'light') { document.documentElement.classList.add('light-mode'); }
  </script>

  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.min.js"></script>
  <script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3.0.0/dist/chartjs-adapter-date-fns.bundle.min.js"></script>
  <!-- chartjs-plugin-zoom usa Hammer.js per il pan a trascinamento (mouse e touch) -
       il suo bundle UMD lo legge da window.Hammer al momento del caricamento (vedi
       "e.Hammer" nella factory): senza questo script lo zoom con la rotellina
       funziona comunque (non serve Hammer), ma il click+drag per il pan non e' mai
       partito, silenziosamente, perche' il gesture recognizer non esiste. -->
  <script src="https://cdn.jsdelivr.net/npm/hammerjs@2.0.8/hammer.min.js"></script>
  <script src="https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.0.1/dist/chartjs-plugin-zoom.min.js"></script>

  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    /* I controlli di form/bottone NON erediscono il font del body di default
       nei browser (usano il font di sistema nativo) - senza questa riga si
       vedevano caratteri visibilmente diversi tra testo normale e
       button/input/select/textarea sulla stessa pagina. */
    button, input, select, textarea { font-family: inherit; }

    /* Stessa palette/forme della pagina web dell'ESP (main.c root_get_handler):
       --bg/--surface/--surface2/--border/--border2/--accent/--accent-fg/--green/
       --red/--yellow/--text/--muted/--radius/--shadow, qui con accento blu/azzurro
       invece del teal usato la'. */
    :root {
      --bg: #10151b; --surface: #19212b; --surface2: #212c38; --border: #2c3a48; --border2: #3a4b5c;
      --accent: #3da9f5; --accent-fg: #07131f;
      --green: #3fcf8e; --red: #ff6b6b; --yellow: #f2b34a; --text: #e7eef5; --muted: #90a0b0;
      --radius: 14px; --shadow: 0 1px 2px rgba(0,0,0,.4);
      --topbar-bg: rgba(16,21,27,.78); --glass-border: var(--border);
      --card-border: var(--border); --btn-bg: var(--surface); --btn-hover: var(--surface2);
      --tag-bg: var(--surface2); --relay-off-dot: var(--border2); --relay-hover-border: var(--border2);
      --accent-soft: rgba(61,169,245,.14); --shadow-hover: var(--shadow);
      --card-shadow: 0 6px 16px -8px rgba(0,0,0,.55);
    }

    :root.light-mode {
      --bg: #eef2f6; --surface: #fff; --surface2: #f4f7fa; --border: #dde4ea; --border2: #c6d0da;
      --accent: #0d72b9; --accent-fg: #fff;
      --green: #1f9d57; --red: #d83a3a; --yellow: #b07a12; --text: #16202b; --muted: #5d6b78;
      --shadow: 0 1px 2px rgba(16,32,48,.06);
      --topbar-bg: rgba(255,255,255,.82); --glass-border: var(--border);
      --card-border: var(--border); --btn-bg: var(--surface); --btn-hover: var(--surface2);
      --tag-bg: var(--surface2); --relay-off-dot: var(--border2); --relay-hover-border: var(--border2);
      --accent-soft: rgba(13,114,185,.10); --shadow-hover: var(--shadow);
      --card-shadow: 0 6px 16px -8px rgba(16,32,48,.18);
    }

    ::-webkit-scrollbar { width: 8px; height: 8px; }
    ::-webkit-scrollbar-track { background: var(--bg); }
    ::-webkit-scrollbar-thumb { background: var(--border); border-radius: 4px; }
    ::-webkit-scrollbar-thumb:hover { background: var(--muted); }

    body { font-family: system-ui,-apple-system,Segoe UI,Roboto,sans-serif; background: var(--bg); color: var(--text); min-height: 100vh; line-height: 1.45; -webkit-font-smoothing: antialiased; transition: background 0.3s, color 0.3s; font-feature-settings: "tnum"; }
    main { padding: 32px 24px; max-width: 1400px; margin: 0 auto; }
    
    .topbar { 
      background: var(--topbar-bg); backdrop-filter: blur(12px); -webkit-backdrop-filter: blur(12px);
      border-bottom: 1px solid var(--glass-border); 
      padding: 0 24px; display: flex; align-items: center; gap: 12px; height: 64px; 
      position: sticky; top: 0; z-index: 100; transition: background 0.3s, border-color 0.3s;
    }
    .topbar .logo { font-size: 14px; font-weight: 800; letter-spacing: .14em; text-transform: uppercase; color: var(--text); margin-right: 16px; display: inline-flex; align-items: center; gap: 9px; }
    .topbar .logo::before { content: ''; width: 11px; height: 11px; border-radius: 3px; background: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); }
    .topbar nav { display: flex; align-items: center; gap: 8px; overflow-x: auto; flex: 1; scrollbar-width: none; }
    .topbar nav::-webkit-scrollbar { display: none; }

    .navbtn, .btn-back { display: inline-flex; align-items: center; gap: 8px; text-decoration: none; font-size: .83rem; font-weight: 600; color: var(--text); padding: 7px 13px; border-radius: 9px; transition: border-color .12s,color .12s; border: 1px solid var(--border); background: var(--surface); white-space: nowrap; }
    .navbtn:hover, .btn-back:hover { border-color: var(--accent); color: var(--accent); }
    .navbtn.active { border-color: var(--accent); color: var(--accent); background: var(--accent-soft); }

    #area-select { max-width: 200px; font-size: .83rem; font-weight: 600; color: var(--text); background: var(--surface); border: 1px solid var(--border); border-radius: 9px; padding: 7px 11px; cursor: pointer; transition: border-color .12s; }
    #area-select:hover { border-color: var(--border2); }
    #area-select:focus { outline: none; border-color: var(--accent); }

    .sync-container { display: flex; align-items: center; gap: 12px; margin-left: auto; background: var(--tag-bg); padding: 6px 14px; border-radius: 20px; border: 1px solid var(--card-border); }
    .sync-ip { font-size: 12px; font-weight: 600; color: var(--accent); font-variant-numeric: tabular-nums; }
    .sync-time { font-size: 12px; color: var(--muted); font-variant-numeric: tabular-nums; }
    .live-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--green); box-shadow: 0 0 0 0 rgba(53,192,138,.6); animation: pulse 2s infinite; }
    @keyframes pulse { 0% { box-shadow: 0 0 0 0 rgba(53,192,138,.5); } 70% { box-shadow: 0 0 0 6px rgba(53,192,138,0); } 100% { box-shadow: 0 0 0 0 rgba(53,192,138,0); } }
    
    .theme-btn { background: transparent; border: 1px solid var(--border); border-radius: 50%; width: 32px; height: 32px; display: flex; align-items: center; justify-content: center; color: var(--muted); cursor: pointer; transition: all 0.2s; margin-left: 8px; }
    .theme-btn:hover { color: var(--text); background: var(--tag-bg); border-color: var(--muted); }

    .section-title { font-size: 12px; font-weight: 700; text-transform: uppercase; letter-spacing: .12em; color: var(--muted); margin-bottom: 16px; margin-top: 36px; display: flex; align-items: center; gap: 12px; }
    .section-title::after { content: ''; flex: 1; height: 1px; background: linear-gradient(90deg, var(--border) 0%, transparent 100%); }
    .section-title:first-child { margin-top: 0; }
    
    .esp-relay-container { display: flex; flex-direction: column; gap: 16px; margin-bottom: 32px; }
    .esp-box { background: var(--surface); border: 1px solid var(--card-border); border-radius: var(--radius); padding: 20px; box-shadow: var(--shadow); transition: background 0.3s, border-color 0.3s; }
    .esp-title { font-size: 14px; font-weight: 600; color: var(--text); margin-bottom: 16px; display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
    .relay-grid { display: flex; gap: 12px; flex-wrap: wrap; }

    .mesh-divider { flex-basis: 100%; display: flex; align-items: center; gap: 10px; margin: 4px 0; }
    .mesh-divider span { font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: .1em; color: var(--muted); white-space: nowrap; }
    .mesh-divider::before, .mesh-divider::after { content: ''; flex: 1; height: 1px; background: var(--border); }

    /* Switch automatico/manuale per stanza */
    .mode-switch { display: flex; align-items: center; gap: 8px; cursor: pointer; font-size: 11px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: .05em; }
    .mode-switch input { display: none; }
    .mode-track { width: 32px; height: 18px; border-radius: 999px; background: var(--relay-off-dot); position: relative; transition: background .25s ease; flex-shrink: 0; }
    .mode-track::after { content: ''; position: absolute; top: 2px; left: 2px; width: 14px; height: 14px; border-radius: 50%; background: #fff; box-shadow: 0 1px 2px rgba(0,0,0,.35); transition: transform .25s cubic-bezier(0.4, 0, 0.2, 1); }
    .mode-switch input:checked + .mode-track { background: var(--accent); }
    .mode-switch input:checked + .mode-track::after { transform: translateX(14px); }

    .relay-btn:disabled, .mesh-slider:disabled { opacity: .4; cursor: not-allowed; }
    .relay-btn:disabled:hover { transform: none; background: var(--btn-bg); }

    /* Switch in stile pillola: .dot e' il binario, ::after e' la manopola */
    .relay-btn { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 7px 12px; border-radius: 9px; border: 1px solid var(--border); background: var(--surface2); cursor: pointer; font-size: .86rem; font-weight: 600; color: var(--text); transition: border-color .12s,color .12s; flex: 1; min-width: 170px; font-family: inherit; }
    .relay-btn .btn-label { order: 1; }
    .relay-btn .dot { order: 2; flex-shrink: 0; width: 38px; height: 22px; border-radius: 999px; background: var(--border2); position: relative; transition: background .2s ease; }
    .relay-btn .dot::after { content: ''; position: absolute; top: 2px; left: 2px; width: 18px; height: 18px; border-radius: 50%; background: var(--surface); box-shadow: var(--shadow); transition: transform .2s ease; }
    .relay-btn.on { border-color: var(--accent); }
    .relay-btn.on .dot { background: var(--green); }
    .relay-btn.on .dot::after { transform: translateX(16px); }
    .relay-btn:hover { border-color: var(--accent); color: var(--accent); }

    /* Un'unica box di sfondo per tutta la lampada: switch on/off, slider e
       dati potenza/energia ci stanno dentro insieme, non solo l'on/off. */
    .mesh-lamp-card { background: var(--surface2); border: 1px solid var(--border); border-radius: 9px; padding: 10px 12px; display: flex; flex-direction: column; gap: 8px; flex: 1; min-width: 170px; }
    .mesh-lamp-card .relay-btn { background: transparent; border: none; padding: 0; min-width: 0; }
    .mesh-slider { -webkit-appearance: none; appearance: none; width: 100%; height: 8px; border-radius: 999px; margin: 4px 0 2px; background: linear-gradient(90deg,var(--accent) var(--p,40%),var(--border) var(--p,40%)); cursor: pointer; outline: none; }
    .mesh-slider::-webkit-slider-thumb { -webkit-appearance: none; width: 20px; height: 20px; border-radius: 50%; background: var(--surface); border: 2px solid var(--accent); cursor: pointer; box-shadow: var(--shadow); }
    .mesh-slider::-moz-range-thumb { width: 18px; height: 18px; border-radius: 50%; background: var(--surface); border: 2px solid var(--accent); cursor: pointer; }
    .mesh-lamp-power { font-size: 11px; color: var(--muted); }
    .companion-badge { display: none; align-items: center; gap: 5px; font-size: 10.5px; font-weight: 700; color: #fff; background: #d97706; border-radius: 999px; padding: 2px 9px; width: fit-content; }
    .companion-badge.active { display: inline-flex; }

    .cards-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: 16px; }
    /* Card sempre quadrate: niente piu' barra colorata laterale, lo stato si
       legge dal contorno (border-color) + ombra, non da un accento decorativo. */
    .card { background: var(--surface2); border: 1px solid var(--border); border-radius: var(--radius); padding: 14px; position: relative; overflow: hidden; box-shadow: var(--card-shadow); transition: border-color .2s; aspect-ratio: 1 / 1; display: flex; flex-direction: column; }
    .card:hover { border-color: var(--accent); }
    /* Bordo sempre uguale per tutte le card (pir o non pir): lo stato si legge
       solo dal testo/valore, non dal contorno. */
    .card.pir-detected .card-value { color: var(--red); }
    .card.pir-clear .card-value { color: var(--green); }

    /* Due badge per card: tipo sensore (Mesh/BLE, a sinistra) + nome (a destra) */
    .sensor-badges { display: flex; align-items: center; gap: 6px; margin-bottom: 10px; }
    .dot-badge { display: inline-flex; align-items: center; font-size: .64rem; font-weight: 700; padding: 3px 8px; border-radius: 999px; max-width: 100%; overflow: hidden; }
    .dot-badge.kind-mesh { background: #3b66ad; color: #eaf2ff; }
    .dot-badge.kind-ble { background: #2f8fe0; color: #fff; }
    .dot-badge.name-badge { margin-left: auto; color: var(--muted); background: var(--tag-bg); white-space: nowrap; text-overflow: ellipsis; overflow: hidden; }

    .stat-row { display: flex; gap: 14px; flex-wrap: nowrap; margin-bottom: 4px; min-width: 0; }
    .stat-block { display: flex; flex-direction: column; min-width: 0; overflow: hidden; }
    .stat-label { font-size: 9px; font-weight: 700; text-transform: uppercase; letter-spacing: .06em; color: var(--muted); margin-bottom: 1px; }
    .card-value, .card-lux { font-size: 22px; font-weight: 600; color: var(--text); line-height: 1.05; margin-bottom: 4px; letter-spacing: -0.01em; font-variant-numeric: tabular-nums; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .card-sub { font-size: 11px; color: var(--muted); line-height: 1.4; font-weight: 500; }
    /* Box riassuntivo: ultimo aggiornamento + media/moda, compatto */
    .card-stats-box {
        margin-top: auto;
        background: var(--tag-bg);
        border: 1px solid var(--border);
        border-radius: 8px;
        padding: 6px 8px;
        font-size: 9px;
        color: var(--muted);
        line-height: 1.4;
        font-variant-numeric: tabular-nums;
        display: flex;
        flex-direction: column;
        gap: 2px;
        overflow: hidden;
    }
    .card-stats-box div { margin: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .card-stats-box .stats-row {
        display: flex;
        flex-direction: row;
        gap: 8px;
        flex-wrap: nowrap;
        overflow: hidden;
    }
    .card-stats-box .stats-row span {
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
        flex: 1;
        min-width: 0;
    }
    .card-stats-box strong { color: var(--text); font-weight: 700; }
    
    .chart-box { background: var(--surface); border: 1px solid var(--card-border); border-radius: var(--radius); overflow: hidden; margin-top: 16px; box-shadow: var(--shadow); transition: all 0.3s; padding: 8px; }
    .chart-box:hover { box-shadow: var(--shadow-hover); border-color: var(--border); }
    .chart-box img { width: 100%; display: block; border-radius: 6px; }

    .charts-grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
    .chart-card { background: var(--surface); border: 1px solid var(--card-border); border-radius: var(--radius); padding: 16px; box-shadow: var(--shadow); transition: all 0.3s; width: 100%; }
    .chart-card:hover { box-shadow: var(--shadow-hover); border-color: var(--border); }
    .chart-reset { background: transparent; border: 1px solid var(--border); color: var(--muted); font-size: 10px; padding: 3px 9px; border-radius: 10px; cursor: pointer; text-transform: none; letter-spacing: 0; font-family: inherit; transition: all .2s; }
    .chart-reset:hover { color: var(--text); border-color: var(--accent); }
    .chart-card canvas { max-height: 320px; width: 100% !important; cursor: grab; }
    .chart-card canvas:active { cursor: grabbing; }
    .chart-empty { font-size: 12px; color: var(--muted); font-style: italic; padding: 80px 0; text-align: center; }

    /* Nuovi stili per il box della stanza */
    .room-container { background: var(--btn-bg); border: 1px solid var(--card-border); border-radius: var(--radius); padding: 20px; margin-bottom: 24px; }

  </style>
</head>
<body>

<div class="topbar">
  <span class="logo">{{ nome_edificio }}</span>
  <nav>
      <a href="/tutte" class="navbtn {{ 'active' if current_area == 'TUTTE' else '' }}">Edificio</a>
      <select id="area-select" onchange="if(this.value)location.href=this.value;">
        {% if current_area == 'TUTTE' %}
          <option value="" selected hidden>Aree</option>
        {% endif %}
        {% for area in lista_aree %}
          <option value="/area/{{ area }}" {{ 'selected' if current_area == area else '' }}>{{ area }}</option>
        {% endfor %}
      </select>
  </nav>
  <div class="sync-container">
    <span class="live-dot"></span>
    <span class="sync-ip">{{ local_ip }}</span>
    <span style="color: var(--border);">|</span>
    <span class="sync-time" id="sync-label">—</span>
  </div>

  <button class="theme-btn" onclick="toggleTheme()" title="Cambia Tema">
    <svg id="moon-icon" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"></path></svg>
    <svg id="sun-icon" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="display:none;"><circle cx="12" cy="12" r="5"></circle><line x1="12" y1="1" x2="12" y2="3"></line><line x1="12" y1="21" x2="12" y2="23"></line><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"></line><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"></line><line x1="1" y1="12" x2="3" y2="12"></line><line x1="21" y1="12" x2="23" y2="12"></line><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"></line><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"></line></svg>
  </button>

  <a href="/config" class="theme-btn" title="Impostazioni" style="text-decoration: none;">
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"></circle><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"></path></svg>
  </a>
</div>

<main>
  {% if current_area != 'TUTTE' %}
    <div class="section-title">
      Comandi Manuali · {{ current_area }}
    </div>
    <div class="esp-relay-container">
      {% for esp_name, grp in manual_grouped.items() %}
        <div class="esp-box" data-esp-box="{{ esp_name }}">
          <div class="esp-title"><span>{{ esp_name }}</span></div>

          {% if grp.relays %}
          <div class="mesh-divider">
            <span>Relè · Modalità</span>
            <label class="mode-switch">
              <input type="checkbox" data-tipo="rele" {{ 'checked' if grp.modalita_rele == 'manuale' else '' }} onchange="toggleEspMode(this, '{{ esp_name }}', 'rele')">
              <span class="mode-track"></span>
              <span class="mode-text">{{ 'Manuale' if grp.modalita_rele == 'manuale' else 'Automatico' }}</span>
            </label>
          </div>
          <div class="relay-grid">
            {% for r in grp.relays %}
              <button class="relay-btn {{ 'on' if r.is_on else 'off' }}" {{ 'disabled' if grp.modalita_rele != 'manuale' else '' }} data-esp="{{ r.esp }}" data-id="{{ r.id }}" data-tipo="rele" onclick="toggleRelay(this)">
                <span class="btn-label">{{ r.label }}</span>
                <div class="dot"></div>
              </button>
            {% endfor %}
          </div>
          {% endif %}

          {% if grp.lamps %}
          <div class="mesh-divider">
            <span>Mesh BLE · Modalità</span>
            <label class="mode-switch">
              <input type="checkbox" data-tipo="mesh" {{ 'checked' if grp.modalita_mesh == 'manuale' else '' }} onchange="toggleEspMode(this, '{{ esp_name }}', 'mesh')">
              <span class="mode-track"></span>
              <span class="mode-text">{{ 'Manuale' if grp.modalita_mesh == 'manuale' else 'Automatico' }}</span>
            </label>
            <div class="companion-badge{{ ' active' if grp.companion_override_until else '' }}" data-esp="{{ esp_name }}" data-companion-until="{{ grp.companion_override_until }}"></div>
          </div>
          <div class="relay-grid" data-mesh-trans-ms="{{ area_mesh_trans_ms }}" data-mesh-delay-ms="{{ area_mesh_delay_ms }}">
            {% for l in grp.lamps %}
              <div class="mesh-lamp-card">
                <button class="relay-btn {{ 'on' if l.is_on else 'off' }}" {{ 'disabled' if grp.modalita_mesh != 'manuale' else '' }} data-esp="{{ l.esp }}" data-addr="{{ l.addr }}" data-tipo="mesh" onclick="toggleMeshLamp(this)">
                  <span class="btn-label">{{ l.name }}</span>
                  <div class="dot"></div>
                </button>
                {% if l.pct is not none %}
                  <input type="range" class="mesh-slider" min="0" max="100" value="{{ l.pct }}" style="--p:{{ l.pct }}%" {{ 'disabled' if grp.modalita_mesh != 'manuale' else '' }} data-esp="{{ l.esp }}" data-addr="{{ l.addr }}" data-tipo="mesh" oninput="this.style.setProperty('--p', this.value+'%')" onchange="setMeshPct(this)">
                {% endif %}
                {% if l.power is not none %}
                  <div class="mesh-lamp-power">{{ l.power }} W &middot; {{ l.energy if l.energy is not none else "?" }} kWh</div>
                {% endif %}
              </div>
            {% endfor %}
          </div>
          {% endif %}
        </div>
      {% else %}
        <p style="color:var(--muted); font-size:13px; font-style: italic;">Nessun dispositivo manuale assegnato a quest'area.</p>
      {% endfor %}
    </div>
  {% endif %}

  <div class="section-title">
    Sensori Rilevati
  </div>
  <div id="cards-box">
    {% for group in sensors|groupby('room') %}
      <div class="room-container">
        <div class="mesh-divider"><span>{{ group.grouper }}</span></div>
        <div class="cards-grid">
          {% for s in group.list %}
            <div class="card {{ s.card_class }}">
              <div class="sensor-badges">
                <span class="dot-badge {{ 'kind-mesh' if s.is_mesh else 'kind-ble' }}">{{ 'Mesh' if s.is_mesh else 'BLE' }}</span>
                <span class="dot-badge name-badge">{{ s.name if s.name else 'Sensore S' + s.sensor_id }}</span>
              </div>
              <div class="stat-row">
                <div class="stat-block">
                  {% if s.value_label %}<div class="stat-label">{{ s.value_label }}</div>{% endif %}
                  <div class="card-value">{{ s.main_value }}</div>
                </div>
                {% if s.secondary_value %}
                <div class="stat-block">
                  <div class="stat-label">{{ s.secondary_label }}</div>
                  <div class="card-value">{{ s.secondary_value }}</div>
                </div>
                {% endif %}
              </div>
              {% if s.lux_value %}<div class="card-lux">{{ s.lux_value }}</div>{% endif %}
              <div class="card-sub">{% for sv in s.sub_values %}{{ sv }}<br>{% endfor %}</div>
              <div class="card-stats-box">
                  <div>Ultimo dato delle: <strong>{{ s.time }}</strong></div>
                  {% if s.updatestats %}{{ s.updatestats | safe }}{% endif %}
              </div>
            </div>
          {% endfor %}
        </div>
      </div>
    {% else %}
      <p style="color:var(--muted); font-size:13px" id="empty-msg">Nessun dato in memoria per quest'area. Attendo MQTT...</p>
    {% endfor %}
  </div>

  <div class="section-title" style="margin-top:40px">
    Analisi Dati · {{ current_area }} <span style="font-weight:400; text-transform:none; letter-spacing:0; font-size:11px; color:var(--muted)">(rotellina = zoom, click+drag = sposta)</span>
  </div>
  <div class="charts-grid">
    <div class="chart-card">
      <div class="esp-title">Temperatura (°C) <button class="chart-reset" onclick="resetChartZoom('temp')">Reset zoom</button></div>
      <canvas id="chart-temp"></canvas>
    </div>
    <div class="chart-card">
      <div class="esp-title">Umidità (%) <button class="chart-reset" onclick="resetChartZoom('hum')">Reset zoom</button></div>
      <canvas id="chart-hum"></canvas>
    </div>
    <div class="chart-card">
      <div class="esp-title">PIR · Passaggi cumulativi <button class="chart-reset" onclick="resetChartZoom('pir')">Reset zoom</button></div>
      <canvas id="chart-pir"></canvas>
    </div>
    <div class="chart-card">
      <div class="esp-title">Luce Ambientale (lux) <button class="chart-reset" onclick="resetChartZoom('lux')">Reset zoom</button></div>
      <canvas id="chart-lux"></canvas>
    </div>
  </div>
</main>

<script>
  const currentArea = "{{ current_area }}";

  // --- GESTIONE TEMA ---
  function getTheme() {
    return document.documentElement.classList.contains('light-mode') ? 'light' : 'dark';
  }

  function toggleTheme() {
    const isLight = document.documentElement.classList.toggle('light-mode');
    localStorage.setItem('theme', isLight ? 'light' : 'dark');
    updateThemeIcon();
    rebuildCharts(); // ricolora i grafici sul nuovo tema
  }

  function updateThemeIcon() {
    const isLight = document.documentElement.classList.contains('light-mode');
    document.getElementById('moon-icon').style.display = isLight ? 'none' : 'block';
    document.getElementById('sun-icon').style.display = isLight ? 'block' : 'none';
  }

  // --- GRAFICI INTERATTIVI (Chart.js): hover sul singolo punto, zoom con la
  // rotellina, pan con click+drag. Aggiornati via JSON da /api/chart/<area>
  // invece del vecchio PNG statico generato da matplotlib. ---
  // Registrazione esplicita: l'auto-registrazione UMD del plugin zoom non
  // sempre scatta in tempo con Chart.js v4 caricato da CDN - senza questa
  // riga lo zoom con la rotellina puo' funzionare (gestito anche altrove)
  // ma il pan con click+drag resta silenziosamente disattivato.
  if (window.Chart && window.ChartZoom) {
    try { Chart.register(window.ChartZoom); } catch (e) {}
  }
  // Chart.js usa di default un font suo (non eredita quello della pagina):
  // senza questo, legenda/assi dei grafici risultavano visibilmente diversi
  // dal resto del testo (card sensori, relay, lampade).
  if (window.Chart) {
    Chart.defaults.font.family = "system-ui,-apple-system,Segoe UI,Roboto,sans-serif";
    Chart.defaults.font.size = 11;
  }

  const CHART_KEYS = ['temp', 'hum', 'pir', 'lux'];
  const chartInstances = {};
  const palette = ['#4d8dff', '#35c08a', '#ff5d5d', '#e0a82e', '#a78bfa', '#22d3ee', '#f472b6', '#fb923c'];

  function chartColors() {
    const light = getTheme() === 'light';
    return {
      text: light ? '#16202e' : '#e6edf6',
      muted: light ? '#5d6b80' : '#8a96a8',
      grid: light ? '#e2e7f0' : '#222b3a',
    };
  }

  function makeChart(key, stepped) {
    const canvas = document.getElementById('chart-' + key);
    if (!canvas) return null;
    const c = chartColors();
    return new Chart(canvas.getContext('2d'), {
      type: 'line',
      data: { datasets: [] },
      options: {
        responsive: true,
        animation: false,
        interaction: { mode: 'nearest', intersect: true },
        stepped: stepped ? 'after' : false,
        plugins: {
          legend: { labels: { color: c.text, font: { size: 11 } } },
          tooltip: {
            callbacks: {
              title: (items) => items.length ? new Date(items[0].parsed.x).toLocaleString('it-IT') : ''
            }
          },
          zoom: {
            pan: { enabled: true, mode: 'x' },
            zoom: { wheel: { enabled: true }, pinch: { enabled: true }, mode: 'x' },
            limits: { x: { minRange: 60 * 1000 } }
          }
        },
        scales: {
          x: { type: 'time', time: { tooltipFormat: 'HH:mm:ss' }, ticks: { color: c.muted }, grid: { color: c.grid } },
          y: { ticks: { color: c.muted }, grid: { color: c.grid }, beginAtZero: key === 'pir' }
        },
        elements: { point: { radius: 3, hoverRadius: 6 }, line: { tension: 0.15, borderWidth: 2 } }
      }
    });
  }

  function resetChartZoom(key) {
    if (chartInstances[key]) chartInstances[key].resetZoom();
  }

  function rebuildCharts() {
    CHART_KEYS.forEach(key => {
      if (chartInstances[key]) { chartInstances[key].destroy(); chartInstances[key] = null; }
      chartInstances[key] = makeChart(key, key === 'pir');
    });
    loadChartsData();
  }

  function loadChartsData() {
    fetch('/api/chart/' + encodeURIComponent(currentArea))
      .then(r => r.json())
      .then(data => {
        CHART_KEYS.forEach(key => {
          const chart = chartInstances[key];
          if (!chart) return;
          const series = data[key] || [];
          chart.data.datasets = series.map((s, i) => ({
            label: s.label,
            data: s.data,
            borderColor: palette[i % palette.length],
            backgroundColor: palette[i % palette.length],
            spanGaps: true,
          }));
          chart.update('none'); // 'none' = niente animazione, non disturba uno zoom/pan in corso
        });
      })
      .catch(() => {});
  }

  document.addEventListener('DOMContentLoaded', () => {
    updateThemeIcon();
    rebuildCharts();
  });

  // --- GESTIONE RELE E AGGIORNAMENTI ---
  function toggleRelay(btn) {
    const esp = btn.getAttribute('data-esp');
    const n = btn.getAttribute('data-id');
    if(!esp || !n) return;
    
    const isCurrentlyOn = btn.classList.contains('on');
    const newState = !isCurrentlyOn;
    btn.className = 'relay-btn ' + (newState ? 'on' : 'off');

    fetch('/api/relay', {
      method: 'POST', 
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ esp: esp, command: `relay:${n}:${newState ? 'ON' : 'OFF'}` })
    }).then(r => r.json()).then(data => {
      if (!data.ok) btn.className = 'relay-btn ' + (isCurrentlyOn ? 'on' : 'off');
    }).catch(err => {
      btn.className = 'relay-btn ' + (isCurrentlyOn ? 'on' : 'off');
    });
  }

  // Il client mesh dell'ESP32 accetta una sola richiesta in volo per
  // destinazione: un MESHCMD sparato mentre il radio e' occupato (polling,
  // un altro comando) puo' andare perso senza nessun errore visibile.
  // Invece di un fire-and-forget, si ripete il comando ogni MESH_RETRY_MS
  // finche' /api/mesh/<hub> non confema che la lampada ha raggiunto lo
  // stato richiesto (o finche' non si esauriscono i tentativi).
  const MESH_RETRY_MS = 1500;
  const MESH_MAX_ATTEMPTS = 15; // ~22s di tentativi prima di rinunciare
  const meshRetryTimers = {};
  const companionUntil = {}; // { "esp|addr": timestamp_secondi } — timer locale override companion

  function sendMeshCmdUntilConfirmed(esp, addr, op, val, checkVal, btn, onGiveUp) {
    const key = esp + '|' + addr + '|' + op;
    if (meshRetryTimers[key]) { clearInterval(meshRetryTimers[key]); delete meshRetryTimers[key]; }

    let attempts = 0;
    function send() {
      fetch('/api/mesh', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ hub: esp, addr: addr, op: op, val: val })
      }).catch(() => {});
    }
    function stop() {
      clearInterval(meshRetryTimers[key]);
      delete meshRetryTimers[key];
    }
    function check() {
      fetch('/api/mesh/' + encodeURIComponent(esp)).then(r => r.json()).then(st => {
        const kv = st[addr];
        const confirmed = kv && (op === 'onoff' ? kv.onoff === checkVal : String(kv.pct) === checkVal);
        if (confirmed) { stop(); return; }
        attempts++;
        if (attempts >= MESH_MAX_ATTEMPTS) {
          stop();
          console.warn('Comando mesh non confermato dopo ' + attempts + ' tentativi:', esp, addr, op, val);
          if (onGiveUp) onGiveUp();
          return;
        }
        send();
      }).catch(() => {});
    }
    send();
    meshRetryTimers[key] = setInterval(check, MESH_RETRY_MS);
  }

  function toggleMeshLamp(btn) {
    const esp = btn.getAttribute('data-esp');
    const addr = btn.getAttribute('data-addr');
    if(!esp || !addr) return;

    const isCurrentlyOn = btn.classList.contains('on');
    const newState = !isCurrentlyOn;
    btn.className = 'relay-btn ' + (newState ? 'on' : 'off');

    sendMeshCmdUntilConfirmed(esp, addr, 'onoff', newState ? '1' : '0', newState ? '1' : '0', btn,
      () => { btn.className = 'relay-btn ' + (isCurrentlyOn ? 'on' : 'off'); });
  }

  function setMeshPct(input) {
    const esp = input.getAttribute('data-esp');
    const addr = input.getAttribute('data-addr');
    if(!esp || !addr) return;

    // trans/delay: impostazione GLOBALE per area (Impostazioni > Aree), non
    // piu' per singola lampada - letta dal data-* sul contenitore .relay-grid
    // (vedi area_mesh_trans_ms/area_mesh_delay_ms lato Python). Inviati come
    // "pct,trans_ms,delay_ms"; il firmware accetta anche il solo pct (0,0).
    const grid = input.closest('.relay-grid');
    const transMs = grid ? (parseInt(grid.getAttribute('data-mesh-trans-ms')) || 0) : 0;
    const delayMs = grid ? (parseInt(grid.getAttribute('data-mesh-delay-ms')) || 0) : 0;
    const pct = input.value;
    const val = (transMs || delayMs) ? `${pct},${transMs},${delayMs}` : pct;

    sendMeshCmdUntilConfirmed(esp, addr, 'pct', val, pct, input);
  }

  function applyEspMode(box, tipo, modalita) {
    const isManuale = modalita === 'manuale';
    box.querySelectorAll(`[data-tipo="${tipo}"]`).forEach(el => {
      if (el.matches('.relay-btn, .mesh-slider')) el.disabled = !isManuale;
    });
    const checkbox = box.querySelector(`.mode-switch input[data-tipo="${tipo}"]`);
    const label = tipo === 'rele' ? 'Relè' : 'Mesh';
    if (checkbox) {
      checkbox.checked = isManuale;
      const text = checkbox.closest('.mode-switch').querySelector('.mode-text');
      if (text) text.textContent = label + ': ' + (isManuale ? 'Manuale' : 'Automatico');
    }
  }

  function toggleEspMode(checkbox, esp, tipo) {
    const modalita = checkbox.checked ? 'manuale' : 'auto';
    const box = checkbox.closest('.esp-box');
    if (box) applyEspMode(box, tipo, modalita);

    fetch('/api/esp_mode', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ esp: esp, tipo: tipo, modalita: modalita })
    }).catch(err => {});
  }

  function updateDashboard() {
    fetch('/api/data/' + encodeURIComponent(currentArea))
      .then(r => r.json())
      .then(data => {
        const box = document.getElementById('cards-box');
        const sensorData = data.sensors || []; 
        
        if (sensorData.length === 0) {
          box.innerHTML = '<p style="color:var(--muted);font-size:13px" id="empty-msg">Nessun dato in memoria. Attendo MQTT...</p>';
        } else {
          // --- INIZIO NUOVA LOGICA DI RAGGRUPPAMENTO ---
          const grouped = {};
          
          // Raggruppo i sensori usando il nome della stanza come chiave
          sensorData.forEach(s => {
            if (!grouped[s.room]) grouped[s.room] = [];
            grouped[s.room].push(s);
          });

          let html = '';
          // Scorro le stanze trovate e creo i contenitori
          for (const [room, s_list] of Object.entries(grouped)) {
            html += `
              <div class="room-container">
                <div class="mesh-divider"><span>${room}</span></div>
                <div class="cards-grid">`;

            // Scorro i sensori di QUESTA stanza
            s_list.forEach(s => {
              const subs = (s.sub_values || []).join('<br>');
              const kindBadge = `<span class="dot-badge ${s.is_mesh ? 'kind-mesh' : 'kind-ble'}">${s.is_mesh ? 'Mesh' : 'BLE'}</span>`;
              const nameBadge = `<span class="dot-badge name-badge">${s.name ? s.name : 'Sensore S' + s.sensor_id}</span>`;
              const valueLabel = s.value_label ? `<div class="stat-label">${s.value_label}</div>` : '';
              const secondary = s.secondary_value ? `
                <div class="stat-block">
                  <div class="stat-label">${s.secondary_label}</div>
                  <div class="card-value">${s.secondary_value}</div>
                </div>` : '';
              const luxRow = s.lux_value ? `<div class="card-lux">${s.lux_value}</div>` : '';
              const updateStatsRow = s.updatestats ? s.updatestats : '';
              html += `
                <div class="card ${s.card_class}">
                  <div class="sensor-badges">${kindBadge}${nameBadge}</div>
                  <div class="stat-row">
                    <div class="stat-block">
                      ${valueLabel}
                      <div class="card-value">${s.main_value}</div>
                    </div>
                    ${secondary}
                  </div>
                  ${luxRow}
                  <div class="card-sub">${subs}</div>
                  <div class="card-stats-box"><div>Ultimo dato delle: <strong>${s.time}</strong>
                  </div>${updateStatsRow}</div>
                </div>`;
            });

            html += `</div></div>`; // Chiudo .cards-grid e .room-container
          }
          box.innerHTML = html;
          // --- FINE NUOVA LOGICA DI RAGGRUPPAMENTO ---
        }
        
        // LOGICA RELÈ INVARIATA
        if (data.relays) {
          document.querySelectorAll('.relay-btn[data-id]').forEach(btn => {
            const esp = btn.getAttribute('data-esp');
            const id = parseInt(btn.getAttribute('data-id'));
            if (data.relays[esp] && data.relays[esp].length >= id) {
                const isOn = (data.relays[esp][id-1] === '1');
                btn.className = 'relay-btn ' + (isOn ? 'on' : 'off');
            }
          });
        }

        // LAMPADE MESH: bottoni on/off + slider, non toccare lo slider se l'utente lo sta usando
        if (data.mesh_lamps) {
          document.querySelectorAll('[data-addr]').forEach(el => {
            const esp = el.getAttribute('data-esp');
            const addr = el.getAttribute('data-addr');
            const list = data.mesh_lamps[esp] || [];
            const found = list.find(l => l.addr === addr);
            if (!found) return;
            if (el.classList.contains('relay-btn')) {
              el.className = 'relay-btn ' + (found.is_on ? 'on' : 'off');
            } else if (el.classList.contains('mesh-slider') && document.activeElement !== el) {
              if (found.pct !== null && found.pct !== undefined) { el.value = found.pct; el.style.setProperty('--p', found.pct + '%'); }
            }
          });
          // Badge companion hub-level: prendi il timer più alto tra tutte le lampade del hub
          Object.entries(data.mesh_lamps).forEach(([esp, lamps]) => {
            const maxUntil = lamps.reduce((m, l) => Math.max(m, l.companion_override_until || 0), 0);
            if (maxUntil > 0) {
              companionUntil[esp] = Math.max(companionUntil[esp] || 0, maxUntil);
              // Cancella retry pendenti di sendMeshCmdUntilConfirmed per tutte le lampade del hub
              lamps.forEach(l => ['onoff', 'pct'].forEach(op => {
                const tk = esp + '|' + l.addr + '|' + op;
                if (meshRetryTimers[tk]) { clearInterval(meshRetryTimers[tk]); delete meshRetryTimers[tk]; }
              }));
            }
          });
        }

        // SWITCH AUTOMATICO/MANUALE: sincronizza se cambiato da un'altra sessione
        // (data.esp_modes[esp] = {rele: 'auto'|'manuale', mesh: 'auto'|'manuale'})
        if (data.esp_modes) {
          document.querySelectorAll('[data-esp-box]').forEach(box => {
            const esp = box.getAttribute('data-esp-box');
            const modes = data.esp_modes[esp];
            if (!modes) return;
            ['rele', 'mesh'].forEach(tipo => {
              const modalita = modes[tipo];
              if (!modalita) return;
              const checkbox = box.querySelector(`.mode-switch input[data-tipo="${tipo}"]`);
              if (checkbox && checkbox.checked === (modalita === 'manuale')) return; // gia' allineato
              applyEspMode(box, tipo, modalita);
            });
          });
        }
      });
    
    // RIMOSSO L'AGGIORNAMENTO DEL GRAFICO DA QUI!
    
    const now = new Date();
    document.getElementById('sync-label').textContent = now.toLocaleTimeString([], {hour: '2-digit', minute:'2-digit', second:'2-digit'});
  }

  // Inizializza timer companion dai dati server-side al caricamento pagina
  document.querySelectorAll('.companion-badge[data-companion-until]').forEach(el => {
    const until = parseFloat(el.getAttribute('data-companion-until') || '0');
    if (until > Date.now() / 1000) {
      companionUntil[el.getAttribute('data-esp')] = until;
    }
  });

  function fmtCountdown(sec) {
    const m = Math.floor(sec / 60), s = Math.floor(sec % 60);
    return '● ' + m + ':' + String(s).padStart(2, '0');
  }

  function refreshCompanionBadges() {
    const nowSec = Date.now() / 1000;
    document.querySelectorAll('.companion-badge[data-esp]').forEach(el => {
      const esp = el.getAttribute('data-esp');
      const until = companionUntil[esp] || 0;
      const remaining = until - nowSec;
      if (remaining > 0) {
        el.classList.add('active');
        el.textContent = fmtCountdown(remaining);
      } else {
        el.classList.remove('active');
        el.textContent = '';
        delete companionUntil[esp];
      }
    });
  }

  refreshCompanionBadges();
  setInterval(refreshCompanionBadges, 1000);

  // Aggiorna l'orologio e i box ogni 2 secondi
  setInterval(updateDashboard, 2000);
  
  // Aggiorna i dati dei grafici ogni 60 secondi (senza distruggere i chart,
  // cosi' un eventuale zoom/pan dell'utente non viene perso)
  setInterval(loadChartsData, 60000);
</script>

</body>
</html>
"""

CONFIG_TEMPLATE = """
<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Configurazione · Sistema</title>
  
  <script>
    if (localStorage.getItem('theme') === 'light') { document.documentElement.classList.add('light-mode'); }
  </script>

  <style>
    :root {
      --bg: #10151b; --surface: #19212b; --surface2: #212c38; --border: #2c3a48; --border2: #3a4b5c;
      --accent: #3da9f5; --accent-fg: #07131f;
      --green: #3fcf8e; --red: #ff6b6b; --yellow: #f2b34a; --text: #e7eef5; --muted: #90a0b0;
      --radius: 14px; --shadow: 0 1px 2px rgba(0,0,0,.4);
      --topbar-bg: rgba(16,21,27,.78); --glass-border: var(--border);
      --card-border: var(--border); --input-bg: var(--surface); --btn-bg: var(--surface); --accent-soft: rgba(61,169,245,.16);
    }

    :root.light-mode {
      --bg: #eef2f6; --surface: #fff; --surface2: #f4f7fa; --border: #dde4ea; --border2: #c6d0da;
      --accent: #0d72b9; --accent-fg: #fff;
      --green: #1f9d57; --red: #d83a3a; --yellow: #b07a12; --text: #16202b; --muted: #5d6b78;
      --shadow: 0 1px 2px rgba(16,32,48,.06);
      --topbar-bg: rgba(255,255,255,.82); --glass-border: var(--border);
      --card-border: var(--border); --input-bg: var(--surface); --btn-bg: var(--surface); --accent-soft: rgba(13,114,185,.12);
    }

    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    button, input, select, textarea { font-family: inherit; }
    ::-webkit-scrollbar { width: 8px; height: 8px; }
    ::-webkit-scrollbar-track { background: var(--bg); }
    ::-webkit-scrollbar-thumb { background: var(--border); border-radius: 4px; }

    body { font-family: system-ui,-apple-system,Segoe UI,Roboto,sans-serif; background: var(--bg); color: var(--text); padding-bottom: 60px; line-height: 1.45; -webkit-font-smoothing: antialiased; transition: background 0.3s, color 0.3s; }

    .topbar {
      background: var(--topbar-bg); backdrop-filter: blur(12px); -webkit-backdrop-filter: blur(12px);
      border-bottom: 1px solid var(--glass-border); padding: 0 24px; display: flex; align-items: center; justify-content: space-between; height: 64px;
      position: sticky; top: 0; z-index: 100; transition: background 0.3s, border-color 0.3s;
    }

    .btn-back { display: inline-flex; align-items: center; gap: 8px; text-decoration: none; font-size: .83rem; font-weight: 600; color: var(--text); padding: 7px 13px; border-radius: 9px; transition: border-color .12s,color .12s; border: 1px solid var(--border); background: var(--surface); }
    .btn-back:hover { border-color: var(--accent); color: var(--accent); }

    .btn-save { background: var(--accent); color: var(--accent-fg); border: 1px solid var(--accent); padding: 7px 14px; border-radius: 9px; font-weight: 600; cursor: pointer; font-size: .86rem; transition: filter .12s; font-family: inherit;}
    .btn-save:hover { filter: brightness(1.06); }

    .theme-btn { background: transparent; border: 1px solid var(--border); border-radius: 50%; width: 32px; height: 32px; display: flex; align-items: center; justify-content: center; color: var(--muted); cursor: pointer; transition: all 0.2s; margin-left: 8px; }
    .theme-btn:hover { color: var(--text); background: var(--btn-bg); border-color: var(--muted); }
    .topbar-actions { display: flex; align-items: center; gap: 12px; }

    main { padding: 32px 24px; max-width: 1000px; margin: 0 auto; }

    /* Sezioni impostazioni: pannello a se' stante per ognuna, con netto distacco
       visivo tra una e l'altra (bordo + sfondo dedicato + margine ampio). */
    .settings-panel { background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius); padding: 24px; margin-bottom: 56px; box-shadow: var(--shadow); }
    .settings-panel:last-child { margin-bottom: 0; }
    .section-title { font-size: 13px; font-weight: 800; text-transform: uppercase; letter-spacing: .1em; color: var(--text); margin: 0 0 20px; display: flex; align-items: center; gap: 10px; }
    .section-title::before { content: ''; width: 4px; height: 16px; border-radius: 3px; background: var(--accent); }
    .section-title::after { content: none; }

    .btn-utility { display: inline-flex; align-items: center; gap: 8px; background: var(--surface); border: 1px solid var(--border); color: var(--text); padding: 9px 18px; border-radius: 9px; font-weight: 600; cursor: pointer; font-size: .86rem; transition: border-color .12s,color .12s; font-family: inherit; }
    .btn-utility.warn:hover { border-color: var(--yellow); color: var(--yellow); }
    .btn-utility.danger:hover { border-color: var(--red); color: var(--red); }
    .danger-zone { margin-top: 56px; padding-top: 24px; border-top: 1px dashed var(--border); display: flex; gap: 12px; flex-wrap: wrap; }

    /* Tutto cio' che e' DENTRO un .settings-panel usa --surface2 (un tono piu'
       chiaro) cosi' si distingue dal pannello che lo contiene (--surface). */
    .card { background: var(--surface2); border: 1px solid var(--border); border-radius: var(--radius); padding: 20px; margin-bottom: 16px; }
    .card:last-child { margin-bottom: 0; }

    .form-group { display: flex; flex-direction: column; gap: 8px; }
    label { font-size: 11px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: 0.08em; }
    input, select { background: var(--surface); border: 1px solid var(--border); color: var(--text); padding: 9px 12px; border-radius: 8px; font-family: inherit; font-size: .86rem; width: 100%; transition: border-color .12s; }
    input:focus, select:focus { outline: none; border-color: var(--accent); }

    /* Stesso stile titolo-card usato nel box "Comandi Manuali" della dashboard */
    .esp-title { font-size: 14px; font-weight: 600; color: var(--text); margin-bottom: 16px; display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; }

    .grid-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 20px; }
    .area-row { display: grid; grid-template-columns: 1.5fr 1fr 1fr 1fr 1fr auto; gap: 16px; align-items: end; background: var(--surface2); padding: 20px; border: 1px solid var(--border); border-radius: var(--radius); margin-bottom: 16px; transition: border-color .12s; }
    .area-row:last-child { margin-bottom: 0; }
    .area-row:hover { border-color: var(--border2); }

    .btn-remove { background: var(--surface); border: 1px solid var(--border); color: var(--red); padding: 0 16px; border-radius: 9px; cursor: pointer; height: 44px; font-weight: 600; font-size: .86rem; transition: border-color .12s; display: flex; align-items: center; justify-content: center; }
    .btn-remove:hover { border-color: var(--red); }

    .btn-add { background: var(--surface2); border: 1px dashed var(--border); color: var(--text); padding: 14px; width: 100%; border-radius: var(--radius); cursor: pointer; margin-top: 8px; font-weight: 600; font-family: inherit; font-size: .86rem; transition: border-color .12s,color .12s; }
    .btn-add:hover { border-color: var(--accent); color: var(--accent); }

    .relay-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 20px; margin-top: 24px; padding-top: 24px; border-top: 1px dashed var(--border); }
    .auto-badge { font-size: 9px; font-weight: 700; background: var(--accent); color: var(--accent-fg); padding: 3px 8px; border-radius: 12px; margin-left: 8px; vertical-align: middle; display: none; letter-spacing: 0.05em;}

    @media (max-width: 850px) {
      .grid-3 { grid-template-columns: 1fr; }
      .area-row { grid-template-columns: 1fr 1fr; gap: 16px; }
      .area-row .nome-area-group { grid-column: span 2; }
      .area-row .btn-remove { grid-column: span 2; width: 100%; margin-top: 8px; }
      .relay-grid { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
<div class="topbar">
  <a href="/" class="btn-back">
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="19" y1="12" x2="5" y2="12"></line><polyline points="12 19 5 12 12 5"></polyline></svg>
    Monitor
  </a>
  <div class="topbar-actions">
    <button class="theme-btn" onclick="toggleTheme()" title="Cambia Tema">
      <svg id="moon-icon" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"></path></svg>
      <svg id="sun-icon" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="display:none;"><circle cx="12" cy="12" r="5"></circle><line x1="12" y1="1" x2="12" y2="3"></line><line x1="12" y1="21" x2="12" y2="23"></line><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"></line><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"></line><line x1="1" y1="12" x2="3" y2="12"></line><line x1="21" y1="12" x2="23" y2="12"></line><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"></line><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"></line></svg>
    </button>
    <button class="btn-save" onclick="salvaConfig()">Salva Modifiche</button>
  </div>
</div>
<main>
  <div class="settings-panel">
  <div class="section-title">
    Impostazioni Generali
  </div>
  <div class="card">
    <div class="form-group">
      <label>Nome Edificio</label>
      <input type="text" id="gen_nome_edificio" placeholder="Edificio" value="{{ config.generali.nome_edificio | default('') }}">
    </div>
  </div>
  <div class="card grid-3">
    <div class="form-group">
      <label>Gestione Stagione</label>
      <select id="gen_modalita" onchange="toggleStagioneManual()">
        <option value="manual" {% if config.generali.modalita_stagione == 'manual' %}selected{% endif %}>Manuale</option>
        <option value="auto" {% if config.generali.modalita_stagione == 'auto' %}selected{% endif %}>Automatica</option>
      </select>
    </div>
    <div class="form-group" id="group_stagione_manual">
      <label>Stagione <span id="badge-auto" class="auto-badge">AUTO</span></label>
      <select id="gen_stagione">
        <option value="inverno" {% if config.generali.stagione == 'inverno' %}selected{% endif %}>Inverno (Riscaldamento)</option>
        <option value="estate" {% if config.generali.stagione == 'estate' %}selected{% endif %}>Estate (Raffreddamento)</option>
      </select>
    </div>
    <div class="form-group">
      <label>Isteresi Termica (°C)</label>
      <input type="number" step="0.1" id="gen_isteresi" value="{{ config.generali.isteresi | default(0.5) }}">
    </div>
    <div class="form-group">
      <label>Inizio Giorno</label>
      <input type="time" id="gen_ora_giorno_inizio" value="{{ config.generali.ora_giorno_inizio | default('07:00') }}">
    </div>
    <div class="form-group">
      <label>Fine Giorno</label>
      <input type="time" id="gen_ora_giorno_fine" value="{{ config.generali.ora_giorno_fine | default('20:00') }}">
    </div>
    <div class="form-group">
      <label>Override companion (minuti)</label>
      <input type="number" min="0.1" max="1440" step="0.1" id="gen_manual_override_minutes" value="{{ config.generali.manual_override_minutes | default(30) }}" title="Minuti di pausa dell'automazione dopo che un pulsante companion cambia una lampada">
    </div>
  </div>
  </div>

  <div class="settings-panel">
  <div class="section-title">
    Aree Edificio
  </div>
  <div id="aree-container"></div>
  <button class="btn-add" onclick="aggiungiArea()">+ Aggiungi Nuova Area</button>
  </div>

  <div class="settings-panel">
  <div class="section-title">
    Configurazione dispositivi stanze
  </div>
  <div id="esps-container">
    {% for esp in esps %}
    <div class="card esp-card" data-esp="{{ esp }}">
      <div class="esp-title">
        <span style="display:flex; align-items:center; gap:10px;">
          {{ esp }}
        </span>
        <button class="btn-remove" style="height: auto; padding: 6px 12px; font-size: 13px; border-radius: 6px;" onclick="rimuoviEsp(this)" title="Rimuovi dispositivo">Elimina</button>
      </div>
      <div class="form-group">
        <label>Assegna ad un'area</label>
        <select class="esp-area-select"><option value="">-- Nessuna Area --</option></select>
      </div>
      <div class="relay-grid">
        {% set en_list = enabled_relays.get(esp, [True, True, True, True, True, True]) %}
        {% for i in range(1, 7) %}
          {% if en_list[i-1] %}
          <div class="form-group">
            <label>Relè {{ i }}</label>
            <select class="esp-relay" data-relay="{{ i }}">
              <option value="libero">Inutilizzato</option>
              <option value="hvac_caldo">HVAC Caldo (Caldaia)</option>
              <option value="hvac_freddo">HVAC Freddo (Split/AC)</option>
              <option value="ventola">Ventilazione</option>
              <option value="luce">Luce</option>
            </select>
          </div>
          {% endif %}
        {% endfor %}
      </div>
    </div>
    {% endfor %}
  </div>
  <button class="btn-add" id="btn-add-esp" onclick="aggiungiEspManuale()">+ Aggiungi Dispositivo Offline</button>
  </div>

  <div class="danger-zone">
    <button class="btn-utility warn" onclick="pulisciMesh()" title="Rimuove subito le lampade fantasma rimaste in memoria dopo un reset della mesh">Pulisci cache mesh</button>
    <button class="btn-utility danger" onclick="riavviaRaspberry()" title="Riavvia il Raspberry Pi">Riavvia Raspberry</button>
  </div>
</main>

<script>
  function toggleTheme() {
    const isLight = document.documentElement.classList.toggle('light-mode');
    localStorage.setItem('theme', isLight ? 'light' : 'dark');
    updateThemeIcon();
  }

  function updateThemeIcon() {
    const isLight = document.documentElement.classList.contains('light-mode');
    document.getElementById('moon-icon').style.display = isLight ? 'none' : 'block';
    document.getElementById('sun-icon').style.display = isLight ? 'block' : 'none';
  }
  document.addEventListener('DOMContentLoaded', updateThemeIcon);

  let areeData = {{ config.aree | tojson | safe }};
  let espsConfig = {{ config.esps | tojson | safe }};
  let enabledRelays = {{ enabled_relays | tojson | safe }};
  let manualSeasonBackup = "{{ config.generali.stagione }}";

  function calculateAutoSeason() {
    const ora = new Date();
    const mese = ora.getMonth(); 
    const giorno = ora.getDate();
    return ((mese > 9 || (mese === 9 && giorno >= 15)) || (mese < 3 || (mese === 3 && giorno <= 15))) ? "inverno" : "estate";
  }

  function toggleStagioneManual() {
    const mode = document.getElementById('gen_modalita').value;
    const seasonSelect = document.getElementById('gen_stagione');
    const badge = document.getElementById('badge-auto');
    
    if (mode === 'auto') {
      if (!seasonSelect.disabled) manualSeasonBackup = seasonSelect.value;
      seasonSelect.value = calculateAutoSeason();
      seasonSelect.disabled = true;
      // Scurisce solo il selettore e cambia il cursore
      seasonSelect.style.opacity = "0.5";
      seasonSelect.style.cursor = "not-allowed";
      badge.style.display = "inline-block";
    } else {
      seasonSelect.disabled = false;
      seasonSelect.value = manualSeasonBackup;
      // Ripristina il selettore
      seasonSelect.style.opacity = "1";
      seasonSelect.style.cursor = "pointer";
      badge.style.display = "none";
    }
  }

  function syncAreeInput() {
    const num = (val, fallback) => { const n = parseFloat(val); return Number.isNaN(n) ? fallback : n; };
    const rows = document.querySelectorAll('.area-row');
    areeData = Array.from(rows).map(row => ({
        nome: row.querySelector('.area-nome').value.trim(),
        t_inverno: num(row.querySelector('.area-inv').value, 20),
        t_estate: num(row.querySelector('.area-est').value, 26),
        timeout_luci: num(row.querySelector('.area-luci').value, 5),
        timeout_hvac: num(row.querySelector('.area-hvac').value, 30),
        lux_giorno_estate: num(row.querySelector('.area-lux-ge').value, 50),
        lux_notte_estate: num(row.querySelector('.area-lux-ne').value, 100),
        lux_giorno_inverno: num(row.querySelector('.area-lux-gi').value, 50),
        lux_notte_inverno: num(row.querySelector('.area-lux-ni').value, 100),
        mesh_trans: num(row.querySelector('.area-mesh-trans').value, 0),
        mesh_delay: num(row.querySelector('.area-mesh-delay').value, 0)
    }));
  }

  function val(v, fallback) { return (v === undefined || v === null) ? fallback : v; }

  function renderAree() {
    const container = document.getElementById('aree-container');
    container.innerHTML = '';
    areeData.forEach((area, index) => {
      container.innerHTML += `
        <div class="area-row" data-index="${index}">
          <div class="form-group nome-area-group">
            <label>Nome Area</label>
            <input type="text" class="area-nome" value="${area.nome || ''}" placeholder="es. Salotto">
          </div>
          <div class="form-group"><label>Target Inverno °C</label><input type="number" step="0.5" class="area-inv" value="${val(area.t_inverno, 20)}"></div>
          <div class="form-group"><label>Target Estate °C</label><input type="number" step="0.5" class="area-est" value="${val(area.t_estate, 26)}"></div>
          <div class="form-group"><label>Tempo Luci (minuti)</label><input type="number" class="area-luci" value="${val(area.timeout_luci, 5)}"></div>
          <div class="form-group"><label>Tempo HVAC (minuti)</label><input type="number" class="area-hvac" value="${val(area.timeout_hvac, 30)}"></div>
          <button class="btn-remove" onclick="rimuoviArea(${index})" title="Rimuovi Area">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg>
          </button>
          <div style="grid-column: 1 / -1; display:grid; grid-template-columns: repeat(4,1fr); gap:16px; margin-top:8px; padding-top:12px; border-top:1px dashed var(--border);">
            <div class="form-group"><label>Lux soglia · Giorno Estate</label><input type="number" class="area-lux-ge" value="${val(area.lux_giorno_estate, 50)}"></div>
            <div class="form-group"><label>Lux soglia · Notte Estate</label><input type="number" class="area-lux-ne" value="${val(area.lux_notte_estate, 100)}"></div>
            <div class="form-group"><label>Lux soglia · Giorno Inverno</label><input type="number" class="area-lux-gi" value="${val(area.lux_giorno_inverno, 50)}"></div>
            <div class="form-group"><label>Lux soglia · Notte Inverno</label><input type="number" class="area-lux-ni" value="${val(area.lux_notte_inverno, 100)}"></div>
          </div>
          <div style="grid-column: 1 / -1; display:grid; grid-template-columns: repeat(2,1fr); gap:16px; margin-top:8px; padding-top:12px; border-top:1px dashed var(--border);">
            <div class="form-group"><label>Lampade mesh · Transizione (s)</label><input type="number" step="0.1" min="0" max="62" class="area-mesh-trans" value="${val(area.mesh_trans, 0)}"></div>
            <div class="form-group"><label>Lampade mesh · Delay (ms)</label><input type="number" step="5" min="0" max="1275" class="area-mesh-delay" value="${val(area.mesh_delay, 0)}"></div>
          </div>
        </div>`;
    });
    aggiornaSelectAree();
  }

  function aggiungiArea() { syncAreeInput(); areeData.push({nome: "Nuova Area", t_inverno: 20, t_estate: 26, timeout_luci: 5, timeout_hvac: 30, lux_giorno_estate: 50, lux_notte_estate: 100, lux_giorno_inverno: 50, lux_notte_inverno: 100, mesh_trans: 0, mesh_delay: 0}); renderAree(); }
  function rimuoviArea(index) { syncAreeInput(); areeData.splice(index, 1); renderAree(); }

  function aggiornaSelectAree() {
    const inputs = document.querySelectorAll('.area-nome');
    let optionsHTML = '<option value="">-- Nessuna Area --</option>';
    inputs.forEach(input => { if(input.value.trim() !== '') optionsHTML += `<option value="${input.value}">${input.value}</option>`; });
    document.querySelectorAll('.esp-area-select').forEach(select => {
      const valAttuale = select.value; select.innerHTML = optionsHTML;
      if(Array.from(select.options).some(o => o.value === valAttuale)) select.value = valAttuale;
    });
  }

  function aggiungiEspManuale() {
    const nomeEsp = prompt("Nome ESP (es. Stanza 2):");
    if(!nomeEsp || document.querySelector(`.esp-card[data-esp="${nomeEsp}"]`)) return;
    const opt = document.querySelector('.esp-area-select').innerHTML;
    let relays = '';
    let enList = enabledRelays[nomeEsp] || [true, true, true, true, true, true];
    for(let i=1; i<=6; i++) {
      if(enList[i-1]) {
        relays += `<div class="form-group"><label>Relè ${i}</label><select class="esp-relay" data-relay="${i}"><option value="libero">Inutilizzato</option><option value="hvac_caldo">HVAC Caldo</option><option value="hvac_freddo">HVAC Freddo</option><option value="ventola">Ventola</option><option value="luce">Luce</option></select></div>`;
      }
    }
    document.getElementById('esps-container').insertAdjacentHTML('beforeend', `
      <div class="card esp-card" data-esp="${nomeEsp}">
        <div class="esp-title">
          <span style="display:flex; align-items:center; gap:10px;">
            ${nomeEsp}
          </span>
          <button class="btn-remove" style="height: auto; padding: 6px 12px; font-size: 13px; border-radius: 6px;" onclick="rimuoviEsp(this)">Elimina</button>
        </div>
        <div class="form-group"><label>Area</label><select class="esp-area-select">${opt}</select></div>
        <div class="relay-grid">${relays}</div>
      </div>`);
  }

  function rimuoviEsp(btn) { if(confirm("Sei sicuro di voler rimuovere questo dispositivo?")) btn.closest('.esp-card').remove(); }

  function initEsps() {
    document.querySelectorAll('.esp-card').forEach(card => {
      const espName = card.getAttribute('data-esp');
      if(espsConfig[espName]) {
        card.querySelector('.esp-area-select').value = espsConfig[espName].area || "";
        const relayConfig = espsConfig[espName].rele || {};
        card.querySelectorAll('.esp-relay').forEach(sel => {
          const rId = sel.getAttribute('data-relay');
          if(relayConfig[rId]) sel.value = relayConfig[rId];
        });
      }
    });
  }

  function salvaConfig() {
    syncAreeInput();
    const config = {
      generali: {
        modalita_stagione: document.getElementById('gen_modalita').value,
        stagione: document.getElementById('gen_stagione').value,
        isteresi: parseFloat(document.getElementById('gen_isteresi').value) || 0.5,
        ora_giorno_inizio: document.getElementById('gen_ora_giorno_inizio').value || '07:00',
        ora_giorno_fine: document.getElementById('gen_ora_giorno_fine').value || '20:00',
        nome_edificio: document.getElementById('gen_nome_edificio').value.trim() || 'Edificio',
        manual_override_minutes: (() => { const v = parseFloat(document.getElementById('gen_manual_override_minutes').value); return isNaN(v) ? 30 : v; })()
      },
      aree: areeData, 
      esps: {}
    };
    document.querySelectorAll('.esp-card').forEach(card => {
      const espName = card.getAttribute('data-esp');
      const rele = {};
      card.querySelectorAll('.esp-relay').forEach(sel => { rele[sel.getAttribute('data-relay')] = sel.value; });
      config.esps[espName] = { area: card.querySelector('.esp-area-select').value, rele: rele };
    });

    const btn = document.querySelector('.btn-save');
    btn.textContent = "Salvataggio...";
    fetch('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(config) })
      .then(r => r.json()).then(res => {
        if(res.ok) {
          btn.textContent = "Modifiche Salvate";
          setTimeout(() => btn.textContent = "Salva Modifiche", 2000);
        }
      });
  }

  function pulisciMesh() {
    if (!confirm("Svuotare la cache delle lampade/sensori mesh? Quelle ancora attive ricompariranno entro pochi secondi.")) return;
    const btn = document.querySelector('.btn-utility.warn');
    btn.textContent = "Pulizia...";
    fetch('/api/mesh_clear', { method: 'POST' })
      .then(r => r.json())
      .then(res => { btn.textContent = res.ok ? "Fatto (" + res.removed + " rimosse)" : "Errore"; setTimeout(() => btn.textContent = "Pulisci cache mesh", 3000); })
      .catch(() => { btn.textContent = "Errore di rete"; setTimeout(() => btn.textContent = "Pulisci cache mesh", 3000); });
  }

  function riavviaRaspberry() {
    if (!confirm('Riavviare il Raspberry Pi adesso? Il sistema sarà irraggiungibile per qualche minuto.')) return;
    const btn = document.querySelector('.btn-utility.danger');
    btn.textContent = "Riavvio...";
    fetch('/api/reboot', { method: 'POST' })
      .then(r => r.json())
      .then(res => { btn.textContent = res.ok ? "Riavvio in corso..." : "Errore: " + (res.error || '?'); })
      .catch(() => { btn.textContent = "Riavvio in corso..."; });
  }

  renderAree();
  setTimeout(() => { initEsps(); toggleStagioneManual(); }, 150);
</script>
</body>
</html>
"""

# --- GESTIONE CONFIGURAZIONE ---
def load_config():
    if not os.path.exists(CONFIG_FILE): return {"generali": {"stagione": "inverno", "isteresi": 0.5}, "aree": [], "esps": {}}
    try:
        with open(CONFIG_FILE, "r") as f: return json.load(f)
    except: return {"generali": {"stagione": "inverno", "isteresi": 0.5}, "aree": [], "esps": {}}

def save_config(data):
    with open(CONFIG_FILE, "w") as f: json.dump(data, f, indent=4)

def load_names_cache():
    """MESH_LAMP_NAMES/SENSOR_NAMES/MESH_SENSOR_NAMES vivono solo in RAM,
    ripopolati dal giro CONFIGREQ -> MESHCONFIG/SENSORCFG/MESHSENSORCFG ad
    ogni (ri)connessione MQTT (vedi on_connect). Quel giro funziona, ma per i
    primi secondi dopo un riavvio di Manager.py (o del Raspberry) la
    dashboard mostra tutto senza nome finche' non arriva la risposta - questa
    cache su disco serve solo a evitare quel buco, NON sostituisce il giro
    MQTT che resta la fonte di verita' (un nodo rinominato o rimosso mentre
    Manager.py era spento si corregge comunque al prossimo CONFIGREQ)."""
    if not os.path.exists(NAMES_CACHE_FILE):
        return
    try:
        with open(NAMES_CACHE_FILE, "r") as f:
            data = json.load(f)
        MESH_LAMP_NAMES.update(data.get("mesh_lamp", {}))
        SENSOR_NAMES.update(data.get("sensor", {}))
        MESH_SENSOR_NAMES.update(data.get("mesh_sensor", {}))
    except Exception:
        pass

def save_names_cache():
    try:
        with open(NAMES_CACHE_FILE, "w") as f:
            json.dump({
                "mesh_lamp": MESH_LAMP_NAMES,
                "sensor": SENSOR_NAMES,
                "mesh_sensor": MESH_SENSOR_NAMES,
            }, f, indent=2)
    except Exception:
        pass

def get_modalita(config, esp, tipo):
    """tipo: 'rele' o 'mesh' - switch automatico/manuale indipendenti per esp.
    Fallback sul vecchio campo unico "modalita" (pre-esistente prima dello
    split), cosi' chi era in manuale non si ritrova resettato in automatico."""
    cfg = config.get("esps", {}).get(esp, {})
    legacy = cfg.get("modalita", "auto")
    chiave = "modalita_rele" if tipo == "rele" else "modalita_mesh"
    return cfg.get(chiave, legacy)

def is_lamp_overridden(hub, addr):
    """True se la lampada ha un override manuale (MESHOVERRIDE) o provisional hold attivo."""
    key = (hub, addr.upper())
    now = datetime.datetime.now()
    until = MANUAL_OVERRIDE_UNTIL.get(key)
    if until is not None and now < until:
        return True
    prov = MESH_PROVISIONAL_OVERRIDE.get(key)
    return prov is not None and now < prov

def _record_auto_cmd(esp, lamps_list, onoff=None, pct=None):
    """Registra in LAST_AUTO_MESH_STATE lo stato che l'automazione ha appena comandato.
    Serve a riconoscere stati 'attesi' vs 'inattesi' per il provisional hold.
    Serve a riconoscere stati attesi vs inattesi per il provisional hold e per
    filtrare falsi badge da arrotondamento DALI (confronto in MESHOVERRIDE handler)."""
    for a, _ in lamps_list:
        key = (esp, a.upper())
        entry = LAST_AUTO_MESH_STATE.setdefault(key, {})
        if onoff is not None:
            entry['onoff'] = str(onoff)
        if pct is not None:
            entry['pct'] = str(pct)

# --- GESTIONE MQTT IN BACKGROUND ---
_mqtt = mqtt_client.Client(mqtt_client.CallbackAPIVersion.VERSION2)

def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"[MQTT] Connesso. Iscrizione a: {MQTT_TOPIC_RX}")
        client.subscribe(MQTT_TOPIC_RX)
        # Manager.py riparte sempre senza sapere nulla (RAM volatile): chiede
        # a TUTTI i gateway in ascolto di rispedire la propria configurazione
        # (CAPS/RELAYSTATE/MESHCONFIG, vedi mqtt_rx_handler in main.c) invece
        # di aspettare passivamente il prossimo aggiornamento periodico.
        client.publish(RELAY_TOPIC, "CONFIGREQ|ALL", qos=0)
        print("[MQTT] CONFIGREQ|ALL inviato (richiesta configurazione completa).")
    else: print(f"[MQTT] Errore di connessione (rc={rc})")

def prune_stale_mesh_status():
    """Rimuove da MESH_STATUS le voci (lampade/sensori mesh) che non si
    aggiornano piu' da MESH_GHOST_TIMEOUT_SEC: tipicamente "lampade fantasma"
    rimaste in memoria dopo un reset/riprovisioning della mesh, il cui
    indirizzo non esiste piu' lato gateway e quindi non ricevera' mai
    piu' un aggiornamento reale."""
    now = datetime.datetime.now()
    for hub in list(MESH_STATUS.keys()):
        addrs = MESH_STATUS[hub]
        for addr in list(addrs.keys()):
            try:
                t = datetime.datetime.strptime(addrs[addr].get("time", ""), DT_FORMAT)
            except (ValueError, TypeError):
                continue
            if (now - t).total_seconds() > MESH_GHOST_TIMEOUT_SEC:
                del addrs[addr]

def handle_mesh_status(hub, addr, kvstring):
    """parts da MESH|hub|addrHex|campo1=val1;campo2=val2 (vedi on_message)."""
    global MESH_STATUS
    try:
        hub, addr = hub.strip(), addr.strip()
        kv = {}
        for pair in kvstring.split(';'):
            if '=' in pair:
                k, v = pair.split('=', 1)
                kv[k.strip()] = v.strip()
        ora_str = datetime.datetime.now().strftime(DT_FORMAT)
        kv["time"] = ora_str
        # update(), non sovrascrittura totale: lo stesso indirizzo puo' arrivare
        # in piu' messaggi separati con campi diversi (es. un "onoff=X" e un
        # "onoff=X;pct=Y" per lo stesso elemento lampada+dimmer) - sovrascrivere
        # l'intera voce faceva sparire "pct" se l'ultimo messaggio arrivato non
        # lo conteneva, con lo slider che spariva dalla dashboard a intermittenza.
        MESH_STATUS.setdefault(hub, {}).setdefault(addr, {}).update(kv)

        # I sensori mesh (presence/lux) finiscono anche in DATA_STORE, nello
        # stesso formato dei sensori classici: cosi' card e grafici li
        # gestiscono gratis tramite get_last_values()/plot_area() esistenti,
        # senza una pipeline duplicata.
        if "presence" in kv:
            DATA_STORE.append({"Stanza": hub, "Sensore": addr, "Chiave": "pir", "data": ora_str, "valore": kv["presence"]})
            try:
                if float(kv.get("lux", -1)) >= 0:
                    DATA_STORE.append({"Stanza": hub, "Sensore": addr, "Chiave": "lux", "data": ora_str, "valore": kv["lux"]})
            except (TypeError, ValueError):
                pass
            if len(DATA_STORE) > 10000: del DATA_STORE[:1000]
    except Exception:
        pass

MODEM_RECOVERY_LOG_FILE = "modem_recovery.log"

def handle_modem_recovery(hub, downtime_sec_str):
    """MODEMRECOVERY|hub|secondi - l'ESP la manda UNA VOLTA appena la sessione
    MQTT torna su dopo un buco (vedi modem_mqtt_set_recovery_callback in
    modem_mqtt.c). Senza questo, un buco di connettivita' (es. campo
    cellulare assente per minuti) si scopriva solo a posteriori dal grafico,
    senza nessuna traccia diagnostica di quando/quanto e' durato - qui lo
    logghiamo su un file persistente (oltre al log di systemd, che puo'
    ruotare) cosi' resta consultabile."""
    try:
        downtime_sec = int(float(downtime_sec_str))
        ora_str = datetime.datetime.now().strftime(DT_FORMAT)
        riga = f"{ora_str} - {hub.strip()} - buco di connettivita' modem: {downtime_sec}s"
        print(f"[MODEM RECOVERY] {riga}")
        with open(MODEM_RECOVERY_LOG_FILE, "a") as f:
            f.write(riga + "\n")
    except Exception:
        pass

def handle_mesh_list(hub, addrs_csv):
    """MESHLIST|hub|addr1,addr2,... - manifest periodico (vedi MeshBridge.cpp,
    meshRequestDump) con gli indirizzi REALMENTE esistenti secondo il gateway
    in questo momento. Qualunque cosa in MESH_STATUS[hub] non sia in questa
    lista per piu' di MESH_LIST_MISS_THRESHOLD dump consecutivi viene
    rimossa: e' la difesa contro le "lampade fantasma" che il broker MQTT
    remoto puo' rispedire a Manager.py alla riconnessione (storico di oggi),
    indipendentemente da reset/riprovisioning della mesh - il gateway via
    UART e' la fonte di verita', il broker no. La tolleranza a piu' dump
    serve perche' un singolo MESHLIST puo' perdere per strada una lampada
    ancora viva (collisione radio sulla mesh BLE), e rimuoverla al primo
    miss la fa sfarfallare nella dashboard senza che sia davvero sparita."""
    hub = hub.strip()
    valid = {a.strip() for a in addrs_csv.split(',') if a.strip()}
    addrs = MESH_STATUS.get(hub)
    if not addrs:
        return
    misses = MESH_LIST_MISSES.setdefault(hub, {})
    now_str = datetime.datetime.now().strftime(DT_FORMAT)
    for addr in list(addrs.keys()):
        if addr in valid:
            misses.pop(addr, None)
            # Rinfresca "time": il gateway confirma che l'indirizzo esiste ancora
            # ANCHE se non manda un MESH| di stato da un po' (diff-publish: una
            # lampada ferma, senza cambi di onoff/pct, non rimanda nulla). Senza
            # questo, prune_stale_mesh_status() la cancellava per "inattivita'"
            # anche se non era affatto fantasma - bastava aspettare 2 minuti.
            addrs[addr]["time"] = now_str
        else:
            misses[addr] = misses.get(addr, 0) + 1
            if misses[addr] >= MESH_LIST_MISS_THRESHOLD:
                del addrs[addr]
                del misses[addr]

def handle_mesh_boot(hub, addrs_csv):
    """MESHBOOT|hub|addr1,addr2,... - annuncio "appena acceso" (stesso ruolo
    di CAPS per i rele'): il gateway lo manda UNA VOLTA, alla prima
    connessione MQTT dopo il boot, con tutti gli indirizzi mesh che considera
    reali in quel momento. A differenza di MESHLIST (periodico, tollerante a
    MESH_LIST_MISS_THRESHOLD dump persi durante il normale funzionamento),
    qui non c'e' nessuna tolleranza: e' il primo segnale dopo un riavvio,
    quindi e' la potatura immediata di qualsiasi fantasma rimasto da prima
    (reset/riprovisioning mesh, o storico rispedito dal broker alla
    riconnessione) senza aspettare i 3 dump di MESHLIST."""
    hub = hub.strip()
    valid = {a.strip() for a in addrs_csv.split(',') if a.strip()}
    addrs = MESH_STATUS.get(hub)
    if addrs:
        now_str = datetime.datetime.now().strftime(DT_FORMAT)
        for addr in list(addrs.keys()):
            if addr not in valid:
                del addrs[addr]
            else:
                addrs[addr]["time"] = now_str  # vedi commento in handle_mesh_list
    MESH_LIST_MISSES.pop(hub, None)
    # Pulisce anche i nomi: se la mesh e' stata resettata/riprovisionata gli
    # indirizzi cambiano, e un nome associato a un addr ormai inesistente
    # (mai piu' aggiornato da MESHCONFIG perche' nessuno l'ha richiesto)
    # restava per sempre in memoria - get_mesh_lamps_grouped() lo trattava
    # come whitelist non vuota e filtrava via TUTTE le lampade reali, perche'
    # nessun addr attuale ci compariva dentro. MESHBOOT e' il segnale "stato
    # vero appena letto dal gateway", quindi e' il punto giusto per potare.
    pruned = False
    for names_map in (MESH_LAMP_NAMES.get(hub), MESH_SENSOR_NAMES.get(hub)):
        if not names_map:
            continue
        for addr in list(names_map.keys()):
            if addr not in valid:
                del names_map[addr]
                pruned = True
    if pruned:
        save_names_cache()

def handle_mesh_config(hub, count_str, names_csv):
    """MESHCONFIG|hub|count|nome1:addr1,nome2:addr2,... - composizione con
    nomi: pubblicata dal bottone "Salva configurazione mesh" sull'hub, oppure
    in risposta a un nostro CONFIGREQ|ALL (vedi on_connect). Aggiorna solo i
    nomi (MESH_LAMP_NAMES); lo stato on/off/pct delle lampade arriva separato
    via i messaggi MESH|hub|addr|... che il gateway manda comunque ad ogni
    cambio - qui ci serve solo "quale nome ha questo indirizzo".
    Sovrascrive l'intera mappa (non merge): il payload e' sempre la
    composizione COMPLETA attuale lato gateway, va trattato come fonte di
    verita' - un merge avrebbe lasciato per sempre indirizzi di lampade
    rimosse/riprovisionate, con l'effetto di filtrare via anche quelle
    reali in get_mesh_lamps_grouped() (whitelist mai vuota ma sbagliata)."""
    hub = hub.strip()
    names = {}
    MESH_LAMP_NAMES[hub] = names
    for item in names_csv.split(','):
        if ':' not in item:
            continue
        nome, addr = item.rsplit(':', 1)
        nome, addr = nome.strip(), addr.strip()
        if addr:
            names[addr] = nome
    save_names_cache()

def handle_sensor_config(hub, names_csv):
    """SENSORCFG|hub|id1:nome1,id2:nome2,... - nomi assegnati ai sensori BLE
    classici (impostati nella pagina /setup dell'hub), stesso ruolo di
    MESHCONFIG ma per gli slot sensore invece che per le lampade mesh."""
    hub = hub.strip()
    names = SENSOR_NAMES.setdefault(hub, {})
    for item in names_csv.split(','):
        if ':' not in item:
            continue
        sid, nome = item.split(':', 1)
        sid, nome = sid.strip(), nome.strip()
        if sid and nome:
            names[sid] = nome
    save_names_cache()

def handle_mesh_sensor_config(hub, names_csv):
    """MESHSENSORCFG|hub|addr1:nome1,addr2:nome2,... - nomi assegnati ai nodi
    Sensor Server della mesh (PIR/lux): publish_meshconfig() li esclude
    apposta (sono "SENSORE", non "LAMPADA"), quindi senza un messaggio
    dedicato il nome dato con /setname restava visibile solo sulla pagina
    web del gateway e non arrivava mai qui."""
    hub = hub.strip()
    names = MESH_SENSOR_NAMES.setdefault(hub, {})
    for item in names_csv.split(','):
        if ':' not in item:
            continue
        addr, nome = item.split(':', 1)
        addr, nome = addr.strip(), nome.strip()
        if addr and nome:
            names[addr] = nome
    save_names_cache()

def on_message(client, userdata, msg):
    """Formato unico su 'modem/display': TIPO|hub|resto-specifico-del-tipo.
       SENSOR|hub|id|k=v;k2=v2   -> sensore classico
       MESH|hub|addrHex|k=v;...  -> sensore/lampada mesh
       MESHLIST|hub|addr1,addr2,... -> manifest periodico, pulisce i fantasmi
       MESHBOOT|hub|addr1,addr2,... -> manifest una volta al boot, pulizia immediata
       MESHCONFIG|hub|count|nome1:addr1,... -> composizione con nomi (bottone "Salva" o risposta a CONFIGREQ)
       SENSORCFG|hub|id1:nome1,... -> nomi sensori BLE classici (annuncio post-setup o risposta a CONFIGREQ)
       MESHSENSORCFG|hub|addr1:nome1,... -> nomi sensori mesh PIR/lux (annuncio post "Salva nome" o risposta a CONFIGREQ)
       RELAYSTATE|hub|bitmask    -> stato relè fisici
       CAPS|hub|pinStr           -> relè abilitati (annuncio post-wizard)"""
    global LAST_RELAY_STATUS, ENABLED_RELAYS
    payload = msg.payload.decode('utf-8').strip()
    try:
        parts = payload.split('|')
        if len(parts) < 2:
            return
        msg_type = parts[0]

        if msg_type == "MESH" and len(parts) == 4:
            hub_m, addr_m = parts[1], parts[2].upper()
            handle_mesh_status(hub_m, addr_m, parts[3])
            # Provisional hold: se lo stato arrivato differisce da quello che
            # l'automazione si aspettava, qualcuno (companion o utente) ha
            # agito prima che MESHOVERRIDE arrivasse via cellulare.
            # Blocca l'automazione per MESH_PROVISIONAL_HOLD_SEC secondi cosi'
            # il MESHOVERRIDE ha tempo di arrivare e prendere il controllo.
            key_m = (hub_m, addr_m)
            last = LAST_AUTO_MESH_STATE.get(key_m)
            if last:
                kv_m = {}
                for pair_m in parts[3].split(';'):
                    if '=' in pair_m:
                        k_m, v_m = pair_m.split('=', 1)
                        kv_m[k_m.strip()] = v_m.strip()
                onoff_now = kv_m.get('onoff')
                pct_now   = kv_m.get('pct')
                state_matches = True
                if onoff_now is not None and last.get('onoff') is not None:
                    state_matches = state_matches and (onoff_now == last['onoff'])
                if pct_now is not None and last.get('pct') is not None:
                    try:
                        state_matches = state_matches and (int(float(pct_now)) == int(float(last['pct'])))
                    except (ValueError, TypeError):
                        pass
                if not state_matches and not is_lamp_overridden(hub_m, addr_m):
                    MESH_PROVISIONAL_OVERRIDE[key_m] = datetime.datetime.now() + datetime.timedelta(seconds=MESH_PROVISIONAL_HOLD_SEC)

        elif msg_type == "MESHOVERRIDE" and len(parts) == 4:
            hub, addr = parts[1], parts[2].upper()
            cfg = load_config()
            minutes = float(cfg.get("generali", {}).get("manual_override_minutes", 30))
            now_ov = datetime.datetime.now()
            until_ov = now_ov + datetime.timedelta(minutes=minutes)
            MANUAL_OVERRIDE_UNTIL[(hub, addr)] = until_ov
            MESH_PROVISIONAL_OVERRIDE.pop((hub, addr), None)
            # Blocca tutta l'automazione del hub per la stessa durata: il companion
            # agisce su GROUP_ALL, ma le MESHOVERRIDE per ogni lampada arrivano
            # scaglionate via cellulare. La prima che arriva ferma subito tutto.
            HUB_MESH_OVERRIDE_UNTIL[hub] = until_ov
            # Badge companion: scatta solo se il cambio NON è arrotondamento DALI
            # di un nostro MESHCMD. Confrontiamo lo stato MESHOVERRIDE con quello
            # che l'automazione ha comandato (LAST_AUTO_MESH_STATE): se onoff
            # coincide e pct differisce <= 10 punti è rounding, non un companion.
            # Se LAST_AUTO_MESH_STATE non ha questa lampada (nessun cmd automazione
            # recente), confrontiamo con MESH_STATUS come fallback.
            try:
                kv_ov = dict(p.split("=",1) for p in parts[3].split(";") if "=" in p)
                auto_ref = LAST_AUTO_MESH_STATE.get((hub, addr), {})
                ref = auto_ref if auto_ref else MESH_STATUS.get(hub, {}).get(addr, {})
                onoff_changed = kv_ov.get("onoff") != ref.get("onoff")
                pct_diff = abs(int(float(kv_ov.get("pct",0) or 0)) - int(float(ref.get("pct",0) or 0)))
                if onoff_changed or pct_diff > 10:
                    COMPANION_BADGE_UNTIL[hub] = until_ov
            except Exception:
                pass
            handle_mesh_status(hub, addr, parts[3])  # aggiorna lo stato come farebbe MESH

        elif msg_type == "MESHLIST" and len(parts) == 3:
            handle_mesh_list(parts[1], parts[2])

        elif msg_type == "MESHBOOT" and len(parts) == 3:
            handle_mesh_boot(parts[1], parts[2])

        elif msg_type == "MESHCONFIG" and len(parts) == 4:
            handle_mesh_config(parts[1], parts[2], parts[3])

        elif msg_type == "SENSORCFG" and len(parts) == 3:
            handle_sensor_config(parts[1], parts[2])

        elif msg_type == "MESHSENSORCFG" and len(parts) == 3:
            handle_mesh_sensor_config(parts[1], parts[2])

        elif msg_type == "CAPS" and len(parts) == 3:
            ENABLED_RELAYS[parts[1].strip()] = [r.strip() == '1' for r in parts[2].split(',')]

        elif msg_type == "RELAYSTATE" and len(parts) == 3:
            LAST_RELAY_STATUS[parts[1].strip()] = parts[2].strip()[:6]

        elif msg_type == "MODEMRECOVERY" and len(parts) == 3:
            handle_modem_recovery(parts[1], parts[2])

        elif msg_type == "SENSOR" and len(parts) == 4:
            stanza, id_sensore, valori_raw = parts[1].strip(), parts[2].strip(), parts[3].strip()
            ora_str = datetime.datetime.now().strftime(DT_FORMAT)
            for coppia in valori_raw.split(';'):
                if '=' not in coppia: continue
                chiave, valore = coppia.split('=', 1)
                DATA_STORE.append({"Stanza": stanza, "Sensore": id_sensore, "Chiave": chiave.strip(), "data": ora_str, "valore": valore.strip()})
            if len(DATA_STORE) > 10000: del DATA_STORE[:1000]
    except Exception:
        pass

def start_mqtt():
    load_names_cache()  # disponibili da subito, prima ancora che arrivi la risposta al CONFIGREQ
    _mqtt.on_connect = on_connect
    _mqtt.on_message = on_message
    try:
        _mqtt.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        _mqtt.loop_start()
    except Exception as e: print(f"MQTT start error: {e}")

def get_current_season(config):
    gen = config.get("generali", {})
    if gen.get("modalita_stagione", "manual") == "manual": return gen.get("stagione", "inverno")
    oggi = datetime.date.today()
    if datetime.date(oggi.year, 4, 15) < oggi < datetime.date(oggi.year, 10, 15): return "estate"
    return "inverno"

def is_daytime(config, now):
    """Fascia oraria globale (impostazioni generali) per scegliere la soglia
    lux giorno/notte nell'automazione lampade mesh."""
    gen = config.get("generali", {})
    try:
        h1, m1 = map(int, gen.get("ora_giorno_inizio", "07:00").split(":"))
        h2, m2 = map(int, gen.get("ora_giorno_fine", "20:00").split(":"))
    except Exception:
        h1, m1, h2, m2 = 7, 0, 20, 0
    t = now.time()
    inizio, fine = datetime.time(h1, m1), datetime.time(h2, m2)
    if inizio <= fine:
        return inizio <= t <= fine
    return t >= inizio or t <= fine  # fascia che attraversa la mezzanotte

# --- HELPERS DATI & AREE ---
def get_clean_df():
    if not DATA_STORE: return pd.DataFrame(columns=["Stanza","Sensore","Chiave","data","valore"])
    df = pd.DataFrame(DATA_STORE)
    df["data"] = pd.to_datetime(df["data"], format=DT_FORMAT, errors="coerce")
    df["Stanza"] = df["Stanza"].astype(str).str.strip()
    df["Sensore"] = df["Sensore"].astype(str).str.strip()
    df["Chiave"] = df["Chiave"].astype(str).str.strip()
    df["valore"] = df["valore"].astype(str).str.strip()
    return df.dropna(subset=["Stanza","Sensore","data"])

def get_esps_in_area(config, area_name):
    if area_name == "TUTTE": return None
    return [esp for esp, cfg in config.get("esps", {}).items() if cfg.get("area") == area_name]

def filter_by_area(df, area_name, config):
    if df.empty or area_name == "TUTTE": return df
    esps = get_esps_in_area(config, area_name)
    return df[df["Stanza"].isin(esps)]

def get_mesh_lamps_grouped(config, area_name):
    """Lampade mesh BLE (onoff/pct) raggruppate per hub, stesso schema dei rele'.
    Mostra SOLO gli indirizzi dichiarati via MESHCONFIG (vedi handle_mesh_config):
    ogni nodo lampada ha piu' elementi con OnOff (es. 4: #0..#3) ma uno solo e'
    "la lampada" vera (quello col dimmer, dichiarato nel manifest) - gli altri
    sono uscite ausiliarie sullo stesso modulo e non vanno mostrate come
    lampade separate, anche se arrivano comunque via MESH|hub|addr|onoff=..."""
    hubs = list(MESH_STATUS.keys()) if area_name == "TUTTE" else (get_esps_in_area(config, area_name) or [])
    grouped = {}
    for hub in hubs:
        named = MESH_LAMP_NAMES.get(hub, {})
        for addr, kv in MESH_STATUS.get(hub, {}).items():
            if "onoff" not in kv:
                continue
            if named and addr not in named:
                continue
            ov_until = COMPANION_BADGE_UNTIL.get(hub)
            lamp = {
                "esp": hub, "addr": addr,
                "name": MESH_LAMP_NAMES.get(hub, {}).get(addr) or f"Lampada {addr}",
                "is_on": kv.get("onoff") == "1",
                "pct": kv.get("pct"),
                # Sensor Server della lampada (potenza/energia): assente sulle
                # lampade senza quel modulo, percio' None se mai arrivato.
                "power": kv.get("power"),
                "energy": kv.get("energy"),
                # Timestamp Unix (secondi) di scadenza badge companion, 0 se non attivo.
                # Usa COMPANION_BADGE_UNTIL (non MANUAL_OVERRIDE_UNTIL) per filtrare
                # i falsi positivi da arrotondamento DALI.
                "companion_override_until": ov_until.timestamp() if (ov_until is not None and datetime.datetime.now() < ov_until) else 0,
            }
            grouped.setdefault(hub, []).append(lamp)
    return grouped

def get_rooms_list(df): return sorted(df["Stanza"].unique().tolist()) if not df.empty else []

def get_last_values(df):
    sensors = []
    if df.empty: return sensors
    for name, g in df.groupby(["Stanza","Sensore"]):
        stanza, sensore_id = str(name[0]), str(name[1])
        g = g.sort_values("data")
        last_time = g.iloc[-1]["data"]
        pivot = {str(row["Chiave"]): str(row["valore"]) for _, row in g.iterrows()}

        diffs = g["data"].drop_duplicates().diff().dropna().dt.total_seconds()
        if len(diffs) > 0:
            media_agg = diffs.mean()
            moda_agg = diffs.mode().iloc[0]
            update_stats = (
                f"Freq. aggiornamenti<br>"
                f"<span style='display:flex;gap:8px;white-space:nowrap;'>"
                f"Media <strong>{media_agg:.0f}s</strong>"
                f"&nbsp;&middot;&nbsp;"
                f" Moda <strong>{moda_agg:.0f}s</strong>"
                f"</span>"
            )
        else:
            update_stats = None
        
        value_label = None
        secondary_value = None
        secondary_label = None
        lux_value = None

        if "temp" in pivot:
            try: main_value = f"{float(pivot['temp']):.1f}°C"
            except: main_value = f"{pivot['temp']}°C"
            value_label = "Temperatura"
            sub_values = []
            if "hum" in pivot:
                secondary_value = f"{pivot['hum']}%"
                secondary_label = "Umidità"
            if "rssi" in pivot: sub_values.append(f"RSSI: {pivot['rssi']} dBm")
            card_class = ""
        elif "pir" in pivot or "pir_state" in pivot:
            state = pivot.get("pir", pivot.get("pir_state"))
            is_detected = str(state).strip().upper() in ["1", "1.0", "1.00", "DETECTED"]
            main_value = "RILEVATO" if is_detected else "LIBERO"
            sub_values = []
            cnt = pivot.get("cnt", pivot.get("pir_count"))
            if cnt:
                try: lux_value = f"{int(float(cnt))} passaggi"
                except: lux_value = f"{cnt} passaggi"
            if "rssi" in pivot: sub_values.append(f"RSSI: {pivot['rssi']} dBm")
            if "lux" in pivot:
                try: lux_value = f"{float(pivot['lux']):.1f} lux"
                except: lux_value = f"{pivot['lux']} lux"
            card_class = "pir-detected" if is_detected else "pir-clear"
        else:
            main_value = " ".join(f"{str(k)}={str(v)}" for k,v in pivot.items() if str(k) != "rssi")
            sub_values = [f"RSSI: {pivot['rssi']} dBm"] if "rssi" in pivot else []
            card_class = ""

        # Gli indirizzi mesh sono sempre 4 caratteri hex (es. "0021"), i sensori
        # classici sono ID brevi non paddati ("1".."10") - distinzione solo
        # estetica per il badge, non cambia la logica.
        is_mesh = len(sensore_id) == 4
        nome = MESH_SENSOR_NAMES.get(stanza, {}).get(sensore_id) if is_mesh else SENSOR_NAMES.get(stanza, {}).get(sensore_id)
        sensors.append({
            "room": stanza, "sensor_id": sensore_id, "name": nome, "main_value": str(main_value),
            "value_label": value_label, "secondary_value": secondary_value, "secondary_label": secondary_label,
            "lux_value": lux_value, "sub_values": sub_values, "card_class": card_class,
            "time": last_time.strftime("%H:%M:%S"), "is_mesh": is_mesh,
            "updatestats": update_stats
        })
    return sorted(sensors, key=lambda x: (x["room"], x["sensor_id"]))

_last_configreq_ts = 0
def maybe_resend_configreq():
    """Rispedisce CONFIGREQ|ALL ogni CONFIGREQ_RESEND_SEC: oltre alla richiesta
    fatta una volta sola al connect (vedi on_connect), questo e' un fallback
    periodico - se il primo CONFIGREQ va perso, o se Manager.py resta in
    esecuzione mentre l'ESP si riavvia (CONFIGREQ del connect ormai passato,
    nessun trigger per rispedirlo), MESH_LAMP_NAMES/SENSOR_NAMES restano vuoti
    per sempre e le lampade appaiono senza nome (o filtrate via, vedi
    get_mesh_lamps_grouped) finche' qualcuno non riavvia Manager.py a mano."""
    global _last_configreq_ts
    now = time.time()
    if now - _last_configreq_ts < CONFIGREQ_RESEND_SEC:
        return
    _last_configreq_ts = now
    try:
        _mqtt.publish(RELAY_TOPIC, "CONFIGREQ|ALL", qos=0)
    except Exception:
        pass

def hvac_automation_loop():
    while True:
        try:
            maybe_resend_configreq()
            prune_stale_mesh_status()
            config = load_config()
            df = get_clean_df()
            if not config.get("aree") or df.empty:
                time.sleep(10)
                continue
                
            stagione = get_current_season(config)
            isteresi = float(config.get("generali", {}).get("isteresi", 0.5))
            ultimi_dati = get_last_values(df)

            stato_esps = {}
            ora_attuale = datetime.datetime.now()
            is_giorno = is_daytime(config, ora_attuale)
            
            for s in ultimi_dati:
                stanza = s["room"]
                if stanza not in stato_esps: stato_esps[stanza] = {"temp": None}
                if "°C" in s["main_value"]:
                    try: stato_esps[stanza]["temp"] = float(s["main_value"].replace("°C", ""))
                    except: pass
                
                # REGISTRA L'ORARIO DELL'ULTIMO MOVIMENTO (1 per rilevato)
                if s["card_class"] == "pir-detected":
                    LAST_MOTION_TIMES[stanza] = ora_attuale

            # Sensori di presenza mesh BLE: l'hub che li riporta E' la stanza
            # (stessa convenzione dei sensori classici). Se in una stanza ce ne
            # sono piu' di uno (mesh e/o classici), basta che UNO rilevi -> OR
            # implicito, dato che tutti scrivono lo stesso LAST_MOTION_TIMES[stanza].
            # Scarta letture troppo vecchie per evitare un "presence=1" rimasto
            # bloccato se il ponte UART/mesh smette di aggiornarsi.
            for hub, elems in MESH_STATUS.items():
                for kv in elems.values():
                    if kv.get("presence") != "1":
                        continue
                    try:
                        t = datetime.datetime.strptime(kv["time"], DT_FORMAT)
                    except Exception:
                        continue
                    if (ora_attuale - t).total_seconds() <= MESH_PRESENCE_MAX_AGE_SEC:
                        LAST_MOTION_TIMES[hub] = ora_attuale

            for area in config.get("aree", []):
                nome_area = area.get("nome")
                target_inv = float(area.get("t_inverno", 20))
                target_est = float(area.get("t_estate", 26))
                
                # Leggiamo i due timeout appena aggiunti (default 5 min per luci, 30 per HVAC)
                timeout_luci_min = float(area.get("timeout_luci", 5))
                timeout_hvac_min = float(area.get("timeout_hvac", 30))
                
                esps_in_area = [esp for esp, cfg in config.get("esps", {}).items() if cfg.get("area") == nome_area]
                if not esps_in_area: continue

                # Temperatura media: puo' mancare (area senza sensori classici, es.
                # solo mesh PIR/lux) - in tal caso semplicemente saltiamo le decisioni
                # HVAC piu' sotto, ma luci/lampade mesh (che non dipendono dalla
                # temperatura) devono comunque funzionare.
                tempi_area = [stato_esps[e]["temp"] for e in esps_in_area if stato_esps.get(e, {}).get("temp") is not None]
                temp_media = sum(tempi_area) / len(tempi_area) if tempi_area else None

                # CALCOLO PRESENZA SDOPPIATO (LUCI vs HVAC)
                presenza_luci = False
                presenza_hvac = False
                for e in esps_in_area:
                    ultimo_mov = LAST_MOTION_TIMES.get(e)
                    if ultimo_mov:
                        sec_trascorsi = (ora_attuale - ultimo_mov).total_seconds()
                        if sec_trascorsi < (timeout_luci_min * 60): presenza_luci = True
                        if sec_trascorsi < (timeout_hvac_min * 60): presenza_hvac = True

                azione_target = None
                tipo_rele_target = None
                tipo_rele_opposto = None
                target_reale = None

                if temp_media is not None and stagione == "inverno":
                    # Il target viene abbassato se non c'è presenza HVAC
                    target_reale = target_inv if presenza_hvac else (target_inv - 2.0)
                    tipo_rele_target = "hvac_caldo"
                    tipo_rele_opposto = "hvac_freddo"
                    if temp_media < (target_reale - isteresi): azione_target = "ON"
                    elif temp_media > (target_reale + isteresi): azione_target = "OFF"
                elif temp_media is not None and stagione == "estate":
                    target_reale = target_est if presenza_hvac else (target_est + 2.0)
                    tipo_rele_target = "hvac_freddo"
                    tipo_rele_opposto = "hvac_caldo"
                    if temp_media > (target_reale + isteresi): azione_target = "ON"
                    elif temp_media < (target_reale - isteresi): azione_target = "OFF"

                for esp in esps_in_area:
                    # In modalita' manuale l'utente ha il controllo esplicito: l'automazione
                    # non deve toccare i comandi di questo esp (altrimenti li sovrascrive ogni 2s).
                    if get_modalita(config, esp, "rele") == "manuale":
                        continue
                    rele_config = config["esps"][esp].get("rele", {})
                    en_list = ENABLED_RELAYS.get(esp, [True]*6)
                    
                    for num_rele, tipo in rele_config.items():
                        try:
                            idx = int(num_rele) - 1
                            if idx < 0 or idx >= len(en_list) or not en_list[idx]:
                                continue
                                
                            status_str = LAST_RELAY_STATUS.get(esp, "000000")
                            rele_is_on = (status_str[idx] == '1') if 0 <= idx < len(status_str) else False
                        except: rele_is_on = False
                        
                        if tipo == tipo_rele_target and azione_target:
                            should_be_on = (azione_target == "ON")
                            if rele_is_on != should_be_on:
                                cmd = f"RELAYCMD|{esp}|{num_rele}|{azione_target}"
                                _mqtt.publish(RELAY_TOPIC, cmd, qos=0)
                        elif tipo == tipo_rele_opposto:
                            if rele_is_on:
                                cmd = f"RELAYCMD|{esp}|{num_rele}|OFF"
                                _mqtt.publish(RELAY_TOPIC, cmd, qos=0)
                        elif tipo == "luce":
                            # LE LUCI USANO presenza_luci (quella più rapida)
                            azione_luce = "ON" if presenza_luci else "OFF"
                            should_be_on = (azione_luce == "ON")
                            if rele_is_on != should_be_on:
                                cmd = f"RELAYCMD|{esp}|{num_rele}|{azione_luce}"
                                _mqtt.publish(RELAY_TOPIC, cmd, qos=0)
                        elif tipo == "ventola":
                            if temp_media is None: continue
                            azione_ventola = None
                            if stagione == "estate":
                                soglia_ventola = target_reale - 1.5
                                if temp_media > (target_reale + isteresi): azione_ventola = "OFF"
                                elif temp_media > soglia_ventola: azione_ventola = "ON"
                                elif temp_media < (soglia_ventola - isteresi): azione_ventola = "OFF"
                            else: azione_ventola = "OFF"

                            if azione_ventola:
                                should_be_on = (azione_ventola == "ON")
                                if rele_is_on != should_be_on:
                                    cmd = f"RELAYCMD|{esp}|{num_rele}|{azione_ventola}"
                                    _mqtt.publish(RELAY_TOPIC, cmd, qos=0)

                # --- LAMPADE MESH: on/off automatico in base a presenza + lux ---
                # Soglia scelta per stagione corrente e fascia giorno/notte (impostazioni
                # generali); se l'area non ha quella soglia configurata o non c'e'
                # nessuna lettura lux valida, non tocchiamo le lampade mesh (nessun
                # dato = nessuna decisione, meglio di accendere/spegnere a caso).
                lux_key = f"lux_{'giorno' if is_giorno else 'notte'}_{stagione}"
                lux_soglia = area.get(lux_key)
                if lux_soglia is not None:
                    lux_values = []
                    for esp in esps_in_area:
                        for kv in MESH_STATUS.get(esp, {}).values():
                            try:
                                lv = float(kv.get("lux", -1))
                                if lv >= 0: lux_values.append(lv)
                            except (TypeError, ValueError):
                                pass
                    if lux_values and float(lux_soglia) > 0:
                        lux_media = sum(lux_values) / len(lux_values)
                        # Indirizzo di gruppo mesh (GROUP_ALL nel gateway): tutte le
                        # lampade ci si iscrivono in automatico in fase di config,
                        # quindi un solo comando qui = una sola trasmissione radio
                        # che le accende/dimmera' davvero tutte insieme, invece di
                        # N comandi unicast sequenziali (causa dell'accensione "a
                        # scaglioni" osservata prima).
                        MESH_GROUP_ADDR = "C000"
                        for esp in esps_in_area:
                            if get_modalita(config, esp, "mesh") == "manuale":
                                continue
                            hub_until = HUB_MESH_OVERRIDE_UNTIL.get(esp)
                            if hub_until is not None and datetime.datetime.now() < hub_until:
                                continue
                            lamps = [(a, kv) for a, kv in MESH_STATUS.get(esp, {}).items() if "onoff" in kv]
                            if not lamps:
                                continue

                            if not presenza_luci:
                                # Spegni solo le lampade NON in override manuale
                                lamps_auto = [(a, kv) for a, kv in lamps if not is_lamp_overridden(esp, a)]
                                if any(kv.get("onoff") == "1" for _, kv in lamps_auto):
                                    if not any(is_lamp_overridden(esp, a) for a, _ in lamps):
                                        # Nessun override: comando di gruppo (simultaneo, niente scaglioni)
                                        _mqtt.publish(RELAY_TOPIC, f"MESHCMD|{esp}|{MESH_GROUP_ADDR}|onoff|0", qos=0)
                                        _record_auto_cmd(esp, lamps, onoff=0)
                                    else:
                                        # Override attivo su almeno una lampada: unicast solo alle altre
                                        for a, kv in lamps_auto:
                                            if kv.get("onoff") == "1":
                                                _mqtt.publish(RELAY_TOPIC, f"MESHCMD|{esp}|{a}|onoff|0", qos=0)
                                                _record_auto_cmd(esp, [(a, kv)], onoff=0)
                                continue

                            # Dimmer: piu' luce ambientale c'e', meno serve la lampada
                            # (rampa lineare, non un taglio netto on/off)
                            pct_target = round(100 * max(0.0, 1.0 - lux_media / float(lux_soglia)))
                            pct_target = max(0, min(100, pct_target))
                            accendi_semplice = lux_media < float(lux_soglia)

                            has_any_override = any(is_lamp_overridden(esp, a) for a, _ in lamps)
                            lamps_auto = [(a, kv) for a, kv in lamps if not is_lamp_overridden(esp, a)]

                            need_on = False
                            need_pct = False
                            for _, kv in lamps_auto:
                                is_on = kv.get("onoff") == "1"
                                if "pct" in kv:
                                    try: pct_attuale = int(float(kv.get("pct")))
                                    except (TypeError, ValueError): pct_attuale = -1
                                    if not is_on: need_on = True
                                    if pct_attuale != pct_target: need_pct = True
                                else:
                                    # Niente dimmer su questa lampada: resta on/off netto sulla soglia
                                    if is_on != accendi_semplice: need_on = True

                            if not has_any_override:
                                # Nessun override: comando di gruppo (simultaneo, niente scaglioni)
                                if need_on:
                                    _mqtt.publish(RELAY_TOPIC, f"MESHCMD|{esp}|{MESH_GROUP_ADDR}|onoff|1", qos=0)
                                    _record_auto_cmd(esp, lamps_auto, onoff=1)
                                if need_pct:
                                    _mqtt.publish(RELAY_TOPIC, f"MESHCMD|{esp}|{MESH_GROUP_ADDR}|pct|{pct_target}", qos=0)
                                    _record_auto_cmd(esp, lamps_auto, pct=pct_target)
                            else:
                                # Override attivo: unicast lampada per lampada (accettabile, sono poche)
                                for a, kv in lamps_auto:
                                    is_on = kv.get("onoff") == "1"
                                    if "pct" in kv:
                                        try: pct_attuale = int(float(kv.get("pct")))
                                        except (TypeError, ValueError): pct_attuale = -1
                                        if not is_on:
                                            _mqtt.publish(RELAY_TOPIC, f"MESHCMD|{esp}|{a}|onoff|1", qos=0)
                                            _record_auto_cmd(esp, [(a, kv)], onoff=1)
                                        if pct_attuale != pct_target:
                                            _mqtt.publish(RELAY_TOPIC, f"MESHCMD|{esp}|{a}|pct|{pct_target}", qos=0)
                                            _record_auto_cmd(esp, [(a, kv)], pct=pct_target)
                                    else:
                                        if is_on != accendi_semplice:
                                            tgt = '1' if accendi_semplice else '0'
                                            _mqtt.publish(RELAY_TOPIC, f"MESHCMD|{esp}|{a}|onoff|{tgt}", qos=0)
                                            _record_auto_cmd(esp, [(a, kv)], onoff=int(tgt))
        except Exception as e: pass
        
        # HO PORTATO L'ATTESA A 2 SECONDI, REATTIVITÀ IMMEDIATA!
        time.sleep(2)

# --- ROTTE FLASK ---
@app.route("/")
def index(): return redirect(url_for("show_all"))

@app.route("/tutte")
def show_all(): return render_area_page("TUTTE")

@app.route("/area/<area_name>")
def show_area(area_name): return render_area_page(area_name)

def render_area_page(area_name):
    df = get_clean_df()
    config = load_config()
    
    lista_aree = sorted(list(set(area.get("nome") for area in config.get("aree", []) if area.get("nome"))))

    label_map = {"hvac_caldo": "Riscaldamento", "hvac_freddo": "Condizionatore", "ventola": "Ventola", "luce": "Luce"}
    
    relays_grouped = {}
    mesh_lamps_grouped = {}
    if area_name != "TUTTE":
        esps = get_esps_in_area(config, area_name)
        if esps:
            for esp in esps:
                cfg_rele = config.get("esps", {}).get(esp, {}).get("rele", {})
                status_str = LAST_RELAY_STATUS.get(esp, "000000")
                en_list = ENABLED_RELAYS.get(esp, [True]*6) # Fallback: mostrali tutti se non ho ancora info
                esp_relays = []
                for i in range(1, 7):
                    # Solo se il relè è abilitato nella lista fornita dall'ESP
                    if len(en_list) >= i and en_list[i-1]:
                        tipo = cfg_rele.get(str(i), "libero")
                        if tipo != "libero":
                            is_on = (status_str[i-1] == '1') if len(status_str) >= i else False
                            esp_relays.append({"esp": esp, "id": i, "label": label_map.get(tipo, tipo), "is_on": is_on})
                if esp_relays: relays_grouped[esp] = esp_relays
        mesh_lamps_grouped = get_mesh_lamps_grouped(config, area_name)

    # Comandi manuali: relè fisici e lampade mesh condividono il box per
    # dispositivo, ma hanno modalita' automatico/manuale indipendenti.
    def _nuovo_grp():
        return {"relays": [], "lamps": [], "modalita_rele": "auto", "modalita_mesh": "auto"}
    manual_grouped = {}
    for esp, lst in relays_grouped.items():
        manual_grouped.setdefault(esp, _nuovo_grp())["relays"] = lst
    for esp, lst in mesh_lamps_grouped.items():
        manual_grouped.setdefault(esp, _nuovo_grp())["lamps"] = lst
    for esp, grp in manual_grouped.items():
        grp["modalita_rele"] = get_modalita(config, esp, "rele")
        grp["modalita_mesh"] = get_modalita(config, esp, "mesh")
        lamps = grp.get("lamps", [])
        grp["companion_override_until"] = max(
            (l.get("companion_override_until", 0) for l in lamps), default=0
        )

    sensors = get_last_values(filter_by_area(df, area_name, config))

    # Transizione/delay del Generic Level: impostazione GLOBALE per area (non
    # piu' per singola lampada), letta dalle Impostazioni (area.mesh_trans
    # in secondi, area.mesh_delay in ms) e applicata a ogni comando pct di
    # quest'area - vedi setMeshPct() lato JS.
    area_cfg = next((a for a in config.get("aree", []) if a.get("nome") == area_name), None)
    area_mesh_trans_ms = int(float(area_cfg.get("mesh_trans", 0)) * 1000) if area_cfg else 0
    area_mesh_delay_ms = int(float(area_cfg.get("mesh_delay", 0))) if area_cfg else 0

    return render_template_string(
        HTML_TEMPLATE,
        sensors=sensors,
        lista_aree=lista_aree,
        current_area=area_name,
        ts=int(datetime.datetime.now().timestamp()),
        manual_grouped=manual_grouped,
        area_mesh_trans_ms=area_mesh_trans_ms,
        area_mesh_delay_ms=area_mesh_delay_ms,
        nome_edificio=config.get("generali", {}).get("nome_edificio") or "Edificio",
        local_ip=get_local_ip()
    )

@app.route("/config")
def show_config():
    config = load_config()
    df = get_clean_df()
    tutti_esps = sorted(list(set(get_rooms_list(df)).union(set(config.get("esps", {}).keys()))))
    return render_template_string(
        CONFIG_TEMPLATE, 
        config=config, 
        esps=tutti_esps, 
        enabled_relays=ENABLED_RELAYS
    )

@app.route("/api/config", methods=["POST"])
def api_save_config():
    try:
        save_config(request.get_json())
        return jsonify({"ok": True})
    except Exception as e: return jsonify({"ok": False}), 500

@app.route("/api/mesh_clear", methods=["POST"])
def api_mesh_clear():
    try:
        removed = sum(len(v) for v in MESH_STATUS.values())
        MESH_STATUS.clear()
        MESH_LIST_MISSES.clear()
        return jsonify({"ok": True, "removed": removed})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500

@app.route("/api/reboot", methods=["POST"])
def api_reboot():
    # Risponde subito e riavvia con un piccolo ritardo in un thread separato,
    # cosi' il browser riceve "ok":true prima che la connessione cada per il reboot.
    try:
        def _do_reboot():
            time.sleep(1)
            subprocess.run(["sudo", "reboot"], check=True)
        threading.Thread(target=_do_reboot, daemon=True).start()
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500

@app.route("/api/data/<area_name>")
def api_data(area_name):
    try:
        config = load_config()
        if area_name == "TUTTE": relays_status = LAST_RELAY_STATUS
        else:
            esps = get_esps_in_area(config, area_name)
            relays_status = {esp: LAST_RELAY_STATUS.get(esp, "000000") for esp in esps} if esps else {}
        sensors = get_last_values(filter_by_area(get_clean_df(), area_name, config))
        mesh_lamps = get_mesh_lamps_grouped(config, area_name) if area_name != "TUTTE" else {}
        esp_modes = {esp: {"rele": get_modalita(config, esp, "rele"), "mesh": get_modalita(config, esp, "mesh")}
                     for esp in config.get("esps", {})}
        return jsonify({"sensors": sensors, "relays": relays_status, "mesh_lamps": mesh_lamps, "esp_modes": esp_modes})
    except Exception as e: return jsonify({"sensors": [], "relays": {}, "mesh_lamps": {}, "esp_modes": {}}), 500

@app.route('/api/relay', methods=['POST'])
def api_relay():
    global LAST_RELAY_STATUS
    data = request.json
    esp = data.get('esp', '')
    base_cmd = data.get('command', '')  # "relay:n:ON" o "relay:n:OFF" dalla UI

    parti_cmd = base_cmd.split(':')
    if len(parti_cmd) != 3 or parti_cmd[0] != 'relay':
        return jsonify({"ok": False}), 400

    full_command = f"RELAYCMD|{esp}|{parti_cmd[1]}|{parti_cmd[2]}"
    try:
        _mqtt.publish(RELAY_TOPIC, full_command, qos=0)
        print(f"[TX MQTT] Comando manuale inviato: {full_command}")

        r_id = int(parti_cmd[1]) - 1
        r_stato = '1' if parti_cmd[2] == 'ON' else '0'
        stato_attuale = list(LAST_RELAY_STATUS.get(esp, "000000").ljust(6, '0'))
        if 0 <= r_id < 6:
            stato_attuale[r_id] = r_stato
            LAST_RELAY_STATUS[esp] = "".join(stato_attuale)
            print(f"[UI SYNC] Stato locale forzato per {esp}: {LAST_RELAY_STATUS[esp]}")

        return jsonify({"ok": True})
    except Exception as e:
        print(f"[ERRORE TX MQTT] {e}")
        return jsonify({"ok": False}), 500

@app.route("/api/mesh/<hub>")
def api_mesh_status(hub):
    return jsonify(MESH_STATUS.get(hub, {}))

@app.route('/api/mesh', methods=['POST'])
def api_mesh_cmd():
    data = request.json
    hub  = data.get('hub', '')
    addr = data.get('addr', '').upper()
    op   = data.get('op', '')    # 'onoff' o 'pct'
    val  = data.get('val', '')

    full_command = f"MESHCMD|{hub}|{addr}|{op}|{val}" if val != '' else f"MESHCMD|{hub}|{addr}|{op}"
    try:
        _mqtt.publish(RELAY_TOPIC, full_command, qos=0)
        print(f"[TX MQTT] Comando mesh inviato: {full_command}")

        # Aggiornamento ottimistico di MESH_STATUS: stesso principio dei relay
        # (LAST_RELAY_STATUS forzato in api_relay). La UI vede il nuovo stato
        # immediatamente senza aspettare il roundtrip cellulare + poll BLE (3-6s).
        # Il valore reale sovrascrivera' questo alla prossima risposta MQTT.
        now_str = datetime.datetime.now().strftime(DT_FORMAT)
        if hub not in MESH_STATUS: MESH_STATUS[hub] = {}
        if addr not in MESH_STATUS[hub]: MESH_STATUS[hub][addr] = {}
        entry = MESH_STATUS[hub][addr]
        if op == 'onoff':
            entry['onoff'] = str(val)
            entry['time']  = now_str
        elif op == 'pct':
            pct_raw = str(val).split(',')[0]  # "50,200,0" -> "50"
            try:
                pct_int = int(float(pct_raw))
                entry['pct']   = str(pct_int)
                entry['onoff'] = '1' if pct_int > 0 else '0'
                entry['time']  = now_str
            except (ValueError, TypeError):
                pass

        return jsonify({"ok": True})
    except Exception as e:
        print(f"[ERRORE TX MQTT MESH] {e}")
        return jsonify({"ok": False}), 500

@app.route('/api/esp_mode', methods=['POST'])
def api_esp_mode():
    data = request.json
    esp = data.get('esp', '')
    tipo = data.get('tipo', 'rele')  # 'rele' o 'mesh' - switch indipendenti
    modalita = data.get('modalita', 'auto')
    if modalita not in ('auto', 'manuale') or tipo not in ('rele', 'mesh') or not esp:
        return jsonify({"ok": False}), 400
    config = load_config()
    chiave = "modalita_rele" if tipo == "rele" else "modalita_mesh"
    config.setdefault("esps", {}).setdefault(esp, {})[chiave] = modalita
    save_config(config)
    return jsonify({"ok": True})

@app.route("/api/chart/<area_name>")
def api_chart(area_name):
    """Dati delle ultime 24h per i grafici interattivi (Chart.js) lato client:
    stessa finestra/filtro per area di plot_area(), ma come JSON invece di
    un PNG statico - cosi' il browser puo' fare hover sul singolo punto,
    zoom e pan senza richiedere nulla al server."""
    df = get_clean_df()
    if df.empty:
        return jsonify({"temp": [], "hum": [], "pir": [], "lux": []})

    df["data"] = pd.to_datetime(df["data"], format=DT_FORMAT, errors="coerce")
    df = df.dropna(subset=["data"])
    now = pd.Timestamp.now()
    df_today = df[df["data"] >= (now - pd.Timedelta(hours=24))].copy()

    config = load_config()
    df_today = filter_by_area(df_today, area_name, config)

    def label_for(stanza, sid):
        # Stesso lookup nome di get_last_values(): se il sensore (classico o
        # mesh PIR/lux) ha un nome assegnato, legenda e tooltip del grafico
        # devono mostrarlo invece del solo "S<id>" grezzo.
        is_mesh = len(str(sid)) == 4
        nome = MESH_SENSOR_NAMES.get(stanza, {}).get(sid) if is_mesh else SENSOR_NAMES.get(stanza, {}).get(sid)
        base = nome if nome else f"S{sid}"
        return base if area_name != "TUTTE" else f"{stanza} {base}"

    def series_for(chiavi):
        out = []
        sub = df_today[df_today["Chiave"].isin(chiavi)].copy()
        if sub.empty:
            return out
        sub["valore_num"] = pd.to_numeric(sub["valore"].astype(str).str.replace(',', '.', regex=False), errors="coerce")
        sub = sub.dropna(subset=["valore_num"]).sort_values("data")
        for (stanza, sid), g in sub.groupby(["Stanza", "Sensore"]):
            out.append({
                "label": label_for(stanza, sid),
                "data": [{"x": t.strftime("%Y-%m-%dT%H:%M:%S"), "y": v} for t, v in zip(g["data"], g["valore_num"])]
            })
        return out

    pir_series = []
    sub_pir = df_today[df_today["Chiave"].isin(["pir", "pir_state"])].copy()
    if not sub_pir.empty:
        sub_pir["is_det"] = sub_pir["valore"].apply(lambda v: 1 if str(v).strip().upper() in ["DETECTED", "1", "1.0", "1.00"] else 0)
        for (stanza, sid), g in sub_pir.groupby(["Stanza", "Sensore"]):
            g = g.sort_values("data")
            g = g[g["is_det"].diff().fillna(g["is_det"]) == 1].copy()
            if not g.empty:
                g["passaggi"] = range(1, len(g) + 1)
                pir_series.append({
                    "label": label_for(stanza, sid),
                    "data": [{"x": t.strftime("%Y-%m-%dT%H:%M:%S"), "y": int(p)} for t, p in zip(g["data"], g["passaggi"])]
                })

    return jsonify({
        "temp": series_for(["temp"]),
        "hum": series_for(["hum"]),
        "pir": pir_series,
        "lux": series_for(["lux"]),
    })

@app.route("/plot/<area_name>.png")
def plot_area(area_name):
    theme = request.args.get('theme', 'dark')
    
    if theme == 'light':
        c_bg, c_text, c_muted, c_grid = "#ffffff", "#16202e", "#5d6b80", "#e2e7f0"
    else:
        c_bg, c_text, c_muted, c_grid = "#121826", "#e6edf6", "#8a96a8", "#222b3a"

    def _empty_plot(msg="Nessun dato in memoria"):
        fig, ax = plt.subplots(figsize=(10, 2), facecolor=c_bg)
        ax.set_facecolor(c_bg)
        ax.text(0.5, 0.5, msg, ha="center", va="center", color=c_muted)
        ax.axis("off")
        buf = io.BytesIO()
        fig.savefig(buf, format="png", bbox_inches="tight", facecolor=c_bg, dpi=250)
        plt.close(fig)
        return Response(buf.getvalue(), mimetype="image/png")

    df = get_clean_df()
    if df.empty:
        return _empty_plot()

    df["data"] = pd.to_datetime(df["data"], format=DT_FORMAT, errors="coerce")
    df = df.dropna(subset=["data"])
    
    now = pd.Timestamp.now()
    df_today = df[df["data"] >= (now - pd.Timedelta(hours=24))].copy()

    config = load_config()
    df_today = filter_by_area(df_today, area_name, config)

    fig, axes = plt.subplots(4, 1, figsize=(10, 8.5), sharex=True, facecolor=c_bg)
    # right=0.8 riserva spazio fisso per le legende esterne (bbox_to_anchor sotto).
    # top/bottom riempiono la canvas: senza bbox_inches="tight" (vedi sotto) il
    # PNG usa sempre tutta la figsize, quindi i margini vanno tenuti minimi a mano.
    # NB: niente bbox_inches="tight" su questa figura - su questa versione di
    # matplotlib, "tight" combinato con legende posizionate fuori dagli assi
    # (bbox_to_anchor) ed assi temporali condivisi (sharex) genera una bbox
    # corrotta enorme e fa esplodere RendererAgg (width assurdo, crash).
    fig.subplots_adjust(hspace=0.4, right=0.8, top=0.96, bottom=0.06)

    tipi = [
        ("temp", "Temperatura (°C)", "T (°C)"),
        ("hum", "Umidità (%)", "H (%)"),
        ("pir", "PIR · Passaggi cumulativi", "Passaggi"),
        ("lux", "Luce Ambientale (lux)", "Lux")
    ]

    for ax, (chiave, titolo, ylabel) in zip(axes, tipi):
        ax.set_facecolor(c_bg)
        
        if df_today.empty:
            df_p = pd.DataFrame()
        else:
            # FIX: Cattura sia "pir" che "pir_state"
            if chiave == "pir":
                df_p = df_today[df_today["Chiave"].isin(["pir", "pir_state"])].copy()
            else:
                df_p = df_today[df_today["Chiave"] == chiave].copy()
        
        if df_p.empty:
            ax.text(0.5, 0.5, "nessun dato", ha="center", va="center", color=c_muted, fontsize=9)
        else:
            if chiave == "pir":
                df_p["is_detected"] = df_p["valore"].apply(lambda v: 1 if str(v).strip().upper() in ["DETECTED", "1", "1.0", "1.00"] else 0)
                for (stanza, sid), g in df_p.groupby(["Stanza", "Sensore"]):
                    g = g.sort_values("data")
                    g = g[g["is_detected"].diff().fillna(g["is_detected"]) == 1].copy()
                    if not g.empty:
                        g["passaggi"] = range(1, len(g) + 1)
                        lbl = f"S{sid}" if area_name != "TUTTE" else f"{stanza} S{sid}"
                        ax.step(g["data"], g["passaggi"], where="post", lw=1.5, marker="o", ms=3, label=lbl)
                ax.yaxis.set_major_locator(plt.MaxNLocator(integer=True))
            else:
                df_p["valore"] = df_p["valore"].astype(str).str.replace(',', '.', regex=False)
                df_p["valore"] = pd.to_numeric(df_p["valore"], errors="coerce")
                df_p = df_p.dropna(subset=["valore"]).sort_values("data")
                
                if df_p.empty:
                    ax.text(0.5, 0.5, "nessun dato valido", ha="center", va="center", color=c_muted, fontsize=9)
                else:
                    for (stanza, sid), g in df_p.groupby(["Stanza", "Sensore"]):
                        lbl = f"S{sid}" if area_name != "TUTTE" else f"{stanza} S{sid}"
                        ax.plot(g["data"], g["valore"], lw=2, marker=".", ms=5, label=lbl)

        ax.set_title(titolo, color=c_text, fontsize=10, loc="left", pad=6)
        ax.set_ylabel(ylabel, color=c_muted, fontsize=9)
        ax.tick_params(colors=c_muted, labelsize=8)
        for spine in ax.spines.values():
            spine.set_color(c_grid)
        ax.grid(True, linestyle="--", alpha=0.5, color=c_grid)
        
        handles, labels = ax.get_legend_handles_labels()
        if handles:
            legend = ax.legend(fontsize=7, framealpha=1.0, facecolor=c_bg, edgecolor=c_grid, loc="upper left", bbox_to_anchor=(1.02, 1))
            if legend:
                for text in legend.get_texts():
                    text.set_color(c_text)

    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    fig.autofmt_xdate(rotation=0, ha="center")

    buf = io.BytesIO()
    fig.savefig(buf, format="png", facecolor=fig.get_facecolor(), dpi=180)
    plt.close(fig)
    return Response(buf.getvalue(), mimetype="image/png")

if __name__ == "__main__":
    start_mqtt() 
    threading.Thread(target=hvac_automation_loop, daemon=True).start()
    app.run(host="0.0.0.0", port=5000, debug=False)