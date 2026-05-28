#include "c_grammar.h"
#include "c_grammar_ast.h"
#include "c_grammar_ast_actions.h"
#include "callbacks.h"
#include "complexity.h"
#include "debug.h"
#include "source_location.h"
#include "symbol_table.h"

#include <easy_pc/easy_pc.h>
#include <errno.h>
#include <getopt.h>
#include <json-c/json.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* --- Command-line option globals --- */
static char *output_filename = NULL;

/* Preprocessing options */
static bool preprocess_flag = true;
static char *include_paths[64];
static int include_paths_count = 0;
static char *defines[64];
static int defines_count = 0;

/* Output options */
static bool json_output = false;

/* Function name filter */
static char **filter_names = NULL;
static int filter_names_count = 0;
static int filter_names_capacity = 0;

/* --- Typedef scope management --- */

typedef struct typedef_scope typedef_scope_t;
struct typedef_scope {
  symbol_table_t *names;
  typedef_scope_t *parent;
};

typedef struct {
  typedef_scope_t *typedef_scopes;
  symbol_table_t *builtins;
  symbol_table_t *pending_names_st;

  int *marker_stack;
  int marker_top;
  int marker_capacity;
} parse_session_ctx_t;

static void typedef_scope_push(parse_session_ctx_t *session) {
  typedef_scope_t *scope = calloc(1, sizeof(*scope));
  if (scope == NULL) {
    return;
  }

  scope->names = symbol_table_create();
  if (scope->names == NULL) {
    free(scope);
    return;
  }

  scope->parent = session->typedef_scopes;
  session->typedef_scopes = scope;
}

static void typedef_scope_pop(parse_session_ctx_t *session) {
  typedef_scope_t *scope = session->typedef_scopes;
  if (scope == NULL) {
    return;
  }

  session->typedef_scopes = scope->parent;
  symbol_table_free(scope->names);
  free(scope);
}

static void typedef_scope_add(parse_session_ctx_t *session, char const *name) {
  if (session->typedef_scopes != NULL) {
    symbol_table_add(session->typedef_scopes->names, name);
  }
}

static bool typedef_scope_contains(parse_session_ctx_t *session,
                                   char const *name) {
  for (typedef_scope_t *scope = session->typedef_scopes; scope != NULL;
       scope = scope->parent) {
    if (symbol_table_contains(scope->names, name)) {
      return true;
    }
  }
  return false;
}

static void typedef_scope_free_all(parse_session_ctx_t *session) {
  while (session->typedef_scopes != NULL) {
    typedef_scope_pop(session);
  }
}

static parse_session_ctx_t *session_ctx_create(void) {
  parse_session_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    return NULL;
  }

  typedef_scope_push(ctx);
  if (ctx->typedef_scopes == NULL) {
    free(ctx);
    return NULL;
  }

  ctx->builtins = symbol_table_create();
  if (ctx->builtins == NULL) {
    typedef_scope_free_all(ctx);
    free(ctx);
    return NULL;
  }

  symbol_table_add(ctx->builtins, "__builtin_va_list");
  symbol_table_add(ctx->builtins, "__builtin_va_arg");
  symbol_table_add(ctx->builtins, "va_list");
  symbol_table_add(ctx->builtins, "va_arg");

  ctx->pending_names_st = symbol_table_create();
  if (ctx->pending_names_st == NULL) {
    symbol_table_free(ctx->builtins);
    typedef_scope_free_all(ctx);
    free(ctx);
    return NULL;
  }

  ctx->marker_capacity = 16;
  ctx->marker_stack = malloc(sizeof(*ctx->marker_stack) * ctx->marker_capacity);
  if (ctx->marker_stack == NULL) {
    symbol_table_free(ctx->pending_names_st);
    symbol_table_free(ctx->builtins);
    typedef_scope_free_all(ctx);
    free(ctx);
    return NULL;
  }
  return ctx;
}

