#include <gtk/gtk.h>
#include <glib.h>
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

#define PROMPT_TEXT "[user@gtk-shell ~]$ "
#define HISTORY_FILE "cmd_history.txt"
#define MAX_HISTORY 1000
#define READ_BUF_SIZE 4096
#define MAX_ARGS 64

/* Global variables from new GUI */
GtkWidget *text_view;
GtkTextBuffer *text_buffer;
GtkTextMark *input_mark;
GPtrArray *history;
int history_index = -1;
FILE *log_file;
int current_font_size = 14;

/* Existing shell globals */
typedef struct {
    char *input_file;
    char *output_file;
    gboolean append_output;
} RedirectionInfo;

/* Function prototypes */
void update_font_css(GtkCssProvider *provider);
void toggle_theme(GtkButton *button, gpointer user_data);
void log_to_file(const char *text);
void append_colored_text(const char *text, const char *tag);
void update_prompt();
void run_command(const char *cmd);
void handle_enter();
void replace_input_line(const char *text);
gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
int parse_command(char *command_line, char *args[], RedirectionInfo *redir);
void cleanup_args(int argc, char *args[], RedirectionInfo *redir);
gboolean handle_builtin(int argc, char *args[]);
void execute_external_command(int argc, char *args[], RedirectionInfo *redir);
gboolean reverse(int argc, char *args[]);
gboolean countdown(int argc, char *args[]);
char *complete_path(const char *path_prefix);

/* GUI Functions */
void update_font_css(GtkCssProvider *provider) {
    char css[512];
    snprintf(css, sizeof(css),
        "textview, textview text {"
        " background-color: #000000;"
        " color: #00FF00;"
        " font-family: 'Monospace';"
        " font-size: %dpx;"
        " }", current_font_size);
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
}

void toggle_theme(GtkButton *button, gpointer user_data) {
    static gboolean dark = TRUE;
    GtkCssProvider *provider = GTK_CSS_PROVIDER(user_data);

    const char *dark_css =
        "textview, textview text {"
        " background-color: #000000;"
        " color: #00FF00;"
        " font-family: 'Monospace';"
        " font-size: 14px;"
        "}";

    const char *light_css =
        "textview, textview text {"
        " background-color: #FFFFFF;"
        " color: #000000;"
        " font-family: 'Monospace';"
        " font-size: 14px;"
        "}";

    gtk_css_provider_load_from_data(provider,
        dark ? light_css : dark_css,
        -1, NULL);
    dark = !dark;
}

void log_to_file(const char *text) {
    if (log_file) {
        fprintf(log_file, "%s", text);
        fflush(log_file);
    }
}

void append_colored_text(const char *text, const char *tag) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(text_buffer, &end);
    gtk_text_buffer_insert_with_tags_by_name(text_buffer, &end, text, -1, tag, NULL);
    GtkTextMark *mark = gtk_text_buffer_get_insert(text_buffer);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(text_view), mark);
    log_to_file(text);
}

void update_prompt() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        char prompt[2048];
        snprintf(prompt, sizeof(prompt), "[user@gtk-shell %s]$ ", cwd);
        append_colored_text(prompt, "prompt");
    } else {
        append_colored_text(PROMPT_TEXT, "prompt");
    }
}

/* Shell Functions */
int parse_command(char *command_line, char *args[], RedirectionInfo *redir) {
    int argc = 0;
    char *token;
    char *saveptr;

    redir->input_file = NULL;
    redir->output_file = NULL;
    redir->append_output = FALSE;

    token = strtok_r(command_line, " \t\n\r", &saveptr);
    while (token != NULL && argc < MAX_ARGS - 1) {
        if (strcmp(token, "<") == 0) {
            token = strtok_r(NULL, " \t\n\r", &saveptr);
            if (token != NULL) redir->input_file = g_strdup(token);
        } else if (strcmp(token, ">") == 0) {
            token = strtok_r(NULL, " \t\n\r", &saveptr);
            if (token != NULL) {
                redir->output_file = g_strdup(token);
                redir->append_output = FALSE;
            }
        } else if (strcmp(token, ">>") == 0) {
            token = strtok_r(NULL, " \t\n\r", &saveptr);
            if (token != NULL) {
                redir->output_file = g_strdup(token);
                redir->append_output = TRUE;
            }
        } else {
            args[argc] = g_strdup(token);
            if (args[argc] == NULL) g_error("g_strdup failed: Out of memory");
            argc++;
        }
        token = strtok_r(NULL, " \t\n\r", &saveptr);
    }
    
    args[argc] = NULL;
    return argc;
}

