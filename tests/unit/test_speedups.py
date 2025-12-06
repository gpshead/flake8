"""Tests for the C extension speedups module.

These tests verify that the C implementations produce identical results
to the Python implementations, ensuring correctness and safe fallback.
"""
from __future__ import annotations

import tokenize

import pytest

from flake8 import processor
from flake8._compat import FSTRING_END
from flake8._compat import FSTRING_MIDDLE
from flake8._compat import TSTRING_END
from flake8._compat import TSTRING_MIDDLE

# Try to import the C extension
try:
    from flake8 import _speedups
    HAS_SPEEDUPS = True
except ImportError:
    _speedups = None
    HAS_SPEEDUPS = False

# Skip all tests if speedups not available
pytestmark = pytest.mark.skipif(
    not HAS_SPEEDUPS,
    reason="C extension not available",
)


class TestMutateStringEquivalence:
    """Test that C and Python mutate_string produce identical results."""

    @pytest.mark.parametrize(
        "string",
        [
            '""',
            "''",
            '"a"',
            "'a'",
            '"abc"',
            "'abc'",
            '"hello world"',
            '""""""',
            "''''''",
            '"""a"""',
            "'''a'''",
            '"""hello world"""',
            "'''hello world'''",
            # With prefixes
            'r"abc"',
            "r'abc'",
            'b"abc"',
            "b'abc'",
            'f"abc"',
            "f'abc'",
            'rb"abc"',
            'br"abc"',
            'r"""abc"""',
            "r'''abc'''",
            # Edge cases
            '"x"',
            "'x'",
            '"""x"""',
            '"xxxxxx"',
            # Unicode
            '"héllo"',
            '"日本語"',
        ],
    )
    def test_mutate_string_equivalence(self, string):
        """Verify C and Python implementations match."""
        python_result = processor.mutate_string(string)
        c_result = _speedups.mutate_string(string)
        assert c_result == python_result, (
            f"Mismatch for {string!r}: C={c_result!r}, Python={python_result!r}"
        )


class TestIsEolTokenEquivalence:
    """Test that C and Python is_eol_token produce identical results."""

    def _python_is_eol_token(self, token):
        """Python implementation of is_eol_token."""
        NEWLINE = frozenset([tokenize.NL, tokenize.NEWLINE])
        return token[0] in NEWLINE or token[4][token[3][1]:].lstrip() == "\\\n"

    @pytest.mark.parametrize(
        "token",
        [
            # NL token
            tokenize.TokenInfo(
                tokenize.NL, "\n", (1, 0), (1, 1), "x = 1\n",
            ),
            # NEWLINE token
            tokenize.TokenInfo(
                tokenize.NEWLINE, "\n", (1, 5), (1, 6), "x = 1\n",
            ),
            # Regular token (NAME)
            tokenize.TokenInfo(
                tokenize.NAME, "x", (1, 0), (1, 1), "x = 1\n",
            ),
            # Line continuation
            tokenize.TokenInfo(
                tokenize.NAME, "foo", (1, 0), (1, 3), "foo \\\n",
            ),
            # Comment token
            tokenize.TokenInfo(
                tokenize.COMMENT, "# comment", (1, 0), (1, 9), "# comment\n",
            ),
            # OP token
            tokenize.TokenInfo(
                tokenize.OP, "=", (1, 2), (1, 3), "x = 1\n",
            ),
        ],
    )
    def test_is_eol_token_equivalence(self, token):
        """Verify C and Python implementations match."""
        NEWLINE = frozenset([tokenize.NL, tokenize.NEWLINE])
        python_result = self._python_is_eol_token(token)
        c_result = _speedups.is_eol_token(token, NEWLINE)
        assert c_result == python_result, (
            f"Mismatch for {token}: C={c_result}, Python={python_result}"
        )


class TestIsMultilineStringEquivalence:
    """Test that C and Python is_multiline_string produce identical results."""

    def _python_is_multiline_string(self, token):
        """Python implementation of is_multiline_string."""
        return token.type in {FSTRING_END, TSTRING_END} or (
            token.type == tokenize.STRING and "\n" in token.string
        )

    @pytest.mark.parametrize(
        "token",
        [
            # Single line string
            tokenize.TokenInfo(
                tokenize.STRING, '"hello"', (1, 0), (1, 7), 'x = "hello"\n',
            ),
            # Multiline string
            tokenize.TokenInfo(
                tokenize.STRING, '"""hello\nworld"""', (1, 0), (2, 8),
                'x = """hello\nworld"""\n',
            ),
            # Non-string token
            tokenize.TokenInfo(
                tokenize.NAME, "foo", (1, 0), (1, 3), "foo = 1\n",
            ),
            # Triple-quoted single line
            tokenize.TokenInfo(
                tokenize.STRING, '"""hello"""', (1, 0), (1, 11),
                'x = """hello"""\n',
            ),
            # String with escaped newline (not multiline)
            tokenize.TokenInfo(
                tokenize.STRING, '"hello\\nworld"', (1, 0), (1, 14),
                'x = "hello\\nworld"\n',
            ),
        ],
    )
    def test_is_multiline_string_equivalence(self, token):
        """Verify C and Python implementations match."""
        python_result = self._python_is_multiline_string(token)
        c_result = _speedups.is_multiline_string(token, FSTRING_END, TSTRING_END)
        assert c_result == python_result, (
            f"Mismatch for {token}: C={c_result}, Python={python_result}"
        )


