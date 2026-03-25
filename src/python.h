/*
 * NovexOS - python.h
 * Native Python 3 subset interpreter.
 *
 * Supported:
 *   - Types       : int, str, bool, None
 *   - Variables   : assignment, augmented (+=, -=, *=)
 *   - Arithmetic  : +, -, *, //, %, unary -
 *   - Comparison  : ==, !=, <, >, <=, >=
 *   - Boolean     : and, or, not
 *   - Builtins    : print(), input(), len(), str(), int(), abs(), chr(), ord()
 *   - Control     : if / elif / else, while, for..in..range(), break, continue,
 * pass
 *   - REPL        : interactive mode with >>> prompt
 *
 * Source read from RAMFS. Max script size: RAMFS_MAX_FILESIZE (4096 bytes).
 */

#ifndef PYTHON_H
#define PYTHON_H

/* Run a Python script stored in RAMFS */
void python_run_file(const char *filename);

/* Launch interactive Python REPL */
void python_repl(void);

#endif /* PYTHON_H */