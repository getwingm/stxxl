# This -*- Makefile -*- gets processed with both GNU make and nmake.
# So keep it simple and compatible.

usage:
	@echo "Choose one of the goals:"
	@echo "    library_g++ library_g++_parallel_mode library_icpc library_icpc_parallel_mode library_msvc"
	@echo "    tests_g++   tests_g++_parallel_mode   tests_icpc   tests_icpc_parallel_mode   tests_msvc"
	@echo "    clean_g++   clean_g++_parallel_mode   clean_icpc   clean_icpc_parallel_mode   clean_msvc"
	@echo "    doxy clean_doxy"

settings_gnu:
	cmp -s make.settings.gnu make.settings || \
		cp make.settings.gnu make.settings

settings_msvc:
	copy make.settings.msvc make.settings


library_g++: settings_gnu
	$(MAKE) -f Makefile.gnu library USE_PARALLEL_MODE=no

library_g++_parallel_mode: settings_gnu
	$(MAKE) -f Makefile.gnu library USE_PARALLEL_MODE=yes

library_icpc: settings_gnu
	$(MAKE) -f Makefile.gnu library USE_PARALLEL_MODE=no USE_ICPC=yes

library_icpc_parallel_mode: settings_gnu
	$(MAKE) -f Makefile.gnu library USE_PARALLEL_MODE=yes USE_ICPC=yes

library_msvc: settings_msvc
	nmake /F Makefile.msvc library
	

tests_g++: settings_gnu
	$(MAKE) -f Makefile.gnu tests USE_PARALLEL_MODE=no

tests_g++_parallel_mode: settings_gnu
	$(MAKE) -f Makefile.gnu tests USE_PARALLEL_MODE=yes

tests_icpc: settings_gnu
	$(MAKE) -f Makefile.gnu tests USE_PARALLEL_MODE=no USE_ICPC=yes

tests_icpc_parallel_mode: settings_gnu
	$(MAKE) -f Makefile.gnu tests USE_PARALLEL_MODE=yes USE_ICPC=yes

tests_msvc: settings_msvc
	nmake /F Makefile.msvc tests


clean_g++: settings_gnu
	$(MAKE) -f Makefile.gnu clean USE_PARALLEL_MODE=no

clean_g++_parallel_mode: settings_gnu
	$(MAKE) -f Makefile.gnu clean USE_PARALLEL_MODE=yes

clean_icpc: settings_gnu
	$(MAKE) -f Makefile.gnu clean USE_PARALLEL_MODE=no USE_ICPC=yes

clean_icpc_parallel_mode: settings_gnu
	$(MAKE) -f Makefile.gnu clean USE_PARALLEL_MODE=yes USE_ICPC=yes

clean_msvc: settings_msvc
	nmake /F Makefile.msvc clean

doxy: Doxyfile
	doxygen

clean_doxy:
	$(RM) -r doc/doxy

# optional parameters:
# DATE=""     if you *don't* want a -YYYYMMDD in the version
# PHASE=snapshot|alpha#|beta#|rc#|release    (defaults to snapshot)
release:
	$(MAKE) -f Makefile.gnu release