static void session_ctx_free(parse_session_ctx_t *ctx) {
  if (ctx == NULL) {
    return;
  }

  typedef_scope_free_all(ctx);
  symbol_table_free(ctx->builtins);
  symbol_table_free(ctx->pending_names_st);
  free(ctx->marker_stack);
  free(ctx);
}

/* --- GDL Callbacks and Predicates --- */

bool is_typedef_name(epc_cpt_node_t *token, epc_parser_ctx_t *parse_ctx,
                     void *parser_data) {
  (void)parser_data;
  parse_session_ctx_t *session =
      (parse_session_ctx_t *)parse_ctx_get_user_ctx(parse_ctx);
  if (session == NULL) {
    return false;
  }

  char const *name = epc_cpt_node_get_semantic_content(token);
  size_t len = epc_cpt_node_get_semantic_len(token);

  char *name_copy = strndup(name, len);
  if (name_copy == NULL) {
    return false;
  }

  bool found = typedef_scope_contains(session, name_copy) ||
               symbol_table_contains(session->builtins, name_copy);

  free(name_copy);
  return found;
}

static void on_capture_entry(epc_parser_t *parser, epc_parser_ctx_t *parse_ctx,
                             void *parser_data) {
  (void)parser;
  (void)parse_ctx;
  (void)parser_data;
}

static bool on_capture_exit(epc_parse_result_t result,
                            epc_parser_ctx_t *parse_ctx, void *parser_data) {
  (void)parser_data;
  if (result.is_error) {
    return true;
  }

  parse_session_ctx_t *session =
      (parse_session_ctx_t *)parse_ctx_get_user_ctx(parse_ctx);
  if (session == NULL) {
    return true;
  }

  char const *name = epc_cpt_node_get_semantic_content(result.data.success);

  if (name == NULL) {
    return true;
  }

  size_t len = epc_cpt_node_get_semantic_len(result.data.success);
  char *name_copy = strndup(name, len);

  if (name_copy == NULL) {
    return true;
  }

  symbol_table_add(session->pending_names_st, name_copy);

  free(name_copy);

  return true;
}

static void on_commit_entry(epc_parser_t *parser, epc_parser_ctx_t *parse_ctx,
                            void *parser_data) {
  (void)parser;
  (void)parser_data;
  parse_session_ctx_t *session =
      (parse_session_ctx_t *)parse_ctx_get_user_ctx(parse_ctx);
  if (session == NULL) {
    return;
  }

  if (session->marker_top >= session->marker_capacity) {
    session->marker_capacity *= 2;
    int *new_marker_stack =
        realloc(session->marker_stack,
                sizeof(*session->marker_stack) * session->marker_capacity);
    if (new_marker_stack == NULL) {
      debug_error("Error: Failed to resize marker stack.");
      return;
    }
    session->marker_stack = new_marker_stack;
  }
  session->marker_stack[session->marker_top++] =
      symbol_table_count(session->pending_names_st);
}

static bool on_commit_exit(epc_parse_result_t result,
                           epc_parser_ctx_t *parse_ctx, void *parser_data) {
  (void)parser_data;
  parse_session_ctx_t *session =
      (parse_session_ctx_t *)parse_ctx_get_user_ctx(parse_ctx);
  if (session == NULL || session->marker_top == 0) {
    return true;
  }

  int marker = session->marker_stack[--session->marker_top];

  if (!result.is_error) {
    for (size_t i = marker; i < symbol_table_count(session->pending_names_st);
         i++) {
      typedef_scope_add(session,
                        symbol_table_name_at(session->pending_names_st, i));
    }
  }
  symbol_table_clear_from(session->pending_names_st, marker);

  return true;
}

epc_wrap_callbacks_t typedef_capture_callbacks = {on_capture_entry,
                                                  on_capture_exit};
epc_wrap_callbacks_t typedef_commit_callbacks = {on_commit_entry,
                                                 on_commit_exit};

static void on_scope_entry(epc_parser_t *parser, epc_parser_ctx_t *parse_ctx,
                           void *parser_data) {
  (void)parser;
  (void)parser_data;
  parse_session_ctx_t *session =
      (parse_session_ctx_t *)parse_ctx_get_user_ctx(parse_ctx);
  if (session != NULL) {
    typedef_scope_push(session);
  }
}

