# Convenience targets for testing the live device and refreshing captures.
#
# Firmware build / flash uses idf.py directly (this Makefile deliberately
# doesn't wrap it -- ESP-IDF's own build system is the source of truth).
#
# Most targets honour these env vars:
#   BROKER_HOST   host or IP of the live device (default: 192.168.22.100)
#   BROKER_AUTH   "user:password" Basic Auth pair (omit if portal is open)
#   PORTAL_URL    full URL form, e.g. http://192.168.22.100 (capture scripts)
#   PORTAL_AUTH   same as BROKER_AUTH, for the Playwright capture scripts
#
# Examples:
#   make test
#   BROKER_AUTH=support:dockyard make test-ntp
#   PORTAL_URL=http://192.168.22.100 PORTAL_AUTH=support:dockyard make captures
#   PORTAL_AUTH=support:dockyard make ota

PYTHON      ?= python3
BROKER_HOST ?= 192.168.22.100
PORTAL_URL  ?= http://$(BROKER_HOST)
PORTAL_AUTH ?=
BROKER_AUTH ?= $(PORTAL_AUTH)

# A single ESP-IDF build + flash cycle. Builds the firmware, then OTAs it
# at the live device. Requires `idf.py build` to have been run (or this
# rule rebuilds via idf.py build before OTAing).
BIN := build/mqtt_broker.bin
OTA_URL := $(PORTAL_URL)/ota-upload

.PHONY: help
help:
	@echo "Common targets:"
	@echo "  test          run all integration tests (MQTT broker + NTP)"
	@echo "  test-ntp      run NTP test suite only ($(BROKER_HOST))"
	@echo "  test-broker   run MQTT broker test suite only"
	@echo "  build         idf.py build"
	@echo "  ota           upload \$$(BIN) to the device's /ota-upload"
	@echo "  captures      refresh docs/screenshots/* against the live device"
	@echo "  fmt-version   print FW_VERSION and the embedded binary version"
	@echo ""
	@echo "Environment:"
	@echo "  BROKER_HOST=$(BROKER_HOST)"
	@echo "  PORTAL_URL=$(PORTAL_URL)"
	@echo "  BROKER_AUTH=$(if $(BROKER_AUTH),<set>,<unset>)"
	@echo "  PORTAL_AUTH=$(if $(PORTAL_AUTH),<set>,<unset>)"

.PHONY: test
test: test-ntp test-broker

.PHONY: test-ntp
test-ntp:
	BROKER_HOST=$(BROKER_HOST) BROKER_AUTH=$(BROKER_AUTH) \
	  $(PYTHON) test_ntp.py

.PHONY: test-broker
test-broker:
	$(PYTHON) test_broker.py $(BROKER_HOST)

.PHONY: build
build:
	idf.py build

.PHONY: ota
ota:
	@if [ ! -f $(BIN) ]; then echo "Run 'make build' first ($(BIN) missing)"; exit 1; fi
	@echo "OTAing $(BIN) -> $(OTA_URL)"
	@if [ -n "$(PORTAL_AUTH)" ]; then \
	  curl -s -m 120 -u '$(PORTAL_AUTH)' -F "firmware=@$(BIN)" $(OTA_URL); \
	else \
	  curl -s -m 120 -F "firmware=@$(BIN)" $(OTA_URL); \
	fi
	@echo ""

.PHONY: captures
captures:
	PORTAL_URL=$(PORTAL_URL) PORTAL_AUTH=$(PORTAL_AUTH) \
	  $(PYTHON) tools/capture_portal.py
	PORTAL_URL=$(PORTAL_URL) PORTAL_AUTH=$(PORTAL_AUTH) \
	  $(PYTHON) tools/capture_reboot.py
	PORTAL_URL=$(PORTAL_URL) PORTAL_AUTH=$(PORTAL_AUTH) \
	  $(PYTHON) tools/capture_save_reboot.py
	PORTAL_URL=$(PORTAL_URL) PORTAL_AUTH=$(PORTAL_AUTH) \
	  $(PYTHON) tools/capture_time.py

.PHONY: fmt-version
fmt-version:
	@grep FW_VERSION main/version.h
	@if [ -f $(BIN) ]; then \
	  echo "binary embeds: $$(strings $(BIN) | grep -E '^0\\.[0-9]+\\.' | head -1)"; \
	fi
