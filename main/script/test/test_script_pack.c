/*
 * Host tests for the on-flash script container parser.
 *
 * The blob this parses is untrusted: it arrives over USB or BLE, survives
 * reboots in flash, and a cancelled upload leaves a partial one behind. Every
 * offset and length in the import table is attacker-controlled, so the tests
 * below spend most of their effort on malformed input rather than on the
 * happy path.
 *
 *   make -C main/script/test
 */

#include "../script_pack.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks = 0;
static int failures = 0;

static void expect(const char *what, long got, long want) {
	checks++;
	if (got != want) {
		failures++;
		printf("FAIL %-52s got %ld, want %ld\n", what, got, want);
	}
}

static void expect_str(const char *what, const char *got, const char *want) {
	checks++;
	if (!got || strcmp(got, want) != 0) {
		failures++;
		printf("FAIL %-52s got %s, want %s\n", what, got ? got : "(null)", want);
	}
}

// Builder mirroring what tools/luapack.py writes, so the two cannot drift
// without a test noticing.
typedef struct {
	uint8_t buf[4096];
	int32_t len;
} blob_t;

static void put_u16(blob_t *b, uint32_t v) {
	b->buf[b->len++] = (uint8_t)(v >> 8);
	b->buf[b->len++] = (uint8_t)v;
}

static void put_i32(blob_t *b, int32_t v) {
	b->buf[b->len++] = (uint8_t)(v >> 24);
	b->buf[b->len++] = (uint8_t)(v >> 16);
	b->buf[b->len++] = (uint8_t)(v >> 8);
	b->buf[b->len++] = (uint8_t)v;
}

static void blob_start(blob_t *b, uint16_t flags, const char *src) {
	memset(b, 0, sizeof(*b));
	b->len = 0;
	put_i32(b, 0);		// size, not checked by the parser
	put_u16(b, 0);		// crc, likewise
	put_u16(b, flags);
	memcpy(b->buf + b->len, src, strlen(src) + 1);
	b->len += (int32_t)strlen(src) + 1;
}

