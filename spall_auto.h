#ifndef SPALL_AUTO_H
#define SPALL_AUTO_H

#ifdef __cplusplus
extern "C" {
#endif

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>

typedef struct {
	char *str;
	int len;
} Name;

typedef struct {
	void *addr;
	Name name;
} SymEntry;

typedef struct {
	SymEntry *arr;
	uint64_t len;
	uint64_t cap;
} SymArr;

typedef struct {
	int64_t *arr;
	uint64_t len;
} HashArr;

typedef struct {
	SymArr  entries;	
	HashArr hashes;	
} AddrHash;

void spall_auto_init(char *filename);
void spall_auto_quit(void);
void spall_auto_thread_init(uint32_t _tid, size_t buffer_size, int64_t symbol_cache_size);
void spall_auto_thread_quit(void);

#define SPALL_DEFAULT_BUFFER_SIZE (64 * 1024 * 1024)
#define SPALL_DEFAULT_SYMBOL_CACHE_SIZE (100000)

#ifdef __cplusplus
}
#endif
#endif

#ifdef SPALL_AUTO_IMPLEMENTATION

#ifndef SPALL_AUTO_IMPLEMENTED
#define SPALL_AUTO_IMPLEMENTED

#ifdef __cplusplus
extern "C" {
#endif

#include "spall.h"

static SpallProfile spall_ctx;
static AddrHash global_addr_map;
static _Thread_local SpallBuffer spall_buffer;
static _Thread_local AddrHash addr_map;
static _Thread_local uint32_t tid;
static _Thread_local bool spall_thread_running = false;

#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>


// we're not checking overflow here...Don't do stupid things with input sizes
SPALL_FN uint64_t next_pow2(uint64_t x) {
	return 1 << (64 - __builtin_clzl(x - 1));
}

// This is not thread-safe... Use one per thread!
SPALL_FN AddrHash ah_init(int64_t size) {
	AddrHash ah;

	ah.entries.cap = size;
	ah.entries.arr = (SymEntry *)calloc(sizeof(SymEntry), size);
	ah.entries.len = 0;

	ah.hashes.len = next_pow2(size);
	ah.hashes.arr = (int64_t *)malloc(sizeof(int64_t) * ah.hashes.len);

	for (int64_t i = 0; i < ah.hashes.len; i++) {
		ah.hashes.arr[i] = -1;
	}

	return ah;
}

SPALL_FN void ah_free(AddrHash *ah) {
	free(ah->entries.arr);
	free(ah->hashes.arr);
	memset(ah, 0, sizeof(AddrHash));
}

// fibhash addresses
SPALL_FN int ah_hash(void *addr) {
	return (int)(((uint32_t)(uintptr_t)addr) * 2654435769);
}

// Replace me with your platform's addr->name resolver if needed
SPALL_FN bool get_addr_name(void *addr, Name *name_ret) {
	Dl_info info;
	if (!dladdr(addr, &info)) {
		return false;
	}

	if (info.dli_sname != NULL) {
		char *str = (char *)info.dli_sname;
		size_t len = strlen(str);
		Name name;
		name.str = str;
		name.len = len;
		*name_ret = name;
		return true;
	}

	return false;
}

SPALL_FN bool ah_insert(AddrHash *ah, void *addr, Name name) {
	int addr_hash = ah_hash(addr);
	uint64_t hv = ((uint64_t)addr_hash) & (ah->hashes.len - 1);
	for (uint64_t i = 0; i < ah->hashes.len; i++) {
		uint64_t idx = (hv + i) & (ah->hashes.len - 1);

		int64_t e_idx = ah->hashes.arr[idx];
		if (e_idx == -1) {
			SymEntry entry = {.addr = addr, .name = name};
			ah->hashes.arr[idx] = ah->entries.len;
			ah->entries.arr[ah->entries.len] = entry;
			ah->entries.len += 1;
			return true;
		}

		if ((uint64_t)ah->entries.arr[e_idx].addr == (uint64_t)addr) {
			return true;
		}
	}

	// The symbol map is full, make the symbol map bigger!
	return false;
}

SPALL_FN bool ah_get(AddrHash *ah, void *addr, Name *name_ret) {
	int addr_hash = ah_hash(addr);
	uint64_t hv = ((uint64_t)addr_hash) & (ah->hashes.len - 1);
	for (uint64_t i = 0; i < ah->hashes.len; i++) {
		uint64_t idx = (hv + i) & (ah->hashes.len - 1);

		int64_t e_idx = ah->hashes.arr[idx];
		if (e_idx == -1) {

			Name name;
			if (!get_addr_name(addr, &name)) {
				// Failed to get a name for the address!
				return false;
			}

			SymEntry entry = {.addr = addr, .name = name};
			ah->hashes.arr[idx] = ah->entries.len;
			ah->entries.arr[ah->entries.len] = entry;
			ah->entries.len += 1;

			*name_ret = name;
			return true;
		}

		if ((uint64_t)ah->entries.arr[e_idx].addr == (uint64_t)addr) {
			*name_ret = ah->entries.arr[e_idx].name;
			return true;
		}
	}

	// The symbol map is full, make the symbol map bigger!
	return false;
}

#ifdef __linux__
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <sys/mman.h>

SPALL_FN uint64_t mul_u64_u32_shr(uint64_t cyc, uint32_t mult, uint32_t shift) {
    __uint128_t x = cyc;
    x *= mult;
    x >>= shift;
    return x;
}

SPALL_FN long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
           int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

SPALL_FN double get_rdtsc_multiplier() {
	struct perf_event_attr pe = {
        .type = PERF_TYPE_HARDWARE,
        .size = sizeof(struct perf_event_attr),
        .config = PERF_COUNT_HW_INSTRUCTIONS,
        .disabled = 1,
        .exclude_kernel = 1,
        .exclude_hv = 1
    };

    int fd = perf_event_open(&pe, 0, -1, -1, 0);
    if (fd == -1) {
        perror("perf_event_open failed");
        return 1;
    }
    void *addr = mmap(NULL, 4*1024, PROT_READ, MAP_SHARED, fd, 0);
    if (!addr) {
        perror("mmap failed");
        return 1;
    }
    struct perf_event_mmap_page *pc = (struct perf_event_mmap_page *)addr;
    if (pc->cap_user_time != 1) {
        fprintf(stderr, "Perf system doesn't support user time\n");
        return 1;
    }
	double nanos = (double)mul_u64_u32_shr(1000000, pc->time_mult, pc->time_shift);
	return nanos / 1000000000;
}


#pragma pack(1)
typedef struct {
	uint8_t  ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entrypoint;
	uint64_t program_hdr_offset;
	uint64_t section_hdr_offset;
	uint32_t flags;
	uint16_t eh_size;
	uint16_t program_hdr_entry_size;
	uint16_t program_hdr_num;
	uint16_t section_hdr_entry_size;
	uint16_t section_hdr_num;
	uint16_t section_hdr_str_idx;
} ELF64_Header;

typedef struct {
	uint32_t name;
	uint32_t type;
	uint64_t flags;
	uint64_t addr;
	uint64_t offset;
	uint64_t size;
	uint32_t link;
	uint32_t info;
	uint64_t addr_align;
	uint64_t entry_size;
} ELF64_Section_Header;

typedef struct {
    uint32_t name;
    uint8_t  info;
    uint8_t  other;
    uint16_t section_hdr_idx;
    uint64_t value;
    uint64_t size;
} ELF64_Sym;
#pragma pack()

int load_self(AddrHash *ah) {
	int fd = open("/proc/self/exe", O_RDONLY);
	uint64_t length = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	#define round_size(addr, size) (((addr) + (size)) - ((addr) % (size)))
	uint64_t aligned_length = round_size(length, 0x1000);
	uint8_t *self = mmap(NULL, aligned_length, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
	close(fd);

	ELF64_Header *elf_hdr = (ELF64_Header *)self;
	ELF64_Section_Header *section_hdr_table = (ELF64_Section_Header *)(self + elf_hdr->section_hdr_offset);
	ELF64_Section_Header *section_strtable_hdr = &section_hdr_table[elf_hdr->section_hdr_str_idx];
	char *strtable_ptr = (char *)self + section_strtable_hdr->offset;

	size_t symtab_idx = 0;
	size_t symstrtab_idx = 0;

	int i = 0;
	for (; i < elf_hdr->section_hdr_num; i += 1) {
		ELF64_Section_Header *s_hdr = &section_hdr_table[i];

		char *name = strtable_ptr + s_hdr->name;
		if (strncmp(name, ".symtab", sizeof(".symtab")) == 0) {
			symtab_idx = i;
			symstrtab_idx = s_hdr->link;
			break;
		}
	}
	if (i == elf_hdr->section_hdr_num) return 0;

	ELF64_Section_Header *symtab_section = (ELF64_Section_Header *)(
		self + elf_hdr->section_hdr_offset + (symtab_idx * elf_hdr->section_hdr_entry_size)
	);
	ELF64_Section_Header *symtab_str_section = (ELF64_Section_Header *)(
		self + elf_hdr->section_hdr_offset + (symstrtab_idx * elf_hdr->section_hdr_entry_size)
	);

	#define ELF64_ST_TYPE(info) ((info)&0xf)
	#define STT_FUNC 2
	for (size_t i = 0; i < symtab_section->size; i += symtab_section->entry_size) {
		ELF64_Sym *sym = (ELF64_Sym *)(self + symtab_section->offset + i);

		uint8_t type = ELF64_ST_TYPE(sym->info);
		if (type != STT_FUNC) {
			continue;
		}

		char *name_str = (char *)&self[symtab_str_section->offset + sym->name];
		
		// load global symbol cache!
		Name name;
		name.str = name_str;
		name.len = strlen(name_str);
		ah_insert(ah, (void *)sym->value, name);
	}

	return 1;
}
#elif __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>

int load_self(AddrHash *ah) {
	return 1;
}

SPALL_FN double get_rdtsc_multiplier() {
	uint64_t freq;
	size_t size = sizeof(freq);

	sysctlbyname("machdep.tsc.frequency", &freq, &size, NULL, 0);

	return 1000000.0 / (double)freq;
}
#endif

void spall_auto_thread_init(uint32_t _tid, size_t buffer_size, int64_t symbol_cache_size) {
	uint8_t *buffer = (uint8_t *)malloc(buffer_size);
	spall_buffer = (SpallBuffer){ .data = buffer, .length = buffer_size };

	// removing initial page-fault bubbles to make the data a little more accurate, at the cost of thread spin-up time
	memset(buffer, 1, buffer_size);

	spall_buffer_init(&spall_ctx, &spall_buffer);

	tid = _tid;
	addr_map = ah_init(symbol_cache_size);
	spall_thread_running = true;
}

void spall_auto_thread_quit(void) {
	spall_thread_running = false;
	ah_free(&addr_map);
	spall_buffer_quit(&spall_ctx, &spall_buffer);
	free(spall_buffer.data);
}

void spall_auto_init(char *filename) {
	spall_ctx = spall_init_file(filename, get_rdtsc_multiplier());
	global_addr_map = ah_init(10000);
	load_self(&global_addr_map);
}

void spall_auto_quit(void) {
	spall_quit(&spall_ctx);
}

char not_found[] = "(unknown name)";
SPALL_NOINSTRUMENT void __cyg_profile_func_enter(void *fn, void *caller) {
	if (!spall_thread_running) {
		return;
	}

	Name name;
	if (!ah_get(&global_addr_map, fn, &name) && !ah_get(&addr_map, fn, &name)) {
		name = (Name){.str = not_found, .len = sizeof(not_found) - 1};
	}

	spall_buffer_begin_ex(&spall_ctx, &spall_buffer, name.str, name.len, __rdtsc(), tid, 0);
}

SPALL_NOINSTRUMENT void __cyg_profile_func_exit(void *fn, void *caller) {
	if (!spall_thread_running) {
		return;
	}

	spall_buffer_end_ex(&spall_ctx, &spall_buffer, __rdtsc(), tid, 0);
}

#ifdef __cplusplus
}
#endif

#endif
#endif
