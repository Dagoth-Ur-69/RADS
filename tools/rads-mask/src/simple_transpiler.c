/**
 * RADS Masquerading - Simple Pattern-Based Transpiler
 *
 * A pragmatic, pattern-matching based transpiler for Python → RADS
 * This is the Ralph Wiggum loop approach: simple, testable, iterative.
 *
 * Version: 0.0.5 "CHAMELEON"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 4096
#define MAX_INDENT 100

// ============================================================================
// Utility Functions
// ============================================================================

static int count_leading_spaces(const char* line) {
    int count = 0;
    while (line[count] == ' ' || line[count] == '\t') {
        if (line[count] == '\t') count += 4;  // Tab = 4 spaces
        else count++;
    }
    return count;
}

static char* trim(char* str) {
    // Trim trailing whitespace
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    *(end + 1) = '\0';
    return str;
}

static int starts_with(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

/*
 * find_assignment() — locate a real Python assignment '=' in a statement.
 * Why: a bare '=' is an assignment, but '==', '!=', '<=', '>=', ':=' and
 * keyword-arguments ('foo(x=1)') must NOT be treated as one. The old code
 * checked the char BEFORE the first '=', which wrongly captured the second
 * '=' of 'a == b' and mangled it into 'turbo a = = b'.
 * How to extend: add more operator exceptions to the skip-conditions, or
 * teach the paren-depth check about brackets/braces for slice defaults.
 */
static char* find_assignment(const char* s) {
    int paren_depth = 0;
    for (const char* p = s; *p; p++) {
        if (*p == '(') paren_depth++;
        else if (*p == ')' && paren_depth > 0) paren_depth--;
        else if (*p == '=' && paren_depth == 0) {
            /* skip ==, !=, <=, >=, := ; guard p[-1] at string start (UB) */
            if (p[1] == '=' ||
                (p > s && (p[-1] == '=' || p[-1] == '!' ||
                           p[-1] == '<' || p[-1] == '>' || p[-1] == ':'))) {
                p++; /* skip the pair so '==' is not re-examined */
                continue;
            }
            return (char*)p;
        }
    }
    return NULL;
}


static void replace_in_line(char* line, const char* from, const char* to) {
    char buffer[MAX_LINE_LENGTH];
    char* pos = line;
    char* temp = buffer;
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);

    while (*pos) {
        if (strncmp(pos, from, from_len) == 0) {
            strcpy(temp, to);
            temp += to_len;
            pos += from_len;
        } else {
            *temp++ = *pos++;
        }
    }
    *temp = '\0';
    strcpy(line, buffer);
}

// ============================================================================
// Transpilation Rules
// ============================================================================

