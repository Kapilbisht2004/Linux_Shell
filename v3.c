#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <errno.h>
#include <pwd.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/utsname.h>

// NEW: Headers for new creative functions
#include <curl/curl.h> // For weather command
#include "tinyexpr.h"  // For calc command

#define HISTORY_FILE ".gtk_shell_history"
#define MAX_HISTORY 1000
#define READ_BUF_SIZE 4096
#define MAX_ARGS 64
#define DEFAULT_FONT_SIZE 12

//--- Structs ---//
typedef struct
{
    char *input_file;
    char *output_file;
    gboolean append_output;
} RedirectionInfo;

// NEW: Helper struct for libcurl to store response data
typedef struct
{
    char *buffer;
    size_t size;
} CurlBuffer;

typedef struct
{
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *text_view;
    GtkTextBuffer *buffer;
    GtkTextMark *input_mark;
    GtkCssProvider *css_provider;
    GPtrArray *history;
    int history_index;
    int current_font_size;
    gboolean is_dark_theme;
    gboolean tab_completion_active;
    char *last_completion_prefix;
} AppContext;

//--- Prototypes ---//
void on_app_activate(GApplication *app, gpointer user_data);
AppContext *app_context_new();
void app_context_free(AppContext *ctx);
void update_styles(AppContext *ctx);
void toggle_theme_cb(GtkToggleButton *button, AppContext *ctx);
void change_font_size_cb(GtkButton *button, AppContext *ctx);
void append_text(AppContext *ctx, const char *text, const char *tag);
void update_prompt(AppContext *ctx);
void handle_enter(AppContext *ctx);
gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, AppContext *ctx);
void on_delete_range(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end, AppContext *ctx);
void on_insert_text(GtkTextBuffer *buffer, GtkTextIter *location, gchar *text, gint len, AppContext *ctx);
void run_command(AppContext *ctx, const gchar *cmd_line);
int parse_command(char *command_line, char *args[], RedirectionInfo *redir);
void cleanup_args(int argc, char *args[], RedirectionInfo *redir);
gboolean handle_builtin(AppContext *ctx, int argc, char *args[]);
void execute_external_command(AppContext *ctx, int argc, char *args[], RedirectionInfo *redir);

// Existing Built-ins
void display_welcome_header(AppContext *ctx);
gboolean builtin_help(AppContext *ctx, int argc, char *args[]);
gboolean builtin_echo(AppContext *ctx, int argc, char *args[]);
gboolean builtin_cat(AppContext *ctx, int argc, char *args[]);
gboolean builtin_rm(AppContext *ctx, int argc, char *args[]);
gboolean builtin_touch(AppContext *ctx, int argc, char *args[]);
gboolean builtin_reverse(AppContext *ctx, int argc, char *args[]);
gboolean builtin_countdown(AppContext *ctx, int argc, char *args[]);
gboolean builtin_pwd(AppContext *ctx, int argc, char *args[]);
gboolean builtin_history(AppContext *ctx, int argc, char *args[]);
gboolean builtin_sysinfo(AppContext *ctx, int argc, char *args[]);
gboolean builtin_search(AppContext *ctx, int argc, char *args[]);
void search_recursive(AppContext *ctx, const char *base_path, const char *pattern, int *match_count);

// NEW: Prototypes for creative functions
gboolean builtin_calc(AppContext *ctx, int argc, char *args[]);
gboolean builtin_plot(AppContext *ctx, int argc, char *args[]);
gboolean builtin_weather(AppContext *ctx, int argc, char *args[]);

void handle_tab_completion(AppContext *ctx);
gchar *get_current_word_for_completion(AppContext *ctx, gint *cursor_pos_in_word);
GPtrArray *find_completion_matches(const char *prefix, const char *dir_path);
char *get_longest_common_prefix(GPtrArray *matches);
void replace_input_line(AppContext *ctx, const char *text);

// --- Styling and Theming (Unchanged) ---
// ... (code from previous step is unchanged here)
void display_welcome_header(AppContext *ctx)
{
    struct passwd *pw = getpwuid(getuid());
    const char *username = pw ? pw->pw_name : "User";
    g_autofree gchar *capitalized = g_strdup(username);
    if (g_ascii_islower(capitalized[0]))
        capitalized[0] = g_ascii_toupper(capitalized[0]);

    g_autofree gchar *welcome_msg = g_strdup_printf("Welcome, %s!\n", capitalized);
    append_text(ctx, welcome_msg, "center");

    append_text(ctx, "HorizonShell Initialized. Type 'help' for a list of commands.\n\n", "center");
}
void update_styles(AppContext *ctx)
{
    const char *bg_color, *fg_color, *prompt_color, *error_color, *highlight_color;
    if (ctx->is_dark_theme)
    {
        bg_color = "#2E2E2E";
        fg_color = "#DCDCDC";
        prompt_color = "#87CEFA";
        error_color = "#FF6347";
        highlight_color = "#F0E68C";
    }
    else
    {
        bg_color = "#FFFFFF";
        fg_color = "#000000";
        prompt_color = "#0000CD";
        error_color = "#DC143C";
        highlight_color = "#DAA520";
    }

    g_autofree gchar *css = g_strdup_printf(
        "textview, textview text {"
        "   background-color: %s;"
        "   color: %s;"
        "   font-family: Monospace;"
        "   font-size: %dpx;"
        "   caret-color: %s;"
        "}",
        bg_color, fg_color, ctx->current_font_size, fg_color);

    gtk_css_provider_load_from_data(ctx->css_provider, css, -1, NULL);

    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(ctx->buffer);
    GtkTextTag *prompt_tag = gtk_text_tag_table_lookup(table, "prompt");
    GtkTextTag *error_tag = gtk_text_tag_table_lookup(table, "error");
    GtkTextTag *highlight_tag = gtk_text_tag_table_lookup(table, "highlight");

    if (prompt_tag)
        g_object_set(prompt_tag, "foreground", prompt_color, NULL);
    if (error_tag)
        g_object_set(error_tag, "foreground", error_color, NULL);
    if (highlight_tag)
        g_object_set(highlight_tag, "foreground", highlight_color, NULL);
}

