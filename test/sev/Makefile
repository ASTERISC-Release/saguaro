# test/test-sev.sh builds this; GRAMINE_LOG_LEVEL=error unless set

GRAMINE_LOG_LEVEL ?= error

all: sev_probe sev_probe.manifest

sev_probe: sev_probe.c
	$(CC) -Wall -Wextra -O2 -o $@ $<

sev_probe.manifest: sev_probe.manifest.template
	gramine-manifest -Dlog_level=$(GRAMINE_LOG_LEVEL) $< $@

clean:
	$(RM) sev_probe sev_probe.manifest

.PHONY: all clean
