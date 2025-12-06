/*
 * Flake8 Performance-Critical Functions
 *
 * This C extension module provides optimized implementations of
 * performance-critical functions identified through profiling.
 *
 * Free-threading (PEP 703) support:
 * - Python 3.13+ with Py_mod_gil slot
 * - Thread-safe one-time initialization
 * - All exported functions are stateless and thread-safe
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

/*
 * Token type constants - dynamically loaded from tokenize module at init.
 * This ensures compatibility across Python versions where token values may differ.
 * These are effectively immutable after initialization.
 */
static int TOK_NL = -1;
static int TOK_NEWLINE = -1;
static int TOK_INDENT = -1;
static int TOK_DEDENT = -1;
static int TOK_COMMENT = -1;
static int TOK_STRING = -1;
static int TOK_ENDMARKER = -1;

/* Cached Python objects for performance (immutable after init) */
static PyObject* NEWLINE_STR = NULL;

/* Thread-safe initialization flag and mutex for Python 3.13+ free-threading */
#if PY_VERSION_HEX >= 0x030D0000
#include <pythread.h>
static int _speedups_initialized = 0;
static PyMutex _speedups_init_mutex = {0};
#endif

/*
 * Helper to get an integer attribute from a module.
 * Returns -1 and sets error on failure.
 */
static int
get_int_attr(PyObject* module, const char* name)
{
    PyObject* attr = PyObject_GetAttrString(module, name);
    if (attr == NULL) {
        return -1;
    }
    int value = (int)PyLong_AsLong(attr);
    Py_DECREF(attr);
    if (PyErr_Occurred()) {
        return -1;
    }
    return value;
}

/* Forward declarations */
static PyObject* build_logical_line_tokens(PyObject* self, PyObject* args);
static PyObject* mutate_string(PyObject* self, PyObject* args);
static PyObject* is_eol_token(PyObject* self, PyObject* args);
static PyObject* is_multiline_string(PyObject* self, PyObject* args);
static PyObject* noqa_line_mapping(PyObject* self, PyObject* args);

/*
 * mutate_string(text) -> str
 *
 * Replace contents of a string literal with 'xxx' to prevent syntax matching.
 * Examples:
 *   '"abc"' -> '"xxx"'
 *   "'''abc'''" -> "'''xxx'''"
 *   "r'abc'" -> "r'xxx'"
 *
 * This implementation works with Unicode characters, not bytes.
 */
static PyObject*
mutate_string(PyObject* self, PyObject* args)
{
    PyObject* text_obj;

    if (!PyArg_ParseTuple(args, "U", &text_obj)) {
        return NULL;
    }

    Py_ssize_t text_len = PyUnicode_GET_LENGTH(text_obj);

    if (text_len < 2) {
        /* String too short to have quotes */
        Py_INCREF(text_obj);
        return text_obj;
    }

    /* Find the quote character (last char of string) */
    Py_UCS4 quote_char = PyUnicode_READ_CHAR(text_obj, text_len - 1);

    /* Find start position after any prefix and opening quotes */
    Py_ssize_t start = 0;
    for (start = 0; start < text_len; start++) {
        if (PyUnicode_READ_CHAR(text_obj, start) == quote_char) {
            start++;
            break;
        }
    }

    Py_ssize_t end = text_len - 1;

    /* Check for triple-quoted strings */
    if (text_len >= 6 &&
        PyUnicode_READ_CHAR(text_obj, text_len - 1) ==
            PyUnicode_READ_CHAR(text_obj, text_len - 2) &&
        PyUnicode_READ_CHAR(text_obj, text_len - 2) ==
            PyUnicode_READ_CHAR(text_obj, text_len - 3) &&
        (quote_char == '"' || quote_char == '\'')) {
        start += 2;
        end -= 2;
    }

    /* Calculate size of x fill */
    Py_ssize_t x_count = end - start;
    if (x_count < 0) x_count = 0;

    /* Build result: prefix + 'x' * x_count + suffix */
    /* Get prefix (text[:start]) */
    PyObject* prefix = PyUnicode_Substring(text_obj, 0, start);
    if (prefix == NULL) return NULL;

    /* Get suffix (text[end:]) */
    PyObject* suffix = PyUnicode_Substring(text_obj, end, text_len);
    if (suffix == NULL) {
        Py_DECREF(prefix);
        return NULL;
    }

    /* Create 'x' * x_count */
    PyObject* x_fill = PyUnicode_New(x_count, 'x');
    if (x_fill == NULL) {
        Py_DECREF(prefix);
        Py_DECREF(suffix);
        return NULL;
    }
    /* Fill with 'x' characters */
    int kind = PyUnicode_KIND(x_fill);
    void* data = PyUnicode_DATA(x_fill);
    for (Py_ssize_t i = 0; i < x_count; i++) {
        PyUnicode_WRITE(kind, data, i, 'x');
    }

    /* Concatenate: prefix + x_fill + suffix */
    PyObject* temp = PyUnicode_Concat(prefix, x_fill);
    Py_DECREF(prefix);
    Py_DECREF(x_fill);
    if (temp == NULL) {
        Py_DECREF(suffix);
        return NULL;
    }

    PyObject* result = PyUnicode_Concat(temp, suffix);
    Py_DECREF(temp);
    Py_DECREF(suffix);

    return result;
}


