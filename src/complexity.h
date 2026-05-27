#pragma once

#include "c_grammar_ast.h"

/*
 * Recursively count decision points in the AST subtree rooted at 'node'.
 * Decision points include: if, while, do-while, for, case, default,
 * ternary (?:), and logical &&/||.
 *
 * This returns the number of decision points (not the full complexity).
 * The caller should compute: cyclomatic_complexity = 1 + decision_points.
 */
unsigned int count_decision_points(c_grammar_node_t const *node);
