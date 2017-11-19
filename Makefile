# GNU Make workspace makefile autogenerated by Premake

ifndef config
  config=debug_x64
endif

ifndef verbose
  SILENT = @
endif

ifeq ($(config),debug_x64)
  unarrlib_config = debug_x64
  test_unix_config = debug_x64
endif
ifeq ($(config),release_x64)
  unarrlib_config = release_x64
  test_unix_config = release_x64
endif

PROJECTS := unarrlib test_unix

.PHONY: all clean help $(PROJECTS) 

all: $(PROJECTS)

unarrlib:
ifneq (,$(unarrlib_config))
	@echo "==== Building unarrlib ($(unarrlib_config)) ===="
	@${MAKE} --no-print-directory -C . -f unarrlib.make config=$(unarrlib_config)
endif

test_unix: unarrlib
ifneq (,$(test_unix_config))
	@echo "==== Building test_unix ($(test_unix_config)) ===="
	@${MAKE} --no-print-directory -C . -f test_unix.make config=$(test_unix_config)
endif

clean:
	@${MAKE} --no-print-directory -C . -f unarrlib.make clean
	@${MAKE} --no-print-directory -C . -f test_unix.make clean

help:
	@echo "Usage: make [config=name] [target]"
	@echo ""
	@echo "CONFIGURATIONS:"
	@echo "  debug_x64"
	@echo "  release_x64"
	@echo ""
	@echo "TARGETS:"
	@echo "   all (default)"
	@echo "   clean"
	@echo "   unarrlib"
	@echo "   test_unix"
	@echo ""
	@echo "For more information, see http://industriousone.com/premake/quick-start"