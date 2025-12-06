"""Packaging logic for Flake8."""
from __future__ import annotations

import os
import sys

import setuptools
from setuptools import Extension

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "src"))

# Define the C extension module
# The extension is optional - if it fails to build, flake8 will use
# pure Python fallbacks for the performance-critical functions
speedups_ext = Extension(
    "flake8._speedups",
    sources=["src/flake8/_speedups.c"],
    optional=True,  # Don't fail the build if extension compilation fails
)


def build_extensions():
    """Build C extensions if possible, otherwise skip gracefully."""
    try:
        return [speedups_ext]
    except Exception:
        # If anything goes wrong, just skip the extension
        return []


setuptools.setup(
    ext_modules=build_extensions(),
)
