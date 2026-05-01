TYPE      ?= nabu
ASM        = vasmz80_std
LINK       = vlink
ASM_FLAGS  = -Fvobj -Lall
LINK_FLAGS = -brawbin1 -Tvlink_${TYPE}.cmd -Bforcearchive
# LINK_FLAGS = -brawbin1 -Bforcearchive

ifeq ($(TYPE), nabu)
EXT    = nabu
else ifeq ($(TYPE), cpm)
EXT    = com
else
$(error TYPE must be either nabu or cpm)
endif

OBJS       = vdp.o interrupts.o emu.o loader.o

# The game ROM to embed.  Must be a raw binary named 000004.nabu (or override
# with:  make ROM=myrom.bin  after symlinking/copying it as 000004.nabu).
# The default name keeps the convention of the original HCCA pak numbering.
ROM       ?= 000004.nabu


.PHONY: all
all: 000001.$(EXT) 000002.nabu
TARGETS    = $(patsubst %,%.$(EXT),$(PORTS))


%.o: %.z80
	@echo "Assembling $<..."
	@$(ASM) $(ASM_FLAGS) -L $(patsubst %.z80,%.lst,$<) -o $@ $< >/dev/null

# 000001.nabu embeds 000002.nabu, 000003.nabu, and $(ROM) via .incbin, so
# those files must exist before the loader is assembled and linked.
000001.$(EXT): $(OBJS) 000002.nabu 000003.nabu $(ROM)
	@echo "Linking loader (with embedded BIOS, patches, and ROM)..."
	@$(LINK) $(LINK_FLAGS) -Tvlink_${TYPE}.cmd -vicelabels coleco.sym -o $@ $(OBJS)

000002.nabu: bios.o
	@echo "Linking bios..."
	@$(LINK) $(LINK_FLAGS) -Tvlink_bios.cmd -vicelabels bios.sym -o $@ $^

#000003.nabu: patches.o
#	@echo "Linking patches..."
#	@$(LINK) $(LINK_FLAGS) -Tvlink_patches.cmd -vicelabels patches.sym -o $@ $^

# Friendly error if the user hasn't supplied a ROM yet.
$(ROM):
	$(error ROM file '$(ROM)' not found. \
	  Copy or symlink your ColecoVision ROM binary as $(ROM))

.PHONY: mame
mame: 000001.nabu
	@echo "Creating coleco.npz"
	zip coleco.npz 000001.nabu

.PHONY: clean
clean:
	@rm -f *.o coleco.npz 000001.nabu *.sym *.lst
