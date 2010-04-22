/*
  repl.c
  system startup, main(), and console interaction
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <libgen.h>
#include <getopt.h>
#ifndef NO_BOEHM_GC
#include <gc.h>
#endif
#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif
#include "llt.h"
#include "julia.h"

static char jl_banner_plain[] =
    "               _      \n"
    "   _       _ _(_)_     |\n"
    "  (_)     | (_) (_)    |  pre-release version\n"
    "   _ _   _| |_  __ _   |\n"
    "  | | | | | | |/ _` |  |\n"
    "  | | |_| | | | (_| |  |  \302\2512009-2010 contributors\n"
    " _/ |\\__'_|_|_|\\__'_|  |  \n"
    "|__/                   |\n\n";

static char jl_banner_color[] =
    "\033[1m               \033[32m_\033[37m      \n"
    "   \033[37m_\033[37m       _ \033[31m_\033[32m(_)\033[35m_\033[37m     |\n"
    "  \033[37m(_)\033[37m     | \033[31m(_) \033[35m(_)\033[37m    |  pre-release version\n"
    "   _ _   _| |_  __ _   |\n"
    "  | | | | | | |/ _` |  |\n"
    "  | | |_| | | | (_| |  |  \302\2512009-2010 contributors\n"
    " _/ |\\__'_|_|_|\\__'_|  |  \n"
    "|__/                   |\033[0m\n\n";

static char jl_prompt_plain[] = "julia> ";
static char jl_prompt_color[] = "\001\033[1m\033[32m\002julia> \001\033[37m\002";
static char jl_answer_color[] = "\033[0m\033[37m";
static char jl_input_color[]  = "\033[1m\033[37m";
static char jl_color_normal[] = "\033[0m\033[37m";

static char jl_history_file[] = ".julia_history";

char *julia_home = NULL; // load is relative to here
static int print_banner = 1;
static int load_start_j = 1;
static int tab_width = 2;

static const char *usage = "julia [options]\n";
static const char *opts =
    " -H --home=<dir>   Load files relative to <dir>\n"
    " -T --tab=<size>   Set tab width to <size>\n"
    " -b --bare         Bare REPL: don't load 'start.j'\n"
    " -q --quiet        Quiet startup without banner\n"
    " -h --help         Print this message\n";

void parse_opts(int *argcp, char ***argvp) {
    static struct option longopts[] = {
        { "home",  required_argument, 0, 'H' },
        { "tab",   required_argument, 0, 'T' },
        { "bare",  no_argument,       0, 'b' },
        { "quiet", no_argument,       0, 'q' },
        { "help",  no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    while ((c = getopt_long(*argcp,*argvp,"H:T:bqh",longopts,0)) != -1) {
        switch (c) {
        case 'H':
            julia_home = strdup(optarg);
            break;
        case 'b':
            load_start_j = 0;
            break;
        case 'q':
            print_banner = 0;
            break;
        case 'h':
            printf("%s%s", usage, opts);
            exit(0);
        case '?':
            ios_printf(ios_stderr, "options:\n%s", opts);
            exit(1);
        default:
            ios_printf(ios_stderr, "ERROR: getopt badness.\n");
            exit(1);
        }
    }
    if (!julia_home) {
        julia_home = getenv("JULIA_HOME");
        if (julia_home) {
            julia_home = strdup(julia_home);
        } else {
            char *julia_path = (char*)malloc(PATH_MAX);
            get_exename(julia_path, PATH_MAX);
            julia_home = strdup(dirname(julia_path));
            free(julia_path);
        }
    }
    char *pwd = getenv("PWD");
    if (julia_home && pwd) {
        int i, prefix = 1;
        for (i=0; pwd[i]; i++) {
            if (pwd[i] != julia_home[i]) {
                prefix = 0;
                break;
            }
        }
        if (prefix && (julia_home[i] == '/' || julia_home[i] == '\0')) {
            while (julia_home[i] == '/') i++;
            if (julia_home[i]) {
                char *p = strdup(julia_home + i);
                free(julia_home);
                julia_home = p;
            } else {
                julia_home = NULL;
            }
        }
    }
    *argvp += optind;
    *argcp -= optind;
    if (*argcp > 0) {
        ios_printf(ios_stderr, "julia: no arguments allowed\n", usage);
        ios_printf(ios_stderr, "usage: %s", usage);
        exit(1);
    }
}

void julia_init()
{
    jl_init_frontend();
    jl_init_types();
    jl_init_modules();
    jl_init_builtins();
    jl_init_codegen();
    jl_init_intrinsic_functions();
}

static int detect_color()
{
#ifdef WIN32
    return 0;
#else
    int tput = system("tput setaf 0 >&/dev/null");
    if (tput == 0) return 1;
    if (tput == 1) return 0;
    char *term = getenv("TERM");
    if (term == NULL) return 0;
    return (!strcmp(term,"xterm") || !strcmp(term,"xterm-color"));
#endif
}

JL_CALLABLE(jl_f_new_closure);

jl_value_t *jl_toplevel_eval(jl_value_t *ast)
{
    //jl_print(ast);
    //ios_printf(ios_stdout, "\n");
    // ast is of the form (quote <lambda-info>)
    jl_value_t *args[2];
    assert(jl_is_expr(ast));
    jl_lambda_info_t *li = (jl_lambda_info_t*)jl_exprarg(ast,0);
    assert(jl_typeof(li) == (jl_type_t*)jl_lambda_info_type);
    args[0] = (jl_value_t*)li;
    args[1] = (jl_value_t*)jl_null;
    jl_value_t *thunk = jl_f_new_closure(NULL, args, 2);
    return jl_apply((jl_function_t*)thunk, NULL, 0);
}

static int have_color;
static int prompt_length;

jmp_buf ExceptionHandler;
jmp_buf *CurrentExceptionHandler = &ExceptionHandler;

static int ends_with_semicolon(const char *input)
{
    char *p = strrchr(input, ';');
    if (p++) {
        while (isspace(*p)) p++;
        if (*p == '\0' || *p == '#')
            return 1;
    }
    return 0;
}

static jl_value_t *ast;

#ifdef USE_READLINE

static int history_offset = -1;

// yes, readline uses inconsistent indexing internally.
#define history_rem(n) remove_history(n-history_base)

static void init_history() {
    using_history();
    struct stat stat_info;
    if (!stat(jl_history_file, &stat_info)) {
        read_history(jl_history_file);
        for (;;) {
            HIST_ENTRY *entry = history_get(history_base);
            if (entry && isspace(entry->line[0]))
                free_history_entry(history_rem(history_base));
            else break;
        }
        int i, j, k;
        for (i=1 ;; i++) {
            HIST_ENTRY *first = history_get(i);
            if (!first) break;
            int length = strlen(first->line)+1;
            for (j = i+1 ;; j++) {
                HIST_ENTRY *child = history_get(j);
                if (!child || !isspace(child->line[0])) break;
                length += strlen(child->line)+1;
            }
            if (j == i+1) continue;
            first->line = (char*)realloc(first->line, length);
            char *p = strchr(first->line, '\0');
            for (k = i+1; k < j; k++) {
                *p = '\n';
                p = stpcpy(p+1, history_get(i+1)->line);
                free_history_entry(history_rem(i+1));
            }
        }
    } else if (errno == ENOENT) {
        write_history(jl_history_file);
    } else {
        jl_errorf("history file error: %s", strerror(errno));
    }
}

static int line_start(int point) {
    if (!point) return 0;
    int i = point-1;
    for (; i && rl_line_buffer[i] != '\n'; i--) ;
    return i ? i+1 : 0;
}

static int line_end(int point) {
    char *nl = strchr(rl_line_buffer + point, '\n');
    if (!nl) return rl_end;
    return nl - rl_line_buffer;
}

static int spaces_after(int point) {
    return strspn(rl_line_buffer + point, " \t\v");
}

static int first_unclosed(int a, int b) {
    int i, first = -1, unclosed = 0;
    for (i=0; i < (b-a); i++) {
        // TODO: handle quotes.
        char c = rl_line_buffer[a+i];
        if (c=='(' || c=='[' || c=='{') {
            if (!unclosed) first = i;
            unclosed++;
        } else if (c==')' || c==']' || c=='}') {
            unclosed--;
            if (!unclosed) first = -1;
        }
    }
    return first;
}

static int newline_callback(int count, int key) {
    int i = line_start(rl_point);
    int j = line_end(rl_point);
    int u = first_unclosed(i,j) + 1;
    int s = u ? u : i ? spaces_after(i) : tab_width;
    if (!i) s += prompt_length;
    rl_insert_text("\n");
    while (s--) rl_insert_text(" ");
    return 0;
}

static int return_callback(int count, int key) {
    if (have_color) {
        ios_printf(ios_stdout, jl_answer_color);
        ios_flush(ios_stdout);
    }
    ast = jl_parse_input_line(rl_line_buffer);
    rl_done = !ast || !jl_is_expr(ast) ||
        (((jl_expr_t*)ast)->head != continue_sym);
    if (!rl_done && have_color) {
        ios_printf(ios_stdout, jl_input_color);
        ios_flush(ios_stdout);
    }
    if (!rl_done) {
        newline_callback(count, key);
    } else {
        rl_point = rl_end;
        rl_redisplay();
    }
    return 0;
}

static int space_callback(int count, int key) {
    if (rl_point > 0)
        rl_insert_text(" ");
    return 0;
}

static void insert_tab() {
    int i;
    for (i=0; i < tab_width; i++)
        rl_insert_text(" ");
}

static int tab_callback(int count, int key) {
    if (rl_point > 0) {
        int i = line_start(rl_point);
        int is = spaces_after(i);
        if (i && rl_point <= i + is) {
            int j = line_start(i-1);
            int js = spaces_after(j);
            int s = js - is;
            if (!j) s -= prompt_length;
            if (s > 0) {
                while (s--) rl_insert_text(" ");
                rl_point = i + spaces_after(i);
                return 0;
            }
        }
        insert_tab();
    }
    return 0;
}

static int line_start_callback(int count, int key) {
    int start = line_start(rl_point);
    int flush_left = rl_point == 0 || rl_point == start + prompt_length;
    rl_point = flush_left ? 0 : (!start ? start : start + prompt_length);
    return 0;
}

static int line_end_callback(int count, int key) {
    int end = line_end(rl_point);
    int flush_right = rl_point == end;
    rl_point = flush_right ? rl_end : end;
    return 0;
}

static int line_kill_callback(int count, int key) {
    int end = line_end(rl_point);
    int flush_right = rl_point == end;
    int kill = flush_right ? end + prompt_length + 1 : end;
    if (kill > rl_end) kill = rl_end;
    rl_kill_text(rl_point, kill);
    return 0;
}

static int left_callback(int count, int key) {
    if (rl_point > 0) {
        int i = line_start(rl_point);
        rl_point = (i == 0 || rl_point-i > prompt_length) ?
            rl_point-1 : i-1;
    }
    return 0;
}

static int right_callback(int count, int key) {
    rl_point += (rl_line_buffer[rl_point] == '\n') ? prompt_length+1 : 1;
    if (rl_end < rl_point) rl_point = rl_end;
    return 0;
}

static int up_callback(int count, int key) {
    int i = line_start(rl_point);
    if (i > 0) {
        int j = line_start(i-1);
        if (j == 0) rl_point -= prompt_length;
        rl_point += j - i;
        if (rl_point >= i) rl_point = i - 1;
    } else {
        if (history_offset >= 0) {
            history_set_pos(history_offset+1);
            history_offset = -1;
        }
        return rl_get_previous_history(count, key);
    }
    return 0;
}

static int down_callback(int count, int key) {
    int j = line_end(rl_point);
    if (j < rl_end) {
        int i = line_start(rl_point);
        if (i == 0) rl_point += prompt_length;
        rl_point += j - i + 1;
        int k = line_end(j+1);
        if (rl_point > k) rl_point = k;
    } else {
        if (history_offset >= 0) {
            history_set_pos(history_offset);
            history_offset = -1;
        }
        return rl_get_next_history(count, key);
    }
    return 0;
}

static int backspace_callback(int count, int key) {
    return 0;
    if (rl_point > 0) {
        int i = line_start(rl_point);
        int j = (i == 0 || rl_point-i > prompt_length) ? rl_point-1 : i-1;
        rl_delete_text(j, i);
    }
    return 0;
}

static void read_expression(char *prompt, int *end, int *doprint)
{
    ast = NULL;
    char *input = readline(prompt);
    if (!input || ios_eof(ios_stdin)) {
        *end = 1;
        return;
    }
    if (ast == NULL) return;

    *doprint = !ends_with_semicolon(input);
    if (input && *input) {
        HIST_ENTRY *entry = current_history();
        if (!entry || strcmp(input, entry->line)) {
            add_history(input);
            append_history(1, jl_history_file);
            history_offset = -1;
        } else {
            history_offset = where_history();
        }
    }

    ios_printf(ios_stdout, "\n");

    free(input);
    return;
}

#else

static char *ios_readline(ios_t *s)
{
    ios_t dest;
    ios_mem(&dest, 0);
    ios_copyuntil(&dest, s, '\n');
    size_t n;
    return ios_takebuf(&dest, &n);
}

static void read_expression(char *prompt, int *end, int *doprint)
{
    ast = NULL;
    char *input;
    ios_printf(ios_stdout, prompt);
    ios_flush(ios_stdout);
    input = ios_readline(ios_stdin);
    ios_purge(ios_stdin);

    if (!input || ios_eof(ios_stdin)) {
        *end = 1;
        return NULL;
    }

    ast = jl_parse_input_line(input);
    if (ast == NULL) return NULL;
    if (jl_is_expr(ast) && ((jl_expr_t*)ast)->head == continue_sym)
        return read_expression(prompt, end, doprint);

    *doprint = !ends_with_semicolon(input);
}

#endif

int main(int argc, char *argv[])
{
    llt_init();
    parse_opts(&argc, &argv);
    julia_init();

    have_color = detect_color();
    char *banner = have_color ? jl_banner_color : jl_banner_plain;
    char *prompt = have_color ? jl_prompt_color : jl_prompt_plain;
    prompt_length = strlen(jl_prompt_plain);

#ifdef USE_READLINE
    init_history();
    rl_bind_key(' ', space_callback);
    rl_bind_key('\t', tab_callback);
    rl_bind_key('\r', return_callback);
    rl_bind_key('\n', return_callback);
    rl_bind_key('\v', line_kill_callback);
    rl_bind_key('\001', line_start_callback);
    rl_bind_key('\005', line_end_callback);
    rl_bind_key('\002', left_callback);
    rl_bind_key('\006', right_callback);
    rl_bind_keyseq("\033[A", up_callback);
    rl_bind_keyseq("\033[B", down_callback);
    rl_bind_keyseq("\033[D", left_callback);
    rl_bind_keyseq("\033[C", right_callback);
#endif

    if (load_start_j) {
        if (!setjmp(ExceptionHandler)) {
            jl_load("start.j");
        } else {
            ios_printf(ios_stderr, "error during startup.\n");
            return 1;
        }
    }
    if (print_banner)
        ios_printf(ios_stdout, "%s", banner);

    while (1) {
        ios_flush(ios_stdout);

        if (!setjmp(ExceptionHandler)) {
            int end = 0;
            int print_value = 1;
            read_expression(prompt, &end, &print_value);
            if (end) {
                ios_printf(ios_stdout, "\n");
                break;
            }
            if (have_color) {
                ios_printf(ios_stdout, jl_answer_color);
                ios_flush(ios_stdout);
            }
            if (ast != NULL) {
                jl_value_t *value = jl_toplevel_eval(ast);
                if (print_value) {
                    jl_print(value);
                    ios_printf(ios_stdout, "\n");
                }
            }
        }

        ios_printf(ios_stdout, "\n");
    }
    if (have_color) {
        ios_printf(ios_stdout, jl_color_normal);
        ios_flush(ios_stdout);
    }

    return 0;
}
