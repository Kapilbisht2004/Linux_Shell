#include <gtk/gtk.h>
#include <glib.h>      // *** Include glib.h *** // GLib Change
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <errno.h>

// --- Globals ---
GtkTextView *output_view;
GtkTextBuffer *output_buffer;
GtkEntry *input_entry;
GtkWindow *main_window;

// --- Constants ---
#define READ_BUF_SIZE 4096
#define MAX_ARGS 64

// --- Function Prototypes ---
void append_output(const char *text);
void update_prompt_and_title(void);
gboolean handle_builtin(int argc, char *args[]);
void execute_external_command(int argc, char *args[]);
void on_entry_activate(GtkEntry *entry, gpointer user_data);
void cleanup_args(int argc, char *args[]);
int parse_command(char *command_line, char *args[]);

// --- Helper: Append Text --- (No changes)
void append_output(const char *text) {
    if (!text) return;
    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(output_buffer, &end_iter);
    gtk_text_buffer_insert(output_buffer, &end_iter, text, -1);
    GtkTextMark *end_mark = gtk_text_buffer_get_insert(output_buffer);
    gtk_text_view_scroll_mark_onscreen(output_view, end_mark);
}

// --- Helper: Update Prompt/Title --- (No changes)
void update_prompt_and_title(void) {
    char cwd[PATH_MAX];
    char prompt[PATH_MAX + 4];
    char title[PATH_MAX + 20];

    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        snprintf(prompt, sizeof(prompt), "%s> ", cwd);
        snprintf(title, sizeof(title), "Linux C Shell - %s", cwd);
    } else {
        perror("getcwd() error");
        snprintf(prompt, sizeof(prompt), "?> ");
        snprintf(title, sizeof(title), "Linux C Shell - Error");
    }

    append_output("\n");
    append_output(prompt);
    gtk_window_set_title(main_window, title);
}

// --- Helper: Parse Command Line ---
// Uses g_strdup now
int parse_command(char *command_line, char *args[]) {
    int argc = 0;
    char *token;
    char *saveptr;

    token = strtok_r(command_line, " \t\n\r", &saveptr);
    while (token != NULL && argc < MAX_ARGS - 1) {
        // *** Use g_strdup *** // GLib Change
        args[argc] = g_strdup(token);
        if (args[argc] == NULL) {
             g_error("g_strdup failed: Out of memory"); // g_error terminates
             // cleanup_args(argc, args); // Not strictly needed after g_error
             // return -1;
        }
        argc++;
        token = strtok_r(NULL, " \t\n\r", &saveptr);
    }
    args[argc] = NULL;
    return argc;
}

// --- Helper: Free Memory Allocated by parse_command ---
// Uses g_free now
void cleanup_args(int argc, char *args[]) {
     if (!args) return;
     for (int i = 0; i < argc; i++) {
          // *** Use g_free *** // GLib Change
          g_free(args[i]);
          args[i] = NULL;
     }
}

// --- Handle Built-in Commands --- (No changes)
gboolean handle_builtin(int argc, char *args[]) {
    if (argc <= 0 || args == NULL || args[0] == NULL) {
        return FALSE;
    }
    // ... (rest of function unchanged) ...
    if (strcmp(args[0], "exit") == 0) {
        gtk_main_quit();
        return TRUE;
    }

    if (strcmp(args[0], "clear") == 0) {
        gtk_text_buffer_set_text(output_buffer, "", 0);
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
            append_output(error_msg);
        }
        return TRUE;
    }
    return FALSE;
}

