# Groove: a music studio for RISC OS.
# Built with riscos-crossdev (GCCSDK GCC 10.2, UnixLib), like the other
# ports: point RCD_DIR at a checkout of a tagged release.
#
#   make            out/groove (ELF), the checks, out/!Groove, out/Groove.zip
#   make test       the Linux tests: engine, interrupt handler, Wimp front end
#   make shots      rendered screenshots of the front end (out/screenshots)
#   make demo       out/demo.wav: the demo song, bounced by the engine
#   make clean
RCD_DIR ?= ../riscos-crossdev
-include $(RCD_DIR)/riscos.mk
.DEFAULT_GOAL := all

ENGINE := engine/song.c engine/engine.c engine/loops.c engine/files.c engine/audioin_null.c \
          engine/gm.c engine/midifile.c engine/history.c engine/place.c engine/maestro.c
# SoftMIDI's library (third_party/softmidi/VERSION): SoundFont sounds.
SM := third_party/softmidi
SOFTMIDI := $(SM)/src/midisynth.c $(SM)/src/output.c $(SM)/src/output_ssb.c $(SM)/src/output_dr.c
SM_INC := -I$(SM)/include -I$(SM)/third_party/TinySoundFont
RISCOS := riscos/main.c riscos/arrange.c riscos/editor.c riscos/browser.c riscos/keys.c \
          riscos/savebox.c riscos/gfx.c riscos/sound.c riscos/layout.c riscos/startlog.c riscos/midiin.c
HEADERS := engine/groove.h engine/place.h engine/audioin.h engine/gm_internal.h riscos/app.h riscos/gfx.h riscos/layout.h
OUT := out

all: $(OUT)/Groove.zip
check: checks test

# --- RISC OS build ---------------------------------------------------------------
RO_OBJS := $(patsubst %.c,$(OUT)/ro/%.o,$(ENGINE) $(RISCOS))
SM_RO_OBJS := $(patsubst %.c,$(OUT)/ro/%.o,$(SOFTMIDI))

$(OUT)/ro/third_party/%.o: third_party/%.c | riscos-toolchain-present
	@mkdir -p $(dir $@)
	$(RO_CC) $(RISCOS_CFLAGS) -std=gnu99 $(SM_INC) -c $< -o $@
$(OUT)/ro/%.o: %.c $(HEADERS) | riscos-toolchain-present
	@mkdir -p $(dir $@)
	$(RO_CC) $(RISCOS_CFLAGS) -std=gnu99 -Wall -Wextra $(SM_INC) -c $< -o $@

$(OUT)/groove: $(RO_OBJS) $(SM_RO_OBJS) riscos/handler.s
	$(RO_CC) $(RISCOS_CFLAGS) $(RISCOS_LDFLAGS) -o $@ $(RO_OBJS) $(SM_RO_OBJS) riscos/handler.s -lpthread -lm

checks: $(OUT)/groove
	$(CHECK_ALL) $(OUT)/groove
	$(CHECK_UNALIGNED) -Iengine -Iriscos $(SM_INC) $(ENGINE) $(RISCOS)

$(OUT)/!Sprites,ff9: app/groove.png app/sm-groove.png app/png2sprite.py
	@mkdir -p $(OUT)
	python3 app/png2sprite.py $@ '!groove=app/groove.png' 'sm!groove=app/sm-groove.png'

