# Build everything into build/; `sudo ./install.sh` installs it.
#   make           exitlag-relayd (Go) and the Wine shim (zig)
#   make dist      release tarball in build/ (prebuilt, plus install.sh)

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
BUILD := build
DIST := exitlag-relay-$(VERSION)-x86_64-linux

.PHONY: all daemon shim dist clean

all: daemon shim

daemon:
	CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o $(BUILD)/exitlag-relayd ./cmd/exitlag-relayd

shim:
	wine/nfshim/build.sh $(abspath $(BUILD))/wine

dist: all
	rm -rf $(BUILD)/$(DIST)
	mkdir -p $(BUILD)/$(DIST)/build/wine $(BUILD)/$(DIST)/gui $(BUILD)/$(DIST)/wine
	cp $(BUILD)/exitlag-relayd $(BUILD)/$(DIST)/build/
	cp $(BUILD)/wine/nfapi.dll $(BUILD)/wine/idle.exe $(BUILD)/wine/nfextlag-wine.sys $(BUILD)/$(DIST)/build/wine/
	cp gui/exitlag_relay.py gui/exitlag_run.py $(BUILD)/$(DIST)/gui/
	cp wine/exitlag-wine.sh wine/exitlag-tray.py $(BUILD)/$(DIST)/wine/
	cp -r dist install.sh README.md LICENSE $(BUILD)/$(DIST)/
	echo $(VERSION) >$(BUILD)/$(DIST)/VERSION
	tar -C $(BUILD) -czf $(BUILD)/$(DIST).tar.gz $(DIST)
	@echo $(BUILD)/$(DIST).tar.gz

clean:
	rm -rf $(BUILD)