void cleanup_args(int argc, char *args[], RedirectionInfo *redir) {
    if (!args) return;
    
    for (int i = 0; i < argc; i++) {
        g_free(args[i]);
        args[i] = NULL;
    }
    
    if (redir) {
        g_free(redir->input_file);
        g_free(redir->output_file);
        redir->input_file = NULL;
        redir->output_file = NULL;
        redir->append_output = FALSE;
    }
}

gboolean reverse(int argc, char *args[]) {
    if (argc < 2) {
        append_colored_text("Usage: reverse <string>\n", "output");
        return TRUE;
    }

    char *input = args[1];
    int length = strlen(input);
    char reversed[length + 1];

    for (int i = 0; i < length; i++) {
        reversed[i] = input[length - i - 1];
    }
    reversed[length] = '\0';

    append_colored_text("Reversed: ", "output");
    append_colored_text(reversed, "output");
    append_colored_text("\n", "output");

    return TRUE;
}

gboolean countdown(int argc, char *args[]) {
    if (argc < 2) {
        append_colored_text("Usage: countdown <seconds>\n", "output");
        return TRUE;
    }

    int seconds = atoi(args[1]);
    if (seconds <= 0) {
        append_colored_text("Please provide a positive number of seconds.\n", "output");
        return TRUE;
    }

    append_colored_text("Starting countdown:\n", "output");
    for (int i = seconds; i >= 0; i--) {
        append_colored_text("Time left: ", "output");
        char time_left[10];
        snprintf(time_left, sizeof(time_left), "%d", i);
        append_colored_text(time_left, "output");
        append_colored_text("\n", "output");
        
        while (gtk_events_pending()) {
            gtk_main_iteration();
        }

        sleep(1);
    }

    append_colored_text("Countdown complete!\n", "output");
    return TRUE;
}

gboolean handle_builtin(int argc, char *args[]) {
    if (argc <= 0 || args == NULL || args[0] == NULL) {
        return FALSE;
    }
    
    if (strcmp(args[0], "exit") == 0) {
        gtk_main_quit();
        return TRUE;
    }

    if (strcmp(args[0], "clear") == 0) {
        gtk_text_buffer_set_text(text_buffer, "", -1);
        update_prompt();
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(text_buffer, &end);
        gtk_text_buffer_move_mark(text_buffer, input_mark, &end);
        return TRUE;
    }

    if (strcmp(args[0], "countdown") == 0) {
        countdown(argc, args);
        return TRUE;
    }
    
    if (strcmp(args[0], "reverse") == 0) {
        reverse(argc, args);
        return TRUE;
    }
    
    if (strcmp(args[0], "cd") == 0) {
        const char *target_dir = NULL;
        
        if (argc > 1) {
            target_dir = args[1];
        } else {
            target_dir = getenv("HOME");
            if (!target_dir) {
                target_dir = "/";
            }
        }

        if (chdir(target_dir) != 0) {
            char error_msg[PATH_MAX + 64];
            snprintf(error_msg, sizeof(error_msg), "cd: %s: %s\n", target_dir, strerror(errno));
            append_colored_text(error_msg, "output");
        }
        return TRUE;
    }
    
    return FALSE;
}

void execute_external_command(int argc, char *args[], RedirectionInfo *redir) {
    if (argc <= 0 || args == NULL || args[0] == NULL) return;

    int pipe_fd[2];
    pid_t pid;

    if (pipe(pipe_fd) == -1) {
        append_colored_text("Error: Failed to create pipe.\n", "output");
        return;
    }

    pid = fork();
    if (pid == -1) {
        append_colored_text("Error: Failed to fork process.\n", "output");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return;
    }

    if (pid == 0) {
        close(pipe_fd[0]);

        if (redir->input_file) {
            FILE *in = fopen(redir->input_file, "r");
            if (!in) {
                perror("input redirection failed");
                _exit(EXIT_FAILURE);
            }
            dup2(fileno(in), STDIN_FILENO);
            fclose(in);
        }

        if (redir->output_file) {
            FILE *out = fopen(redir->output_file, redir->append_output ? "a" : "w");
            if (!out) {
                perror("output redirection failed");
                _exit(EXIT_FAILURE);
            }
            dup2(fileno(out), STDOUT_FILENO);
            dup2(fileno(out), STDERR_FILENO);
            fclose(out);
        } else {
            dup2(pipe_fd[1], STDOUT_FILENO);
            dup2(pipe_fd[1], STDERR_FILENO);
        }

        close(pipe_fd[1]);
        execvp(args[0], args);
        perror(args[0]);
        _exit(EXIT_FAILURE);
    } else {
        close(pipe_fd[1]);
        char buffer[READ_BUF_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            append_colored_text(buffer, "output");
        }
        
        if (bytes_read == -1) {
            append_colored_text("\nError reading command output.\n", "output");
        }
        close(pipe_fd[0]);

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "\nProcess exited with status %d\n", WEXITSTATUS(status));
            append_colored_text(msg, "output");
        } else if (WIFSIGNALED(status)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "\nProcess terminated by signal %d\n", WTERMSIG(status));
            append_colored_text(msg, "output");
        }
    }
}