static bool on_scope_exit(epc_parse_result_t result,
                          epc_parser_ctx_t *parse_ctx, void *parser_data) {
  (void)parser_data;
  (void)result;
  parse_session_ctx_t *session =
      (parse_session_ctx_t *)parse_ctx_get_user_ctx(parse_ctx);
  if (session != NULL) {
    typedef_scope_pop(session);
  }

  return true;
}

epc_wrap_callbacks_t typedef_scope_callbacks = {on_scope_entry, on_scope_exit};

/* --- Preprocessing --- */

static int preprocess_file(char const *input_path, char const *output_path) {
  int num_args = 4;
  if (output_path) {
    num_args += 2;
  }
  num_args += (include_paths_count * 2) + (defines_count * 2);
  num_args += 1;

  char **argv = calloc(num_args, sizeof(*argv));
  if (!argv) {
    debug_error("Error: Failed to allocate memory for preprocessing command.");
    return -1;
  }
  int arg_idx = 0;

  argv[arg_idx++] = strdup("clang");
  argv[arg_idx++] = strdup("-E");

  for (int i = 0; i < include_paths_count; i++) {
    char *path_arg;
    if (asprintf(&path_arg, "-I%s", include_paths[i]) == -1) {
      for (int j = 0; j < arg_idx; j++)
        free(argv[j]);
      free(argv);
      return -1;
    }
    argv[arg_idx++] = path_arg;
  }

  for (int i = 0; i < defines_count; i++) {
    char *define_arg;
    if (asprintf(&define_arg, "-D%s", defines[i]) == -1) {
      for (int j = 0; j < arg_idx; j++)
        free(argv[j]);
      free(argv);
      return -1;
    }
    argv[arg_idx++] = define_arg;
  }

  argv[arg_idx++] = strdup(input_path);

  if (output_path) {
    argv[arg_idx++] = strdup("-o");
    argv[arg_idx++] = strdup(output_path);
  }

  argv[arg_idx] = NULL;

  pid_t pid;
  int status = posix_spawn(&pid, "/usr/bin/clang", NULL, NULL, argv, environ);
  if (status != 0) {
    debug_error("posix_spawn failed: %s", strerror(status));
    for (int i = 0; i < arg_idx; i++) {
      free(argv[i]);
    }
    free(argv);
    return -1;
  }

  int wait_status;
  if (waitpid(pid, &wait_status, 0) == -1) {
    debug_error("waitpid failed: %s", strerror(errno));
    for (int i = 0; i < arg_idx; i++)
      free(argv[i]);
    free(argv);
    return -1;
  }

  for (int i = 0; i < arg_idx; i++) {
    free(argv[i]);
  }
  free(argv);

  if (WIFEXITED(wait_status)) {
    int exit_code = WEXITSTATUS(wait_status);
    if (exit_code != 0) {
      debug_error("Preprocessor failed with exit code %d", exit_code);
      return -1;
    }
    return 0;
  } else if (WIFSIGNALED(wait_status)) {
    debug_error("Preprocessor killed by signal %d", WTERMSIG(wait_status));
    return -1;
  }

  return -1;
}

/* --- Source location tracking for preprocessor line markers --- */

static void
process_preprocessor_line_marker(c_grammar_node_t const *node,
                                 source_location_tracker_t *loc_tracker) {
  ast_node_preprocessor_line_marker_t const *marker = &node->line_marker;

  source_location_tracker_add_entry(loc_tracker, node->source_data.view,
                                    marker->line_number, marker->filename);

  for (size_t i = 0; i < marker->flags_count; i++) {
    if (marker->flags[i] == 1) {
      source_location_tracker_push_include(loc_tracker, marker->filename,
                                           marker->line_number);
    } else if (marker->flags[i] == 2) {
      source_location_tracker_pop_include(loc_tracker);
    }
  }
}