APPFILES := app/!Run,feb app/!Boot,feb app/!Help,fff
$(OUT)/Groove.zip: $(OUT)/groove $(OUT)/!Sprites,ff9 $(APPFILES) $(wildcard app/Licences/*) docs/guide/index.html $(wildcard docs/guide/images/*.png) | checks
	rm -rf $(OUT)/!Groove && mkdir -p $(OUT)/!Groove
	$(RO_STRIP) -o $(OUT)/groove.stripped $(OUT)/groove
	$(ELF2AIF) -e $(OUT)/groove.stripped $(OUT)/!Groove/!RunImage,ff8 >/dev/null
	cp $(APPFILES) $(OUT)/!Sprites,ff9 $(OUT)/!Groove/
	mkdir -p $(OUT)/!Groove/Licences
	cp app/Licences/*,fff $(OUT)/!Groove/Licences/
	mkdir -p $(OUT)/!Groove/Guide/images
	cp docs/guide/index.html $(OUT)/!Groove/Guide/
	cp docs/guide/images/*.png $(OUT)/!Groove/Guide/images/
	$(ROZIP) $@ -C $(OUT) !Groove
	@echo "$@: unzip it on RISC OS (SparkFS or InfoZip) and run !Groove"

# --- Linux tests --------------------------------------------------------------------
HOSTCC ?= gcc
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_CFLAGS := -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE $(SAN)

test: test-engine test-handler test-ui

# The engine: units, a recording round trip, files, a bounce (64-bit).
$(OUT)/test_engine: host/test_engine.c $(ENGINE) $(HEADERS) $(SOFTMIDI)
	@mkdir -p $(OUT)
	$(HOSTCC) $(HOST_CFLAGS) $(SM_INC) -o $@ host/test_engine.c $(ENGINE) $(SOFTMIDI) -lpthread -lm
# The SoundFont tests use TimGM6mb (GPL-2; for the tests only, never
# shipped), fetched from Ubuntu's package and checked by sha256.
TEST_SF ?= $(OUT)/TimGM6mb.sf2
TIMGM_DEB := http://archive.ubuntu.com/ubuntu/pool/universe/t/timgm6mb-soundfont/timgm6mb-soundfont_1.3-5_all.deb
soundfont: $(OUT)/TimGM6mb.sf2
$(OUT)/TimGM6mb.sf2:
	@mkdir -p $(OUT)/dl
	curl -fsSL -o $(OUT)/dl/timgm.deb $(TIMGM_DEB)
	cd $(OUT)/dl && ar x timgm.deb data.tar.xz && tar xJf data.tar.xz ./usr/share/sounds/sf2/TimGM6mb.sf2
	echo "c5378b62028c920cb11e4803327983fee2f2cdff5dc89c708e39da417e51c854  $(OUT)/dl/usr/share/sounds/sf2/TimGM6mb.sf2" | sha256sum -c --quiet
	mv $(OUT)/dl/usr/share/sounds/sf2/TimGM6mb.sf2 $@ && rm -rf $(OUT)/dl
test-engine: $(OUT)/test_engine $(TEST_SF)
	GROOVE_TEST_SOUNDFONT=$(TEST_SF) $(OUT)/test_engine $(OUT)/test.wav

# The SharedSound handler, as ARM code in Unicorn (needs clang, llvm-objcopy
# and "pip install unicorn").
$(OUT)/%.bin: riscos/%.s
	@mkdir -p $(OUT)
	clang --target=armv7a-none-eabi -c $< -o $(OUT)/$*.o
	llvm-objcopy -O binary --only-section=.text $(OUT)/$*.o $@
$(OUT)/fake_fills.bin: host/fake_fills.s
	@mkdir -p $(OUT)
	clang --target=armv7a-none-eabi -c $< -o $(OUT)/fake_fills.o
	llvm-objcopy -O binary --only-section=.text $(OUT)/fake_fills.o $@
test-handler: $(OUT)/handler.bin $(OUT)/fake_fills.bin
	python3 host/test_handler.py $^

# The Wimp front end against the fake RISC OS: 32-bit, so ints hold
# pointers as on RISC OS (needs gcc-13-multilib).
$(OUT)/test_ui: host/test_ui.c host/fake_ro.c host/fake_ro.h host/fake_font.h $(ENGINE) $(RISCOS) $(HEADERS) $(SOFTMIDI)
	@mkdir -p $(OUT)
	$(HOSTCC) -m32 $(HOST_CFLAGS) $(SM_INC) -Ihost/stubs -Dmain=groove_main -c riscos/main.c -o $(OUT)/ui_main.o
	$(HOSTCC) -m32 $(HOST_CFLAGS) $(SM_INC) -Ihost/stubs -o $@ host/test_ui.c host/fake_ro.c $(OUT)/ui_main.o \
	    $(filter-out riscos/main.c,$(RISCOS)) $(ENGINE) $(SOFTMIDI) -lpthread -lm
test-ui: $(OUT)/test_ui $(TEST_SF)
	GROOVE_TEST_SOUNDFONT=$(TEST_SF) $(OUT)/test_ui

shots: test-ui
	python3 host/shots.py /tmp/groove_shots/*.log $(OUT)/screenshots

demo: $(OUT)/test_engine
	$(OUT)/test_engine $(OUT)/demo.wav

clean:
	rm -rf $(OUT)

.PHONY: soundfont all check checks test test-engine test-handler test-ui shots demo clean
