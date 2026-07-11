# 💿 Ante-Millennium OS (Ante-M) x86
# - NEWS: now renamed III-Millennium OS -
## V 0.1 Alpha - build 110

![Version](https://img.shields.io/badge/version-0.1_Alpha-blue.svg)
![Architecture](https://img.shields.io/badge/arch-x86_32--bit-red.svg)
![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)

<img width="870" height="616" alt="FLAT_BIANCO_NUOVISSIMO_TESTO_LOGO_DEFINITIVO_ANTE_M" src="https://github.com/user-attachments/assets/3dd02963-0615-4d2e-be62-85f315b9cc59" />


**Ante-Millennium OS** is an experimental 32-bit (x86) operating system written entirely from scratch (bare-metal) in C and Assembly.

It was born as a technical exploration project aimed at recreating the complex architectures of classic operating systems (including Preemptive Multitasking, Demand Paging, an Ext2 Virtual File System, and Loadable Kernel Modules), combined with a native graphical interface ("Retained Mode") inspired by the iconic aesthetics of the '90s and early 2000s. The system is a fully self-sufficient ecosystem: it relies on no standard library or third-party code, communicates directly with the hardware, and includes its own SDK (the antem_libc library) for developing and compiling User-Space applications in the proprietary .edxi format.


<img width="977" height="764" alt="Screenshot 2026-06-27 alle 18 02 49" src="https://github.com/user-attachments/assets/917542ad-5370-4c9f-90d5-abe792b745ad" />


---

## ✨ Architecture and What's New in the Alpha Build

The latest evolution of the system introduces deep structural changes, implementing advanced Operating Systems Engineering concepts:

* 🖥️ **Event-Driven Window Manager and Taskbar:** The graphical interface is rendered in memory via Double-Buffering to guarantee the complete absence of flickering. The system boasts an interactive Start Menu (protected by Z-Ordering logic), a Taskbar with dynamic space management, and 16-color icons. The closing and minimizing animations are driven by an asynchronous engine based on Linear Interpolation (Lerp) hooked into the hardware timer, producing smooth, pixel-perfect trajectories. CPU usage is drastically optimized through a hybrid V-Sync (50/100 FPS) and a smart Scheduler that puts Ring 3 processes to sleep (Syscall Yield), waking up strictly only the application with active focus at the exact moment of user input. Windows also support Content-Aware resizing, adapting their bounds to the internal interface.
* 🛡️ **Ring 3, Paging, and Demand Paging:** Applications operate in total isolation in User Mode (Ring 3), protected by an advanced virtual memory architecture. The system implements a bitmap-based Physical Frame Allocator (PFA) to manage physical RAM up to 1 GB. Instead of static allocations, the Kernel uses Demand Paging: memory is dynamically mapped in 4 KB pages only when the application actually requests it, handling hardware Page Faults in real time. Each process has its own private Page Table, ensuring data integrity during the context-switching performed by the 100 Hz scheduler. The system correctly handles transitions between Rings via the Task State Segment (TSS) to secure the Kernel's emergency stack.
* 🗂️ **Ext2 File System and VFS:** The ATA PIO driver has been enhanced with LBA48 support to interface with large disks. Alongside the basic Path Parser, a complete Virtual File System (VFS) has been added that manages the dynamic allocation of Blocks and Inodes by querying the Bitmaps of the various Block Groups. The block translator natively supports Single and Double Indirect pointers, breaking through file size limits. Complex engines have been integrated for secure data deletion, recursive directory destruction, and a highly efficient streaming file-copy system (using just 1 KB of RAM), safeguarded by a global VFS Lock (Spinlock) to prevent data corruption caused by concurrent I/O operations in the scheduler.
* 📚 **Standard Library and GUI Toolkit (antem_libc):** Ring 3 applications are supported by a powerful proprietary C library written from scratch. The ecosystem has been enriched with a C Runtime Bootstrapper (crt0) for safely starting and terminating processes, and with a User Space Heap manager expanded to 1.8 MB (malloc/free). The package includes advanced formatting functions (variadic printf), a unified stream-based File I/O manager (fopen, fread, fwrite with internal buffering), and, above all, a native GUI Toolkit (Retained Mode). The latter provides high-level abstractions for dynamically rendering buttons, interactive text boxes, text layout, and 32-bit images directly on the Window Manager through Interrupt 0x80.

---

## 🚀 Getting Started (Quick Start)

The system is currently distributed as binary files ready to run in an emulator. **QEMU** is recommended.

1. Download the `.zip` archive from the latest available [Release](https://github.com/511break-AS/Ante-Millennium-OS/releases).
2. Extract the files.
3. Open a terminal in the extraction folder and run this command (it varies depending on the operating system you are using):

```bash
qemu-system-i386 -kernel myos.bin -drive file=disk.img,format=raw,index=0,media=disk -m 512M -device ac97
```

> **Technical Note:** The `-m 512M` parameter sets the RAM, while `-device ac97` tells QEMU about the audio.

---

## ⌨️ Available Terminal Commands

The integrated Terminal lets you interact directly with the Kernel and the File System. Type `help` in the terminal for a quick list.


## 👨‍💻 The Author

The creator and lead developer of Ante-Millennium OS is **Alberto Sanfelice**.

On the web, his digital identity is historically tied to the number **511** (from which the proprietary system executable extension, `.edxi`, is derived, where "e" stands for executable, while "dxi" is the Roman numeral 511).

📹 **Official YouTube Channel:** [511break](https://www.youtube.com/@511break) - *Follow the "behind the scenes" and the code development.*

---

## ⚖️ License

This project is released under the GNU General Public License v2.0.
*Copyright (c) 2026 Alberto Sanfelice (511break).*

> **Disclaimer:** This operating system is an experimental project provided "AS IS", without any express or implied warranty. For full details, read the `LICENSE.TXT` file included in the release.