void toggle_theme_cb(GtkToggleButton *button, AppContext *ctx)
{
    ctx->is_dark_theme = gtk_toggle_button_get_active(button);
    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-application-prefer-dark-theme", ctx->is_dark_theme, NULL);
    update_styles(ctx);
    gtk_widget_queue_draw(GTK_WIDGET(ctx->text_view));
    gtk_button_set_label(GTK_BUTTON(button), ctx->is_dark_theme ? "Light Mode" : "Dark Mode");
}

void change_font_size_cb(GtkButton *button, AppContext *ctx)
{
    const gchar *label = gtk_button_get_label(button);
    if (g_strcmp0(label, "+") == 0)
    {
        if (ctx->current_font_size < 40)
            ctx->current_font_size++;
    }
    else if (g_strcmp0(label, "-") == 0)
    {
        if (ctx->current_font_size > 8)
            ctx->current_font_size--;
    }
    else
    {
        ctx->current_font_size = DEFAULT_FONT_SIZE;
    }
    update_styles(ctx);
}

//--- Text Buffer and Prompt (Unchanged) ---
// ... (code from previous step is unchanged here)
void append_text(AppContext *ctx, const char *text, const char *tag)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(ctx->buffer, &end);
    if (tag)
    {
        gtk_text_buffer_insert_with_tags_by_name(ctx->buffer, &end, text, -1, tag, NULL);
    }
    else
    {
        gtk_text_buffer_insert(ctx->buffer, &end, text, -1);
    }
    GtkTextMark *mark = gtk_text_buffer_get_insert(ctx->buffer);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(ctx->text_view), mark, 0.0, TRUE, 0.0, 1.0);
}

void update_prompt(AppContext *ctx)
{
    char cwd[PATH_MAX], hostname[HOST_NAME_MAX], *username, prompt[PATH_MAX + HOST_NAME_MAX + 128];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        strcpy(cwd, "?");
    if (gethostname(hostname, sizeof(hostname)) != 0)
        strcpy(hostname, "localhost");
    struct passwd *pw = getpwuid(getuid());
    username = pw ? pw->pw_name : "user";
    const char *home_dir = g_get_home_dir();
    if (home_dir && strncmp(cwd, home_dir, strlen(home_dir)) == 0)
    {
        char new_cwd[PATH_MAX];
        snprintf(new_cwd, sizeof(new_cwd), "~%s", cwd + strlen(home_dir));
        strcpy(cwd, new_cwd);
    }
    snprintf(prompt, sizeof(prompt), "%s@%s %s$ ", username, hostname, cwd);
    append_text(ctx, prompt, "prompt");
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(ctx->buffer, &end);
    gtk_text_buffer_move_mark(ctx->buffer, ctx->input_mark, &end);
}

// --- Event Handlers (Unchanged) ---
// ... (code from previous step is unchanged here)
void on_delete_range(GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end, AppContext *ctx)
{
    GtkTextIter input_start_iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &input_start_iter, ctx->input_mark);
    if (gtk_text_iter_compare(start, &input_start_iter) < 0)
    {
        if (gtk_text_iter_compare(end, &input_start_iter) > 0)
        {
            g_signal_stop_emission_by_name(buffer, "delete-range");
            gtk_text_buffer_delete(buffer, &input_start_iter, end);
        }
        else
        {
            g_signal_stop_emission_by_name(buffer, "delete-range");
        }
    }
}

void on_insert_text(GtkTextBuffer *buffer, GtkTextIter *location, gchar *text, gint len, AppContext *ctx)
{
    GtkTextIter input_start_iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &input_start_iter, ctx->input_mark);
    if (gtk_text_iter_compare(location, &input_start_iter) < 0)
    {
        g_signal_stop_emission_by_name(buffer, "insert-text");
    }
}

gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, AppContext *ctx)
{
    GtkTextIter cursor_iter, input_start_iter;
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &cursor_iter, gtk_text_buffer_get_insert(ctx->buffer));
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &input_start_iter, ctx->input_mark);
    if (event->keyval != GDK_KEY_Tab)
    {
        ctx->tab_completion_active = FALSE;
        g_free(ctx->last_completion_prefix);
        ctx->last_completion_prefix = NULL;
    }

    switch (event->keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        handle_enter(ctx);
        return TRUE;
    case GDK_KEY_Up:
        if (ctx->history_index > 0)
        {
            ctx->history_index--;
            replace_input_line(ctx, g_ptr_array_index(ctx->history, ctx->history_index));
        }
        return TRUE;
    case GDK_KEY_Down:
        if (ctx->history_index < (int)ctx->history->len - 1)
        {
            ctx->history_index++;
            replace_input_line(ctx, g_ptr_array_index(ctx->history, ctx->history_index));
        }
        else
        {
            ctx->history_index = ctx->history->len;
            replace_input_line(ctx, "");
        }
        return TRUE;
    case GDK_KEY_Tab:
        handle_tab_completion(ctx);
        return TRUE;
    case GDK_KEY_Left:
        if (gtk_text_iter_equal(&cursor_iter, &input_start_iter))
            return TRUE;
        break;
    case GDK_KEY_Home:
        gtk_text_buffer_place_cursor(ctx->buffer, &input_start_iter);
        return TRUE;
    case GDK_KEY_BackSpace:
        if (gtk_text_iter_equal(&cursor_iter, &input_start_iter))
            return TRUE;
        break;
    }
    return FALSE;
}

void handle_enter(AppContext *ctx)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &start, ctx->input_mark);
    gtk_text_buffer_get_end_iter(ctx->buffer, &end);
    gchar *cmd_line = gtk_text_buffer_get_text(ctx->buffer, &start, &end, FALSE);
    gchar *trimmed_cmd = g_strstrip(cmd_line);
    append_text(ctx, "\n", NULL);
    if (strlen(trimmed_cmd) > 0)
    {
        if (ctx->history->len == 0 || strcmp(trimmed_cmd, (char *)g_ptr_array_index(ctx->history, ctx->history->len - 1)) != 0)
        {
            g_ptr_array_add(ctx->history, g_strdup(trimmed_cmd));
        }
        ctx->history_index = ctx->history->len;
        run_command(ctx, trimmed_cmd);
    }
    update_prompt(ctx);
    g_free(cmd_line);
}

void replace_input_line(AppContext *ctx, const char *text)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &start, ctx->input_mark);
    gtk_text_buffer_get_end_iter(ctx->buffer, &end);
    g_signal_handlers_block_by_func(ctx->buffer, on_delete_range, ctx);
    g_signal_handlers_block_by_func(ctx->buffer, on_insert_text, ctx);
    gtk_text_buffer_delete(ctx->buffer, &start, &end);
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &start, ctx->input_mark);
    gtk_text_buffer_insert(ctx->buffer, &start, text, -1);
    g_signal_handlers_unblock_by_func(ctx->buffer, on_delete_range, ctx);
    g_signal_handlers_unblock_by_func(ctx->buffer, on_insert_text, ctx);
    gtk_text_buffer_get_end_iter(ctx->buffer, &end);
    gtk_text_buffer_place_cursor(ctx->buffer, &end);
}

// --- Shell Command Logic ---

void run_command(AppContext *ctx, const gchar *cmd_line_const)
{
    char *cmd_line = g_strdup(cmd_line_const), *args[MAX_ARGS];
    RedirectionInfo redir;
    int argc = parse_command(cmd_line, args, &redir);
    if (argc > 0 && !handle_builtin(ctx, argc, args))
    {
        execute_external_command(ctx, argc, args, &redir);
    }
    cleanup_args(argc, args, &redir);
    g_free(cmd_line);
}

// ... (parse_command and cleanup_args are unchanged)
int parse_command(char *command_line, char *args[], RedirectionInfo *redir)
{
    int argc = 0;
    char *token, *saveptr;
    redir->input_file = NULL;
    redir->output_file = NULL;
    redir->append_output = FALSE;
    token = strtok_r(command_line, " \t\n\r", &saveptr);
    while (token != NULL && argc < MAX_ARGS - 1)
    {
        if (strcmp(token, "<") == 0)
        {
            if ((token = strtok_r(NULL, " \t\n\r", &saveptr)))
                redir->input_file = g_strdup(token);
        }
        else if (strcmp(token, ">") == 0)
        {
            if ((token = strtok_r(NULL, " \t\n\r", &saveptr)))
            {
                redir->output_file = g_strdup(token);
                redir->append_output = FALSE;
            }
        }
        else if (strcmp(token, ">>") == 0)
        {
            if ((token = strtok_r(NULL, " \t\n\r", &saveptr)))
            {
                redir->output_file = g_strdup(token);
                redir->append_output = TRUE;
            }
        }
        else
        {
            args[argc++] = g_strdup(token);
        }
        token = strtok_r(NULL, " \t\n\r", &saveptr);
    }
    args[argc] = NULL;
    return argc;
}

void cleanup_args(int argc, char *args[], RedirectionInfo *redir)
{
    for (int i = 0; i < argc; i++)
        g_free(args[i]);
    g_free(redir->input_file);
    g_free(redir->output_file);
}

