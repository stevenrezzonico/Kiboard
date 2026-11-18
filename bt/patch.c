/* Writes the changed copy of the Kindle's libbluetooth.so that bt.sh mounts over the original (bt.sh says why).
 * The three places are found by function name and by the instructions themselves, not by address, so a
 * library from another firmware works when it still has the same code there. Anything else is refused:
 * each pair of instructions must be in its function exactly once. */
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *func; uint32_t at[2]; int which; uint32_t to; } PATCH[] = {
	/* bic r3, r3, #3; strb r3, [r4]  ->  no bic: keep the "discoverable" flags of the advertisement */
	{ "btm_ble_update_dmt_flag_bits", { 0xe3c33003, 0xe5c43000 }, 0, 0xe1a00000 },
	/* push {r4-r7, lr}; mov r4, r0  ->  mov r4, #0: privacy always off = public address */
	{ "BTM_BleConfigPrivacy", { 0xe92d40f0, 0xe1a04000 }, 1, 0xe3a04000 },
	/* ldrb r2, [r4, #11]; mov r1, #1  ->  movw r1, #0xffff: no idle disconnect */
	{ "gatt_update_app_use_link_flag", { 0xe5d4200b, 0xe3a01001 }, 1, 0xe30f1fff },
};

/* File offset and size of a function from the dynamic symbol table; 0 = not found. */
static size_t find(const unsigned char *d, size_t len, const char *name, size_t *size)
{
	const Elf32_Ehdr *e = (const void *) d;
	if (len < sizeof *e || memcmp(e->e_ident, ELFMAG "\1\1", 6) || e->e_machine != EM_ARM || e->e_shentsize != sizeof(Elf32_Shdr)
	    || e->e_shoff > len || (len - e->e_shoff) / sizeof(Elf32_Shdr) < e->e_shnum) return 0;
	const Elf32_Shdr *sh = (const void *) (d + e->e_shoff);
	for (int i = 0; i < e->e_shnum; i++) {
		if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= e->e_shnum) continue;
		const Elf32_Shdr *str = &sh[sh[i].sh_link];
		if (sh[i].sh_offset > len || sh[i].sh_size > len - sh[i].sh_offset || str->sh_offset > len || str->sh_size > len - str->sh_offset
		    || !str->sh_size || d[str->sh_offset + str->sh_size - 1]) return 0;
		const Elf32_Sym *sym = (const void *) (d + sh[i].sh_offset);
		for (size_t s = 0; s < sh[i].sh_size / sizeof *sym; s++) {
			if (ELF32_ST_TYPE(sym[s].st_info) != STT_FUNC || sym[s].st_name >= str->sh_size
			    || strcmp((const char *) d + str->sh_offset + sym[s].st_name, name)) continue;
			size_t addr = sym[s].st_value & ~1u;
			for (int c = 0; c < e->e_shnum; c++) { /* the section with that address (the symbol's own section number is wrong in this library) */
				if (sh[c].sh_type != SHT_PROGBITS || addr < sh[c].sh_addr || addr - sh[c].sh_addr >= sh[c].sh_size) continue;
				size_t off = addr - sh[c].sh_addr + sh[c].sh_offset;
				if (off > len || sym[s].st_size > len - off) return 0;
				*size = sym[s].st_size;
				return off;
			}
			return 0;
		}
	}
	return 0;
}

int kbt_patch(const char *src, const char *dst)
{
	FILE *f = fopen(src, "rb");
	unsigned char *d = malloc(16 << 20);
	size_t len = f && d ? fread(d, 1, 16 << 20, f) : 0;
	if (f) fclose(f);
	if (!len || len == 16 << 20) { fprintf(stderr, "bt: cannot read %s\n", src); return 1; }
	for (size_t p = 0; p < sizeof PATCH / sizeof PATCH[0]; p++) {
		size_t size = 0, off = find(d, len, PATCH[p].func, &size), hit = 0;
		int n = 0;
		for (size_t i = 0; off && i + 8 <= size; i += 4)
			if (!memcmp(d + off + i, PATCH[p].at, 8)) { hit = off + i; n++; } /* an ARM library on an ARM Kindle: same byte order */
		if (n != 1) {
			fprintf(stderr, "bt: %s: %s. Bluetooth mode does not know this firmware.\n", PATCH[p].func,
			        !off ? "function not found" : n ? "code found more than once" : "code not found");
			return 1;
		}
		hit += 4 * PATCH[p].which;
		memcpy(d + hit, &PATCH[p].to, 4);
		fprintf(stderr, "bt: %s changed at 0x%zx\n", PATCH[p].func, hit);
	}
	f = fopen(dst, "wb");
	if (!f || fwrite(d, 1, len, f) != len || fclose(f)) { fprintf(stderr, "bt: cannot write %s\n", dst); return 1; }
	free(d);
	return 0;
}

#ifdef PATCH_MAIN /* PC check, run by build.sh */
int main(int argc, char **argv) { return argc == 3 ? kbt_patch(argv[1], argv[2]) : 2; }
#endif