/* --- Function name extraction (reused from declarator walk) --- */

static char const *
get_function_name_from_declarator(c_grammar_node_t const *declarator_node) {
  if (declarator_node == NULL || declarator_node->type != AST_NODE_DECLARATOR) {
    return NULL;
  }

  c_grammar_node_t const *direct_decl =
      declarator_node->declarator.direct_declarator;
  if (direct_decl == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < direct_decl->list.count; i++) {
    if (direct_decl->list.children[i]->type == AST_NODE_IDENTIFIER) {
      return direct_decl->list.children[i]->text;
    }
  }
  return NULL;
}

/* --- Complexity collection --- */

typedef struct {
  char const *function_name;
  unsigned int complexity;
  size_t line_number;
  char const *filename;
} func_complexity_t;

typedef struct {
  func_complexity_t *entries;
  size_t count;
  size_t capacity;
} func_complexity_list_t;

static void func_complexity_list_add(func_complexity_list_t *list,
                                     char const *name, unsigned int complexity,
                                     size_t line, char const *file) {
  if (list->count >= list->capacity) {
    size_t new_cap = list->capacity == 0 ? 16 : list->capacity * 2;
    func_complexity_t *new_entries =
        realloc(list->entries, new_cap * sizeof(*new_entries));
    if (new_entries == NULL) {
      return;
    }
    list->entries = new_entries;
    list->capacity = new_cap;
  }

  func_complexity_t *entry = &list->entries[list->count++];
  entry->function_name = name;
  entry->complexity = complexity;
  entry->line_number = line;
  entry->filename = file;
}

static int compare_complexity_desc(void const *a, void const *b) {
  func_complexity_t const *fa = a;
  func_complexity_t const *fb = b;
  if (fa->complexity > fb->complexity)
    return -1;
  if (fa->complexity < fb->complexity)
    return 1;
  return 0;
}

/*
 * Recursively walk the AST looking for function definitions and collecting
 * their cyclomatic complexity. Maintains source location tracking via
 * preprocessor line markers.
 */
static void collect_function_complexities_recursive(
    c_grammar_node_t const *node, func_complexity_list_t *results,
    char const *input_filename, source_location_tracker_t *loc_tracker) {
  if (node == NULL) {
    return;
  }

  switch (node->type) {
  case AST_NODE_FUNCTION_DEFINITION: {
    char const *fn = NULL;
    c_grammar_node_t const *declarator_node =
        node->function_definition.declarator;
    if (declarator_node != NULL) {
      fn = get_function_name_from_declarator(declarator_node);
    }
    if (fn == NULL) {
      fn = "<anonymous>";
    }

    c_grammar_node_t const *body = node->function_definition.body;

    unsigned int decision_points = count_decision_points(body);
    unsigned int complexity = 1 + decision_points;

    /* Calculate original source line for the function */
    size_t report_line = node->source_data.view.line_number;
    char const *report_file = input_filename;

    if (loc_tracker != NULL && loc_tracker->count > 1) {
      source_location_entry_t const *loc_entry = source_location_tracker_find(
          loc_tracker, node->source_data.view.offset);
      if (loc_entry != NULL) {
        report_file = loc_entry->original_filename;
        epc_parser_input_view_t pp_entry_view = loc_entry->preprocessed_view;
        report_line = loc_entry->original_line +
                      node->source_data.view.line_number -
                      pp_entry_view.line_number - 1;
      }
    }

    func_complexity_list_add(results, fn, complexity, report_line, report_file);

    /*
     * Don't recurse into function body children here -- the body is already
     * processed by count_decision_points().
     */
    return;
  }

  case AST_NODE_PREPROCESSOR_LINE_MARKER: {
    if (loc_tracker != NULL) {
      process_preprocessor_line_marker(node, loc_tracker);
    }
    break;
  }

  default:
    break;
  }

  /* Recurse into all children */
  for (size_t i = 0; i < node->list.count; i++) {
    collect_function_complexities_recursive(node->list.children[i], results,
                                            input_filename, loc_tracker);
  }
}

