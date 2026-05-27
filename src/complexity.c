#include "complexity.h"

static unsigned int
count_decision_points_in_children(c_grammar_node_t const *node) {
  if (node == NULL) {
    return 0;
  }

  unsigned int count = 0;
  for (size_t i = 0; i < node->list.count; i++) {
    count += count_decision_points(node->list.children[i]);
  }
  return count;
}

unsigned int count_decision_points(c_grammar_node_t const *node) {
  if (node == NULL) {
    return 0;
  }

  unsigned int count = 0;

  switch (node->type) {
  case AST_NODE_IF_STATEMENT:
  case AST_NODE_WHILE_STATEMENT:
  case AST_NODE_DO_WHILE_STATEMENT:
  case AST_NODE_FOR_STATEMENT:
  case AST_NODE_CASE_LABEL:
  case AST_NODE_DEFAULT_STATEMENT:
  case AST_NODE_CONDITIONAL_EXPRESSION:
  case AST_NODE_LOGICAL_EXPRESSION:
    count += 1;
    break;

  default:
    break;
  }

  count += count_decision_points_in_children(node);

  return count;
}