static void transpile_line(const char* input, char* output, int* indent_level) {
    char line[MAX_LINE_LENGTH];
    strncpy(line, input, MAX_LINE_LENGTH - 1);
    line[MAX_LINE_LENGTH - 1] = '\0';

    // Calculate indentation
    int leading_spaces = count_leading_spaces(line);
    int new_indent = leading_spaces / 4;

    // Skip to non-whitespace
    const char* trimmed = line + leading_spaces;

    // Skip empty lines, comments, and pass statements
    if (*trimmed == '\0' || *trimmed == '#' || strcmp(trimmed, "pass") == 0) {
        output[0] = '\0';
        return;
    }

    // Start building output with proper indentation
    output[0] = '\0';
    for (int i = 0; i < new_indent; i++) {
        strcat(output, "    ");
    }

    char converted[MAX_LINE_LENGTH];
    strcpy(converted, trimmed);

    // Rule 1: def function_name(...): → blast function_name(...) {
    if (starts_with(converted, "def ")) {
        char* colon = strrchr(converted, ':');
        if (colon) {
            *colon = '\0';
            sprintf(output + strlen(output), "blast %s {", converted + 4);
            *indent_level = new_indent + 1;
            return;
        }
    }

    // Rule 2: Variable assignment with type inference
    // Python: x = 42  → RADS: turbo x = 42;
    // Uses find_assignment() so comparisons (==) and kwargs are not mangled.
    // 'return x = ...' can't happen in Python, but 'return x == y' must not
    // be captured here — Rule 8 runs first (moved below its check).
    if (!starts_with(converted, "return")) {
        char* equals = find_assignment(converted);
        if (equals) {
            char var_name[256];
            char* start = converted;
            while (isspace((unsigned char)*start)) start++;
            char* end = equals - 1;
            while (end > start && isspace((unsigned char)*end)) end--;
            int len = (int)(end - start) + 1;
            if (len > 0 && len < (int)sizeof(var_name)) {
                strncpy(var_name, start, len);
                var_name[len] = '\0';

                char* value = equals + 1;
                while (isspace((unsigned char)*value)) value++;

                /* strip trailing newline carriage leftovers before formatting */
                trim(converted);

                // Always use turbo for variable declarations
                sprintf(output + strlen(output), "turbo %s = %s;", var_name, value);
                trim(output);
                return;
            }
        }
    }

    // Rule 3: if condition: → if (condition) {
    if (starts_with(converted, "if ")) {
        char* colon = strrchr(converted, ':');
        if (colon) {
            *colon = '\0';
            char condition[MAX_LINE_LENGTH];
            strcpy(condition, converted + 3);
            trim(condition);

            // Convert Python booleans to RADS
            replace_in_line(condition, "True", "true");
            replace_in_line(condition, "False", "false");
            replace_in_line(condition, "None", "null");
            replace_in_line(condition, " and ", " && ");
            replace_in_line(condition, " or ", " || ");
            replace_in_line(condition, " not ", " ! ");

            sprintf(output + strlen(output), "if (%s) {", condition);
            *indent_level = new_indent + 1;
            return;
        }
    }

    // Rule 4: elif condition: → } elif (condition) {
    if (starts_with(converted, "elif ")) {
        char* colon = strrchr(converted, ':');
        if (colon) {
            *colon = '\0';
            char condition[MAX_LINE_LENGTH];
            strcpy(condition, converted + 5);
            trim(condition);

            // Convert Python booleans to RADS
            replace_in_line(condition, "True", "true");
            replace_in_line(condition, "False", "false");

            // Reduce indent for closing brace — align with the opening 'if'
            output[0] = '\0';
            for (int i = 0; i < new_indent; i++) {
                strcat(output, "    ");
            }
            sprintf(output + strlen(output), "} elif (%s) {", condition);
            *indent_level = new_indent;
            return;
        }
    }

    // Rule 5: else: → } else {
    if (starts_with(converted, "else:") || strcmp(converted, "else:") == 0) {
        output[0] = '\0';
        for (int i = 0; i < new_indent; i++) {
            strcat(output, "    ");
        }
        strcat(output, "} else {");
        *indent_level = new_indent + 1;
        return;
    }

    // Rule 6: while condition: → loop (condition) {
    if (starts_with(converted, "while ")) {
        char* colon = strrchr(converted, ':');
        if (colon) {
            *colon = '\0';
            char condition[MAX_LINE_LENGTH];
            strcpy(condition, converted + 6);
            trim(condition);

            replace_in_line(condition, "True", "true");
            replace_in_line(condition, "False", "false");

            sprintf(output + strlen(output), "loop (%s) {", condition);
            *indent_level = new_indent + 1;
            return;
        }
    }

    // Rule 7: for item in items: → cruise (items) { turbo item
    if (starts_with(converted, "for ")) {
        char* in_pos = strstr(converted, " in ");
        if (in_pos) {
            char* colon = strrchr(converted, ':');
            if (colon) {
                *colon = '\0';
                *in_pos = '\0';

                char var[256];
                strcpy(var, converted + 4);
                trim(var);

                char collection[MAX_LINE_LENGTH];
                strcpy(collection, in_pos + 4);
                trim(collection);

                /* Python range(...) → RADS native `lo..hi` half-open range.
                 * (array.range never existed — that mapping emitted a call
                 * to an undefined function.)
                 * Supported: range(n) → 0..n, range(a, b) → a..b.
                 * range(a, b, step) has no RADS equivalent yet → left as-is
                 * so the interpreter's "Undefined function 'range'" names it. */
                if (starts_with(collection, "range(")) {
                    char* rp = collection + 6;
                    char* rend = strrchr(rp, ')');
                    if (rend) {
                        *rend = '\0';
                        /* count top-level commas to detect the 3-arg form */
                        int commas = 0, depth = 0, in_str = 0;
                        for (char* q = rp; *q; q++) {
                            if (in_str) { if (*q == '"') in_str = 0; }
                            else if (*q == '"') in_str = 1;
                            else if (*q == '(' || *q == '[') depth++;
                            else if (*q == ')' || *q == ']') depth--;
                            else if (*q == ',' && depth == 0) commas++;
                        }
                        char range_buf[MAX_LINE_LENGTH]; // scratch: rp aliases collection
                        if (commas == 0) {
                            snprintf(range_buf, sizeof(range_buf), "0..%s", rp);
                            strcpy(collection, range_buf);
                        } else if (commas == 1) {
                            char* c1 = strchr(rp, ',');
                            *c1 = '\0';
                            char* lo = rp;
                            char* hi = c1 + 1;
                            while (*lo == ' ') lo++;
                            while (*hi == ' ') hi++;
                            char* he = hi + strlen(hi) - 1;
                            while (he > hi && *he == ' ') *he-- = '\0';
                            snprintf(range_buf, sizeof(range_buf), "%s..%s", lo, hi);
                            strcpy(collection, range_buf);
                        }
                        /* 3-arg range: leave as `range(...)` text; the parse
                         * error will name it explicitly rather than emit a
                         * call to the nonexistent array.range */
                    }
                }

                // NOTE: no `turbo var;` here — cruise binds the loop variable
                // itself; redeclaring with turbo SHADOWS it with null.
                snprintf(output + strlen(output),
                         MAX_LINE_LENGTH - strlen(output),
                         "cruise %s in %s {", var, collection);
                *indent_level = new_indent + 1;
                return;
            }
        }
    }

    // Rule 8: return value → return value;
    if (starts_with(converted, "return")) {
        /* Python literal names differ in RADS — convert them so
         * 'return True' doesn't leak Python keywords into RADS code. */
        replace_in_line(converted, "True", "true");
        replace_in_line(converted, "False", "false");
        replace_in_line(converted, "None", "null");
        sprintf(output + strlen(output), "%s;", converted);
        trim(output);
        return;
    }

    // Rule 9: break / continue
    if (strcmp(converted, "break") == 0 || strcmp(converted, "continue") == 0) {
        sprintf(output + strlen(output), "%s;", converted);
        return;
    }

    // Rule 10: print(...) → echo(...)
    if (starts_with(converted, "print(")) {
        char* rest = converted + 5;  // Skip "print"
        replace_in_line(rest, "True", "true");
        replace_in_line(rest, "False", "false");
        replace_in_line(rest, "None", "null");

        // Rule 10b: multi-arg print folds into string concatenation.
        // Python `print(a, b, c)` space-separates; RADS echo takes ONE expr.
        // → echo(str(a) + " " + str(b) + " " + str(c));
        // Only fires when rest is exactly "(...)" — anything else (trailing
        // comment, inline statement) keeps the legacy single-echo path.
        size_t rlen = strlen(rest);
        if (rlen >= 2 && rest[0] == '(' && rest[rlen - 1] == ')' && strchr(rest, ',')) {
            char args_buf[MAX_LINE_LENGTH];
            strncpy(args_buf, rest + 1, rlen - 2);
            args_buf[rlen - 2] = '\0';

            // Split on top-level commas (paren/bracket/brace depth + quotes)
            char* argv[64];
            int ac = 0, depth = 0, in_str = 0;
            char quote = 0;
            char* start = args_buf;
            char* p;
            for (p = args_buf; ; p++) {
                if (*p == '\0') { argv[ac++] = start; break; }
                if (in_str) {
                    if (*p == quote) in_str = 0;
                } else if (*p == '"' || *p == '\'') {
                    in_str = 1; quote = *p;
                } else if (*p == '(' || *p == '[' || *p == '{') {
                    depth++;
                } else if (*p == ')' || *p == ']' || *p == '}') {
                    depth--;
                } else if (*p == ',' && depth == 0) {
                    *p = '\0';
                    argv[ac++] = start;
                    start = p + 1;
                    if (ac >= 64) { ac = -1; break; } // too many args: bail out
                }
            }

            if (ac > 1) {
                char joined[MAX_LINE_LENGTH * 2];
                size_t jlen = 0, jcap = sizeof(joined);
                int overflow = 0;
                jlen += (size_t)snprintf(joined, jcap, "echo(");
                for (int i = 0; i < ac && !overflow; i++) {
                    // trim the arg (leading spaces; trailing cut by ',' NUL)
                    while (*argv[i] == ' ' || *argv[i] == '\t') argv[i]++;
                    char* endp = argv[i] + strlen(argv[i]) - 1;
                    while (endp > argv[i] && (*endp == ' ' || *endp == '\t')) *endp-- = '\0';
                    if (i > 0)
                        jlen += (size_t)snprintf(joined + jlen, jcap - jlen, " + \" \" + ");
                    jlen += (size_t)snprintf(joined + jlen, jcap - jlen, "str(%s)", argv[i]);
                    if (jlen >= jcap - 1) overflow = 1; // truncated: bail to legacy
                }
                if (!overflow) {
                    snprintf(joined + jlen, jcap - jlen, ");");
                    // bounded concat: joined can be ~2x MAX_LINE_LENGTH, so
                    // plain sprintf into output could overflow it
                    size_t clen = strlen(output);
                    if (clen + strlen(joined) + 1 <= MAX_LINE_LENGTH) {
                        strcpy(output + clen, joined);
                        trim(output);
                        return;
                    }
                    // didn't fit: fall through to legacy echo below
                }
                // overflow or weird arg count: fall through to legacy echo
            }
        }

        sprintf(output + strlen(output), "echo%s;", rest);
        trim(output);
        return;
    }

    // Rule 11: Function calls and expressions
    if (strchr(converted, '(') && !strchr(converted, '=')) {
        sprintf(output + strlen(output), "%s;", converted);
        trim(output);
        return;
    }

    // Rule 12: import module → import "module.rads";
    if (starts_with(converted, "import ")) {
        char module[256];
        strcpy(module, converted + 7);
        trim(module);
        sprintf(output + strlen(output), "import \"%s.rads\";", module);
        return;
    }

    // Default: output as-is with semicolon (if not already there)
    sprintf(output + strlen(output), "%s", converted);
    trim(output);
    if (output[strlen(output) - 1] != ';' && output[strlen(output) - 1] != '{') {
        strcat(output, ";");
    }
}