/*
 * Check if token type should be skipped (NL, NEWLINE, INDENT, DEDENT)
 */
static inline int
should_skip_token(int token_type)
{
    return (token_type == TOK_NL ||
            token_type == TOK_NEWLINE ||
            token_type == TOK_INDENT ||
            token_type == TOK_DEDENT);
}


/*
 * is_eol_token(token, newline_types) -> bool
 *
 * Check if the token is an end-of-line token.
 * This is a hot function called for every token during processing.
 *
 * Args:
 *   token: TokenInfo tuple (type, text, start, end, line)
 *   newline_types: frozenset of newline token types {NL, NEWLINE}
 *
 * Returns:
 *   True if token is end-of-line, False otherwise
 */
static PyObject*
is_eol_token(PyObject* self, PyObject* args)
{
    PyObject* token;
    PyObject* newline_types;

    if (!PyArg_ParseTuple(args, "OO", &token, &newline_types)) {
        return NULL;
    }

    if (!PyTuple_Check(token) || PyTuple_GET_SIZE(token) < 5) {
        Py_RETURN_FALSE;
    }

    /* Get token type */
    PyObject* py_token_type = PyTuple_GET_ITEM(token, 0);
    int token_type = (int)PyLong_AsLong(py_token_type);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        Py_RETURN_FALSE;
    }

    /* Check if token type is in newline_types */
    int in_newline = PySet_Contains(newline_types, py_token_type);
    if (in_newline == 1) {
        Py_RETURN_TRUE;
    }
    if (in_newline == -1) {
        PyErr_Clear();
    }

    /* Check for line continuation: token[4][token[3][1]:].lstrip() == "\\\n" */
    PyObject* py_line = PyTuple_GET_ITEM(token, 4);
    PyObject* py_end = PyTuple_GET_ITEM(token, 3);

    if (!PyUnicode_Check(py_line) || !PyTuple_Check(py_end) ||
        PyTuple_GET_SIZE(py_end) < 2) {
        Py_RETURN_FALSE;
    }

    Py_ssize_t end_col = PyLong_AsSsize_t(PyTuple_GET_ITEM(py_end, 1));
    if (PyErr_Occurred()) {
        PyErr_Clear();
        Py_RETURN_FALSE;
    }

    const char* line_str = PyUnicode_AsUTF8(py_line);
    Py_ssize_t line_len = PyUnicode_GET_LENGTH(py_line);

    if (!line_str || end_col < 0 || end_col > line_len) {
        Py_RETURN_FALSE;
    }

    /* Skip whitespace from end_col position */
    Py_ssize_t i = end_col;
    while (i < line_len && (line_str[i] == ' ' || line_str[i] == '\t')) {
        i++;
    }

    /* Check if remaining is "\\\n" */
    if (i + 2 <= line_len && line_str[i] == '\\' && line_str[i + 1] == '\n') {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}


