base_dir=$(abspath ./)

CHISEL_VERSION=3.6.0
MODEL ?= TestHarness
PROJECT ?= freechips.rocketchip.system
CFG_PROJECT ?= $(PROJECT)
CONFIG ?= $(CFG_PROJECT).DefaultConfig
MILL ?= mill -i

verilog:
	cd $(base_dir) && $(MILL) "emulator[freechips.rocketchip.system.TestHarness,$(CONFIG)].mfccompiler.compile"

emulator:
	cd $(base_dir) && $(MILL) "emulator[freechips.rocketchip.system.TestHarness,$(CONFIG)].elf"

clean:
	rm -rf out/
