# Copyright (c) 2001-2005 Python Software Foundation.  All rights reserved.
#
# This code is released under the standard PSF license.
# See the file LICENSE.

# Primitive Makefile, interfacing to setup.py.  Targets:
#
# all
#     builds the extension wrapper (in place)
# test
#     tests the version just built
# install
#     installs it
# clean
#     removes build artifacts

# Set this to your favorite Python version.
PYTHON=python2.1
ALTPYTHON=python2.2

all:
	$(PYTHON) setup.py -q build_ext
	$(ALTPYTHON) setup.py -q build_ext

install: all
	$(PYTHON) setup.py install
	$(ALTPYTHON) setup.py install

test: all
	$(PYTHON) testspread.py -v
	$(ALTPYTHON) testspread.py -v

clean:
	$(PYTHON) setup.py clean -a
	$(ALTPYTHON) setup.py clean -a
	-rm -f *.o *.so
	-rm -rf build