// MODIFIED: Added dispatches for creative commands
gboolean handle_builtin(AppContext *ctx, int argc, char *args[])
{
    if (argc <= 0)
        return FALSE;

    if (strcmp(args[0], "help") == 0)
        return builtin_help(ctx, argc, args);
    if (strcmp(args[0], "exit") == 0)
    {
        gtk_window_close(GTK_WINDOW(ctx->window));
        return TRUE;
    }
    if (strcmp(args[0], "clear") == 0)
    {
        // Block the signal handlers that prevent modification of past output.
        g_signal_handlers_block_by_func(ctx->buffer, (gpointer)on_delete_range, ctx);
        g_signal_handlers_block_by_func(ctx->buffer, (gpointer)on_insert_text, ctx);

        gtk_text_buffer_set_text(ctx->buffer, "", -1);
        
        // FIX: Re-display the welcome header.
        display_welcome_header(ctx);

        // Unblock the signal handlers so they work for user input again.
        g_signal_handlers_unblock_by_func(ctx->buffer, (gpointer)on_delete_range, ctx);
        g_signal_handlers_unblock_by_func(ctx->buffer, (gpointer)on_insert_text, ctx);
                
        return TRUE;
    }
    if (strcmp(args[0], "cd") == 0)
    {
        const char *dir = (argc > 1) ? args[1] : g_get_home_dir();
        if (chdir(dir) != 0)
        {
            g_autofree gchar *error_msg = g_strdup_printf("Error: Could not change directory to '%s': %s\n", dir, strerror(errno));
            append_text(ctx, error_msg, "error");
        }
        return TRUE;
    }

    
    if (strcmp(args[0], "cat") == 0)
        return builtin_cat(ctx, argc, args);
    if (strcmp(args[0], "rm") == 0)
        return builtin_rm(ctx, argc, args);
    if (strcmp(args[0], "delete") == 0)
        return builtin_rm(ctx, argc, args);
    if (strcmp(args[0], "touch") == 0)
        return builtin_touch(ctx, argc, args);
    if (strcmp(args[0], "mkfile") == 0)
        return builtin_touch(ctx, argc, args);
    if (strcmp(args[0], "reverse") == 0)
        return builtin_reverse(ctx, argc, args);
    if (strcmp(args[0], "countdown") == 0)
        return builtin_countdown(ctx, argc, args);
    if (strcmp(args[0], "pwd") == 0)
        return builtin_pwd(ctx, argc, args);
    if (strcmp(args[0], "history") == 0)
        return builtin_history(ctx, argc, args);
    if (strcmp(args[0], "sysinfo") == 0)
        return builtin_sysinfo(ctx, argc, args);
    if (strcmp(args[0], "search") == 0)
        return builtin_search(ctx, argc, args);

    // NEW: Dispatch to creative functions
    if (strcmp(args[0], "calc") == 0)
        return builtin_calc(ctx, argc, args);
    if (strcmp(args[0], "plot") == 0)
        return builtin_plot(ctx, argc, args);
    if (strcmp(args[0], "weather") == 0)
        return builtin_weather(ctx, argc, args);

    return FALSE;
}

// --- Built-in Command Implementations ---

// MODIFIED: Updated help text
gboolean builtin_help(AppContext *ctx, int argc, char *args[])
{
    const char *help_text =
        "HorizonShell Built-in Commands:\n\n"
        "--- Standard ---\n"
        "  help                 - Shows this help message.\n"
        "  exit                 - Closes the shell.\n"
        "  clear                - Clears the terminal screen.\n"
        "  cd [dir]             - Changes the current directory.\n"
        "  pwd                  - Prints the current working directory.\n"
        "  echo [text]          - Prints text to the screen.\n"
        "  cat [file...]        - Displays the content of one or more files.\n"
        "  touch [file...]      - Creates files or updates their timestamp.\n"
        "  delete [file...]      - Deletes file or files.\n"
        "  mkfile [file...]      - Creates files or updates their timestamp.\n"
        "  history              - Displays command history.\n"
        "  search <pat> [dir]   - Recursively searches for a file pattern.\n"
        "\n--- Creative & Utility ---\n"
        "  calc <expression>    - Evaluates a mathematical expression (e.g., '5 * (2+3)').\n"
        "  plot <nums...>       - Displays a text-based bar chart of numbers.\n"
        "  weather [location]   - Shows the current weather for a location.\n"
        "  sysinfo              - Displays basic system information.\n"
        "  reverse <text>       - Reverses a string.\n"
        "  countdown <secs>     - Starts a countdown for a given number of seconds.\n"
        "\nRedirection is supported for external commands (e.g., ls > out.txt).\n";

    append_text(ctx, help_text, "highlight");
    return TRUE;
}

// ... (Implementations for cat, rm, delete touch, mkfile, reverse, countdown, etc. are unchanged)
// Place this function definition with the other builtin_ functions

