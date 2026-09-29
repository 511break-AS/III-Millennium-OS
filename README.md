# 💿 Third-Millennium OS (III-Millennium) x86

## V 0.1 Alpha

![Version](https://img.shields.io/badge/version-0.1_Alpha-blue.svg)
![Architecture](https://img.shields.io/badge/arch-x86_32--bit-red.svg)
![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)

<img width="240" height="240" alt="logo-third-millennium" src="https://github.com/user-attachments/assets/b093043b-d2cb-44cb-be4c-c2f3fd91b3f9" />




**Third-Millennium OS** is an experimental 32-bit (x86) operating system written entirely from scratch (bare-metal) in C and Assembly.

It was born as a technical exploration project aimed at recreating the architectures of classic operating systems (Preemptive Multitasking, Demand Paging, an Ext2 Virtual File System, Loadable Kernel Modules) and grew into a complete desktop environment: a glass-and-Aqua graphical interface inspired by the early 2000s, its own TCP/IP network stack, and a local AI assistant that apps can only reach with the user's explicit, physical permission.

The system is a fully self-sufficient ecosystem: it relies on no standard library or third-party code, talks directly to the hardware, and ships its own SDK (the `antem_libc` library) for developing User-Space applications in the proprietary `.edxi` format.


<img width="960" height="721" alt="Third-Millennium_OS" src="https://github.com/user-attachments/assets/68586858-cfe3-4454-be6c-6a1119d12130" />



---

## 🤖 The Robot: AI Access as a Physical Permission

Third-Millennium OS includes an AI assistant connected to a large language model running locally (through [Ollama](https://ollama.com)). What makes it different is how permission works: **no app can talk to the AI on its own.**

The little robot mascot normally rests in its tray on the taskbar. To let an application use the AI, you drag the robot with the mouse and drop it onto the tray in that app's title bar. From that moment, and only for that window, the app can send questions to the model. Drag the robot away, and the permission is gone, even in the middle of an answer.

* The permission check lives in the **kernel**, not in the app, so a buggy or malicious program cannot bypass it.
* There is only one robot, so only **one app at a time** can use the AI.
* Answers are **streamed**: the kernel writes the text into the app's text area word by word as the model generates it, without blocking the interface.

---

## ✨ Architecture and Features

* 🖥️ **Window Manager and Compositor:** The interface is rendered via Double-Buffering, with a tile-based buffer swap that only copies what changed and never touches the software cursor. Each window keeps a **render cache**: a signature of everything that affects its appearance decides whether it must be redrawn or can simply be copied, which makes dragging windows smooth even with many open apps. The taskbar has its own cache, window shadows are precomputed, and windows partially outside the screen or maximized are cached as well. Windows can be moved, minimized (with an animated "genie" effect toward their taskbar button), maximized (the title bar slides under the top panel and its controls move into it) and resized with a classic **outline frame** that shows the new size and redraws the window only once, on release.
* 🪟 **Aqua Glass Interface:** Glossy Aqua-style buttons with light rays, a glass taskbar lit from below, a glass "display" widget, soft window shadows and themes loaded from JSON files (the default theme is applied automatically at boot). The top menu bar hosts the menus of the focused application, and the redesigned **Start Menu** is a two-column glass panel: installed apps on the left with a **live search** that filters while you type, system entries on the right, and a guarded Exit button that shuts the machine down. Menu entries open on mouse release, so a click can still be cancelled by moving away.
* 🌐 **Networking from Scratch:** A complete network stack written from the ground up: **Ethernet, ARP, IPv4, ICMP (ping), a TCP client** with retransmission, and an **HTTP client** that speaks JSON with the local AI server, including streamed responses. The network card driver (Realtek **RTL8139**) was chosen because it was one of the most common NICs in PCs of the late '90s, the system's long-term hardware target.
* 🔌 **Loadable Drivers:** Drivers are separate binaries loaded from `/system/drivers` at boot and linked to the kernel through an API table. They are compiled as **position-independent flat binaries**, so they run correctly at any load address with private, isolated state. Current drivers: **AC'97 audio** and **RTL8139 network**.
* 🛡️ **Ring 3, Paging, and Demand Paging:** Applications run isolated in User Mode (Ring 3). A bitmap-based Physical Frame Allocator (PFA) manages physical RAM up to 1 GB, and the kernel uses Demand Paging: memory is mapped in 4 KB pages only when an application actually touches it, handling Page Faults in real time. Each process has its own private Page Table, preserved across context switches by the 100 Hz preemptive scheduler, and Ring transitions go through the Task State Segment (TSS). The kernel also provides a general heap and a dedicated **DMA heap** of physically contiguous memory for bus-master devices. Every way an app can end (close button, `exit()`, a crash, the `kill` command) goes through a single cleanup path, so windows, memory and the taskbar are always released consistently.
* ⌨️ **Input:** The PS/2 mouse is **interrupt-driven** (IRQ12) with a ring buffer, so no movement is lost while the system is busy drawing. Applications sleep while idle and are woken by mouse or keyboard input addressed to them.
* 🗂️ **Ext2 File System and VFS:** The ATA PIO driver supports LBA48 for large disks. A Virtual File System manages the dynamic allocation of Blocks and Inodes through the Block Group bitmaps, with Single and Double Indirect pointers. It includes secure deletion, recursive directory removal and a streaming file-copy engine that uses just 1 KB of RAM, protected by a global VFS lock against concurrent I/O.
* 🔤 **Text and Fonts:** A proportional bitmap font in regular and bold, with scaled rendering for large digits. Inside the system every character is a single byte (Latin-1): UTF-8 text coming from applications or from the AI is converted once, at the kernel boundary, so **accented letters** (à, è, é, ì, ò, ù...) display correctly while text layout and cursors keep working one byte per character.
* 📚 **Standard Library and GUI Toolkit (antem_libc):** Ring 3 applications are supported by a proprietary C library written from scratch, with a C Runtime Bootstrapper (crt0), a User-Space heap (malloc/free), variadic printf, and stream-based File I/O (fopen, fread, fwrite). Its native GUI toolkit (Retained Mode, over Interrupt 0x80) provides:
  * buttons, text labels (regular and bold), text boxes, panels and 32-bit images, placed through a 9-point **anchoring** system;
  * a **multiline text area** with word wrap, scrollbar, full cursor navigation and a read-only mode, whose text lives in the kernel (up to 8 KB per area);
  * the **glass display** widget, with large digits that automatically shrink when the text is too long;
  * **menus** in the top bar, **child windows**, separate opening and **minimum window sizes**;
  * the **robot API** (`robot_status`, `robot_ask`) to query the AI when the user grants permission.

---

## 🧩 Included Applications

* **Terminal:** direct access to the kernel and the file system.
* **Robot:** a chat with the local AI assistant, active only while the robot sits on its window.
* **Calculator:** twelve significant digits on a glass display, chain operations, pocket-style percentages and full keyboard support.
* **Notes:** a simple multiline text editor.
* **Media Player:** WAV playback through the AC'97 driver.
* **Info:** system information and credits.

---

## 🚀 Getting Started (Quick Start)

The system is distributed as binary files ready to run in an emulator. **QEMU** is recommended.

1. Download the `.zip` archive from the latest available [Release](https://github.com/511break-AS/Ante-Millennium-OS/releases).
2. Extract the files.
3. Open a terminal in the extraction folder and run:

```bash
qemu-system-i386 -kernel myos.bin -drive file=disk.img,format=raw,index=0,media=disk -m 512M -device ac97 -netdev user,id=net0 -device rtl8139,netdev=net0
```

> **Technical Note:** `-m 512M` sets the RAM, `-device ac97` enables audio, and the two `net` parameters add the RTL8139 network card on QEMU's virtual network, where the system is `10.0.2.15` and the host computer is reachable at `10.0.2.2`.

### 🤖 Enabling the AI assistant (optional)

The Robot app connects to [Ollama](https://ollama.com) running on the **same computer** as QEMU:

1. Install Ollama and download the model: `ollama pull qwen3.5:2b`
2. Keep Ollama running (it listens on port `11434` by default, no configuration needed).
3. In the system, open the Robot app, drag the robot from the taskbar onto the tray in the app's title bar, and ask your question.

> The first answer can take a while: Ollama has to load the model into memory. Later answers start almost immediately.


---

## 👨‍💻 The Author

The creator and lead developer of Third-Millennium OS is **Alberto Sanfelice**.

On the web, his digital identity is historically tied to the number **511** (from which the proprietary executable extension `.edxi` is derived: "e" stands for executable, while "dxi" is the Roman numeral 511).

📹 **Official YouTube Channel:** [511break](https://www.youtube.com/@511break) - *Follow the "behind the scenes" and the code development.*

---

## ⚖️ License

This project is released under the GNU General Public License v2.0.
*Copyright (c) 2026 Alberto Sanfelice (511break).*

> **Disclaimer:** This operating system is an experimental project provided "AS IS", without any express or implied warranty. For full details, read the `LICENSE.TXT` file included in the release.
