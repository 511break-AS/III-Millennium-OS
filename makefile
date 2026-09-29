CC     = i686-elf-gcc
CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra
OBJS   = boot.o kernel.o gfx.o

all: myos.bin

boot.o: boot.S
	i686-elf-as boot.S -o boot.o

%.o: %.c gfx.h
	$(CC) -c $< -o $@ $(CFLAGS)

myos.bin: $(OBJS) linker.ld
	$(CC) -T linker.ld -o myos.bin -ffreestanding -O2 -nostdlib $(OBJS) -lgcc

run: myos.bin
	qemu-system-i386 -kernel myos.bin -drive file=disk.img,format=raw,index=0,media=disk -m 512M -device ac97 -netdev user,id=net0 -device rtl8139,netdev=net0 -object filter-dump,id=cap0,netdev=net0,file=rete.pcap

clean:
	rm -f *.o myos.bin