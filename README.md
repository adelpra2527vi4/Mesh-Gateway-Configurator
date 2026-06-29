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
pairing), il firmware espone anche `CFG:SETHUBNAME`, `CFG:RELAYCFG`,
`CFG:SENSORCFG`, `CFG:RESETSENSORS`, `CFG:RESETSLOT`, `CFG:MESHSAVE` — non
nella specifica PWA originale, aggiunti perché senza di loro nome hub, relè
abilitati e configurazione dei sensori BLE classici non sarebbero più
raggiungibili da nessuna parte. Questa PWA non ha ancora una UI per quei
comandi: vanno lanciati a mano dal pannello "Comandi avanzati" componendo
la riga, oppure aggiungendo una sezione dedicata in un secondo giro.