/*
 * is_multiline_string(token, fstring_end_type, tstring_end_type) -> bool
 *
 * Check if this is a multiline string token.
 *
 * Args:
 *   token: TokenInfo tuple (type, text, start, end, line)
 *   fstring_end_type: FSTRING_END token type or -1
 *   tstring_end_type: TSTRING_END token type or -1
 *
 * Returns:
 *   True if token is multiline string, False otherwise
 */
static PyObject*
is_multiline_string(PyObject* self, PyObject* args)
{
    PyObject* token;
    int fstring_end_type;
    int tstring_end_type;

    if (!PyArg_ParseTuple(args, "Oii", &token, &fstring_end_type, &tstring_end_type)) {
        return NULL;
    }

    if (!PyTuple_Check(token) || PyTuple_GET_SIZE(token) < 2) {
        Py_RETURN_FALSE;
    }

    PyObject* py_token_type = PyTuple_GET_ITEM(token, 0);
    int token_type = (int)PyLong_AsLong(py_token_type);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        Py_RETURN_FALSE;
    }

    /* Check if FSTRING_END or TSTRING_END */
    if (token_type == fstring_end_type || token_type == tstring_end_type) {
        Py_RETURN_TRUE;
    }

    /* Check if STRING with newline */
    if (token_type == TOK_STRING) {
        PyObject* py_text = PyTuple_GET_ITEM(token, 1);
        if (PyUnicode_Check(py_text) && NEWLINE_STR != NULL) {
            Py_ssize_t idx = PyUnicode_Find(py_text,
                NEWLINE_STR, 0, PyUnicode_GET_LENGTH(py_text), 1);
            if (idx >= 0) {
                Py_RETURN_TRUE;
            }
        }
    }

    Py_RETURN_FALSE;
}


/*
 * noqa_line_mapping(file_tokens, lines, endmarker_type, dedent_type) -> dict
 *
 * Build mapping from line number to the line we'll search for `noqa` in.
 * This is called once per file but is expensive due to iterating all tokens.
 *
 * Args:
 *   file_tokens: list of all tokens in the file
 *   lines: list of source lines
 *   endmarker_type: ENDMARKER token type
 *   dedent_type: DEDENT token type
 *
 * Returns:
 *   dict mapping line numbers to joined lines for noqa searching
 */
static PyObject*
noqa_line_mapping(PyObject* self, PyObject* args)
{
    PyObject* file_tokens;
    PyObject* lines;
    int endmarker_type;
    int dedent_type;
    int nl_type;
    int newline_type;

    if (!PyArg_ParseTuple(args, "OOiiii", &file_tokens, &lines,
                          &endmarker_type, &dedent_type, &nl_type, &newline_type)) {
        return NULL;
    }

    if (!PyList_Check(file_tokens) || !PyList_Check(lines)) {
        PyErr_SetString(PyExc_TypeError, "file_tokens and lines must be lists");
        return NULL;
    }

    Py_ssize_t num_tokens = PyList_GET_SIZE(file_tokens);
    Py_ssize_t num_lines = PyList_GET_SIZE(lines);

    PyObject* ret = PyDict_New();
    if (!ret) return NULL;

    Py_ssize_t min_line = num_lines + 2;
    Py_ssize_t max_line = -1;

    for (Py_ssize_t i = 0; i < num_tokens; i++) {
        PyObject* token = PyList_GET_ITEM(file_tokens, i);
        if (!PyTuple_Check(token) || PyTuple_GET_SIZE(token) < 4) {
            continue;
        }

        int tp = (int)PyLong_AsLong(PyTuple_GET_ITEM(token, 0));
        if (PyErr_Occurred()) {
            PyErr_Clear();
            continue;
        }

        if (tp == endmarker_type || tp == dedent_type) {
            continue;
        }

        PyObject* py_start = PyTuple_GET_ITEM(token, 2);
        PyObject* py_end = PyTuple_GET_ITEM(token, 3);

        if (!PyTuple_Check(py_start) || !PyTuple_Check(py_end)) {
            continue;
        }

        Py_ssize_t s_line = PyLong_AsSsize_t(PyTuple_GET_ITEM(py_start, 0));
        Py_ssize_t e_line = PyLong_AsSsize_t(PyTuple_GET_ITEM(py_end, 0));
        if (PyErr_Occurred()) {
            PyErr_Clear();
            continue;
        }

        if (s_line < min_line) min_line = s_line;
        if (e_line > max_line) max_line = e_line;

        /* Check for NL or NEWLINE */
        if (tp == nl_type || tp == newline_type) {
            /* Update ret with range mapping */
            if (min_line <= max_line && min_line >= 1 && max_line <= num_lines) {
                /* Join lines from min_line to max_line */
                PyObject* parts = PyList_New(0);
                if (parts) {
                    for (Py_ssize_t ln = min_line - 1; ln < max_line; ln++) {
                        PyList_Append(parts, PyList_GET_ITEM(lines, ln));
                    }
                    PyObject* empty = PyUnicode_FromString("");
                    PyObject* joined = PyUnicode_Join(empty, parts);
                    Py_DECREF(empty);
                    Py_DECREF(parts);

                    if (joined) {
                        for (Py_ssize_t ln = min_line; ln <= max_line; ln++) {
                            PyObject* key = PyLong_FromSsize_t(ln);
                            if (key) {
                                PyDict_SetItem(ret, key, joined);
                                Py_DECREF(key);
                            }
                        }
                        Py_DECREF(joined);
                    }
                }
            }

            min_line = num_lines + 2;
            max_line = -1;
        }
    }

    return ret;
}


