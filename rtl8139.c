// Third-Millennium Operating System - Driver di rete Realtek RTL8139
// Copyright (C) 2026  Alberto Sanfelice

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; version 2
// of the License.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, see
// <https://www.gnu.org/licenses/>.

// COMPILAZIONE E INSTALLAZIONE:
// i686-elf-gcc -m32 -O2 -fPIE -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -ffunction-sections -nostdlib -c rtl8139.c -o rtl8139.o
// i686-elf-ld -m elf_i386 -T driver.ld -o rtl8139.elf rtl8139.o
// i686-elf-objcopy -O binary rtl8139.elf rtl8139.drv
// e2cp rtl8139.drv disk.img:/system/drivers/rtl8139.drv
//
// TRE REGOLE PER I DRIVER COMPILATI COSI' (verificate, non teoriche):
// 1. Nessuna rilocazione a mano. Con -fPIE ogni accesso a variabili, testi e
//    funzioni e' calcolato rispetto a dove il codice si trova davvero, quindi
//    il driver funziona a qualunque indirizzo il kernel lo carichi, e le sue
//    variabili vivono dentro il driver stesso, non vicino all'indirizzo zero.
// 2. Mai un puntatore inizializzato staticamente (per esempio una tabella di
//    funzioni scritta in un inizializzatore): il compilatore ci metterebbe
//    l'indirizzo calcolato da zero. I puntatori si assegnano in _start.
// 3. _start deve essere il primo byte del file. Con -O2 il compilatore puo'
//    riordinare le funzioni: -ffunction-sections da' a ciascuna una sezione
//    propria, e driver.ld mette esplicitamente _start in testa.

// ============================================================================
// TIPI CONDIVISI CON IL KERNEL
// ============================================================================
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

// Deve coincidere campo per campo con kernel_api_t del kernel. I campi nuovi
// si aggiungono sempre IN CODA: cosi' i driver gia' compilati, che conoscono
// solo i primi campi, continuano a funzionare.
typedef struct {
    void  (*print_term)(const char*);
    void* (*kmalloc)(u32);
    void  (*kfree)(void*);
    void  (*outb)(u16, u8);
    u8    (*inb)(u16);
    void  (*outw)(u16, u16);
    u16   (*inw)(u16);
    void* (*kmalloc_dma)(u32);
    int   (*ext2_read_file)(const char*, char*, int, u32*);
    void  (*kfree_dma)(void*);
    void  (*register_audio)(void*);
    void  (*register_net)(void*);
} kernel_api_t;

// I servizi che questo driver offre al kernel. Deve coincidere con
// net_driver_t del kernel.
typedef struct {
    int  (*send)(const u8* frame, u16 len);   // 1 = inviato, 0 = slot occupato
    int  (*receive)(u8* dest, u16 max);       // byte copiati, 0 = niente in arrivo
    void (*get_mac)(u8* mac);
    u16  io;
    u8   irq;
} net_driver_t;

// ============================================================================
// REGISTRI DELLA SCHEDA (scostamenti dalla porta base)
// ============================================================================
#define RTL_VENDOR   0x10EC
#define RTL_DEVICE   0x8139

#define RTL_IDR0     0x00   // indirizzo MAC, 6 byte
#define RTL_TSD0     0x10   // stato trasmissione: 4 registri da 4 byte
#define RTL_TSAD0    0x20   // indirizzo dei buffer di trasmissione: 4 registri
#define RTL_RBSTART  0x30   // indirizzo fisico del buffer di ricezione
#define RTL_CMD      0x37
#define RTL_CAPR     0x38   // fin dove abbiamo letto nel buffer di ricezione
#define RTL_CBR      0x3A   // fin dove la scheda ha scritto
#define RTL_IMR      0x3C   // maschera degli interrupt
#define RTL_ISR      0x3E   // stato degli interrupt
#define RTL_RCR      0x44   // configurazione della ricezione
#define RTL_CONFIG1  0x52

#define CMD_BUFE     0x01   // buffer di ricezione vuoto
#define CMD_TE       0x04
#define CMD_RE       0x08
#define CMD_RST      0x10
#define TSD_OWN      (1u << 13)   // la scheda ha finito di leggere lo slot