/* --- Usage --- */

static void filter_names_add(char const *name) {
  if (filter_names_capacity == 0) {
    filter_names_capacity = 8;
    filter_names = malloc(filter_names_capacity * sizeof(*filter_names));
  } else if (filter_names_count >= filter_names_capacity) {
    filter_names_capacity *= 2;
    filter_names = realloc(filter_names,
                           filter_names_capacity * sizeof(*filter_names));
  }
  if (filter_names != NULL) {
    filter_names[filter_names_count++] = strdup(name);
  }
}

static void filter_names_cleanup(void) {
  if (filter_names != NULL) {
    for (int i = 0; i < filter_names_count; i++) {
      free(filter_names[i]);
    }
    free(filter_names);
    filter_names = NULL;
  }
  filter_names_count = 0;
  filter_names_capacity = 0;
}

static bool is_function_wanted(char const *name) {
  if (filter_names_count == 0) {
    return true;
  }
  for (int i = 0; i < filter_names_count; i++) {
    if (strcmp(filter_names[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static void print_usage(char const *prog_name) {
  fprintf(stderr, "Usage: %s [options] <filename>\n", prog_name);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --no-preprocess Skip preprocessing\n");
  fprintf(
      stderr,
      "  -o <file>       Specify output filename (for preprocessed output)\n");
  fprintf(stderr,
          "  -I <dir>        Add include directory for preprocessing\n");
  fprintf(stderr, "  -D <macro>      Define macro for preprocessing\n");
  fprintf(stderr, "  -j              Output results in JSON format\n");
  fprintf(stderr,
          "  -f <name>       Only include function with this name (may be "
          "repeated)\n");
  fprintf(stderr, "  -h, --help      Display this help message\n");
}

/* --- Main --- */

int main(int argc, char *argv[]) {
  static struct option long_options[] = {{"help", no_argument, 0, 'h'},
                                         {"no-preprocess", no_argument, 0, 256},
                                         {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "o:hI:D:f:j", long_options,
                            &option_index)) != -1) {
    switch (opt) {
    case 'o':
      output_filename = optarg;
      break;
    case 'I':
      if (include_paths_count < 64) {
        include_paths[include_paths_count++] = optarg;
      }
      break;
    case 'D':
      if (defines_count < 64)
        defines[defines_count++] = optarg;
      break;
    case 'j':
      json_output = true;
      break;
    case 'f':
      filter_names_add(optarg);
      break;
    case 256:
      preprocess_flag = false;
      break;
    case 'h':
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    default:
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "Error: Missing input filename.\n");
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  char const *filename = argv[optind];

  bool should_preprocess = preprocess_flag;
  char const *actual_input_file = filename;
  char *preprocessed_temp_file = NULL;

  if (should_preprocess) {
    preprocessed_temp_file = strdup("/tmp/ncc_preproc_XXXXXX");
    int fd = mkstemp(preprocessed_temp_file);
    if (fd == -1) {
      debug_error("Error: Failed to create temp file for preprocessing.");
      free(preprocessed_temp_file);
      preprocessed_temp_file = NULL;
    } else {
      close(fd);
    }

    if (preprocessed_temp_file != NULL) {
      int prep_result =
          preprocess_file(filename, preprocessed_temp_file);
      if (prep_result != 0) {
        debug_error("Error: Preprocessing failed for %s", filename);
        free(preprocessed_temp_file);
        preprocessed_temp_file = NULL;
      } else {
        actual_input_file = preprocessed_temp_file;
      }
    }
  }

  epc_parser_list *list = epc_parser_list_create();
  if (list == NULL) {
    debug_error("Failed to create parser list.");
    if (preprocessed_temp_file) {
      free(preprocessed_temp_file);
    }
    return EXIT_FAILURE;
  }

  parse_session_ctx_t *session_ctx = session_ctx_create();
  if (session_ctx == NULL) {
    debug_error("Failed to create session context.");
    epc_parser_list_free(list);
    return EXIT_FAILURE;
  }

  epc_parser_t *c_parser = create_c_grammar_parser(list);
  if (c_parser == NULL) {
    debug_error("Failed to create C parser.");
    session_ctx_free(session_ctx);
    epc_parser_list_free(list);
    return EXIT_FAILURE;
  }

  epc_parse_session_t session =
      epc_parse_file(c_parser, actual_input_file, session_ctx);

  if (session.result.is_error) {
    epc_parser_error_t *err = session.result.data.error;
    debug_error("Parse Error: %s", err->message);
    debug_error("At line %zu, col %zu", err->view.line_number,
                err->view.column_number);
    debug_error("Expected: %s", err->expected);
    debug_error("Found: %s", err->found);

    epc_parse_session_destroy(&session);
    session_ctx_free(session_ctx);
    epc_parser_list_free(list);
    return EXIT_FAILURE;
  }

  /* Build the AST */
  int exit_code = EXIT_SUCCESS;
  epc_ast_hook_registry_t *registry =
      epc_ast_hook_registry_create(C_GRAMMAR_AST_ACTION_COUNT__);

  if (registry != NULL) {
    c_grammar_ast_hook_registry_init(registry);
    epc_ast_result_t ast_result =
        epc_ast_build(session.result.data.success, registry, NULL);

    if (!ast_result.has_error) {
      c_grammar_node_t *ast_root = ast_result.ast_root;

      source_location_tracker_t loc_tracker;
      source_location_tracker_init(&loc_tracker);
      source_location_tracker_push_include(&loc_tracker, filename, 1);

      epc_parser_input_view_t initial_view = {
          .line_number = 1,
          .column_number = 1,
      };
      source_location_tracker_add_entry(&loc_tracker, initial_view, 1,
                                        filename);

      /* Collect function complexities */
      func_complexity_list_t results = {NULL, 0, 0};
      collect_function_complexities_recursive(ast_root, &results, filename,
                                              &loc_tracker);

      /* Sort by complexity descending */
      qsort(results.entries, results.count, sizeof(*results.entries),
            compare_complexity_desc);

      /* Output results */
      if (json_output) {
        struct json_object *arr = json_object_new_array();
        for (size_t i = 0; i < results.count; i++) {
          func_complexity_t *f = &results.entries[i];
          if (!is_function_wanted(f->function_name)) {
            continue;
          }
          struct json_object *obj = json_object_new_object();
          json_object_object_add(obj, "function_name",
                                 json_object_new_string(f->function_name));
          json_object_object_add(obj, "complexity",
                                 json_object_new_int((int)f->complexity));
          json_object_object_add(obj, "line_number",
                                 json_object_new_int((int)f->line_number));
          json_object_object_add(obj, "source_file",
                                 json_object_new_string(f->filename));
          json_object_array_add(arr, obj);
        }
        fprintf(stdout, "%s\n",
                json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PLAIN));
        json_object_put(arr);
      } else {
        for (size_t i = 0; i < results.count; i++) {
          func_complexity_t *f = &results.entries[i];
          if (!is_function_wanted(f->function_name)) {
            continue;
          }
          fprintf(stdout,
                  "%s:%zu: function '%s' has cyclomatic complexity %u\n",
                  f->filename, f->line_number, f->function_name,
                  f->complexity);
        }
      }

      free(results.entries);
      source_location_tracker_free(&loc_tracker);
      c_grammar_node_free(ast_root, NULL);
    } else {
      debug_error("AST Build Error: %s", ast_result.error_message);
      exit_code = EXIT_FAILURE;
    }
    epc_ast_hook_registry_free(registry);
  } else {
    debug_error("Failed to create AST registry.");
    exit_code = EXIT_FAILURE;
  }

  epc_parse_session_destroy(&session);
  session_ctx_free(session_ctx);
  epc_parser_list_free(list);

  if (preprocessed_temp_file) {
    remove(preprocessed_temp_file);
    free(preprocessed_temp_file);
  }

  filter_names_cleanup();

  return exit_code;
}
