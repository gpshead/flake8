==========================
 C Extension (``_speedups``)
==========================

Flake8 includes an optional C extension module (``_speedups``) that provides
optimized implementations of performance-critical functions. The extension is
optional - flake8 will fall back to pure Python implementations if the C
extension is not available.

Overview
========

The C extension optimizes the following functions identified through profiling
as hot spots:

- ``build_logical_line_tokens``: Builds logical line token lists from raw tokens
- ``mutate_string``: Replaces string contents with placeholders
- ``is_eol_token``: Checks for end-of-line tokens
- ``is_multiline_string``: Detects multi-line string tokens
- ``noqa_line_mapping``: Builds line-to-code mapping for noqa processing

These functions are called thousands of times during linting and benefit
from C implementation, particularly on larger codebases.

Benchmark Results
=================

Benchmarks were performed on Python 3.11 using the ``--benchmark`` flag.
The workload tested was 100 Python files with 500 lines each (50,000 total
lines), run single-threaded (``-j1``) to measure actual processing time
without multiprocessing overhead.

.. code-block:: text

    Workload: 100 files, 50,000 lines (single-threaded)

    With C Extension:    3.30s average
    Without C Extension: 3.44s average

    Speedup: ~4%

For smaller codebases (< 5,000 lines), the C extension overhead may negate
any performance benefits. The extension is most beneficial for large projects.

Reproducing Benchmarks
----------------------

To reproduce the benchmark results:

1. Create test files:

   .. code-block:: bash

       mkdir /tmp/benchmark && cd /tmp/benchmark
       for i in $(seq 1 100); do
           python3 -c "
       lines = ['from __future__ import annotations', '', '']
       for f in range(80):
           lines.extend([
               f'def function_{f}(arg1, arg2):',
               '    \"\"\"Docstring.\"\"\"',
               '    return arg1 + arg2',
               '',
               ''
           ])
       while len(lines) < 500:
           lines.append(f'# Line {len(lines) + 1}')
       print('\n'.join(lines[:500]))
       " > "file_${i}.py"
       done

2. Run benchmark with C extension:

   .. code-block:: bash

       flake8 --benchmark -j1 /tmp/benchmark

3. Run benchmark without C extension (for comparison):

   .. code-block:: python

       # In Python
       import flake8.processor as p
       p._USE_SPEEDUPS = False
       from flake8.main import cli
       cli.main(['--benchmark', '-j1', '/tmp/benchmark'])

Building the Extension
======================

The C extension is built automatically during installation if a C compiler
is available. To build manually:

.. code-block:: bash

    python setup.py build_ext --inplace

The extension is marked as ``optional=True`` in setup.py, so installation
will succeed even if compilation fails.

Python Version Compatibility
============================

The C extension dynamically loads token type constants from the ``tokenize``
module at initialization time, ensuring compatibility across Python versions
where token values may differ.

PyPy Compatibility
------------------

The C extension may not be available on PyPy. Flake8 automatically falls
back to pure Python implementations in this case, ensuring full functionality
on all supported Python implementations.

Testing
=======

The extension includes comprehensive equivalence tests in
``tests/unit/test_speedups.py`` that verify C and Python implementations
produce identical results for all functions. Run them with:

.. code-block:: bash

    pytest tests/unit/test_speedups.py -v

Type Stubs
==========

Type stubs are provided in ``src/flake8/_speedups.pyi`` for mypy and other
type checkers. This allows proper type checking even when the C extension
is not available at type-check time.

Future Work
===========

The following improvements are planned or under consideration for the C
extension:

Fuzz Testing
------------

Add property-based testing using `hypothesis <https://hypothesis.readthedocs.io/>`_
to generate edge-case inputs and verify C/Python equivalence across a wider
range of inputs. This would help catch Unicode edge cases, malformed tokens,
and other corner cases that manual test cases might miss.

Example test structure:

.. code-block:: python

    from hypothesis import given, strategies as st

    @given(st.text())
    def test_mutate_string_fuzz(s):
        # Generate valid string literals and verify equivalence
        if len(s) >= 2 and s[0] == s[-1] and s[0] in '"\'':
            assert _speedups.mutate_string(s) == python_mutate_string(s)

Benchmark Regression Testing
----------------------------

Add automated benchmark tracking to CI to detect performance regressions
over time. This could use `pytest-benchmark <https://pytest-benchmark.readthedocs.io/>`_
or `asv (airspeed velocity) <https://asv.readthedocs.io/>`_ to:

- Track performance across commits
- Alert on significant regressions
- Maintain historical performance data

Cython Migration
----------------

Consider migrating from raw C to `Cython <https://cython.org/>`_ for improved
maintainability. Benefits would include:

- Python-like syntax with C performance
- Automatic reference counting (reduces memory leak risk)
- Easier to maintain and extend
- Better integration with Python's type system
- Automatic fallback generation

The tradeoff is an additional build dependency, though Cython-generated C
files could be included in source distributions.

Pre-built Wheels
----------------

Publish pre-built binary wheels for common platforms to avoid requiring
users to have a C compiler. This would involve:

- Setting up `cibuildwheel <https://cibuildwheel.readthedocs.io/>`_ in CI
- Building wheels for Linux (manylinux), macOS, and Windows
- Supporting multiple Python versions (3.10+)
- Publishing to PyPI alongside the source distribution

This would improve installation experience, especially on Windows where
C compilers are less commonly available.

Additional Optimization Targets
-------------------------------

Profiling identified additional hot spots that could benefit from C
optimization in the future:

- ``tokenize._tokenize``: The largest single time consumer (~11% of runtime),
  though this is in the standard library and would require a different
  approach (perhaps a custom tokenizer for common cases).

- Batch pycodestyle checks: Currently checks run individually; batching
  similar checks could reduce per-check overhead.

- ``expand_indent``: Simple function but called frequently; could be
  optimized if profiling shows it as a bottleneck.