/* Command Handling */
void handle_enter() {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_mark(text_buffer, &start, input_mark);
    gtk_text_buffer_get_end_iter(text_buffer, &end);
    gchar *cmd = gtk_text_buffer_get_text(text_buffer, &start, &end, FALSE);
    g_strstrip(cmd);

    if (strlen(cmd) > 0) {
        g_ptr_array_add(history, g_strdup(cmd));
        history_index = history->len;
        append_colored_text("\n", "output");

        char *args[MAX_ARGS];
        RedirectionInfo redir = {0};
        int argc = parse_command(cmd, args, &redir);
        
        if (!handle_builtin(argc, args)) {
            execute_external_command(argc, args, &redir);
        }
        
        cleanup_args(argc, args, &redir);
    }

    update_prompt();
    gtk_text_buffer_get_end_iter(text_buffer, &end);
    gtk_text_buffer_move_mark(text_buffer, input_mark, &end);
    g_free(cmd);
}

void replace_input_line(const char *text) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_mark(text_buffer, &start, input_mark);
    gtk_text_buffer_get_end_iter(text_buffer, &end);
    gtk_text_buffer_delete(text_buffer, &start, &end);
    gtk_text_buffer_insert(text_buffer, &start, text, -1);
}

gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    GtkCssProvider *css = GTK_CSS_PROVIDER(user_data);

    if ((event->state & GDK_CONTROL_MASK)) {
        if (event->keyval == GDK_KEY_plus || event->keyval == GDK_KEY_equal) {
            if (current_font_size < 40) current_font_size++;
            update_font_css(css);
            return TRUE;
        } else if (event->keyval == GDK_KEY_minus) {
            if (current_font_size > 8) current_font_size--;
            update_font_css(css);
            return TRUE;
        } else if (event->keyval == GDK_KEY_0) {
            current_font_size = 14;
            update_font_css(css);
            return TRUE;
        }
    }

    if (event->keyval == GDK_KEY_Return) {
        handle_enter();
        return TRUE;
    } else if (event->keyval == GDK_KEY_Up) {
        if (history_index > 0) {
            history_index--;
            replace_input_line(g_ptr_array_index(history, history_index));
        }
        return TRUE;
    } else if (event->keyval == GDK_KEY_Down) {
        if (history_index < (int)history->len - 1) {
            history_index++;
            replace_input_line(g_ptr_array_index(history, history_index));
        } else {
            replace_input_line("");
        }
        return TRUE;
    }

    return FALSE;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    log_file = fopen("gtk_shell.log", "a");

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GTK Terminal");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkCssProvider *css = gtk_css_provider_new();
    update_font_css(css);

    GtkWidget *button = gtk_button_new_with_label("Dark Mode");
    gtk_box_pack_start(GTK_BOX(vbox), button, FALSE, FALSE, 0);
    g_signal_connect(button, "clicked", G_CALLBACK(toggle_theme), css);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    text_view = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(scrolled), text_view);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), TRUE);

    text_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_buffer_create_tag(text_buffer, "prompt", "foreground", "#00BFFF", NULL);
    gtk_text_buffer_create_tag(text_buffer, "output", "foreground", "#00FF00", NULL);
    gtk_text_buffer_create_tag(text_buffer, "highlight", "foreground", "#FFFFFF", NULL);

    GtkStyleContext *context = gtk_widget_get_style_context(text_view);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER);

    history = g_ptr_array_new_with_free_func(g_free);

    append_colored_text("Welcome to GTK Shell\n", "output");
    update_prompt();

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(text_buffer, &end);
    input_mark = gtk_text_buffer_create_mark(text_buffer, "input", &end, TRUE);

    g_signal_connect(text_view, "key-press-event", G_CALLBACK(on_key_press), css);

    gtk_widget_show_all(window);
    gtk_main();

    if (log_file) fclose(log_file);
    g_ptr_array_free(history, TRUE);
    return 0;
}