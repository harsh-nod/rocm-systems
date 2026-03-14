##############################################################################
# MIT License
#
# Copyright (c) 2026 Advanced Micro Devices, Inc. All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
##############################################################################

"""Vendored dependencies for rocprofiler-compute.

This package contains external dependencies vendored (included directly) into
the source tree to eliminate external dependencies in profile mode.

All vendored packages are pure Python (no C extensions) for maximum portability.

For vendoring guidelines and adding new packages, see CONTRIBUTING.md.
"""

try:
    from .pyyaml.lib import yaml
except ImportError as e:
    raise ImportError(
        "\n" + "=" * 80 + "\n"
        "ERROR: Vendored PyYAML not found!\n"
        "\n"
        "Git submodules have not been initialized.\n"
        "\n"
        "To fix this, run:\n"
        "    git submodule update --init --recursive\n"
        "\n"
        "If building with CMake, this will be done automatically.\n"
        "\n"
        "See README.md for development setup instructions.\n" + "=" * 80
    ) from e

__all__ = ["yaml"]
