# Isolated unit seam. Not a live bar. Does not start mach/server.
SRC = ../src
ODIR = obj
BIN = bin
CFLAGS = -std=c99 -Wall -g -O0 -fvisibility=hidden -fno-common \
         -target arm64-apple-macos11 -I$(SRC) -DITEM_WINDOW_REUSE_TEST
LIBS = -framework Carbon \
       -framework AppKit \
       -framework QuartzCore \
       -framework CoreAudio \
       -framework CoreWLAN \
       -framework CoreVideo \
       -framework IOKit \
       -F/System/Library/PrivateFrameworks \
       -framework SkyLight \
       -framework DisplayServices \
       -framework MediaRemote

_OBJ = alias.o background.o bar_item.o custom_events.o event.o graph.o \
       image.o mouse.o shadow.o font.o text.o message.o bar.o color.o \
       window.o bar_manager.o display.o display_nsscreen.om group.o mach.o \
       popup.o animation.o workspace.om volume.o slider.o power.o wifi.om \
       media.om hotload.o app_windows.o layer.om surface.o context.o
OBJ = $(patsubst %, $(ODIR)/%, $(_OBJ))

.PHONY: test test-asan test-peek red clean
test: $(BIN)/lazy-item-window-reuse-test
	$(BIN)/lazy-item-window-reuse-test

test-peek: $(BIN)/lazy-item-windows-test
	$(BIN)/lazy-item-windows-test

test-asan: CFLAGS += -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
test-asan: clean $(BIN)/lazy-item-window-reuse-test
	$(BIN)/lazy-item-window-reuse-test

$(BIN)/lazy-item-window-reuse-test: $(ODIR)/lazy_item_window_reuse_test.o $(OBJ) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(BIN)/lazy-item-windows-test: $(ODIR)/lazy_item_windows_test.o $(OBJ) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(ODIR)/lazy_item_window_reuse_test.o: lazy_item_window_reuse_test.c | $(ODIR)
	$(CC) -c -o $@ $< $(CFLAGS)

$(ODIR)/lazy_item_windows_test.o: lazy_item_windows_test.c | $(ODIR)
	$(CC) -c -o $@ $< $(CFLAGS)

$(ODIR)/%.o: $(SRC)/%.c | $(ODIR)
	$(CC) -c -o $@ $< $(CFLAGS)

$(ODIR)/%.om: $(SRC)/%.m | $(ODIR)
	$(CC) -c -o $@ $< $(CFLAGS)

$(ODIR) $(BIN):
	mkdir -p $@

clean:
	rm -rf $(ODIR) $(BIN)
