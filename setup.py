#! /usr/bin/env python
""" SpreadModule:  Python wrapper for Spread client libraries

This package contains a simple Python wrapper module for the Spread
toolkit.  The wrapper is known to be compatible with Python 2.3, 2.4,
2.6 and 2.7.
It may work with earlier Pythons, but this has not been tested.

Spread (www.spread.org) is a group communications package.  You'll
need to download and install it separately.  The Python API has been
built and tested against Spreads 3.16.1 through 3.17.3 and also against
4.20.
At least Spread 3.17 is required to use this version of the wrapper.
4.20 is recommended.

Copyright (c) 2001-2005 Python Software Foundation.  All rights reserved.
Copyright (c) 2013 Gyepi Sam.

This code is released under the standard PSF license.
See the file LICENSE.
"""

# The (non-obvious!) choices for the Trove Development Status line:
# Development Status :: 5 - Production/Stable
# Development Status :: 4 - Beta
# Development Status :: 3 - Alpha

classifiers = """\
Development Status :: 5 - Production/Stable
Intended Audience :: Developers
License :: OSI Approved :: Python Software Foundation License
Programming Language :: Python
Topic :: System :: Distributed Computing
Topic :: Software Development :: Libraries :: Python Modules
Operating System :: Microsoft :: Windows
Operating System :: Unix
"""

import sys
import os
from distutils.core import setup, Extension

if sys.version_info < (2, 3):
    _setup = setup
    def setup(**kwargs):
        if kwargs.has_key("classifiers"):
            del kwargs["classifiers"]
        _setup(**kwargs)

doclines = __doc__.split('\n')

if os.name == 'nt':
    # The directory into which Tim unpacks the Spread bin tarball on Windows.
    SPREAD_DIR = r"\spread-bin-3.17.3"
    ext = Extension('spread', ['spreadmodule.c'],
                include_dirs = [SPREAD_DIR + r"\include"],
                library_dirs = [SPREAD_DIR + r"\win"],
                # Must use 'libtspread' here for Spread 3.17.3, but
                # 'libtsp' for Spreads before that.
                libraries = ['libtspread', 'wsock32'],
                # Must use 'libcmt' here for Spread 3.17.3, but 'libc'
                # for Spreads before that.
                extra_link_args = ['/NODEFAULTLIB:libcmt'],
                )
else:
    SPREAD_DIR = "/usr/local"
    ext = Extension('spread', ['spreadmodule.c'],
                include_dirs = [SPREAD_DIR + "/include"],
                library_dirs = [SPREAD_DIR + "/lib"],
                libraries = ['tspread-core'],
                )

setup(name = "SpreadModule",
      version = "1.5",
      maintainer = "Zope Corporation",
      maintainer_email = "zodb-dev@zope.org",
      description = doclines[0],
      long_description = '\n'.join(doclines[2:]),
      license = "Python",
      platforms = ["unix", "ms-windows"],
      url = "http://zope.org/Members/tim_one/spread",
      classifiers = filter(None, classifiers.split("\n")),
      ext_modules = [ext],
      )
