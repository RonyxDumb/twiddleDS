# Twiddlefinger DS

Port per **Nintendo DS** della mod di Friday Night Funkin' **Twiddlefinger**
([sorgente originale](https://github.com/FixedData/fnf-TwiddleFinger)).

Non è un porting di HaxeFlixel: il gioco è riscritto da zero in C (libnds /
devkitARM) e usa gli asset originali della mod convertiti per il DS
(sprite, chart, eventi, canzone, video).

## Cosa c'è

- **Menu "MAX DESIGN PRO"** come nella mod: pulsanti *start* / *options*,
  poi la sequenza con la faccia e l'occhio prima della canzone.
- **Canzone completa** (inst + voci in streaming, le voci si zittiscono quando sbagli).
- **Tutti gli eventi dello stage `twiddle.hx`**: intro con le barre nere e lo
  sfondo finto, zoom della camera (`infryzoom`), shader "scary"
  (contrasto/oscurità emulato con le palette), fuoco, cambi personaggio,
  **momento "HELP"**, video dei testi con sottotitoli, *red mode*,
  finali video **buono** e **cattivo**, immagini finali.
- **HELP = 👍**: quando appare il pulsante devi **toccare il touch screen**
  (al posto del click del mouse). Servono almeno 75 tocchi prima del "judge"
  per ottenere il finale buono.
- Batteria (barra della vita) e icone come nella mod; schermo inferiore con
  punteggio, errori, precisione.
- **Pausa** (START): Resume / Restart Song / Exit to menu, con la musica "Tea Time".
- Game over, ritorno al menu a fine canzone.
- **Opzioni**: Downscroll, Ghost tapping, Camera zooms, Note offset, Botplay
  (salvate su `fat:/data/twiddleds.cfg` se la scheda SD è accessibile).

## Controlli

| Azione | Tasti |
| --- | --- |
| Frecce (sinistra, giù, su, destra) | D-pad **oppure** Y, B, X, A |
| Pausa | START |
| Menu | D-pad + A / B, oppure touch |
| HELP | tocca il pulsante sul touch screen |

## Come giocare

Scarica [`release/TwiddlefingerDS.nds`](release/TwiddlefingerDS.nds) (oppure
l'artifact della GitHub Action) e copialo sulla scheda SD della flashcart, o
aprilo con un emulatore. Consigliato **melonDS** (DeSmuME non supporta bene il
nuovo runtime di libnds 2.0).

Il gioco legge gli asset con NitroFS (incorporati nel `.nds`), quindi serve un
loader che passi `argv` (tutte le flashcart moderne con nds-bootstrap /
TWiLight Menu++ lo fanno).

## Compilare

Serve Docker (o un'installazione di devkitPro con `nds-dev`):

```sh
./build.sh           # usa l'immagine devkitpro/devkitarm
```

Gli asset già convertiti sono nel repository (`nitrofiles/`, `maxmod_data/`,
`source/gen_assets.c`). Per rigenerarli dalla mod originale:

```sh
git clone https://github.com/FixedData/fnf-TwiddleFinger mod
python3 tools/convert.py mod      # sprite, chart, eventi, font, audio (serve ffmpeg + Pillow)
python3 tools/videos.py mod       # video -> formato .vid (serve ffmpeg + un compilatore C)
```

## Come funziona (in breve)

- **Schermo superiore**: tutto è disegnato con il motore 3D del DS come quad
  texturizzati (camera con zoom, trasparenze, flash, fade). I frame dei
  personaggi sono in RAM e vengono caricati in VRAM durante il vblank solo
  quando cambiano.
- **Shader "scary"** della mod: applicato alle palette delle texture
  (è una funzione per colore, quindi il risultato è identico per i pixel opachi).
- **Video**: frame 256×192 a 256 colori compressi LZ77, letti e decompressi a
  pezzi su più frame per non perdere fluidità; il fuoco è un layer 2D affine
  in blending additivo.
- **Audio**: inst e voci (22 kHz) in streaming da NitroFS con maxmod, mixati
  in software; la posizione della canzone è sincronizzata sui campioni audio.
- **Schermo inferiore**: bitmap 16 bit disegnata in software.

`tools/melonheadless/` contiene un piccolo frontend headless per melonDS usato
per i test automatici (screenshot, input simulato, dump audio).

## Crediti

Tutti gli asset (grafica, musica, chart, video) appartengono agli autori della
mod Twiddlefinger e di Friday Night Funkin'. Questo è un progetto fan-made non
ufficiale.
