# Host tests for the ST7701 pixel conversion and rotation.
#
# The functions under test are extracted from the driver rather than copied,
# so the test cannot drift from what ships. If the driver stops defining one
# of them the extraction produces nothing and the build fails.

LISPBM ?= ../../lispBM
DRIVER ?= ../disp_st7701_rgb.c

# No -Wconversion: it fires inside tinygfx's COLOR_TO_RGB888 macro and in the
# extracted driver code, neither of which this test owns.
CFLAGS += -O1 -g -Wall -Wextra -std=c11
CFLAGS += -I. -I$(LISPBM)/utils

.PHONY: all clean run
all: run

# Everything from the first helper up to the line before the first function
# that touches hardware.
rgb_helpers_extracted.h: $(DRIVER)
	@printf '/* Generated from %s -- do not edit. */\n' "$(DRIVER)" > $@
	@sed -n '/^static inline uint16_t rgb888_to_rgb565/,/^\/\/ An RGB panel owns/p' $(DRIVER) \
		| sed '$$d' >> $@
	@grep -q 'convert_to_rgb565' $@ || \
		{ echo "extraction failed: convert_to_rgb565 not found in $(DRIVER)"; exit 1; }
	@grep -q 'rotate_rgb565' $@ || \
		{ echo "extraction failed: rotate_rgb565 not found in $(DRIVER)"; exit 1; }

test_rgb_convert: test_rgb_convert.c rgb_helpers_extracted.h
	$(CC) $(CFLAGS) -o $@ test_rgb_convert.c

run: test_rgb_convert
	./test_rgb_convert

clean:
	rm -f test_rgb_convert rgb_helpers_extracted.h