// --- Execute External Command --- (No changes)
void execute_external_command(int argc, char *args[]) {
     // ... (function unchanged) ...
    if (argc <= 0 || args == NULL || args[0] == NULL) {
        return;
    }

    int pipe_fd[2];
    pid_t pid;

    if (pipe(pipe_fd) == -1) {
        perror("pipe failed");
        append_output("Error: Failed to create pipe.\n");
        return;
    }

    pid = fork();
    if (pid == -1) {
        perror("fork failed");
        append_output("Error: Failed to fork process.\n");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return;
    }

    if (pid == 0) { // Child
        close(pipe_fd[0]);
        if (dup2(pipe_fd[1], STDOUT_FILENO) == -1) {
            perror("dup2 stdout failed"); close(pipe_fd[1]); _exit(EXIT_FAILURE);
        }
        if (dup2(pipe_fd[1], STDERR_FILENO) == -1) {
             perror("dup2 stderr failed"); close(pipe_fd[1]); _exit(EXIT_FAILURE);
        }
        close(pipe_fd[1]);
        execvp(args[0], args);
        perror(args[0]);
        _exit(EXIT_FAILURE);

    } else { // Parent
        close(pipe_fd[1]);
        char buffer[READ_BUF_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            append_output(buffer);
            while (gtk_events_pending()) { gtk_main_iteration(); }
        }
        if (bytes_read == -1) {
             perror("read from pipe failed"); append_output("\nError reading command output.\n");
        }
        close(pipe_fd[0]);
        int status;
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid failed"); append_output("\nError waiting for child process.\n");
        } else {
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code != 0) {
                    char status_msg[64];
                    snprintf(status_msg, sizeof(status_msg), "\nProcess exited with status %d", exit_code);
                    append_output(status_msg);
                }
            } else if (WIFSIGNALED(status)) {
                 char signal_msg[64];
                 snprintf(signal_msg, sizeof(signal_msg), "\nProcess terminated by signal %d", WTERMSIG(status));
                 append_output(signal_msg);
            }
        }
    }
}

// --- Signal Handler for Input Entry ---
// Uses g_strdup and g_free now
void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;

    const char *command_const = gtk_entry_get_text(entry);
    if (!command_const) return;

    // *** Use g_strdup *** // GLib Change
    char *command_line = g_strdup(command_const);
    if (!command_line) {
        g_error("g_strdup failed: Out of memory"); // Terminate on critical error
        return;
    }

    // Trim leading/trailing whitespace
    char *start = command_line;
    while (g_ascii_isspace(*start)) start++;
    char *end = start + strlen(start) - 1;
    while (end > start && g_ascii_isspace(*end)) end--;
    *(end + 1) = '\0';

    gboolean show_prompt = TRUE;

    if (strlen(start) > 0) {
        append_output("\n");
        append_output(start);
        append_output("\n");

        char *args[MAX_ARGS];
        int argc = parse_command(start, args); // Uses g_strdup internally now

        if (argc < 0) { // Error already reported by g_error in parse_command
            // append_output("Error parsing command.\n"); // No longer needed
        } else {
            // Keep track if 'clear' was called to potentially skip prompt
            gboolean was_clear = (argc > 0 && strcmp(args[0],"clear") == 0);

            if (!handle_builtin(argc, args)) {
                execute_external_command(argc, args);
            }

            if(was_clear) { // Skip prompt only if clear was the command
                show_prompt = FALSE;
            }

            // Cleanup memory allocated by parse_command (using g_free now)
            cleanup_args(argc, args);
        }
    } else {
         append_output("\n");
    }

    // *** Use g_free *** // GLib Change
    g_free(command_line);
    gtk_entry_set_text(entry, "");

    if (show_prompt) {
         update_prompt_and_title();
    }
}

// --- Main Function --- (No changes)
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    main_window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(main_window, "Linux C Shell");
    gtk_window_set_default_size(main_window, 800, 600);
    g_signal_connect(main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);
    output_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(output_view, FALSE);
    gtk_text_view_set_cursor_visible(output_view, FALSE);
    gtk_text_view_set_wrap_mode(output_view, GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled_window), GTK_WIDGET(output_view));
    output_buffer = gtk_text_view_get_buffer(output_view);
    input_entry = GTK_ENTRY(gtk_entry_new());
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(input_entry), FALSE, FALSE, 0);
    g_signal_connect(input_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    append_output("Welcome to Linux C Shell!\n");
    update_prompt_and_title();
    gtk_widget_show_all(GTK_WIDGET(main_window));
    gtk_widget_grab_focus(GTK_WIDGET(input_entry));
    gtk_main();
    return 0;
}
