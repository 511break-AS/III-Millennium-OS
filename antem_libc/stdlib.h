// Third-Millennium Operating System - antem_stdlib
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

// antem_libc/stdlib.h
// Libreria Standard per la gestione della Memoria in Ante-Millennium OS

// CONTRIB: 
// Andrea Simonetti - > SplitMix32 for random numbers


#ifndef _ANTEM_STDLIB_H
#define _ANTEM_STDLIB_H

// Un tipo standard per rappresentare le dimensioni in byte
typedef unsigned int size_t;

// Alloca un blocco di memoria dinamica
void* malloc(size_t size);

// Libera un blocco di memoria dinamica precedentemente allocato
void free(void* ptr);

// Termina immediatamente l'applicazione
void exit(int status) __attribute__((noreturn));


// Converte una stringa di testo in un numero intero
int atoi(const char* str);

void* realloc(void* ptr, size_t size);

// --- GENERATORE DI NUMERI CASUALI ---
#define RAND_MAX 0x7FFFFFFF

int rand(void);
void srand(unsigned int seed);

// --- TCC WRAPPERS (Mock) ---
unsigned long int strtoul(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
long int strtol(const char *nptr, char **endptr, int base);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
char *getenv(const char *name);


unsigned long long int strtoull(const char *nptr, char **endptr, int base);
long long int strtoll(const char *nptr, char **endptr, int base);
int close(int fd);
int execvp(const char *file, char *const argv[]);

#endif