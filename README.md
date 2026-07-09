# 💿 Ante-Millennium OS (Ante-M) x86
# - NEWS: ora rinominato in III-Millennium OS - 
## V 0.1 Alpha - build 110

![Version](https://img.shields.io/badge/version-0.1_Alpha-blue.svg)
![Architecture](https://img.shields.io/badge/arch-x86_32--bit-red.svg)
![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)

<img width="870" height="616" alt="FLAT_BIANCO_NUOVISSIMO_TESTO_LOGO_DEFINITIVO_ANTE_M" src="https://github.com/user-attachments/assets/3dd02963-0615-4d2e-be62-85f315b9cc59" />


**Ante-Millennium OS** è un sistema operativo sperimentale a 32-bit (x86) scritto interamente da zero (bare-metal) in C e Assembly. 

Nasce come progetto di esplorazione tecnica per ricreare le complesse architetture dei sistemi operativi classici (tra cui Multitasking Preemptive, Demand Paging, Virtual File System Ext2 e Moduli Kernel Caricabili), unita a un'interfaccia grafica nativa ("Retained Mode") ispirata all'estetica iconica degli anni '90 e dei primi anni 2000. Il sistema è un ecosistema totalmente autosufficiente: non si appoggia ad alcuna libreria standard o codice di terze parti, comunica direttamente con l'hardware e include un proprio SDK (la libreria antem_libc) per lo sviluppo e la compilazione di applicazioni User-Space nel formato proprietario .edxi.


<img width="977" height="764" alt="Screenshot 2026-06-27 alle 18 02 49" src="https://github.com/user-attachments/assets/917542ad-5370-4c9f-90d5-abe792b745ad" />


---

## ✨ Architettura e Novità della Build Alpha

L'ultima evoluzione del sistema introduce cambiamenti strutturali profondi, implementando concetti avanzati di Ingegneria dei Sistemi Operativi:

* 🖥️ **Window Manager Event-Driven e Taskbar:** Il rendering dell'interfaccia grafica avviene in memoria tramite Double-Buffering per garantire l'assenza totale di flickering. Il sistema vanta un Menu Start interattivo (protetto da logiche di Z-Ordering), una Taskbar con gestione dinamica dello spazio e icone a 16 colori. Le animazioni di chiusura e riduzione a icona sono guidate da un motore asincrono basato su Interpolazione Lineare (Lerp) agganciato al timer hardware, per traiettorie fluide e pixel-perfect. Il consumo di CPU è drasticamente ottimizzato tramite un V-Sync ibrido (50/100 FPS) e uno Scheduler intelligente che addormenta i processi in Ring 3 (Syscall Yield), risvegliando rigorosamente solo l'applicazione con il focus attivo nel momento esatto dell'input utente. Le finestre supportano inoltre il ridimensionamento Content-Aware, adattando i propri limiti all'interfaccia interna.
* 🛡️ **Ring 3, Paging e Demand Paging:** Le applicazioni operano in totale isolamento nello User Mode (Ring 3), protette da un'architettura di memoria virtuale avanzata. Il sistema implementa un Physical Frame Allocator (PFA) basato su bitmap per la gestione di RAM fisica fino a 1 GB. Invece di allocazioni statiche, il Kernel utilizza il Demand Paging: la memoria viene mappata dinamicamente in pagine da 4 KB solo quando l'applicazione ne fa effettiva richiesta, gestendo i Page Fault hardware in tempo reale. Ogni processo dispone di una Page Table privata, garantendo l'integrità dei dati durante il context-switching eseguito dallo scheduler a 100 Hz. Il sistema gestisce correttamente il passaggio tra Ring tramite il Task State Segment (TSS) per la messa in sicurezza dello stack di emergenza del Kernel.
* 🗂️ **File System Ext2 e VFS:** Il driver ATA PIO è stato potenziato con il supporto LBA48 per interfacciarsi con dischi di grandi dimensioni. Al Path Parser di base è stato affiancato un Virtual File System (VFS) completo che gestisce l'allocazione dinamica di Blocchi e Inode interrogando le Bitmap dei vari Block Groups. Il traduttore di blocchi supporta nativamente i puntatori Singoli e Doppi Indiretti, abbattendo i limiti di grandezza dei file. Sono stati integrati motori complessi per l'eliminazione sicura dei dati, la distruzione ricorsiva delle directory e un sistema di copia file in streaming ad altissima efficienza (1 KB di RAM utilizzata), blindato da un VFS Lock globale (Spinlock) per prevenire la corruzione dei dati causata da operazioni di I/O concorrenti nello scheduler.
* 📚 **Libreria Standard e GUI Toolkit (antem_libc):** Le applicazioni in Ring 3 sono supportate da una potente libreria C proprietaria scritta da zero. L'ecosistema è stato arricchito da un C Runtime Bootstrapper (crt0) per l'avvio e la terminazione sicura dei processi, e da un gestore dell'Heap in User Space espanso a 1.8 MB (malloc/free). Il pacchetto include funzioni di formattazione avanzata (printf variadica), un gestore unificato per il File I/O basato su stream (fopen, fread, fwrite con buffering interno) e, soprattutto, un GUI Toolkit nativo (Retained Mode). Quest'ultimo fornisce astrazioni ad alto livello per renderizzare dinamicamente bottoni, caselle di testo interattive, impaginazione del testo e immagini a 32-bit direttamente sul Window Manager tramite l'Interrupt 0x80.

---

## 🚀 Guida all'Avvio (Quick Start)

Attualmente il sistema è distribuito sotto forma di file binari pronti per essere eseguiti in un emulatore. Si raccomanda **QEMU**.

1. Scarica l'archivio `.zip` dall'ultima [Release](https://github.com/511break-AS/Ante-Millennium-OS/releases) disponibile.
2. Estrai i file.
3. Apri un terminale nella cartella di estrazione e lancia questo comando (varia in base al sistema operativo utilizzato):

```bash
qemu-system-i386 -kernel myos.bin -drive file=disk.img,format=raw,index=0,media=disk -m 512M -device ac97
```

> **Nota Tecnica:** Il parametro `-m 512M` è la RAM, `-device ac97` specifica a QEMU riguarda l'audio.

---

## ⌨️ Comandi Terminale Disponibili

Il Terminale integrato permette di interagire direttamente con il Kernel e il File System. Digita `help` nel terminale per una lista rapida.


## 👨‍💻 L'Autore

Il creatore e sviluppatore principale di Ante-Millennium OS è **Alberto Sanfelice**.

Sul web, la sua identità digitale è storicamente legata al numero **511** (da cui deriva l'estensione proprietaria degli eseguibili di sistema, `.edxi`, dove "e" sta per executable, mentre "dxi" è il numero romano 511). 

📹 **Canale YouTube Ufficiale:** [511break](https://www.youtube.com/@511break) - *Segui i "dietro le quinte" e lo sviluppo del codice.*

---

## ⚖️ Licenza

Questo progetto è rilasciato sotto la licenza GNU General Public License v2.0.
*Copyright (c) 2026 Alberto Sanfelice (511break).*

> **Disclaimer:** Questo sistema operativo è un progetto sperimentale fornito "COSÌ COM'È", senza alcuna garanzia esplicita o implicita. Per i dettagli completi, leggi il file `LICENSE.TXT` incluso nella release.