gboolean builtin_rm(AppContext *ctx, int argc, char *args[])
{
    if (argc < 2)
    {
        append_text(ctx, "Usage: rm <file1> [file2] ...\n", "highlight");
        return TRUE;
    }

    for (int i = 1; i < argc; i++)
    {
        // The remove() function (from <stdio.h>) deletes a file.
        // It returns 0 on success and a non-zero value on error.
        if (remove(args[i]) != 0)
        {
            // If it fails, report the error using strerror(errno)
            g_autofree gchar *error_msg = g_strdup_printf("rm: %s: %s\n", args[i], strerror(errno));
            append_text(ctx, error_msg, "error");
        }
        // On success, we print nothing, which is standard behavior for rm.
    }
    return TRUE;
}
gboolean builtin_cat(AppContext *ctx, int argc, char *args[])
{
    if (argc < 2)
    {
        append_text(ctx, "Usage: cat <file1> [file2] ...\n", "highlight");
        return TRUE;
    }
    for (int i = 1; i < argc; i++)
    {
        gchar *contents;
        gsize length;
        if (g_file_get_contents(args[i], &contents, &length, NULL))
        {
            append_text(ctx, contents, NULL);
            g_free(contents);
        }
        else
        {
            g_autofree gchar *error_msg = g_strdup_printf("cat: %s: %s\n", args[i], strerror(errno));
            append_text(ctx, error_msg, "error");
        }
    }
    return TRUE;
}
gboolean builtin_touch(AppContext *ctx, int argc, char *args[])
{
    if (argc < 2)
    {
        append_text(ctx, "Usage: touch <file1> [file2] ...\n", "highlight");
        return TRUE;
    }
    for (int i = 1; i < argc; i++)
    {
        int fd = open(args[i], O_WRONLY | O_CREAT | O_NONBLOCK, 0664);
        if (fd == -1)
        {
            g_autofree gchar *error_msg = g_strdup_printf("touch: %s: %s\n", args[i], strerror(errno));
            append_text(ctx, error_msg, "error");
        }
        else
        {
            close(fd);
        }
    }
    return TRUE;
}
gboolean builtin_reverse(AppContext *ctx, int argc, char *args[])
{
    if (argc < 2)
    {
        append_text(ctx, " Usage: reverse <text>\n", "highlight");
        return TRUE;
    }
    char *input = args[1];
    int len = strlen(input);
    g_autofree char *reversed = g_new(char, len + 1);
    for (int i = 0; i < len; i++)
        reversed[i] = input[len - 1 - i];
    reversed[len] = '\0';
    g_autofree char *out_str = g_strconcat("Reversed: ", reversed, "\n", NULL);
    append_text(ctx, out_str, "highlight");
    return TRUE;
}
gboolean builtin_countdown(AppContext *ctx, int argc, char *args[])
{
    if (argc < 2)
    {
        append_text(ctx, "Usage: countdown <number>\n", "center");
        return TRUE;
    }
    int seconds = atoi(args[1]);
    if (seconds <= 0)
    {
        append_text(ctx, "Invalid input: Countdown time must be a positive integer.\n", "error");
        return TRUE;
    }
    for (int i = seconds; i > 0; i--)
    {
        g_autofree char *buf = g_strdup_printf("Time left : %d\n", i);
        append_text(ctx, buf, NULL);
        while (gtk_events_pending())
            gtk_main_iteration();
        sleep(1);
    }
    append_text(ctx, "Countdown complete. Blast off!\n", "center");
    return TRUE;
}
gboolean builtin_pwd(AppContext *ctx, int argc, char *args[])
{
    g_autofree char *cwd = g_get_current_dir();
    if (cwd)
    {
        g_autofree char *out_str = g_strconcat(cwd, "\n", NULL);
        append_text(ctx, "Current working directory is : ",NULL);
        append_text(ctx, out_str, "center");
    }
    else
    {
        append_text(ctx, "Error: Cannot get current working directory.\n", "error");
    }
    return TRUE;
}
gboolean builtin_history(AppContext *ctx, int argc, char *args[])
{
    for (guint i = 0; i < ctx->history->len; i++)
    {
        g_autofree gchar *line = g_strdup_printf("%4d  %s\n", i + 1, (char *)g_ptr_array_index(ctx->history, i));
        append_text(ctx, line, NULL);
    }
    return TRUE;
}
gboolean builtin_sysinfo(AppContext *ctx, int argc, char *args[])
{
    struct utsname kernel_info;
    if (uname(&kernel_info) != 0)
    {
        append_text(ctx, "Error: Unable to fetch system information.\n", "error");
        return TRUE;
    }
    g_autofree gchar *output = g_strdup_printf(
        "System Information:\n"
        "  OS       : %s\n"
        "  Hostname : %s\n"
        "  Kernel   : %s\n"
        "  Version  : %s\n"
        "  Arch     : %s\n",
        kernel_info.sysname, kernel_info.nodename, kernel_info.release, kernel_info.version, kernel_info.machine);
    append_text(ctx, output, "highlight");
    return TRUE;
}
void search_recursive(AppContext *ctx, const char *base_path, const char *pattern, int *match_count)
{
    DIR *dir = opendir(base_path);
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        g_autofree gchar *full_path = g_build_filename(base_path, entry->d_name, NULL);
        if (strstr(entry->d_name, pattern) != NULL)
        {
            (*match_count)++;
            append_text(ctx, full_path, NULL);
            append_text(ctx, "\n", NULL);
        }
        while (gtk_events_pending())
            gtk_main_iteration();
        if (g_file_test(full_path, G_FILE_TEST_IS_DIR))
        {
            search_recursive(ctx, full_path, pattern, match_count);
        }
    }
    closedir(dir);
}
gboolean builtin_search(AppContext *ctx, int argc, char *args[])
{
    if (argc < 2)
    {
        append_text(ctx, "Usage: search <pattern> [directory]\n", "highlight");
        return TRUE;
    }
    const char *pattern = args[1];
    const char *start_dir = (argc > 2) ? args[2] : ".";
    int match_count = 0;
    g_autofree gchar *start_msg = g_strdup_printf("Searching for '%s' in '%s'...\n", pattern, start_dir);
    append_text(ctx, start_msg, NULL);
    search_recursive(ctx, start_dir, pattern, &match_count);
    g_autofree gchar *end_msg = g_strdup_printf("\nSearch complete. Found %d match(es).\n", match_count);
    append_text(ctx, end_msg, "highlight");
    return TRUE;
}