// Buffer di ricezione: 8 KB, piu' i 16 byte di margine che la scheda richiede,
// piu' 1500: con il bit WRAP la scheda scrive l'ultimo pacchetto OLTRE la fine
// invece di spezzarlo, cosi' ogni pacchetto resta contiguo in memoria.
#define RX_RING      8192
#define RX_SIZE      (RX_RING + 16 + 1500)
#define TX_SIZE      1536

// ============================================================================
// STATO DEL DRIVER
// Tutte le variabili sono static: -fPIE le raggiunge rispetto alla posizione
// del codice, e driver.ld le include nel file anche quando valgono zero.
// ============================================================================
static kernel_api_t* kapi = 0;
static net_driver_t  ops;
static u8   pci_bus, pci_slot;
static u16  io;
static u8   mac[6];
static u8*  rx_buf;
static u8*  tx_buf[4];
static int  tx_used[4];     // lo slot e' stato usato almeno una volta
static int  tx_next;
static u16  rx_off;

// ============================================================================
// ACCESSO ALL'HARDWARE
// ============================================================================
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p) { u8  r; __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(p)); return r; }
static inline u16 inw(u16 p) { u16 r; __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(p)); return r; }
static inline u32 inl(u16 p) { u32 r; __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(p)); return r; }

static u32 pci_read(u8 bus, u8 slot, u8 off) {
    outl(0xCF8, (u32)((bus << 16) | (slot << 11) | (off & 0xFC) | 0x80000000u));
    return inl(0xCFC);
}

static void pci_write(u8 bus, u8 slot, u8 off, u32 val) {
    outl(0xCF8, (u32)((bus << 16) | (slot << 11) | (off & 0xFC) | 0x80000000u));
    outl(0xCFC, val);
}

// ============================================================================
// SERVIZI OFFERTI AL KERNEL
// ============================================================================

// Invio di un frame Ethernet completo (intestazione compresa, CRC escluso:
// lo calcola la scheda). La scheda ha quattro slot che usa a rotazione.
static int rtl_send(const u8* frame, u16 len) {
    if (len > 1514) return 0;

    int s = tx_next;
    // Uno slot gia' usato e' riutilizzabile solo quando la scheda ha finito di
    // leggerlo (bit OWN acceso). Altrimenti tutti e quattro sono in volo.
    if (tx_used[s] && !(inl(io + RTL_TSD0 + s * 4) & TSD_OWN)) return 0;

    u8* b = tx_buf[s];
    for (u16 i = 0; i < len; i++) b[i] = frame[i];
    // Ethernet non ammette frame sotto i 60 byte: si completa con zeri
    while (len < 60) b[len++] = 0;

    outl(io + RTL_TSAD0 + s * 4, (u32)b);
    outl(io + RTL_TSD0  + s * 4, len);   // scrivere la lunghezza avvia l'invio

    tx_used[s] = 1;
    tx_next = (s + 1) & 3;
    return 1;
}

// Copia il prossimo frame ricevuto. Nel buffer circolare ogni pacchetto e'
// preceduto da 4 byte della scheda: 2 di stato e 2 di lunghezza (CRC incluso).
static int rtl_receive(u8* dest, u16 max) {
    if (inb(io + RTL_CMD) & CMD_BUFE) return 0;

    u8* p = rx_buf + rx_off;
    u16 status = (u16)(p[0] | (p[1] << 8));
    u16 len    = (u16)(p[2] | (p[3] << 8));

    // Pacchetto corrotto o lunghezza impossibile: si riparte dal punto in cui
    // sta scrivendo la scheda, rinunciando a cio' che c'e' in mezzo.
    if (!(status & 0x01) || len < 18 || len > 1522) {
        rx_off = (u16)(inw(io + RTL_CBR) % RX_RING);
        outw(io + RTL_CAPR, (u16)(rx_off - 16));
        return 0;
    }

    u16 plen = (u16)(len - 4);            // senza CRC
    u16 n = plen < max ? plen : max;
    for (u16 i = 0; i < n; i++) dest[i] = p[4 + i];

    // Avanti di intestazione + pacchetto, allineati a 4 byte, dentro l'anello
    rx_off = (u16)((rx_off + len + 4 + 3) & ~3u);
    if (rx_off >= RX_RING) rx_off -= RX_RING;

    // La scheda vuole la posizione di lettura con 16 byte di differenza:
    // e' una stranezza documentata del chip, non un errore.
    outw(io + RTL_CAPR, (u16)(rx_off - 16));
    outw(io + RTL_ISR, 0x0001);           // ricezione riconosciuta
    return n;
}

