"""Type stubs for the flake8._speedups C extension module."""
from __future__ import annotations

from typing import Sequence

def mutate_string(text: str) -> str:
    """Replace contents of a string literal with 'xxx' to prevent syntax matching.

    Args:
        text: String literal including quotes (e.g., '"abc"', "'''xyz'''")

    Returns:
        Mutated string with contents replaced by 'x' characters
    """
    ...

def is_eol_token(
    token: tuple[int, str, tuple[int, int], tuple[int, int], str],
    newline_types: frozenset[int],
) -> bool:
    """Check if the token is an end-of-line token.

    Args:
        token: TokenInfo tuple (type, text, start, end, line)
        newline_types: frozenset containing NL and NEWLINE token types

    Returns:
        True if token is end-of-line, False otherwise
    """
    ...

def is_multiline_string(
    token: tuple[int, str, tuple[int, int], tuple[int, int], str],
    fstring_end_type: int,
    tstring_end_type: int,
) -> bool:
    """Check if this is a multiline string token.

    Args:
        token: TokenInfo tuple (type, text, start, end, line)
        fstring_end_type: FSTRING_END token type or -1 if not available
        tstring_end_type: TSTRING_END token type or -1 if not available

    Returns:
        True if token is a multiline string, False otherwise
    """
    ...

def build_logical_line_tokens(
    tokens: list[tuple[int, str, tuple[int, int], tuple[int, int], str]],
    lines: list[str],
    fstring_middle_type: int,
    tstring_middle_type: int,
) -> tuple[list[str], list[str], list[tuple[int, tuple[int, int]]]]:
    """Build the mapping, comments, and logical line lists from tokens.

    Args:
        tokens: List of token tuples (type, text, start, end, line)
        lines: List of source lines
        fstring_middle_type: FSTRING_MIDDLE token type or -1 if not available
        tstring_middle_type: TSTRING_MIDDLE token type or -1 if not available

    Returns:
        Tuple of (comments, logical, mapping) where:
        - comments: List of comment strings
        - logical: List of strings forming the logical line
        - mapping: List of (length, (row, col)) tuples
    """
    ...

def noqa_line_mapping(
    file_tokens: Sequence[tuple[int, str, tuple[int, int], tuple[int, int], str]],
    lines: Sequence[str],
    endmarker_type: int,
    dedent_type: int,
    nl_type: int,
    newline_type: int,
) -> dict[int, str]:
    """Build mapping from line number to the line we'll search for noqa in.

    Args:
        file_tokens: List of all tokens in the file
        lines: List of source lines
        endmarker_type: ENDMARKER token type
        dedent_type: DEDENT token type
        nl_type: NL token type
        newline_type: NEWLINE token type

    Returns:
        Dict mapping line numbers to joined lines for noqa searching
    """
    ...