// NEW: `calc` implementation
gboolean builtin_calc(AppContext *ctx, int argc, char *args[])
{
    if (argc < 2)
    {
        append_text(ctx, "Usage: calc <expression>\n  Example: calc 5 * (2+10) / 2\n", "highlight");
        return TRUE;
    }

    // Join all arguments into a single string
    g_autofree gchar *expression = g_strjoinv(" ", &args[1]);
    int err;
    double result = te_interp(expression, &err);

    if (err)
    {
        g_autofree gchar *error_msg = g_strdup_printf("Calculation error at character %d: '%s'\n", err, expression);
        append_text(ctx, error_msg, "error");
    }
    else
    {
        g_autofree gchar *result_msg = g_strdup_printf("Result => %g\n", result);
        append_text(ctx, result_msg, "center");
    }
    return TRUE;
}

// NEW: `plot` implementation
gboolean builtin_plot(AppContext *ctx, int argc, char *args[])
{
    if (argc < 2)
    {
        append_text(ctx, "Usage: plot <number1> <number2> ...\n", "highlight");
        return TRUE;
    }

    double values[argc - 1];
    double max_val = 0.0;
    for (int i = 1; i < argc; i++)
    {
        values[i - 1] = atof(args[i]);
        if (values[i - 1] > max_val)
        {
            max_val = values[i - 1];
        }
    }

    if (max_val == 0)
    {
        append_text(ctx, "Cannot plot zero or negative values.\n", "error");
        return TRUE;
    }

    const int max_bar_width = 50;
    for (int i = 0; i < argc - 1; i++)
    {
        int bar_width = (values[i] / max_val) * max_bar_width;
        if (values[i] > 0 && bar_width == 0)
            bar_width = 1; // Show at least one char for small positive values

        g_autofree gchar *bar = g_strnfill(bar_width, L'.');
        g_autofree gchar *line = g_strdup_printf("  %10g | %s\n", values[i], bar);
        append_text(ctx, line, NULL);
    }

    return TRUE;
}

// NEW: `weather` implementation (with libcurl)
// libcurl write-back function
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    CurlBuffer *mem = (CurlBuffer *)userp;
    char *ptr = realloc(mem->buffer, mem->size + realsize + 1);
    if (ptr == NULL)
    {
        printf("Out of memory!\n");
        return 0;
    }
    mem->buffer = ptr;
    memcpy(&(mem->buffer[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = 0;
    return realsize;
}

gboolean builtin_weather(AppContext *ctx, int argc, char *args[])
{
    CURL *curl;
    CURLcode res;
    const char *location = (argc > 1) ? args[1] : ""; // Default to auto-location by IP

    // Initialize libcurl once
    static gboolean curl_initialized = FALSE;
    if (!curl_initialized)
    {
        curl_global_init(CURL_GLOBAL_ALL);
        curl_initialized = TRUE;
    }

    curl = curl_easy_init();
    if (curl)
    {
        CurlBuffer chunk;
        chunk.buffer = malloc(1);
        chunk.size = 0;

        g_autofree gchar *url = g_strdup_printf("http://wttr.in/%s?format=%%l:%%20%%C%%20%%t%%20%%w", location);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        append_text(ctx, "Fetching weather...\n", "highlight");
        while (gtk_events_pending())
            gtk_main_iteration(); // Make GUI responsive

        res = curl_easy_perform(curl);

        if (res != CURLE_OK)
        {
            g_autofree gchar *err_msg = g_strdup_printf("weather: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            append_text(ctx, err_msg, "error");
        }
        else
        {
            append_text(ctx, chunk.buffer, "center");
            append_text(ctx, "\n", NULL);
        }

        curl_easy_cleanup(curl);
        free(chunk.buffer);
    }
    return TRUE;
}

// ... All other code (execute_external_command, tab completion, main, etc.) is unchanged ...
// The following is provided for completeness.

void execute_external_command(AppContext *ctx, int argc, char *args[], RedirectionInfo *redir)
{
    int out_fd[2];
    pid_t pid;
    if (pipe(out_fd) == -1)
    {
        append_text(ctx, "System Error: Unable to create internal pipe.\n", "error");
        return;
    }
    pid = fork();
    if (pid == -1)
    {
        append_text(ctx, "System Error: Could not fork child process.\n", "error");
        close(out_fd[0]);
        close(out_fd[1]);
        return;
    }
    if (pid == 0)
    { // Child
        close(out_fd[0]);
        if (redir->input_file)
        {
            int in_fd = open(redir->input_file, O_RDONLY);
            if (in_fd == -1)
            {
                perror("open input");
                _exit(127);
            }
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
        }
        if (redir->output_file)
        {
            int flags = O_WRONLY | O_CREAT | (redir->append_output ? O_APPEND : O_TRUNC);
            int out_redir_fd = open(redir->output_file, flags, 0644);
            if (out_redir_fd == -1)
            {
                perror("open output");
                _exit(127);
            }
            dup2(out_redir_fd, STDOUT_FILENO);
            dup2(out_redir_fd, STDERR_FILENO);
            close(out_redir_fd);
        }
        else
        {
            dup2(out_fd[1], STDOUT_FILENO);
            dup2(out_fd[1], STDERR_FILENO);
        }
        close(out_fd[1]);
        execvp(args[0], args);
        perror(args[0]);
        _exit(126);
    }
    else
    { // Parent
        close(out_fd[1]);
        char buffer[READ_BUF_SIZE];
        ssize_t n_read;
        while ((n_read = read(out_fd[0], buffer, sizeof(buffer) - 1)) > 0)
        {
            buffer[n_read] = '\0';
            append_text(ctx, buffer, NULL);
            while (gtk_events_pending())
                gtk_main_iteration();
        }
        close(out_fd[0]);
        waitpid(pid, NULL, 0);
    }
}

//--- Tab Completion Logic ---//
gchar *get_current_word_for_completion(AppContext *ctx, gint *cursor_pos_in_word)
{
    GtkTextIter start_iter, cursor_iter;
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &start_iter, ctx->input_mark);
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &cursor_iter, gtk_text_buffer_get_insert(ctx->buffer));
    gchar *text = gtk_text_buffer_get_text(ctx->buffer, &start_iter, &cursor_iter, FALSE);
    const gchar *p = text + strlen(text) - 1;
    while (p >= text && *p != ' ' && *p != '\t')
    {
        p--;
    }
    if (cursor_pos_in_word)
    {
        *cursor_pos_in_word = (text + strlen(text)) - (p + 1);
    }
    char *result = g_strdup(p + 1);
    g_free(text);
    return result;
}

GPtrArray *find_completion_matches(const char *prefix, const char *dir_path)
{
    GPtrArray *matches = g_ptr_array_new_with_free_func(g_free);
    DIR *d = opendir(dir_path);
    if (!d)
        return matches;
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL)
    {
        if (strncmp(prefix, dir->d_name, strlen(prefix)) == 0)
        {
            if (dir->d_name[0] == '.' && prefix[0] != '.')
                continue;
            g_ptr_array_add(matches, g_strdup(dir->d_name));
        }
    }
    closedir(d);
    return matches;
}