static void rtl_get_mac(u8* out) {
    for (int i = 0; i < 6; i++) out[i] = mac[i];
}

// ============================================================================
// AVVIO
// ============================================================================
int _start(kernel_api_t* api, void* my_base) {
    (void)my_base;              // non serve piu': -fPIE fa tutto da solo
    kapi = api;

    // 1. Cerca la scheda sul bus PCI
    int found = 0;
    for (int b = 0; b < 256 && !found; b++) {
        for (int s = 0; s < 32; s++) {
            if (pci_read((u8)b, (u8)s, 0) == (((u32)RTL_DEVICE << 16) | RTL_VENDOR)) {
                pci_bus = (u8)b;
                pci_slot = (u8)s;
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        kapi->print_term("RTL8139: nessuna scheda trovata.");
        return 0;
    }

    // 2. Porta base e linea di interrupt
    io = (u16)(pci_read(pci_bus, pci_slot, 0x10) & ~0x3u);
    u8 irq = (u8)(pci_read(pci_bus, pci_slot, 0x3C) & 0xFF);

    // 3. Accesso alle porte (bit 0) e bus mastering (bit 2). Il secondo
    //    permette alla scheda di scrivere i pacchetti in memoria da sola.
    u32 cmd = pci_read(pci_bus, pci_slot, 0x04) & 0xFFFF;
    pci_write(pci_bus, pci_slot, 0x04, cmd | 0x0005);

    // 4. Accensione e reset: il bit RST torna a zero a reset concluso
    outb(io + RTL_CONFIG1, 0x00);
    outb(io + RTL_CMD, CMD_RST);
    for (int t = 0; t < 1000000 && (inb(io + RTL_CMD) & CMD_RST); t++) ;

    // 5. Buffer nella memoria DMA: la scheda lavora con indirizzi fisici
    rx_buf = (u8*)kapi->kmalloc_dma(RX_SIZE);
    if (!rx_buf) { kapi->print_term("RTL8139: memoria DMA esaurita."); return 0; }
    for (int i = 0; i < 4; i++) {
        tx_buf[i] = (u8*)kapi->kmalloc_dma(TX_SIZE);
        if (!tx_buf[i]) { kapi->print_term("RTL8139: memoria DMA esaurita."); return 0; }
        tx_used[i] = 0;
    }
    outl(io + RTL_RBSTART, (u32)rx_buf);

    // 6. Per ora niente interrupt: il kernel interroghera' la scheda
    outw(io + RTL_IMR, 0x0000);

    // 7. Accetta pacchetti per il nostro MAC (APM), broadcast (AB) e
    //    multicast (AM), con il WRAP descritto sopra. Niente modalita' promiscua.
    outl(io + RTL_RCR, 0x0E | 0x80);

    // 8. Ricezione e trasmissione accese
    outb(io + RTL_CMD, CMD_RE | CMD_TE);

    // 9. Indirizzo MAC, scritto nella scheda dal costruttore
    for (int i = 0; i < 6; i++) mac[i] = inb(io + RTL_IDR0 + i);

    tx_next = 0;
    rx_off = 0;

    // 10. Registrazione presso il kernel. I puntatori si assegnano QUI, a
    //     runtime (regola 2): -fPIE li calcola sulla posizione reale.
    ops.send    = rtl_send;
    ops.receive = rtl_receive;
    ops.get_mac = rtl_get_mac;
    ops.io      = io;
    ops.irq     = irq;
    kapi->register_net(&ops);

    // 11. Presentazione
    const char* hex = "0123456789ABCDEF";
    char msg[64];
    const char* pre = "RTL8139: scheda pronta, MAC ";
    int k = 0;
    while (*pre) msg[k++] = *pre++;
    for (int i = 0; i < 6; i++) {
        msg[k++] = hex[mac[i] >> 4];
        msg[k++] = hex[mac[i] & 0xF];
        if (i < 5) msg[k++] = ':';
    }
    msg[k] = '\0';
    kapi->print_term(msg);

    return 1;   // resta residente
}