class TestBuildLogicalLineTokensEquivalence:
    """Test that C and Python build_logical_line_tokens produce identical results."""

    def test_simple_assignment(self, default_options):
        """Test simple assignment statement."""
        lines = ["x = 1\n"]
        fp = processor.FileProcessor("-", default_options, lines=lines)

        # Get tokens
        tokens = list(fp.generate_tokens())
        fp.tokens = tokens[:-1]  # Remove ENDMARKER

        # Get Python result
        processor._USE_SPEEDUPS = False
        python_result = fp._build_logical_line_tokens_python()

        # Get C result
        c_result = _speedups.build_logical_line_tokens(
            fp.tokens, fp.lines, FSTRING_MIDDLE, TSTRING_MIDDLE,
        )

        assert c_result[0] == python_result[0], "Comments mismatch"
        assert c_result[1] == python_result[1], "Logical line mismatch"
        assert c_result[2] == python_result[2], "Mapping mismatch"

    def test_function_definition(self, default_options):
        """Test function definition."""
        lines = ["def foo(a, b):\n", "    return a + b\n"]
        fp = processor.FileProcessor("-", default_options, lines=lines)

        # Tokenize first line
        fp.tokens = []
        for token in fp.generate_tokens():
            fp.tokens.append(token)
            if token.type == tokenize.NEWLINE:
                break

        # Get Python result
        python_result = fp._build_logical_line_tokens_python()

        # Get C result
        c_result = _speedups.build_logical_line_tokens(
            fp.tokens, fp.lines, FSTRING_MIDDLE, TSTRING_MIDDLE,
        )

        assert c_result[0] == python_result[0], "Comments mismatch"
        assert c_result[1] == python_result[1], "Logical line mismatch"

    def test_with_comment(self, default_options):
        """Test line with comment."""
        lines = ["x = 1  # comment\n"]
        fp = processor.FileProcessor("-", default_options, lines=lines)

        tokens = list(fp.generate_tokens())
        fp.tokens = tokens[:-1]

        python_result = fp._build_logical_line_tokens_python()
        c_result = _speedups.build_logical_line_tokens(
            fp.tokens, fp.lines, FSTRING_MIDDLE, TSTRING_MIDDLE,
        )

        assert c_result[0] == python_result[0], "Comments mismatch"
        assert c_result[1] == python_result[1], "Logical line mismatch"

    def test_with_string(self, default_options):
        """Test line with string literal."""
        lines = ['x = "hello world"\n']
        fp = processor.FileProcessor("-", default_options, lines=lines)

        tokens = list(fp.generate_tokens())
        fp.tokens = tokens[:-1]

        python_result = fp._build_logical_line_tokens_python()
        c_result = _speedups.build_logical_line_tokens(
            fp.tokens, fp.lines, FSTRING_MIDDLE, TSTRING_MIDDLE,
        )

        assert c_result[0] == python_result[0], "Comments mismatch"
        assert c_result[1] == python_result[1], "Logical line mismatch"

    def test_multiline_statement(self, default_options):
        """Test multiline statement with continuation."""
        lines = ["x = (1 +\n", "     2)\n"]
        fp = processor.FileProcessor("-", default_options, lines=lines)

        # Collect all tokens for the logical line
        fp.tokens = []
        paren_depth = 0
        for token in fp.generate_tokens():
            fp.tokens.append(token)
            if token.type == tokenize.OP:
                if token.string in "([{":
                    paren_depth += 1
                elif token.string in ")]}":
                    paren_depth -= 1
            if token.type == tokenize.NEWLINE and paren_depth == 0:
                break

        python_result = fp._build_logical_line_tokens_python()
        c_result = _speedups.build_logical_line_tokens(
            fp.tokens, fp.lines, FSTRING_MIDDLE, TSTRING_MIDDLE,
        )

        assert c_result[0] == python_result[0], "Comments mismatch"
        assert c_result[1] == python_result[1], "Logical line mismatch"

    def test_empty_tokens(self, default_options):
        """Test with empty token list."""
        lines = ["x = 1\n"]
        fp = processor.FileProcessor("-", default_options, lines=lines)
        fp.tokens = []

        python_result = fp._build_logical_line_tokens_python()
        c_result = _speedups.build_logical_line_tokens(
            fp.tokens, fp.lines, FSTRING_MIDDLE, TSTRING_MIDDLE,
        )

        assert c_result == python_result