char *get_longest_common_prefix(GPtrArray *matches)
{
    if (matches->len == 0)
        return g_strdup("");
    if (matches->len == 1)
        return g_strdup(g_ptr_array_index(matches, 0));
    char *first = (char *)g_ptr_array_index(matches, 0);
    int max_len = strlen(first);
    for (int i = 0; i < max_len; i++)
    {
        char c = first[i];
        for (guint j = 1; j < matches->len; j++)
        {
            char *current = (char *)g_ptr_array_index(matches, j);
            if (i >= (int)strlen(current) || current[i] != c)
            {
                return g_strndup(first, i);
            }
        }
    }
    return g_strdup(first);
}

void handle_tab_completion(AppContext *ctx)
{
    gint prefix_len_in_line;
    g_autofree gchar *prefix = get_current_word_for_completion(ctx, &prefix_len_in_line);
    if (ctx->tab_completion_active && ctx->last_completion_prefix && strcmp(prefix, ctx->last_completion_prefix) == 0)
    {
        g_autoptr(GPtrArray) matches = find_completion_matches(prefix, ".");
        if (matches->len > 1)
        {
            append_text(ctx, "\n", NULL);
            for (guint i = 0; i < matches->len; i++)
            {
                append_text(ctx, (char *)g_ptr_array_index(matches, i), NULL);
                append_text(ctx, (i % 5 == 4 || i == matches->len - 1) ? "\n" : "\t", NULL);
            }
            update_prompt(ctx);
            replace_input_line(ctx, prefix);
        }
        ctx->tab_completion_active = FALSE;
        return;
    }
    g_autoptr(GPtrArray) matches = find_completion_matches(prefix, ".");
    if (matches->len == 0)
        return;
    g_autofree char *common_prefix = get_longest_common_prefix(matches);
    GtkTextIter start_iter, cursor_iter;
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &start_iter, ctx->input_mark);
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &cursor_iter, gtk_text_buffer_get_insert(ctx->buffer));
    gchar *full_input = gtk_text_buffer_get_text(ctx->buffer, &start_iter, &cursor_iter, FALSE);
    char *new_input_end = g_strdup(common_prefix);
    if (matches->len == 1)
    {
        gchar *full_path = g_build_filename(".", (char *)g_ptr_array_index(matches, 0), NULL);
        if (g_file_test(full_path, G_FILE_TEST_IS_DIR))
        {
            char *temp = new_input_end;
            new_input_end = g_strconcat(temp, "/", NULL);
            g_free(temp);
        }
        g_free(full_path);
    }
    full_input[strlen(full_input) - prefix_len_in_line] = '\0';
    gchar *final_line = g_strconcat(full_input, new_input_end, NULL);
    replace_input_line(ctx, final_line);
    g_free(full_input);
    g_free(new_input_end);
    g_free(final_line);
    ctx->tab_completion_active = TRUE;
    g_free(ctx->last_completion_prefix);
    ctx->last_completion_prefix = g_strdup(common_prefix);
}

