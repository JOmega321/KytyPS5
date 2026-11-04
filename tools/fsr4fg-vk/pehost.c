// SPDX-License-Identifier: GPL-2.0-or-later
// pehost: a Linux host for the CPU code of AMD's FidelityFX Vulkan provider DLL (see pehost.h).
#define _GNU_SOURCE
#include "pehost.h"

#include <asm/prctl.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define W WINAPI_ABI
typedef uint16_t wchar16;

static void die(const char* what, const char* detail) {
	fprintf(stderr, "pehost: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	fflush(stderr);
	_exit(PE_EXIT_FATAL);
}

// ---------------------------------------------------------------------------------------------------- PE image
struct PeModule {
	uint8_t* base;
	size_t   size;
	uint32_t entry;
	char     path[4096];
	char     name[256]; // lowercase file name
	// TLS directory (VAs already relocated)
	uint64_t tls_start, tls_end, tls_index_va, tls_callbacks_va;
	uint32_t tls_zero_fill;
	bool     has_tls;
};

static PeModule*        g_module;
static PeImportResolver g_resolver;
static PeGdiAdapter     g_gdi;
static bool             g_gdi_set;
static uint64_t         g_minimum_address;

void PeSetMinimumAddress(uint64_t address) { g_minimum_address = address; }
void PeSetImportResolver(PeImportResolver r) { g_resolver = r; }
void PeSetGdiAdapter(const PeGdiAdapter* a) {
	g_gdi     = *a;
	g_gdi_set = true;
}

static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void lower(char* s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static void* resolve_import(const char* dll, const char* name);
static void* missing_stub(const char* dll, const char* name);

PeModule* PeLoad(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) die("cannot open DLL", path);
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsize < 0x400 || fsize > (1L << 31)) die("bad DLL size", path);
	uint8_t* file = malloc((size_t)fsize);
	if (!file || fread(file, 1, (size_t)fsize, f) != (size_t)fsize) die("cannot read DLL", path);
	fclose(f);
	if (file[0] != 'M' || file[1] != 'Z') die("not a PE file", path);
	uint32_t pe = rd32(file + 0x3c);
	if ((uint64_t)pe + 0x108 > (uint64_t)fsize || memcmp(file + pe, "PE\0\0", 4)) die("not a PE file", path);
	if (rd16(file + pe + 4) != 0x8664) die("not an x86-64 DLL", path);
	uint16_t nsec   = rd16(file + pe + 6);
	uint16_t optsz  = rd16(file + pe + 20);
	const uint8_t* opt = file + pe + 24;
	if (rd16(opt) != 0x20b) die("not PE32+", path);
	uint64_t image_base = rd64(opt + 24);
	uint32_t image_size = rd32(opt + 56);
	uint32_t header_size = rd32(opt + 60);
	const uint8_t* dirs = opt + 112;
	const uint8_t* sec  = opt + optsz;
	if (header_size > (uint32_t)fsize || (size_t)(sec - file) + 40u * nsec > (size_t)fsize) die("bad PE headers", path);

	PeModule* m = calloc(1, sizeof(*m));
	snprintf(m->path, sizeof(m->path), "%s", path);
	const char* slash = strrchr(path, '/');
	snprintf(m->name, sizeof(m->name), "%s", slash ? slash + 1 : path);
	lower(m->name);
	m->size  = image_size;
	m->entry = rd32(opt + 16);
	m->base = MAP_FAILED;
	if (image_base >= g_minimum_address)
		m->base = mmap((void*)(uintptr_t)image_base, image_size, PROT_READ | PROT_WRITE | PROT_EXEC,
		               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (m->base == MAP_FAILED)
		m->base = mmap(NULL, image_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m->base == MAP_FAILED) die("cannot map image", strerror(errno));
	if ((uint64_t)(uintptr_t)m->base < g_minimum_address) die("image mapped below the minimum address", path);
	memcpy(m->base, file, header_size);
	for (uint16_t i = 0; i < nsec; i++) {
		const uint8_t* s   = sec + 40u * i;
		uint32_t       vsz = rd32(s + 8), va = rd32(s + 12), rsz = rd32(s + 16), raw = rd32(s + 20);
		if ((uint64_t)va + (vsz > rsz ? vsz : rsz) > image_size + 0x10000ull) die("section outside image", path);
		uint32_t n = rsz < vsz || vsz == 0 ? rsz : vsz;
		if ((uint64_t)raw + n > (uint64_t)fsize) die("section outside file", path);
		if (va + n > image_size) n = image_size - va;
		memcpy(m->base + va, file + raw, n);
	}
	// Relocations
	int64_t delta = (int64_t)((uint64_t)(uintptr_t)m->base - image_base);
	uint32_t rel_rva = rd32(dirs + 5 * 8), rel_size = rd32(dirs + 5 * 8 + 4);
	if (delta && !rel_size) die("image needs relocation but has none", path);
	if (delta) {
		uint32_t at = 0;
		while (at + 8 <= rel_size) {
			uint32_t page = rd32(m->base + rel_rva + at), block = rd32(m->base + rel_rva + at + 4);
			if (block < 8 || at + block > rel_size) die("bad relocation block", path);
			for (uint32_t k = 8; k + 2 <= block; k += 2) {
				uint16_t e = rd16(m->base + rel_rva + at + k);
				int type = e >> 12;
				uint32_t off = page + (e & 0xfff);
				if (type == 0) continue;
				if (type != 10 || off + 8 > image_size) die("unsupported relocation", path);
				uint64_t v = rd64(m->base + off) + (uint64_t)delta;
				memcpy(m->base + off, &v, 8);
			}
			at += block;
		}
	}
	// Imports
	uint32_t imp_rva = rd32(dirs + 1 * 8);
	if (imp_rva) {
		for (uint8_t* d = m->base + imp_rva; rd32(d + 12); d += 20) {
			char dll[256];
			snprintf(dll, sizeof(dll), "%s", (const char*)m->base + rd32(d + 12));
			lower(dll);
			uint32_t names = rd32(d) ? rd32(d) : rd32(d + 16), iat = rd32(d + 16);
			for (uint32_t k = 0;; k++) {
				uint64_t entry = rd64(m->base + names + 8u * k);
				if (!entry) break;
				if (entry >> 63) die("import by ordinal unsupported", dll);
				const char* fname = (const char*)m->base + (uint32_t)entry + 2;
				void* fn = resolve_import(dll, fname);
				if (!fn) fn = missing_stub(dll, fname);
				uint64_t v = (uint64_t)(uintptr_t)fn;
				memcpy(m->base + iat + 8u * k, &v, 8);
			}
		}
	}
	// TLS
	uint32_t tls_rva = rd32(dirs + 9 * 8);
	if (tls_rva) {
		const uint8_t* t = m->base + tls_rva;
		m->has_tls          = true;
		m->tls_start        = rd64(t);
		m->tls_end          = rd64(t + 8);
		m->tls_index_va     = rd64(t + 16);
		m->tls_callbacks_va = rd64(t + 24);
		m->tls_zero_fill    = rd32(t + 32);
		if (m->tls_index_va) *(uint32_t*)(uintptr_t)m->tls_index_va = 0;
	}
	free(file);
	g_module = m;
	fprintf(stderr, "pehost: loaded %s at %p (%s base)\n", m->name, (void*)m->base, delta ? "relocated" : "preferred");
	return m;
}

void PeImageRange(const PeModule* m, uintptr_t* base, size_t* size) {
	*base = (uintptr_t)m->base;
	*size = m->size;
}

void* PeExport(PeModule* m, const char* name) {
	uint32_t pe = rd32(m->base + 0x3c);
	const uint8_t* dirs = m->base + pe + 24 + 112;
	uint32_t exp = rd32(dirs);
	if (!exp) return NULL;
	const uint8_t* e = m->base + exp;
	uint32_t n = rd32(e + 24), funcs = rd32(e + 28), names = rd32(e + 32), ords = rd32(e + 36);
	for (uint32_t i = 0; i < n; i++)
		if (!strcmp((const char*)m->base + rd32(m->base + names + 4 * i), name))
			return m->base + rd32(m->base + funcs + 4u * rd16(m->base + ords + 2 * i));
	return NULL;
}

// ---------------------------------------------------------------------------------------------------- threads/TEB
typedef struct Teb {
	uint8_t raw[0x2000];
} Teb;
static uint8_t g_peb[0x1000];

// The module's implicit TLS block for the calling thread (TEB->ThreadLocalStoragePointer[0]).
static void install_tls(Teb* teb) {
	if (!g_module || !g_module->has_tls) return;
	uint64_t v;
	memcpy(&v, teb->raw + 0x58, 8);
	if (v) return;
	size_t raw = (size_t)(g_module->tls_end - g_module->tls_start);
	uint8_t* block = calloc(1, raw + g_module->tls_zero_fill + 64);
	memcpy(block, (void*)(uintptr_t)g_module->tls_start, raw);
	void** slots = calloc(64, sizeof(void*));
	slots[0] = block;
	v = (uint64_t)(uintptr_t)slots;
	memcpy(teb->raw + 0x58, &v, 8);
}
static Teb* current_teb(void) {
	unsigned long base = 0;
	if (syscall(SYS_arch_prctl, ARCH_GET_GS, &base) || !base) die("not on a thread prepared by PeRunOnWindowsThread", NULL);
	return (Teb*)(uintptr_t)base;
}

static void setup_teb(void* stack_low, void* stack_high) {
	Teb* teb = calloc(1, sizeof(Teb));
	uint64_t v;
	v = (uint64_t)(uintptr_t)stack_high; memcpy(teb->raw + 0x08, &v, 8); // StackBase
	v = (uint64_t)(uintptr_t)stack_low;  memcpy(teb->raw + 0x10, &v, 8); // StackLimit
	v = (uint64_t)(uintptr_t)teb;        memcpy(teb->raw + 0x30, &v, 8); // Self
	v = (uint64_t)getpid();              memcpy(teb->raw + 0x40, &v, 8);
	v = (uint64_t)syscall(SYS_gettid);   memcpy(teb->raw + 0x48, &v, 8);
	v = (uint64_t)(uintptr_t)g_peb;      memcpy(teb->raw + 0x60, &v, 8);
	install_tls(teb);
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)(uintptr_t)teb)) die("arch_prctl(ARCH_SET_GS) failed", strerror(errno));
}

