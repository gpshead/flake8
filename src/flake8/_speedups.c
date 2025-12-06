/*
 * Flake8 Performance-Critical Functions
 *
 * This C extension module provides optimized implementations of
 * performance-critical functions identified through profiling.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Token type constants - must match tokenize module (Python 3.11+) */
#define TOK_NL 62
#define TOK_NEWLINE 4
#define TOK_INDENT 5
#define TOK_DEDENT 6
#define TOK_COMMENT 61
#define TOK_STRING 3

/* Forward declarations */
static PyObject* build_logical_line_tokens(PyObject* self, PyObject* args);
static PyObject* mutate_string(PyObject* self, PyObject* args);

/*
 * mutate_string(text) -> str
 *
 * Replace contents of a string literal with 'xxx' to prevent syntax matching.
 * Examples:
 *   '"abc"' -> '"xxx"'
 *   "'''abc'''" -> "'''xxx'''"
 *   "r'abc'" -> "r'xxx'"
 */
static PyObject*
mutate_string(PyObject* self, PyObject* args)
{
    const char* text;
    Py_ssize_t text_len;

    if (!PyArg_ParseTuple(args, "s#", &text, &text_len)) {
        return NULL;
    }

    if (text_len < 2) {
        /* String too short to have quotes */
        return PyUnicode_FromStringAndSize(text, text_len);
    }

    /* Find the quote character (last char of string) */
    char quote_char = text[text_len - 1];

    /* Find start position after any prefix and opening quotes */
    Py_ssize_t start = 0;
    for (start = 0; start < text_len; start++) {
        if (text[start] == quote_char) {
            start++;
            break;
        }
    }

    Py_ssize_t end = text_len - 1;

    /* Check for triple-quoted strings */
    if (text_len >= 6 &&
        text[text_len - 1] == text[text_len - 2] &&
        text[text_len - 2] == text[text_len - 3] &&
        (text[text_len - 1] == '"' || text[text_len - 1] == '\'')) {
        start += 2;
        end -= 2;
    }

    /* Calculate size of result */
    Py_ssize_t prefix_len = start;
    Py_ssize_t suffix_len = text_len - end;
    Py_ssize_t x_count = end - start;

    if (x_count < 0) x_count = 0;

    Py_ssize_t result_len = prefix_len + x_count + suffix_len;

    /* Build result string */
    char* result = PyMem_Malloc(result_len + 1);
    if (result == NULL) {
        return PyErr_NoMemory();
    }

    /* Copy prefix */
    memcpy(result, text, prefix_len);

    /* Fill with 'x' */
    memset(result + prefix_len, 'x', x_count);

    /* Copy suffix */
    memcpy(result + prefix_len + x_count, text + end, suffix_len);

    result[result_len] = '\0';

    PyObject* py_result = PyUnicode_FromStringAndSize(result, result_len);
    PyMem_Free(result);

    return py_result;
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
    {NULL, NULL, 0, NULL}
};


/* Module definition */
static struct PyModuleDef speedupsmodule = {
    PyModuleDef_HEAD_INIT,
    "_speedups",
    "Flake8 performance-critical functions implemented in C",
    -1,
    SpeedupsMethods
};


/* Module initialization */
PyMODINIT_FUNC
PyInit__speedups(void)
{
    return PyModule_Create(&speedupsmodule);
}