void on_app_activate(GApplication *app, gpointer user_data)
{
    AppContext *ctx = (AppContext *)user_data;
    ctx->app = GTK_APPLICATION(app);
    ctx->window = gtk_application_window_new(ctx->app);
    gtk_window_set_title(GTK_WINDOW(ctx->window), "HorizonShell v1.0");
    gtk_window_set_default_size(GTK_WINDOW(ctx->window), 800, 600);


    // Header Bar and Menu
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "HorizonShell v1.0");
    gtk_window_set_titlebar(GTK_WINDOW(ctx->window), header);

    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_button);

    // --- MENU BUTTON FIX IS HERE (Part 1) ---
    // Use the modern, simpler way to set an icon on a button.
    gtk_button_set_image(GTK_BUTTON(menu_button),
                         gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));

    // --- MENU POPOVER FIX IS HERE (Part 2) ---
    // Create the popover with NULL, then attach it to the button. This is the standard pattern.
    GtkWidget *popover = gtk_popover_new(NULL);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button), popover);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(popover), vbox);
    gtk_widget_show_all(popover);

    // Menu Contents (Theme Toggle)
    GtkWidget *theme_toggle = gtk_toggle_button_new_with_label("Light Mode");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(theme_toggle), ctx->is_dark_theme);
    g_signal_connect(theme_toggle, "toggled", G_CALLBACK(toggle_theme_cb), ctx);
    gtk_box_pack_start(GTK_BOX(vbox), theme_toggle, FALSE, TRUE, 0);

    // Menu Contents (Font Size)
    GtkWidget *font_label = gtk_label_new("Font Size");
    gtk_widget_set_halign(font_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), font_label, FALSE, TRUE, 0);

    GtkWidget *font_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(font_box), "linked");

    GtkWidget *font_down = gtk_button_new_with_label("-");
    GtkWidget *font_reset = gtk_button_new_with_label("Reset");
    GtkWidget *font_up = gtk_button_new_with_label("+");

    g_signal_connect(font_down, "clicked", G_CALLBACK(change_font_size_cb), ctx);
    g_signal_connect(font_reset, "clicked", G_CALLBACK(change_font_size_cb), ctx);
    g_signal_connect(font_up, "clicked", G_CALLBACK(change_font_size_cb), ctx);

    gtk_box_pack_start(GTK_BOX(font_box), font_down, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(font_box), font_reset, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(font_box), font_up, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), font_box, FALSE, TRUE, 0);
    gtk_widget_show_all(popover);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctx->window), scrolled);

    ctx->text_view = gtk_text_view_new();
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(ctx->text_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(ctx->text_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(ctx->text_view), 15);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(ctx->text_view), 8);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx->text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ctx->text_view), TRUE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(ctx->text_view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ctx->text_view), TRUE);
    gtk_container_add(GTK_CONTAINER(scrolled), ctx->text_view);

    ctx->css_provider = gtk_css_provider_new();
    GtkStyleContext *style_context = gtk_widget_get_style_context(ctx->text_view);
    gtk_style_context_add_provider(style_context, GTK_STYLE_PROVIDER(ctx->css_provider), GTK_STYLE_PROVIDER_PRIORITY_USER);

    ctx->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->text_view));
    gtk_text_buffer_create_tag(ctx->buffer, "prompt", "foreground", "#87CEFA", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(ctx->buffer, "error", "foreground", "#FF6347", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(ctx->buffer, "highlight", "foreground", "#F0E68C", NULL);
    gtk_text_buffer_create_tag(ctx->buffer, "center",
                               "justification", GTK_JUSTIFY_CENTER,
                               "foreground", "#F0E68C",
                               "weight", PANGO_WEIGHT_BOLD,
                               NULL);
    update_styles(ctx);

    struct passwd *pw = getpwuid(getuid());
    const char *username = pw ? pw->pw_name : "User";
    g_autofree gchar *capitalized = g_strdup(username);
    if (g_ascii_islower(capitalized[0]))
        capitalized[0] = g_ascii_toupper(capitalized[0]);
    g_autofree gchar *welcome_msg = g_strdup_printf("Welcome, %s!\n", capitalized);
    append_text(ctx, welcome_msg, "center");

    append_text(ctx, "HorizonShell Initialized. Type 'help' for a list of commands.\n\n", "center");

    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(ctx->buffer, &end_iter);
    ctx->input_mark = gtk_text_buffer_create_mark(ctx->buffer, "input_start_mark", &end_iter, TRUE);

    g_signal_connect(ctx->text_view, "key-press-event", G_CALLBACK(on_key_press), ctx);
    g_signal_connect(ctx->buffer, "insert-text", G_CALLBACK(on_insert_text), ctx);
    g_signal_connect(ctx->buffer, "delete-range", G_CALLBACK(on_delete_range), ctx);

    update_prompt(ctx);
    gtk_widget_show_all(ctx->window);
    gtk_widget_grab_focus(ctx->text_view);
}

AppContext *app_context_new()
{
    AppContext *ctx = g_new0(AppContext, 1);
    ctx->history = g_ptr_array_new_with_free_func(g_free);
    ctx->history_index = 0;
    ctx->current_font_size = DEFAULT_FONT_SIZE;
    ctx->is_dark_theme = TRUE;
    return ctx;
}

void app_context_free(AppContext *ctx)
{
    if (!ctx)
        return;
    g_ptr_array_free(ctx->history, TRUE);
    g_free(ctx->last_completion_prefix);
    if (ctx->css_provider)
        g_object_unref(ctx->css_provider);
    g_free(ctx);
}

int main(int argc, char *argv[])
{
    AppContext *ctx = app_context_new();
    GtkApplication *app = gtk_application_new("com.github.user.gtkshell", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), ctx);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    app_context_free(ctx);
    return status;
}