# test/sev/test-sev.sh builds this; SAGUARO_LOG_LEVEL=error unless set

SAGUARO_LOG_LEVEL ?= error

all: sev_probe sev_probe.manifest sev_attest sev_attest.manifest

sev_probe: sev_probe.c
	$(CC) -Wall -Wextra -O2 -o $@ $<

sev_probe.manifest: sev_probe.manifest.template
	saguaro-manifest -Dlog_level=$(SAGUARO_LOG_LEVEL) $< $@

sev_attest: sev_attest.c
	$(CC) -Wall -Wextra -O2 -o $@ $<

sev_attest.manifest: sev_attest.manifest.template
	saguaro-manifest -Dlog_level=$(SAGUARO_LOG_LEVEL) $< $@

clean:
	$(RM) sev_probe sev_probe.manifest sev_attest sev_attest.manifest

.PHONY: all clean
