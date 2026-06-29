# Mesh Gateway Configurator (PWA)

Configuratore per il gateway BLE Mesh ESP32-S3, sostituisce la vecchia  
pagina web servita dal SoftAP. Comunica col gateway via **Web Serial API**
(USB CDC), protocollo a righe di testo `CFG:` (vedi `serial.js` per il
dettaglio del parsing).

## Uso

1. Apri la pagina (da GitHub Pages, o in locale con un server qualsiasi —
   serve solo per servire i file, Web Serial non passa dalla rete).
2. Collega il gateway via USB.
3. Clicca **Connetti** e scegli la porta nella finestra del browser.

## Browser supportati

Solo **Chrome** e **Edge** (≥ 89): Web Serial non è disponibile su Firefox
o Safari. La pagina mostra un banner se l'API non c'è.

## Sviluppo locale

Niente build step (HTML/CSS/JS vanilla). Per testare in locale:

```
npx serve .
```

(`python3 -m http.server` funziona per i file statici, ma Web Serial
richiede un contesto sicuro — `localhost` va bene comunque; in caso di
problemi usa `npx serve`, che e' HTTPS-capable).

## Linux: permessi porta seriale

```
sudo usermod -a -G dialout $USER
```

Poi fai logout/login prima di riprovare a connetterti.

## Deploy su GitHub Pages

Il repo ha `index.html` nella root (non in `/docs`). Bastano queste pagine
attivate nelle impostazioni del repo, branch `main` (o quello che usi),
cartella `/ (root)`.

## Protocollo

Vedi i commenti in testa a `serial.js` e i comandi inviati in `ui.js`. Il
firmware lato gateway implementa il dispatcher in `usb_cfg_handle_line()`
dentro `main.c` (progetto `provisioner_unified`).

Oltre ai comandi della specifica originale (provisioning/lampade/sensori/
pairing), il firmware espone anche un secondo gruppo di comandi pensati per
replicare la vecchia pagina web del gateway (relè, sensori BLE classici,
sniffer, nome nodo) — non nella specifica PWA originale, aggiunti per non
perdere quella funzionalità:

- `CFG:SETNAME` — nome di un nodo mesh (tab Mesh).
- `CFG:SETHUBNAME`, `CFG:RELAYCFG`, `CFG:SENSORCFG`, `CFG:RESETSENSORS`,
  `CFG:RESETSLOT`, `CFG:MESHSAVE` — configurazione hub/relè/sensori BLE
  classici (tab Setup).
- `CFG:STATUS` (poll separato da `CFG:STATE`, ogni 2s) — stato live relè
  (`CFG:RELAY`) e sensori BLE classici (`CFG:BLESENSOR`/`CFG:BLERULES`/
  `CFG:BLELAST`, righe separate perché `rules`/`last` possono contenere `;`).
- `CFG:RELAYSET` — accende/spegne un relè (stato live, diverso da
  `CFG:RELAYCFG` che ne cambia solo l'abilitazione).
- `CFG:SNIFFERSTART`/`CFG:SNIFFERSTOP`/`CFG:SNIFFERDATA` — sniffer BLE
  classico (tab Setup, poll a 1s mentre attivo): mostra tutti i dispositivi
  BLE nei dintorni con i byte del payload, evidenziando quelli che cambiano,
  per scrivere le regole di decodifica di un sensore nuovo.

Vedi `main.c` (progetto `provisioner_unified`) per l'implementazione di
questi comandi extra.