int main(void) {
	// Happy path, no imports.
	{
		blob_t b;
		blob_start(&b, SCRIPT_FLAG_LANG_LUA, "return 1");
		script_blob_t p;
		expect("plain lua blob parses", script_pack_parse(b.buf, b.len, &p), 1);
		expect_str("source", p.src, "return 1");
		expect("source length", p.src_len, 8);
		expect("language is lua", p.lang, SCRIPT_LANG_LUA);
		expect("no imports", p.num_imports, 0);
	}

	// A blob with flags zero is lisp, which is what every package built
	// before the flag existed says.
	{
		blob_t b;
		blob_start(&b, 0, "(print 1)");
		script_blob_t p;
		expect("flags zero parses", script_pack_parse(b.buf, b.len, &p), 1);
		expect("language is lisp", p.lang, SCRIPT_LANG_LISP);
	}

	// Import table, two entries.
	{
		blob_t b;
		blob_start(&b, SCRIPT_FLAG_LANG_LUA, "require 'a'");
		int32_t table_at = b.len;
		put_u16(&b, 2);
		// Entries first, payloads appended afterwards, so offsets are known.
		int32_t hdr = table_at + 2 + (int32_t)strlen("a") + 1 + 8 +
				(int32_t)strlen("bee") + 1 + 8;
		int32_t off_a = hdr - SCRIPT_HEADER_SIZE;
		int32_t off_b = off_a + 5;
		memcpy(b.buf + b.len, "a", 2); b.len += 2;
		put_i32(&b, off_a);
		put_i32(&b, 5);
		memcpy(b.buf + b.len, "bee", 4); b.len += 4;
		put_i32(&b, off_b);
		put_i32(&b, 3);
		memcpy(b.buf + b.len, "AAAAA", 5); b.len += 5;
		memcpy(b.buf + b.len, "BBB", 3); b.len += 3;

		script_blob_t p;
		expect("blob with imports parses", script_pack_parse(b.buf, b.len, &p), 1);
		expect("two imports", p.num_imports, 2);

		const uint8_t *d = NULL;
		int32_t dl = 0;
		expect("import a found", script_pack_import(&p, "a", &d, &dl), 1);
		expect("import a length", dl, 5);
		checks++;
		if (!d || memcmp(d, "AAAAA", 5) != 0) {
			failures++;
			printf("FAIL import a payload\n");
		}

		expect("import bee found", script_pack_import(&p, "bee", &d, &dl), 1);
		expect("import bee length", dl, 3);
		checks++;
		if (!d || memcmp(d, "BBB", 3) != 0) {
			failures++;
			printf("FAIL import bee payload\n");
		}

		expect("missing import not found",
				script_pack_import(&p, "nope", &d, &dl), 0);

		const char *nm = NULL;
		expect("iterate entry 0", script_pack_import_at(&p, 0, &nm, &d, &dl), 1);
		expect_str("entry 0 name", nm, "a");
		expect("iterate past end", script_pack_import_at(&p, 2, &nm, &d, &dl), 0);
		expect("negative index refused",
				script_pack_import_at(&p, -1, &nm, &d, &dl), 0);
	}

	// Malformed input. None of these may read out of bounds; run this file
	// under ASan in CI so that "did not crash" means something.
	{
		script_blob_t p;
		expect("null blob refused", script_pack_parse(NULL, 100, &p), 0);
		expect("zero length refused", script_pack_parse((uint8_t *)"", 0, &p), 0);

		uint8_t tiny[SCRIPT_HEADER_SIZE] = {0};
		expect("header only refused",
				script_pack_parse(tiny, SCRIPT_HEADER_SIZE, &p), 0);

		// Source with no terminator: the whole payload is source bytes.
		uint8_t unterm[16];
		memset(unterm, 'x', sizeof(unterm));
		expect("unterminated source refused",
				script_pack_parse(unterm, sizeof(unterm), &p), 0);

		// Import count claiming more entries than the blob can hold.
		blob_t b;
		blob_start(&b, SCRIPT_FLAG_LANG_LUA, "x");
		put_u16(&b, 400);
		expect("oversized count still parses the source",
				script_pack_parse(b.buf, b.len, &p), 1);
		const uint8_t *d = NULL;
		int32_t dl = 0;
		expect("truncated table yields nothing",
				script_pack_import(&p, "a", &d, &dl), 0);

		// Count above the 500 cap is treated as absent.
		blob_start(&b, SCRIPT_FLAG_LANG_LUA, "x");
		put_u16(&b, 500);
		expect("count at the cap parses", script_pack_parse(b.buf, b.len, &p), 1);
		expect("count at the cap means no imports", p.num_imports, 0);

		// An entry whose offset and length point past the end.
		blob_start(&b, SCRIPT_FLAG_LANG_LUA, "x");
		put_u16(&b, 1);
		memcpy(b.buf + b.len, "a", 2); b.len += 2;
		put_i32(&b, 0);
		put_i32(&b, 100000);
		expect("out of range length parses", script_pack_parse(b.buf, b.len, &p), 1);
		expect("out of range import refused",
				script_pack_import(&p, "a", &d, &dl), 0);

		// Negative offset, which is representable because the field is signed.
		blob_start(&b, SCRIPT_FLAG_LANG_LUA, "x");
		put_u16(&b, 1);
		memcpy(b.buf + b.len, "a", 2); b.len += 2;
		put_i32(&b, -8);
		put_i32(&b, 4);
		expect("negative offset parses", script_pack_parse(b.buf, b.len, &p), 1);
		expect("negative offset refused",
				script_pack_import(&p, "a", &d, &dl), 0);

		// Offset plus length chosen to overflow a 32-bit addition.
		blob_start(&b, SCRIPT_FLAG_LANG_LUA, "x");
		put_u16(&b, 1);
		memcpy(b.buf + b.len, "a", 2); b.len += 2;
		put_i32(&b, 0x7FFFFFFF);
		put_i32(&b, 0x7FFFFFFF);
		expect("overflowing pair parses", script_pack_parse(b.buf, b.len, &p), 1);
		expect("overflowing pair refused",
				script_pack_import(&p, "a", &d, &dl), 0);

		// Entry name with no terminator before the end of the blob.
		blob_start(&b, SCRIPT_FLAG_LANG_LUA, "x");
		put_u16(&b, 1);
		memset(b.buf + b.len, 'n', 8);
		b.len += 8;
		expect("unterminated entry name parses", script_pack_parse(b.buf, b.len, &p), 1);
		expect("unterminated entry name refused",
				script_pack_import(&p, "nnnnnnnn", &d, &dl), 0);
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