class TestNoqaLineMappingEquivalence:
    """Test that C and Python _noqa_line_mapping produce identical results."""

    def _python_noqa_line_mapping(self, file_tokens, lines):
        """Python implementation of noqa_line_mapping."""
        ret = {}
        min_line = len(lines) + 2
        max_line = -1

        for tp, _, (s_line, _), (e_line, _), _ in file_tokens:
            if tp == tokenize.ENDMARKER or tp == tokenize.DEDENT:
                continue

            min_line = min(min_line, s_line)
            max_line = max(max_line, e_line)

            if tp in (tokenize.NL, tokenize.NEWLINE):
                line_range = range(min_line, max_line + 1)
                joined = "".join(lines[min_line - 1: max_line])
                ret.update(dict.fromkeys(line_range, joined))

                min_line = len(lines) + 2
                max_line = -1

        return ret

    def test_simple_file(self, default_options):
        """Test simple file."""
        lines = ["x = 1\n", "y = 2\n"]
        fp = processor.FileProcessor("-", default_options, lines=lines)

        file_tokens = fp.file_tokens
        python_result = self._python_noqa_line_mapping(file_tokens, lines)
        c_result = _speedups.noqa_line_mapping(
            file_tokens, lines,
            tokenize.ENDMARKER, tokenize.DEDENT,
            tokenize.NL, tokenize.NEWLINE,
        )

        assert c_result == python_result

    def test_multiline_string(self, default_options):
        """Test file with multiline string."""
        lines = ['x = """\n', "hello\n", 'world"""\n']
        fp = processor.FileProcessor("-", default_options, lines=lines)

        file_tokens = fp.file_tokens
        python_result = self._python_noqa_line_mapping(file_tokens, lines)
        c_result = _speedups.noqa_line_mapping(
            file_tokens, lines,
            tokenize.ENDMARKER, tokenize.DEDENT,
            tokenize.NL, tokenize.NEWLINE,
        )

        assert c_result == python_result

    def test_line_continuation(self, default_options):
        """Test file with line continuation."""
        lines = ["x = 1 + \\\n", "    2\n"]
        fp = processor.FileProcessor("-", default_options, lines=lines)

        file_tokens = fp.file_tokens
        python_result = self._python_noqa_line_mapping(file_tokens, lines)
        c_result = _speedups.noqa_line_mapping(
            file_tokens, lines,
            tokenize.ENDMARKER, tokenize.DEDENT,
            tokenize.NL, tokenize.NEWLINE,
        )

        assert c_result == python_result

    def test_empty_file(self, default_options):
        """Test empty file."""
        lines = []
        # Can't use FileProcessor for empty file, test directly
        c_result = _speedups.noqa_line_mapping(
            [], lines,
            tokenize.ENDMARKER, tokenize.DEDENT,
            tokenize.NL, tokenize.NEWLINE,
        )

        assert c_result == {}


class TestSpeedupsEdgeCases:
    """Test edge cases and error handling in the C extension."""

    def test_mutate_string_empty(self):
        """Test mutate_string with minimal input."""
        # Very short strings
        assert _speedups.mutate_string("") == ""
        assert _speedups.mutate_string("x") == "x"

    def test_is_eol_token_invalid_token(self):
        """Test is_eol_token with invalid token format."""
        NEWLINE = frozenset([tokenize.NL, tokenize.NEWLINE])

        # Too short tuple - should return False, not crash
        result = _speedups.is_eol_token((1,), NEWLINE)
        assert result is False

        result = _speedups.is_eol_token((1, 2, 3), NEWLINE)
        assert result is False

    def test_is_multiline_string_invalid_token(self):
        """Test is_multiline_string with invalid token format."""
        # Too short tuple - should return False, not crash
        result = _speedups.is_multiline_string((1,), FSTRING_END, TSTRING_END)
        assert result is False

    def test_build_logical_line_tokens_invalid_input(self):
        """Test build_logical_line_tokens with invalid input."""
        # Should raise TypeError for non-list input
        with pytest.raises(TypeError):
            _speedups.build_logical_line_tokens(
                "not a list", [], FSTRING_MIDDLE, TSTRING_MIDDLE,
            )

        with pytest.raises(TypeError):
            _speedups.build_logical_line_tokens(
                [], "not a list", FSTRING_MIDDLE, TSTRING_MIDDLE,
            )

    def test_noqa_line_mapping_invalid_input(self):
        """Test noqa_line_mapping with invalid input."""
        with pytest.raises(TypeError):
            _speedups.noqa_line_mapping(
                "not a list", [],
                tokenize.ENDMARKER, tokenize.DEDENT,
                tokenize.NL, tokenize.NEWLINE,
            )