/*
 * build_logical_line_tokens(tokens, lines, fstring_middle_type, tstring_middle_type)
 *     -> (comments, logical, mapping)
 *
 * Build the mapping, comments, and logical line lists from tokens.
 * This is the hot function called for every logical line.
 *
 * Args:
 *   tokens: list of (type, text, start, end, line) tuples
 *   lines: list of source lines
 *   fstring_middle_type: token type for FSTRING_MIDDLE (or -1 if not available)
 *   tstring_middle_type: token type for TSTRING_MIDDLE (or -1 if not available)
 *
 * Returns:
 *   (comments, logical, mapping) where:
 *   - comments: list of comment strings
 *   - logical: list of strings forming the logical line
 *   - mapping: list of (length, (row, col)) tuples
 */
static PyObject*
build_logical_line_tokens(PyObject* self, PyObject* args)
{
    PyObject* tokens;
    PyObject* lines;
    int fstring_middle_type;
    int tstring_middle_type;

    if (!PyArg_ParseTuple(args, "OOii", &tokens, &lines,
                          &fstring_middle_type, &tstring_middle_type)) {
        return NULL;
    }

    if (!PyList_Check(tokens)) {
        PyErr_SetString(PyExc_TypeError, "tokens must be a list");
        return NULL;
    }

    if (!PyList_Check(lines)) {
        PyErr_SetString(PyExc_TypeError, "lines must be a list");
        return NULL;
    }

    Py_ssize_t num_tokens = PyList_GET_SIZE(tokens);

    /* Create result lists */
    PyObject* logical = PyList_New(0);
    PyObject* comments = PyList_New(0);
    PyObject* mapping = PyList_New(0);

    if (!logical || !comments || !mapping) {
        Py_XDECREF(logical);
        Py_XDECREF(comments);
        Py_XDECREF(mapping);
        return NULL;
    }

    Py_ssize_t length = 0;
    Py_ssize_t previous_row = -1;
    Py_ssize_t previous_column = -1;

    for (Py_ssize_t i = 0; i < num_tokens; i++) {
        PyObject* token = PyList_GET_ITEM(tokens, i);

        if (!PyTuple_Check(token) || PyTuple_GET_SIZE(token) < 5) {
            continue;
        }

        /* Unpack token: (type, text, start, end, line) */
        PyObject* py_token_type = PyTuple_GET_ITEM(token, 0);
        PyObject* py_text = PyTuple_GET_ITEM(token, 1);
        PyObject* py_start = PyTuple_GET_ITEM(token, 2);
        PyObject* py_end = PyTuple_GET_ITEM(token, 3);
        PyObject* py_line = PyTuple_GET_ITEM(token, 4);

        int token_type = (int)PyLong_AsLong(py_token_type);
        if (PyErr_Occurred()) continue;

        /* Skip certain token types */
        if (should_skip_token(token_type)) {
            continue;
        }

        /* Handle first mapping entry */
        if (PyList_GET_SIZE(mapping) == 0) {
            PyObject* entry = PyTuple_Pack(2, PyLong_FromSsize_t(0), py_start);
            if (entry) {
                PyList_Append(mapping, entry);
                Py_DECREF(entry);
            }
        }

        /* Handle comments */
        if (token_type == TOK_COMMENT) {
            PyList_Append(comments, py_text);
            continue;
        }

        /* Get text as C string */
        const char* text_str = PyUnicode_AsUTF8(py_text);
        if (!text_str) continue;
        Py_ssize_t text_len = PyUnicode_GET_LENGTH(py_text);

        /* Handle STRING tokens - mutate to prevent syntax matching */
        PyObject* processed_text = NULL;
        if (token_type == TOK_STRING) {
            /* Call mutate_string */
            PyObject* mutate_args = PyTuple_Pack(1, py_text);
            if (mutate_args) {
                processed_text = mutate_string(self, mutate_args);
                Py_DECREF(mutate_args);
            }
            if (!processed_text) {
                processed_text = py_text;
                Py_INCREF(processed_text);
            }
        }
        /* Handle FSTRING_MIDDLE/TSTRING_MIDDLE tokens */
        else if (token_type == fstring_middle_type ||
                 token_type == tstring_middle_type) {
            /* Count braces and adjust */
            Py_ssize_t brace_offset = 0;
            for (Py_ssize_t j = 0; j < text_len; j++) {
                if (text_str[j] == '{' || text_str[j] == '}') {
                    brace_offset++;
                }
            }
            Py_ssize_t new_len = text_len + brace_offset;
            char* new_text = PyMem_Malloc(new_len + 1);
            if (new_text) {
                memset(new_text, 'x', new_len);
                new_text[new_len] = '\0';
                processed_text = PyUnicode_FromStringAndSize(new_text, new_len);
                PyMem_Free(new_text);
            }
            if (!processed_text) {
                processed_text = py_text;
                Py_INCREF(processed_text);
            }
        }
        else {
            processed_text = py_text;
            Py_INCREF(processed_text);
        }

        /* Get start position */
        if (!PyTuple_Check(py_start) || PyTuple_GET_SIZE(py_start) < 2) {
            Py_DECREF(processed_text);
            continue;
        }
        Py_ssize_t start_row = PyLong_AsSsize_t(PyTuple_GET_ITEM(py_start, 0));
        Py_ssize_t start_column = PyLong_AsSsize_t(PyTuple_GET_ITEM(py_start, 1));

        /* Get end position */
        if (!PyTuple_Check(py_end) || PyTuple_GET_SIZE(py_end) < 2) {
            Py_DECREF(processed_text);
            continue;
        }
        Py_ssize_t end_row = PyLong_AsSsize_t(PyTuple_GET_ITEM(py_end, 0));
        Py_ssize_t end_column = PyLong_AsSsize_t(PyTuple_GET_ITEM(py_end, 1));

        /* Handle line continuations and spacing */
        PyObject* final_text = processed_text;
        if (previous_row != -1 && previous_column != -1) {
            if (previous_row != start_row) {
                /* Different row - may need to add space */
                Py_ssize_t row_index = previous_row - 1;
                if (row_index >= 0 && row_index < PyList_GET_SIZE(lines)) {
                    PyObject* prev_line = PyList_GET_ITEM(lines, row_index);
                    const char* prev_line_str = PyUnicode_AsUTF8(prev_line);
                    Py_ssize_t prev_line_len = PyUnicode_GET_LENGTH(prev_line);
                    Py_ssize_t col_index = previous_column - 1;

                    if (prev_line_str && col_index >= 0 && col_index < prev_line_len) {
                        char previous_text_char = prev_line_str[col_index];
                        const char* current_text = PyUnicode_AsUTF8(processed_text);

                        if (current_text && strlen(current_text) > 0) {
                            char current_first = current_text[0];

                            /* Check if we need space between tokens */
                            if (previous_text_char == ',' ||
                                (previous_text_char != '{' &&
                                 previous_text_char != '[' &&
                                 previous_text_char != '(' &&
                                 current_first != '}' &&
                                 current_first != ']' &&
                                 current_first != ')')) {
                                /* Prepend space */
                                PyObject* space = PyUnicode_FromString(" ");
                                if (space) {
                                    final_text = PyUnicode_Concat(space, processed_text);
                                    Py_DECREF(space);
                                    Py_DECREF(processed_text);
                                    if (!final_text) {
                                        continue;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else if (previous_column != start_column) {
                /* Same row, different column - copy intervening text */
                Py_ssize_t row_index = start_row - 1;
                if (row_index >= 0 && row_index < PyList_GET_SIZE(lines)) {
                    PyObject* line_obj = PyList_GET_ITEM(lines, row_index);
                    const char* line_str = PyUnicode_AsUTF8(line_obj);
                    Py_ssize_t line_len = PyUnicode_GET_LENGTH(line_obj);

                    if (line_str && previous_column >= 0 &&
                        previous_column < line_len && start_column <= line_len) {
                        Py_ssize_t gap_len = start_column - previous_column;
                        if (gap_len > 0) {
                            PyObject* gap = PyUnicode_FromStringAndSize(
                                line_str + previous_column, gap_len);
                            if (gap) {
                                final_text = PyUnicode_Concat(gap, processed_text);
                                Py_DECREF(gap);
                                Py_DECREF(processed_text);
                                if (!final_text) {
                                    continue;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Add to logical list */
        PyList_Append(logical, final_text);
        length += PyUnicode_GET_LENGTH(final_text);

        /* Add mapping entry */
        PyObject* mapping_entry = PyTuple_Pack(2,
            PyLong_FromSsize_t(length),
            py_end);
        if (mapping_entry) {
            PyList_Append(mapping, mapping_entry);
            Py_DECREF(mapping_entry);
        }

        previous_row = end_row;
        previous_column = end_column;

        if (final_text != processed_text) {
            Py_DECREF(final_text);
        } else {
            Py_DECREF(processed_text);
        }
    }

    /* Build result tuple */
    PyObject* result = PyTuple_Pack(3, comments, logical, mapping);
    Py_DECREF(comments);
    Py_DECREF(logical);
    Py_DECREF(mapping);

    return result;
}


/* Module method table */
static PyMethodDef SpeedupsMethods[] = {
    {"build_logical_line_tokens", build_logical_line_tokens, METH_VARARGS,
     "Build logical line tokens from a token list.\n\n"
     "Args:\n"
     "    tokens: list of token tuples (type, text, start, end, line)\n"
     "    lines: list of source lines\n"
     "    fstring_middle_type: FSTRING_MIDDLE token type or -1\n"
     "    tstring_middle_type: TSTRING_MIDDLE token type or -1\n\n"
     "Returns:\n"
     "    (comments, logical, mapping) tuple"},
    {"mutate_string", mutate_string, METH_VARARGS,
     "Replace string contents with 'xxx' to prevent syntax matching.\n\n"
     "Args:\n"
     "    text: string literal including quotes\n\n"
     "Returns:\n"
     "    mutated string"},
    {"is_eol_token", is_eol_token, METH_VARARGS,
     "Check if token is an end-of-line token.\n\n"
     "Args:\n"
     "    token: TokenInfo tuple\n"
     "    newline_types: frozenset of newline token types\n\n"
     "Returns:\n"
     "    True if end-of-line token, False otherwise"},
    {"is_multiline_string", is_multiline_string, METH_VARARGS,
     "Check if token is a multiline string.\n\n"
     "Args:\n"
     "    token: TokenInfo tuple\n"
     "    fstring_end_type: FSTRING_END token type or -1\n"
     "    tstring_end_type: TSTRING_END token type or -1\n\n"
     "Returns:\n"
     "    True if multiline string, False otherwise"},
    {"noqa_line_mapping", noqa_line_mapping, METH_VARARGS,
     "Build mapping from line number to noqa search line.\n\n"
     "Args:\n"
     "    file_tokens: list of all tokens\n"
     "    lines: list of source lines\n"
     "    endmarker_type: ENDMARKER token type\n"
     "    dedent_type: DEDENT token type\n"
     "    nl_type: NL token type\n"
     "    newline_type: NEWLINE token type\n\n"
     "Returns:\n"
     "    dict mapping line numbers to joined lines"},
    {NULL, NULL, 0, NULL}
};


/*
 * Module execution function - performs one-time initialization.
 * Called during module import. Thread-safe for free-threading builds.
 */
static int
speedups_exec(PyObject *module)
{
#if PY_VERSION_HEX >= 0x030D0000
    /* Thread-safe initialization for free-threading builds */
    PyMutex_Lock(&_speedups_init_mutex);
    if (_speedups_initialized) {
        PyMutex_Unlock(&_speedups_init_mutex);
        return 0;  /* Already initialized */
    }
#endif

    /* Initialize cached Python objects */
    if (NEWLINE_STR == NULL) {
        NEWLINE_STR = PyUnicode_FromString("\n");
        if (NEWLINE_STR == NULL) {
#if PY_VERSION_HEX >= 0x030D0000
            PyMutex_Unlock(&_speedups_init_mutex);
#endif
            return -1;
        }
    }

    /* Load token type constants from tokenize module */
    if (TOK_NL < 0) {
        PyObject* tokenize = PyImport_ImportModule("tokenize");
        if (tokenize == NULL) {
            Py_CLEAR(NEWLINE_STR);
#if PY_VERSION_HEX >= 0x030D0000
            PyMutex_Unlock(&_speedups_init_mutex);
#endif
            return -1;
        }

        TOK_NL = get_int_attr(tokenize, "NL");
        TOK_NEWLINE = get_int_attr(tokenize, "NEWLINE");
        TOK_INDENT = get_int_attr(tokenize, "INDENT");
        TOK_DEDENT = get_int_attr(tokenize, "DEDENT");
        TOK_COMMENT = get_int_attr(tokenize, "COMMENT");
        TOK_STRING = get_int_attr(tokenize, "STRING");
        TOK_ENDMARKER = get_int_attr(tokenize, "ENDMARKER");

        Py_DECREF(tokenize);

        /* Check if any attribute lookup failed */
        if (TOK_NL < 0 || TOK_NEWLINE < 0 || TOK_INDENT < 0 ||
            TOK_DEDENT < 0 || TOK_COMMENT < 0 || TOK_STRING < 0 ||
            TOK_ENDMARKER < 0) {
            Py_CLEAR(NEWLINE_STR);
            TOK_NL = -1;  /* Reset to invalid state */
#if PY_VERSION_HEX >= 0x030D0000
            PyMutex_Unlock(&_speedups_init_mutex);
#endif
            PyErr_SetString(PyExc_RuntimeError,
                            "Failed to load token constants from tokenize module");
            return -1;
        }
    }

#if PY_VERSION_HEX >= 0x030D0000
    _speedups_initialized = 1;
    PyMutex_Unlock(&_speedups_init_mutex);
#endif

    return 0;  /* Success */
}


/* Module slot definitions for multi-phase initialization */
static PyModuleDef_Slot speedups_slots[] = {
    {Py_mod_exec, speedups_exec},
#if PY_VERSION_HEX >= 0x030D0000
    /* Declare that this module supports free-threading (PEP 703) */
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
#if PY_VERSION_HEX >= 0x030C0000
    /* Support multiple interpreters (Python 3.12+) */
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
    {0, NULL}
};


/* Module definition - using multi-phase initialization */
static struct PyModuleDef speedupsmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_speedups",
    .m_doc = "Flake8 performance-critical functions implemented in C.\n\n"
             "This module provides optimized implementations of hot-spot\n"
             "functions identified through profiling. It supports Python 3.13+\n"
             "free-threading mode (PEP 703).",
    .m_size = 0,  /* No per-module state needed; globals are immutable after init */
    .m_methods = SpeedupsMethods,
    .m_slots = speedups_slots,
};


/* Module initialization - returns module def for multi-phase init */
PyMODINIT_FUNC
PyInit__speedups(void)
{
    return PyModuleDef_Init(&speedupsmodule);
}
