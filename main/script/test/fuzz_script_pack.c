/*
 * Property fuzzer for the script container parser.
 *
 * The property is not "does not crash" -- ASan decides that -- but that every
 * import the parser hands back lies inside the blob it was given. A parser
 * that returns an out-of-range pointer without crashing on the fuzzer's
 * corpus would still be a remote read primitive on the board.
 */

#include "../script_pack.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size > 0x40000) {
		return 0;
	}

	script_blob_t p;
	if (!script_pack_parse(data, (int32_t)size, &p)) {
		return 0;
	}

	// The source must lie inside the blob and be terminated within it.
	if (p.src < (const char *)data ||
			p.src + p.src_len + 1 > (const char *)data + size) {
		abort();
	}

	for (int i = 0; i < (int)p.num_imports; i++) {
		const char *name = NULL;
		const uint8_t *d = NULL;
		int32_t dl = 0;
		if (!script_pack_import_at(&p, i, &name, &d, &dl)) {
			continue;
		}
		if (d < data || dl < 0 || d + dl > data + size) {
			abort();
		}
		if (name < (const char *)data || name >= (const char *)data + size) {
			abort();
		}
	}

	return 0;
}
