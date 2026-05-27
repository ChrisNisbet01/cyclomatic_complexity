# cyclomatic_complexity

A standalone C tool that parses C source files and reports the **cyclomatic complexity** (McCabe) of each function, sorted from highest to lowest.

## Building

```bash
cmake -S . -B build
cmake --build build
```

Requires: CMake 3.16+, C11 compiler, `easy_pc` library (fetched automatically).

## Usage

```bash
# Parse a source file directly (no preprocessing)
build/cyclomatic_complexity --no-preprocess file.c

# Parse with clang preprocessing (handles #include, -I, -D)
build/cyclomatic_complexity file.c
build/cyclomatic_complexity -I /path/to/includes -DDEBUG file.c

# Preprocess only, output to stdout
build/cyclomatic_complexity -E file.c
```

Output format:
```
filename:line: function 'name' has cyclomatic complexity N
```

Results are sorted from highest complexity to lowest.

### Options

| Flag | Description |
|------|-------------|
| `--no-preprocess` | Skip preprocessing, parse the file directly |
| `-E` | Preprocess only, output to stdout |
| `-o <file>` | Output filename for preprocessed output |
| `-I <dir>` | Add include directory |
| `-D <macro>` | Define macro |
| `-h, --help` | Show help |

## Cyclomatic Complexity

The tool uses McCabe's cyclomatic complexity metric:

```
M = 1 + number of decision points
```

Decision points counted: `if`, `else if`, `while`, `do-while`, `for`, `case`, `default`, ternary `?:`, and logical `&&`/`||` operators.

- A function with no control flow has complexity 1
- A function with one if-statement has complexity 2
- Higher values indicate harder-to-test, more complex code

## Examples

```bash
build/cyclomatic_complexity myfile.c
build/cyclomatic_complexity --no-preprocess myfile.c
build/cyclomatic_complexity -I src/ -DDEBUG myfile.c
```