typedef struct ThreadStart {
	int (*fn)(void*);
	void* arg;
	int   result;
	void* stack;
	size_t stack_size;
} ThreadStart;

static void* host_thread(void* p) {
	ThreadStart* s = p;
	setup_teb(s->stack, (uint8_t*)s->stack + s->stack_size);
	s->result = s->fn(s->arg);
	return NULL;
}

static pthread_t spawn(ThreadStart* s) {
	s->stack_size = 64u << 20;
	s->stack = mmap(NULL, s->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (s->stack == MAP_FAILED) die("thread stack", strerror(errno));
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstack(&a, s->stack, s->stack_size);
	pthread_t t;
	if (pthread_create(&t, &a, host_thread, s)) die("pthread_create", NULL);
	pthread_attr_destroy(&a);
	return t;
}

int PeRunOnWindowsThread(int (*fn)(void*), void* arg) {
	ThreadStart s = {fn, arg, 0, NULL, 0};
	pthread_t t = spawn(&s);
	pthread_join(t, NULL);
	return s.result;
}

typedef W void (*TlsCallback)(void*, uint32_t, void*);
typedef W int (*DllMainFn)(void*, uint32_t, void*);

int PeAttach(PeModule* m) {
	memcpy(g_peb + 0x10, &m->base, 8); // ImageBaseAddress (the DLL stands in for the process image)
	install_tls(current_teb());        // (the loading thread's TEB predates the module)
	if (m->has_tls && m->tls_callbacks_va)
		for (uint64_t* cb = (uint64_t*)(uintptr_t)m->tls_callbacks_va; *cb; cb++) ((TlsCallback)(uintptr_t)*cb)(m->base, 1, NULL);
	if (!m->entry) return 1;
	return ((DllMainFn)(void*)(m->base + m->entry))(m->base, 1, NULL);
}

// ---------------------------------------------------------------------------------------------------- handles/sync
enum { H_EVENT = 1, H_THREAD = 2 };
typedef struct Handle {
	int  type;
	bool manual, signaled;
	pthread_mutex_t m;
	pthread_cond_t  c;
	ThreadStart     start;
	pthread_t       thread;
} Handle;

static Handle* new_handle(int type) {
	Handle* h = calloc(1, sizeof(Handle));
	h->type = type;
	pthread_mutex_init(&h->m, NULL);
	pthread_cond_init(&h->c, NULL);
	return h;
}
static void signal_handle(Handle* h) {
	pthread_mutex_lock(&h->m);
	h->signaled = true;
	pthread_cond_broadcast(&h->c);
	pthread_mutex_unlock(&h->m);
}

static pthread_mutex_t g_sync = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_sync_cond = PTHREAD_COND_INITIALIZER;
static uint64_t        g_cv_generation;

static void deadline(struct timespec* ts, uint32_t ms) {
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

// ---------------------------------------------------------------------------------------------------- string helpers
static size_t wlen16(const wchar16* s) { size_t n = 0; if (s) while (s[n]) n++; return n; }
static void utf16_to_utf8(const wchar16* s, size_t n, char* out, size_t cap) {
	size_t o = 0;
	for (size_t i = 0; i < n && o + 4 < cap; i++) {
		uint32_t c = s[i];
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) { c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00); i++; }
		if (c < 0x80) out[o++] = (char)c;
		else if (c < 0x800) { out[o++] = (char)(0xc0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
		else if (c < 0x10000) { out[o++] = (char)(0xe0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
		else { out[o++] = (char)(0xf0 | c >> 18); out[o++] = (char)(0x80 | (c >> 12 & 63)); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
	}
	out[o] = 0;
}
static size_t utf8_to_utf16(const char* s, size_t n, wchar16* out, size_t cap) {
	size_t o = 0;
	for (size_t i = 0; i < n;) {
		uint32_t c = (uint8_t)s[i++];
		int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
		if (extra) c &= 0x3f >> extra;
		while (extra-- && i < n) c = c << 6 | ((uint8_t)s[i++] & 63);
		if (c >= 0x10000) {
			if (out && o + 1 < cap) { out[o] = (wchar16)(0xd800 + ((c - 0x10000) >> 10)); out[o + 1] = (wchar16)(0xdc00 + ((c - 0x10000) & 0x3ff)); }
			o += 2;
		} else {
			if (out && o < cap) out[o] = (wchar16)c;
			o++;
		}
	}
	return o;
}

// Windows printf family: the format is parsed here, each conversion formatted by glibc from the Windows va_list
// (a char* walking 8-byte argument slots; doubles are passed in the slot as their bits).
static int win_format(char* out, size_t cap, const void* fmt, bool wide_fmt, bool wide_out, uint8_t* args) {
	size_t o = 0;
#define PUT(ch) do { if (o + 1 < cap) out[o] = (char)(ch); o++; } while (0)
#define NEXT64() ({ uint64_t _v; memcpy(&_v, args, 8); args += 8; _v; })
	size_t i = 0;
	for (;;) {
		uint32_t c = wide_fmt ? ((const wchar16*)fmt)[i] : (uint8_t)((const char*)fmt)[i];
		if (!c) break;
		i++;
		if (c != '%') {
			if (c < 0x80) PUT(c);
			else { char tmp[8]; wchar16 w = (wchar16)c; utf16_to_utf8(&w, 1, tmp, sizeof(tmp)); for (char* p = tmp; *p; p++) PUT(*p); }
			continue;
		}
		char spec[64];
		size_t sl = 0;
		spec[sl++] = '%';
#define FC() (wide_fmt ? ((const wchar16*)fmt)[i] : (uint8_t)((const char*)fmt)[i])
		while (strchr("-+ #0", (int)FC()) && FC()) spec[sl++] = (char)FC(), i++;
		if (FC() == '*') { int v = (int)NEXT64(); sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", v); i++; }
		else while (FC() >= '0' && FC() <= '9') spec[sl++] = (char)FC(), i++;
		if (FC() == '.') {
			spec[sl++] = '.'; i++;
			if (FC() == '*') { int v = (int)NEXT64(); sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", v); i++; }
			else while (FC() >= '0' && FC() <= '9') spec[sl++] = (char)FC(), i++;
		}
		int size = 0; // 0 int, 1 long long/64, -1 short, -2 char, 2 long double
		int str_wide = -1;
		for (;;) {
			uint32_t m = FC();
			if (m == 'l') { i++; if (FC() == 'l') { i++; size = 1; } else { size = 0; str_wide = 1; } }
			else if (m == 'h') { i++; if (FC() == 'h') { i++; size = -2; } else { size = -1; str_wide = 0; } }
			else if (m == 'I' && (((wide_fmt ? ((const wchar16*)fmt)[i + 1] : ((const char*)fmt)[i + 1]) == '6'))) { i += 3; size = 1; }
			else if (m == 'I' && (((wide_fmt ? ((const wchar16*)fmt)[i + 1] : ((const char*)fmt)[i + 1]) == '3'))) { i += 3; size = 0; }
			else if (m == 'I' || m == 'z' || m == 'j' || m == 't') { i++; size = 1; }
			else if (m == 'L') { i++; size = 2; }
			else if (m == 'w') { i++; str_wide = 1; }
			else break;
		}
		uint32_t conv = FC();
		if (!conv) break;
		i++;
		char tmp[1024];
		tmp[0] = 0;
		switch (conv) {
		case '%': PUT('%'); continue;
		case 'd': case 'i': {
			int64_t v = (int64_t)NEXT64();
			if (size == 0) v = (int32_t)v; else if (size == -1) v = (int16_t)v; else if (size == -2) v = (int8_t)v;
			memcpy(spec + sl, "lld", 4);
			snprintf(tmp, sizeof(tmp), spec, (long long)v);
			break;
		}
		case 'u': case 'x': case 'X': case 'o': {
			uint64_t v = NEXT64();
			if (size == 0) v = (uint32_t)v; else if (size == -1) v = (uint16_t)v; else if (size == -2) v = (uint8_t)v;
			spec[sl] = 'l'; spec[sl + 1] = 'l'; spec[sl + 2] = (char)conv; spec[sl + 3] = 0;
			snprintf(tmp, sizeof(tmp), spec, (unsigned long long)v);
			break;
		}
		case 'p': snprintf(tmp, sizeof(tmp), "%016llX", (unsigned long long)NEXT64()); break;
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
			uint64_t bits = NEXT64();
			double d;
			memcpy(&d, &bits, 8);
			spec[sl] = (char)conv; spec[sl + 1] = 0;
			snprintf(tmp, sizeof(tmp), spec, d);
			break;
		}
		case 'c': case 'C': {
			uint64_t v = NEXT64();
			bool wide = conv == 'C' ? !wide_fmt : (str_wide == 1 || (str_wide == -1 && wide_fmt));
			wchar16 w = (wchar16)v;
			if (wide) utf16_to_utf8(&w, 1, tmp, sizeof(tmp)); else { tmp[0] = (char)v; tmp[1] = 0; }
			break;
		}
		case 's': case 'S': {
			const void* p = (const void*)(uintptr_t)NEXT64();
			bool wide = str_wide == 1 ? true : str_wide == 0 ? false : (conv == 'S') != wide_fmt;
			char s8[2048];
			if (!p) snprintf(s8, sizeof(s8), "(null)");
			else if (wide) utf16_to_utf8(p, wlen16(p), s8, sizeof(s8));
			else snprintf(s8, sizeof(s8), "%s", (const char*)p);
			spec[sl] = 's'; spec[sl + 1] = 0;
			snprintf(tmp, sizeof(tmp), spec, s8);
			break;
		}
		case 'n': (void)NEXT64(); break;
		default: snprintf(tmp, sizeof(tmp), "%%%c", (char)conv); break;
		}
		for (char* p = tmp; *p; p++) PUT(*p);
#undef FC
	}
	if (cap) out[o < cap ? o : cap - 1] = 0;
	(void)wide_out;
	return (int)o;
#undef PUT
#undef NEXT64
}

// ---------------------------------------------------------------------------------------------------- KERNEL32
static __thread uint32_t t_last_error;
#define HOST_HANDLE_GDI32 ((void*)(uintptr_t)0x7ff0000010000ull)
#define HOST_HANDLE_EXE   ((void*)(uintptr_t)0x7ff0000020000ull)

W static int k_WideCharToMultiByte(uint32_t cp, uint32_t flags, const wchar16* s, int n, char* out, int cap, const char* def, int* used) {
	(void)cp; (void)flags; (void)def;
	if (used) *used = 0;
	size_t len = n < 0 ? wlen16(s) + 1 : (size_t)n;
	char* tmp = malloc(len * 4 + 8);
	utf16_to_utf8(s, len, tmp, len * 4 + 8);
	size_t bytes = strlen(tmp) + (n < 0 ? 1 : 0);
	if (cap == 0) { free(tmp); return (int)bytes; }
	if ((size_t)cap < bytes) { free(tmp); t_last_error = 122; return 0; }
	memcpy(out, tmp, bytes);
	free(tmp);
	return (int)bytes;
}
W static int k_MultiByteToWideChar(uint32_t cp, uint32_t flags, const char* s, int n, wchar16* out, int cap) {
	(void)cp; (void)flags;
	size_t len = n < 0 ? strlen(s) + 1 : (size_t)n;
	size_t need = utf8_to_utf16(s, len, NULL, 0);
	if (cap == 0) return (int)need;
	if ((size_t)cap < need) { t_last_error = 122; return 0; }
	utf8_to_utf16(s, len, out, (size_t)cap);
	return (int)need;
}
W static void k_OutputDebugStringW(const wchar16* s) {
	char b[4096];
	utf16_to_utf8(s, wlen16(s), b, sizeof(b));
	fprintf(stderr, "[provider debug] %s", b);
	if (!*b || b[strlen(b) - 1] != '\n') fputc('\n', stderr);
}
W static int32_t k_SetThreadDescription(void* h, const wchar16* s) { (void)h; (void)s; return 0; }
W static int k_SetThreadPriority(void* h, int p) { (void)h; (void)p; return 1; }
// CRITICAL_SECTION (40 bytes, caller-owned): its first 8 bytes point at a recursive pthread mutex.
W static void k_InitializeCriticalSection(void* cs) {
	pthread_mutex_t* m = malloc(sizeof(*m));
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(m, &a);
	memset(cs, 0, 40);
	memcpy(cs, &m, 8);
}
static pthread_mutex_t* cs_mutex(void* cs) { pthread_mutex_t* m; memcpy(&m, cs, 8); if (!m) die("uninitialized CRITICAL_SECTION", NULL); return m; }
W static void k_EnterCriticalSection(void* cs) { pthread_mutex_lock(cs_mutex(cs)); }
W static void k_LeaveCriticalSection(void* cs) { pthread_mutex_unlock(cs_mutex(cs)); }
W static void k_DeleteCriticalSection(void* cs) { pthread_mutex_t* m = cs_mutex(cs); pthread_mutex_destroy(m); free(m); memset(cs, 0, 8); }
W static void* k_CreateEventW(void* attr, int manual, int initial, const wchar16* name) {
	(void)attr; (void)name;
	Handle* h = new_handle(H_EVENT);
	h->manual   = manual != 0;
	h->signaled = initial != 0;
	return h;
}
W static int k_SetEvent(Handle* h) { if (!h || h->type != H_EVENT) die("SetEvent on a non-event handle", NULL); signal_handle(h); return 1; }
W static uint32_t k_WaitForSingleObject(Handle* h, uint32_t ms) {
	if (!h || (h->type != H_EVENT && h->type != H_THREAD)) die("WaitForSingleObject: unsupported handle", NULL);
	pthread_mutex_lock(&h->m);
	struct timespec ts;
	if (ms != 0xffffffffu) deadline(&ts, ms);
	while (!h->signaled) {
		if (ms == 0xffffffffu) pthread_cond_wait(&h->c, &h->m);
		else if (pthread_cond_timedwait(&h->c, &h->m, &ts) == ETIMEDOUT) break;
	}
	bool ok = h->signaled;
	if (ok && h->type == H_EVENT && !h->manual) h->signaled = false;
	pthread_mutex_unlock(&h->m);
	return ok ? 0 : 0x102;
}
W static int k_CloseHandle(void* h) { (void)h; return 1; }
typedef W uint32_t (*WinThreadFn)(void*);
typedef struct WinThreadArg { WinThreadFn fn; void* arg; Handle* handle; } WinThreadArg;
static int win_thread_body(void* p) {
	WinThreadArg* a = p;
	uint32_t r = a->fn(a->arg);
	signal_handle(a->handle);
	return (int)r;
}
W static void* k_CreateThread(void* attr, size_t stack, WinThreadFn fn, void* arg, uint32_t flags, uint32_t* tid) {
	(void)attr; (void)stack;
	if (flags & 4) die("CreateThread: suspended threads are not supported", NULL);
	Handle* h = new_handle(H_THREAD);
	WinThreadArg* a = malloc(sizeof(*a));
	a->fn = fn; a->arg = arg; a->handle = h;
	h->start.fn = win_thread_body;
	h->start.arg = a;
	h->thread = spawn(&h->start);
	pthread_detach(h->thread);
	if (tid) *tid = (uint32_t)(uintptr_t)h;
	fprintf(stderr, "pehost: provider started a thread\n");
	return h;
}
W static int k_QueryPerformanceFrequency(int64_t* f) { *f = 1000000000LL; return 1; }
W static int k_QueryPerformanceCounter(int64_t* c) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); *c = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; return 1; }
W static uint64_t k_GetTickCount64(void) { int64_t c; k_QueryPerformanceCounter(&c); return (uint64_t)c / 1000000ull; }
W static void k_GetSystemTimeAsFileTime(uint64_t* ft) {
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	*ft = (uint64_t)t.tv_sec * 10000000ull + (uint64_t)t.tv_nsec / 100 + 116444736000000000ull;
}
W static uint32_t k_GetModuleFileNameA(void* h, char* out, uint32_t cap) {
	const char* p = (h == NULL || h == HOST_HANDLE_EXE) ? "C:\\kyty\\fsr4fg-recorder.exe" : (g_module && h == g_module->base) ? g_module->path : "";
	uint32_t n = (uint32_t)strlen(p);
	if (!cap) return 0;
	snprintf(out, cap, "%s", p);
	return n < cap ? n : cap;
}
static bool name_is(const char* a, const char* b) {
	const char* s = strrchr(a, '\\'); if (s) a = s + 1;
	s = strrchr(a, '/'); if (s) a = s + 1;
	return !strcasecmp(a, b);
}
static void* load_library(const char* name) {
	if (!name) return NULL;
	if (g_module && name_is(name, g_module->name)) return g_module->base;
	if (name_is(name, "gdi32.dll") && g_gdi_set) { fprintf(stderr, "pehost: LoadLibrary(%s) -> synthetic GDI adapter queries\n", name); return HOST_HANDLE_GDI32; }
	fprintf(stderr, "pehost: LoadLibrary(%s) -> not available\n", name);
	t_last_error = 126;
	return NULL;
}
W static void* k_LoadLibraryA(const char* name) { return load_library(name); }
W static void* k_LoadLibraryExA(const char* name, void* f, uint32_t flags) { (void)f; (void)flags; return load_library(name); }
W static void* k_GetModuleHandleA(const char* name) {
	if (!name) return HOST_HANDLE_EXE;
	if (g_module && name_is(name, g_module->name)) return g_module->base;
	return NULL;
}
W static int k_FreeLibrary(void* h) { (void)h; return 1; }
// Synthetic Windows adapter queries (as tools/fsr412-vk/capture_main.c answers them): one adapter, the
// configured ASIC family and revision. No physical GPU is involved.
W static int32_t gdi_EnumAdapters(void* out) {
	uint8_t* p = out;
	memset(p, 0, 324);
	*(uint32_t*)p = 1;
	*(uint32_t*)(p + 4) = 1;
	memset(p + 8, 3, 8);
	return 0;
}
W static int32_t gdi_QueryAdapterInfo(void* query) {
	uint8_t* p = query;
	void* data; memcpy(&data, p + 8, 8);
	uint32_t size = *(uint32_t*)(p + 16);
	if (*(uint32_t*)(p + 4) != 0 || size < 0x38 || size > 0x100000 || !data) return (int32_t)0xc000000d;
	memset(data, 0, size);
	*(uint32_t*)((uint8_t*)data + 0x30) = g_gdi.asic_family;
	*(uint32_t*)((uint8_t*)data + 0x34) = g_gdi.asic_revision;
	fprintf(stderr, "pehost: SIMULATED Windows ASIC query: family=%u revision=%u (no physical GPU)\n", g_gdi.asic_family, g_gdi.asic_revision);
	return 0;
}
W static void* k_GetProcAddress(void* h, const char* name) {
	if ((uintptr_t)name < 65536) die("GetProcAddress by ordinal", NULL);
	if (h == HOST_HANDLE_GDI32) {
		if (!strcmp(name, "D3DKMTEnumAdapters")) return (void*)gdi_EnumAdapters;
		if (!strcmp(name, "D3DKMTQueryAdapterInfo")) return (void*)gdi_QueryAdapterInfo;
		return NULL;
	}
	if (g_module && h == g_module->base) return PeExport(g_module, name);
	fprintf(stderr, "pehost: GetProcAddress(%p, %s) -> not available\n", h, name);
	return NULL;
}
W static uint32_t k_GetCurrentProcessId(void) { return (uint32_t)getpid(); }
W static uint32_t k_GetCurrentThreadId(void) { return (uint32_t)syscall(SYS_gettid); }
W static void* k_GetCurrentProcess(void) { return (void*)(intptr_t)-1; }
W static int k_IsDebuggerPresent(void) { return 0; }
W static void k_InitializeSListHead(void* p) { memset(p, 0, 16); }
W static int k_DisableThreadLibraryCalls(void* h) { (void)h; return 1; }
W static int k_IsProcessorFeaturePresent(uint32_t f) { return f == 23 ? 0 : 1; } // 23: PF_FASTFAIL_AVAILABLE
W static int k_TerminateProcess(void* p, uint32_t code) { (void)p; fprintf(stderr, "pehost: provider terminated the process (%u)\n", code); _exit(4); }
W static void* k_SetUnhandledExceptionFilter(void* f) { (void)f; return NULL; }
W static int32_t k_UnhandledExceptionFilter(void* e) { (void)e; die("provider raised an unhandled exception", NULL); return 0; }
W static void k_RtlCaptureContext(void* c) { (void)c; die("provider reported a fatal error (security check or fast fail)", NULL); }
W static void* k_RtlLookupFunctionEntry(uint64_t pc, uint64_t* base, void* table) { (void)pc; (void)table; if (base) *base = 0; return NULL; }
W static void* k_RtlVirtualUnwind(void) { die("provider attempted stack unwinding", NULL); return NULL; }
// SRW locks and condition variables (pointer-sized words in caller memory): one global mutex and condition.
W static void k_AcquireSRWLockExclusive(uintptr_t* l) {
	pthread_mutex_lock(&g_sync);
	while (*l) pthread_cond_wait(&g_sync_cond, &g_sync);
	*l = 1;
	pthread_mutex_unlock(&g_sync);
}
W static void k_ReleaseSRWLockExclusive(uintptr_t* l) {
	pthread_mutex_lock(&g_sync);
	*l = 0;
	pthread_cond_broadcast(&g_sync_cond);
	pthread_mutex_unlock(&g_sync);
}
W static int k_SleepConditionVariableSRW(uintptr_t* cv, uintptr_t* l, uint32_t ms, uint32_t flags) {
	(void)cv;
	if (flags) die("shared SRW condition waits are not supported", NULL);
	pthread_mutex_lock(&g_sync);
	*l = 0;
	uint64_t gen = g_cv_generation;
	pthread_cond_broadcast(&g_sync_cond);
	struct timespec ts;
	if (ms != 0xffffffffu) deadline(&ts, ms);
	bool timed_out = false;
	while (gen == g_cv_generation && !timed_out) {
		if (ms == 0xffffffffu) pthread_cond_wait(&g_sync_cond, &g_sync);
		else timed_out = pthread_cond_timedwait(&g_sync_cond, &g_sync, &ts) == ETIMEDOUT;
	}
	while (*l) pthread_cond_wait(&g_sync_cond, &g_sync);
	*l = 1;
	pthread_mutex_unlock(&g_sync);
	if (timed_out) { t_last_error = 1460; return 0; }
	return 1;
}
W static void k_WakeAllConditionVariable(uintptr_t* cv) {
	(void)cv;
	pthread_mutex_lock(&g_sync);
	g_cv_generation++;
	pthread_cond_broadcast(&g_sync_cond);
	pthread_mutex_unlock(&g_sync);
}

// ---------------------------------------------------------------------------------------------------- USER32/VERSION
W static int64_t u_zero(void) { return 0; }
W static uint32_t v_GetFileVersionInfoSizeA(const char* f, uint32_t* h) {
	fprintf(stderr, "pehost: GetFileVersionInfoSizeA(%s) -> none\n", f ? f : "(null)");
	if (h) *h = 0;
	t_last_error = 2;
	return 0;
}

// ---------------------------------------------------------------------------------------------------- CRT/VCRUNTIME/MSVCP
W static void* c_malloc(size_t n) { return malloc(n); }
W static void* c_calloc(size_t n, size_t s) { return calloc(n, s); }
W static void  c_free(void* p) { free(p); }
W static int   c_callnewh(size_t n) { (void)n; return 0; }
W static void* c_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
W static void* c_memmove(void* d, const void* s, size_t n) { return memmove(d, s, n); }
W static void* c_memset(void* d, int v, size_t n) { return memset(d, v, n); }
W static int   c_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
W static int   c_strcmp(const char* a, const char* b) { return strcmp(a, b); }
W static int c_strncpy_s(char* d, size_t cap, const char* s, size_t count) {
	if (!d || !cap) return 22;
	size_t n = strnlen(s, count == (size_t)-1 ? cap - 1 : count);
	if (n >= cap) { d[0] = 0; return 80; }
	memcpy(d, s, n);
	d[n] = 0;
	return 0;
}
W static int c_wcscpy_s(wchar16* d, size_t cap, const wchar16* s) {
	size_t n = wlen16(s);
	if (!d || n >= cap) { if (d && cap) d[0] = 0; return 34; }
	memcpy(d, s, (n + 1) * 2);
	return 0;
}
W static int c_getenv_s(size_t* req, char* buf, size_t cap, const char* name) {
	const char* v = getenv(name);
	fprintf(stderr, "pehost: provider read environment variable %s%s\n", name, v ? " (set)" : "");
	if (!v) { *req = 0; if (buf && cap) buf[0] = 0; return 0; }
	*req = strlen(v) + 1;
	if (!buf || cap < *req) return buf ? 34 : 0;
	memcpy(buf, v, *req);
	return 0;
}
W static int c_wgetenv_s(size_t* req, wchar16* buf, size_t cap, const wchar16* name) {
	char n8[512];
	utf16_to_utf8(name, wlen16(name), n8, sizeof(n8));
	const char* v = getenv(n8);
	fprintf(stderr, "pehost: provider read environment variable %s%s\n", n8, v ? " (set)" : "");
	if (!v) { *req = 0; if (buf && cap) buf[0] = 0; return 0; }
	size_t need = utf8_to_utf16(v, strlen(v), NULL, 0) + 1;
	*req = need;
	if (!buf || cap < need) return buf ? 34 : 0;
	utf8_to_utf16(v, strlen(v), buf, cap);
	buf[need - 1] = 0;
	return 0;
}
W static int c_stdio_common_vsprintf(uint64_t options, char* buf, size_t cap, const char* fmt, void* locale, uint8_t* args) {
	(void)locale;
	char tmp[8192];
	int n = win_format(tmp, sizeof(tmp), fmt, false, false, args);
	if (buf && cap) {
		size_t c = (size_t)n < cap ? (size_t)n : cap - 1;
		memcpy(buf, tmp, c);
		buf[c] = 0;
		if ((size_t)n >= cap && !(options & 2)) return -1;
	}
	return n;
}
static int wide_printf(uint64_t options, wchar16* buf, size_t cap, const wchar16* fmt, uint8_t* args, bool secure) {
	char tmp[8192];
	win_format(tmp, sizeof(tmp), fmt, true, true, args);
	size_t need = utf8_to_utf16(tmp, strlen(tmp), NULL, 0);
	if (!buf || !cap) return (int)need;
	if (need >= cap) {
		if (secure) { buf[0] = 0; return -1; }
		utf8_to_utf16(tmp, strlen(tmp), buf, cap - 1);
		buf[cap - 1] = 0;
		return (options & 2) ? (int)need : -1;
	}
	utf8_to_utf16(tmp, strlen(tmp), buf, cap);
	buf[need] = 0;
	return (int)need;
}
W static int c_stdio_common_vswprintf(uint64_t o, wchar16* b, size_t c, const wchar16* f, void* l, uint8_t* a) { (void)l; return wide_printf(o, b, c, f, a, false); }
W static int c_stdio_common_vswprintf_s(uint64_t o, wchar16* b, size_t c, const wchar16* f, void* l, uint8_t* a) { (void)l; return wide_printf(o, b, c, f, a, true); }
typedef W void (*Pv)(void);
typedef W int (*Pi)(void);
W static void c_initterm(Pv* a, Pv* b) { for (; a < b; a++) if (*a) (*a)(); }
W static int  c_initterm_e(Pi* a, Pi* b) { for (; a < b; a++) if (*a) { int r = (*a)(); if (r) return r; } return 0; }
W static int  c_zero(void) { return 0; }
W static void c_noop(void) {}
W static void c_abort_watson(void) { die("provider invoked _invoke_watson (invalid parameter)", NULL); }
W static void c_terminate(void) { die("provider called terminate()", NULL); }
W static void c_purecall(void) { die("provider called a pure virtual function", NULL); }
W static void c_throw(void* obj, void* info) { (void)obj; (void)info; die("provider threw a C++ exception (exceptions are not supported by this host)", NULL); }
W static void c_frame_handler(void) { die("exception dispatch reached a C++/SEH frame handler", NULL); }
W static void c_xlength_error(const char* m) { die("std::length_error", m); }
W static void c_throw_cpp_error(int code) { char b[32]; snprintf(b, sizeof(b), "%d", code); die("std::_Throw_Cpp_error", b); }
W static void c_std_exception_copy(const void* from, void* to) {
	const char* what; memcpy(&what, from, 8);
	char* copy = what ? strdup(what) : NULL;
	uint8_t one = copy != NULL;
	memcpy(to, &copy, 8);
	memcpy((uint8_t*)to + 8, &one, 1);
}
W static void c_std_exception_destroy(void* e) {
	char* what; memcpy(&what, e, 8);
	if (((uint8_t*)e)[8]) free(what);
	memset(e, 0, 9);
}
static void* g_exception_slot[2];
W static void* c_current_exception(void) { return &g_exception_slot[0]; }
W static void* c_current_exception_context(void) { return &g_exception_slot[1]; }
// std::mutex / recursive_mutex storage (_Mtx_internal_imp_t, 80 bytes in place): type at 0 (set by the STL), and here a
// lock word at 16, owner thread at 24, recursion count at 28 (all zero for a constructed, unlocked mutex).
W static void m_init_in_situ(uint8_t* m, int type) { memset(m, 0, 80); memcpy(m, &type, 4); }
W static int m_lock(uint8_t* m) {
	int type; memcpy(&type, m, 4);
	uint32_t self = (uint32_t)syscall(SYS_gettid);
	pthread_mutex_lock(&g_sync);
	uint32_t* word = (uint32_t*)(m + 16), *owner = (uint32_t*)(m + 24), *count = (uint32_t*)(m + 28);
	if (*word && *owner == self) {
		if (!(type & 0x100)) die("recursive lock of a non-recursive std::mutex", NULL);
		(*count)++;
	} else {
		while (*word) pthread_cond_wait(&g_sync_cond, &g_sync);
		*word = 1; *owner = self; *count = 1;
	}
	pthread_mutex_unlock(&g_sync);
	return 0;
}
W static int m_unlock(uint8_t* m) {
	pthread_mutex_lock(&g_sync);
	uint32_t* word = (uint32_t*)(m + 16), *count = (uint32_t*)(m + 28);
	if (!*word) die("unlock of an unlocked std::mutex", NULL);
	if (--*count == 0) { *word = 0; *(uint32_t*)(m + 24) = 0; pthread_cond_broadcast(&g_sync_cond); }
	pthread_mutex_unlock(&g_sync);
	return 0;
}
W static float  f_atanf(float x) { return atanf(x); }
W static float  f_cosf(float x) { return cosf(x); }
W static float  f_sinf(float x) { return sinf(x); }
W static float  f_tanf(float x) { return tanf(x); }
W static float  f_sqrtf(float x) { return sqrtf(x); }
W static float  f_floorf(float x) { return floorf(x); }
W static float  f_log2f(float x) { return log2f(x); }
W static double f_floor(double x) { return floor(x); }
W static double f_sqrt(double x) { return sqrt(x); }
W static double f_log2(double x) { return log2(x); }

// ---------------------------------------------------------------------------------------------------- import table
typedef struct Export { const char* name; void* fn; } Export;
#define E(n, f) {n, (void*)(f)}
static const Export g_exports[] = {
	// KERNEL32
	E("WideCharToMultiByte", k_WideCharToMultiByte), E("MultiByteToWideChar", k_MultiByteToWideChar),
	E("OutputDebugStringW", k_OutputDebugStringW), E("SetThreadDescription", k_SetThreadDescription),
	E("SetThreadPriority", k_SetThreadPriority), E("EnterCriticalSection", k_EnterCriticalSection),
	E("LeaveCriticalSection", k_LeaveCriticalSection), E("InitializeCriticalSection", k_InitializeCriticalSection),
	E("DeleteCriticalSection", k_DeleteCriticalSection), E("WaitForSingleObject", k_WaitForSingleObject),
	E("CreateEventW", k_CreateEventW), E("SetEvent", k_SetEvent), E("CloseHandle", k_CloseHandle),
	E("CreateThread", k_CreateThread), E("QueryPerformanceFrequency", k_QueryPerformanceFrequency),
	E("QueryPerformanceCounter", k_QueryPerformanceCounter), E("GetModuleFileNameA", k_GetModuleFileNameA),
	E("LoadLibraryExA", k_LoadLibraryExA), E("LoadLibraryA", k_LoadLibraryA), E("GetModuleHandleA", k_GetModuleHandleA),
	E("GetTickCount64", k_GetTickCount64), E("GetProcAddress", k_GetProcAddress), E("FreeLibrary", k_FreeLibrary),
	E("GetCurrentProcessId", k_GetCurrentProcessId), E("GetCurrentThreadId", k_GetCurrentThreadId),
	E("RtlCaptureContext", k_RtlCaptureContext), E("IsDebuggerPresent", k_IsDebuggerPresent),
	E("InitializeSListHead", k_InitializeSListHead), E("DisableThreadLibraryCalls", k_DisableThreadLibraryCalls),
	E("GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime), E("SleepConditionVariableSRW", k_SleepConditionVariableSRW),
	E("WakeAllConditionVariable", k_WakeAllConditionVariable), E("AcquireSRWLockExclusive", k_AcquireSRWLockExclusive),
	E("ReleaseSRWLockExclusive", k_ReleaseSRWLockExclusive), E("IsProcessorFeaturePresent", k_IsProcessorFeaturePresent),
	E("TerminateProcess", k_TerminateProcess), E("GetCurrentProcess", k_GetCurrentProcess),
	E("SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter), E("UnhandledExceptionFilter", k_UnhandledExceptionFilter),
	E("RtlVirtualUnwind", k_RtlVirtualUnwind), E("RtlLookupFunctionEntry", k_RtlLookupFunctionEntry),
	// USER32 (window queries for the frame generation swapchain, which this host does not create)
	E("GetMonitorInfoW", u_zero), E("OffsetRect", u_zero), E("ClientToScreen", u_zero),
	E("GetWindowThreadProcessId", u_zero), E("MonitorFromWindow", u_zero), E("EqualRect", u_zero),
	E("IsWindowVisible", u_zero), E("GetClientRect", u_zero), E("EnumWindows", u_zero),
	// VERSION
	E("GetFileVersionInfoSizeA", v_GetFileVersionInfoSizeA), E("GetFileVersionInfoA", u_zero), E("VerQueryValueA", u_zero),
	// MSVCP140
	E("_Mtx_init_in_situ", m_init_in_situ), E("_Mtx_lock", m_lock), E("_Mtx_unlock", m_unlock),
	E("?_Xlength_error@std@@YAXPEBD@Z", c_xlength_error), E("?_Throw_Cpp_error@std@@YAXH@Z", c_throw_cpp_error),
	// VCRUNTIME140(_1)
	E("__std_exception_destroy", c_std_exception_destroy), E("__std_exception_copy", c_std_exception_copy),
	E("__current_exception", c_current_exception), E("__current_exception_context", c_current_exception_context),
	E("__C_specific_handler", c_frame_handler), E("__CxxFrameHandler4", c_frame_handler),
	E("__std_terminate", c_terminate), E("__std_type_info_destroy_list", c_noop), E("_CxxThrowException", c_throw),
	E("_purecall", c_purecall), E("memcmp", c_memcmp), E("memset", c_memset), E("memmove", c_memmove), E("memcpy", c_memcpy),
	// UCRT
	E("_callnewh", c_callnewh), E("calloc", c_calloc), E("malloc", c_malloc), E("free", c_free),
	E("__stdio_common_vsprintf", c_stdio_common_vsprintf), E("__stdio_common_vswprintf", c_stdio_common_vswprintf),
	E("__stdio_common_vswprintf_s", c_stdio_common_vswprintf_s),
	E("_invoke_watson", c_abort_watson), E("_execute_onexit_table", c_zero), E("_register_onexit_function", c_zero),
	E("_initialize_onexit_table", c_zero), E("_initialize_narrow_environment", c_zero), E("_configure_narrow_argv", c_zero),
	E("_seh_filter_dll", c_zero), E("terminate", c_terminate), E("_cexit", c_noop), E("_initterm_e", c_initterm_e),
	E("_initterm", c_initterm), E("_crt_atexit", c_zero),
	E("wcscpy_s", c_wcscpy_s), E("strcmp", c_strcmp), E("strncpy_s", c_strncpy_s),
	E("getenv_s", c_getenv_s), E("_wgetenv_s", c_wgetenv_s),
	E("atanf", f_atanf), E("cosf", f_cosf), E("floor", f_floor), E("sqrt", f_sqrt), E("sqrtf", f_sqrtf), E("log2f", f_log2f),
	E("tanf", f_tanf), E("floorf", f_floorf), E("sinf", f_sinf), E("log2", f_log2),
};

static const char* const g_host_dlls[] = {"kernel32.dll", "user32.dll", "version.dll", "msvcp140.dll", "vcruntime140.dll",
                                          "vcruntime140_1.dll"};

static void* resolve_import(const char* dll, const char* name) {
	bool host = !strncmp(dll, "api-ms-win-crt-", 15);
	for (size_t i = 0; !host && i < sizeof(g_host_dlls) / sizeof(*g_host_dlls); i++) host = !strcmp(dll, g_host_dlls[i]);
	if (host) {
		for (size_t i = 0; i < sizeof(g_exports) / sizeof(*g_exports); i++)
			if (!strcmp(g_exports[i].name, name)) return g_exports[i].fn;
		return NULL;
	}
	return g_resolver ? g_resolver(dll, name) : NULL;
}

// An import nobody serves: a stub that names it and stops the process if the provider ever calls it.
static void missing_called(const char* what) { die("provider called an unimplemented import", what); }
static void* missing_stub(const char* dll, const char* name) {
	static uint8_t* page;
	static size_t used = 4096;
	if (used + 32 > 4096) {
		page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page == MAP_FAILED) die("stub page", NULL);
		used = 0;
	}
	char* what = malloc(strlen(dll) + strlen(name) + 2);
	sprintf(what, "%s!%s", dll, name);
	uint8_t* p = page + used;
	// mov rdi, imm64 ; mov rax, imm64 ; jmp rax
	p[0] = 0x48; p[1] = 0xbf; uint64_t a = (uint64_t)(uintptr_t)what; memcpy(p + 2, &a, 8);
	p[10] = 0x48; p[11] = 0xb8; uint64_t f = (uint64_t)(uintptr_t)missing_called; memcpy(p + 12, &f, 8);
	p[20] = 0xff; p[21] = 0xe0;
	used += 32;
	fprintf(stderr, "pehost: unresolved import %s (stops if called)\n", what);
	return p;
}