// ============================================================================
// Main Transpilation Function
// ============================================================================

int transpile_python_to_rads(FILE* input, FILE* output) {
    char line[MAX_LINE_LENGTH];
    char converted[MAX_LINE_LENGTH];
    int indent_level = 0;
    int prev_indent = 0;
    int line_number = 0;

    // Header
    fprintf(output, "// Auto-generated by rads-mask v0.0.5 \"CHAMELEON\"\n");
    fprintf(output, "// Transpiled from Python to RADS\n\n");

    while (fgets(line, sizeof(line), input)) {
        line_number++;

        /* strip trailing newline/CR so rules never embed '\n' mid-statement
         * (this was the cause of stray lone ';' lines after echo/return) */
        size_t llen = strlen(line);
        while (llen > 0 && (line[llen - 1] == '\n' || line[llen - 1] == '\r')) {
            line[--llen] = '\0';
        }

        int current_indent = count_leading_spaces(line) / 4;

        /* 'elif'/'else' emit their own leading '}' (RADS: "} elif (...) {"),
         * so the generic dedent closer must not run for them — running both
         * produced a double '}' before every elif/else chain link. */
        const char* first_token = line + count_leading_spaces(line);
        int continues_chain = starts_with(first_token, "elif ") ||
                              strcmp(first_token, "else:") == 0;

        // Handle dedents (closing braces).
        // 'elif'/'else' lines print their own leading '}' which closes the
        // immediately-enclosing body, so the generic closer must emit only
        // (dedent_count - 1) braces for them — none when the chain continues
        // at the same nesting level, extra ones when dedenting out of nested
        // blocks (e.g. inner 'else:' followed by outer 'elif:').
        if (current_indent < prev_indent) {
            int dedent_count = prev_indent - current_indent;
            if (continues_chain) dedent_count--;
            for (int i = 0; i < dedent_count; i++) {
                for (int j = 0; j < prev_indent - i - 1; j++) {
                    fprintf(output, "    ");
                }
                fprintf(output, "}\n");
            }
        }

        // Transpile the line
        transpile_line(line, converted, &indent_level);

        // Output non-empty lines
        if (converted[0] != '\0') {
            fprintf(output, "%s\n", converted);
        }

        prev_indent = current_indent;
    }

    // Close any remaining open blocks
    while (prev_indent > 0) {
        prev_indent--;
        for (int i = 0; i < prev_indent; i++) {
            fprintf(output, "    ");
        }
        fprintf(output, "}\n");
    }

    return 0;
}

// ============================================================================
// CLI Entry Point
// ============================================================================

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("RADS Masquerading Tool v0.0.5 \"CHAMELEON\"\n");
        printf("Usage: %s <input.py> [-o <output.rads>]\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s hello.py                 # Output to stdout\n", argv[0]);
        printf("  %s hello.py -o hello.rads  # Output to file\n", argv[0]);
        return 1;
    }

    const char* input_file = argv[1];
    const char* output_file = NULL;

    // Parse arguments
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[i + 1];
            i++;
        }
    }

    // Open input file
    FILE* input = fopen(input_file, "r");
    if (!input) {
        fprintf(stderr, "Error: Cannot open input file '%s'\n", input_file);
        return 1;
    }

    // Open output file or use stdout
    FILE* output = stdout;
    if (output_file) {
        output = fopen(output_file, "w");
        if (!output) {
            fprintf(stderr, "Error: Cannot create output file '%s'\n", output_file);
            fclose(input);
            return 1;
        }
    }

    // Perform transpilation
    int result = transpile_python_to_rads(input, output);

    // Cleanup
    fclose(input);
    if (output != stdout) {
        fclose(output);
        printf("✓ Successfully transpiled '%s' → '%s'\n", input_file, output_file);
    }

    return result;
}
