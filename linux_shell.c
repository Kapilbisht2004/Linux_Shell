#include <gtk/gtk.h>
#include <glib.h>      // *** Include glib.h *** // GLib Change
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <errno.h>
#include <pwd.h>       // Include for getpwuid 
#include <sys/types.h> // Include for uid_t 
#include <dirent.h> // For directory traversal


#define HISTORY_FILE "cmd_history.txt"
#define MAX_HISTORY 1000

GPtrArray *command_history = NULL;
int history_index = -1;


// --- Globals ---
GtkTextView *output_view;
GtkTextBuffer *output_buffer;
GtkEntry *input_entry;
GtkWindow *main_window;

// --- Constants --- 
#define READ_BUF_SIZE 4096
#define MAX_ARGS 64

typedef struct {
    char *input_file;
    char *output_file;
    gboolean append_output;
} RedirectionInfo;

// --- Function Prototypes ---
void append_output(const char *text);
void update_prompt_and_title(void);
gboolean handle_builtin(int argc, char *args[]);
gboolean reverse(int argc, char *args[]);
gboolean countdown(int argc, char *args[]);
void execute_external_command(int argc, char *args[], RedirectionInfo *redir);

// --- Helper: Autocomplete Path Based on Prefix ---
char *complete_path(const char *path_prefix) {
    char *last_slash = strrchr(path_prefix, '/');
    char dir_path[PATH_MAX];
    const char *partial;

    if (last_slash) {
        strncpy(dir_path, path_prefix, last_slash - path_prefix);
        dir_path[last_slash - path_prefix] = '\0';
        partial = last_slash + 1;
    } else {
        strcpy(dir_path, ".");
        partial = path_prefix;
    }

    DIR *dir = opendir(*dir_path ? dir_path : ".");
    if (!dir) return NULL;

    struct dirent *entry;
    char *match = NULL;
    size_t partial_len = strlen(partial);
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, partial, partial_len) == 0) {
            if (!match) {
                match = g_strdup(entry->d_name);
            } else {
                // More than one match
                g_free(match);
                closedir(dir);
                return NULL;
            }
        }
    }
    closedir(dir);

    if (match) {
        char *completed_path;
        if (last_slash) {
            size_t prefix_len = last_slash - path_prefix + 1;
            completed_path = g_malloc(prefix_len + strlen(match) + 1);
            strncpy(completed_path, path_prefix, prefix_len);
            completed_path[prefix_len] = '\0';
            strcat(completed_path, match);
        } else {
            completed_path = g_strdup(match);
        }
        g_free(match);
        return completed_path;
    }
    return NULL;
}




//?arrow keys function
// gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
//     if (!command_history || command_history->len == 0)
//         return FALSE;

//     if (event->keyval == GDK_KEY_Up) {
//         if (history_index > 0)
//             history_index--;
//         const char *cmd = g_ptr_array_index(command_history, history_index);
//         gtk_entry_set_text(GTK_ENTRY(widget), cmd);
//         gtk_editable_set_position(GTK_EDITABLE(widget), -1);
//         return TRUE;
//     } else if (event->keyval == GDK_KEY_Down) {
//         if (history_index < (int)command_history->len - 1) {
//             history_index++;
//             const char *cmd = g_ptr_array_index(command_history, history_index);
//             gtk_entry_set_text(GTK_ENTRY(widget), cmd);
//         } else {
//             gtk_entry_set_text(GTK_ENTRY(widget), "");
//             history_index = command_history->len;
//         }
//         gtk_editable_set_position(GTK_EDITABLE(widget), -1);
//         return TRUE;
//     }

//     return FALSE;
// }

gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    if (!command_history || command_history->len == 0)
        return FALSE;

    if (event->keyval == GDK_KEY_Up) {
        if (history_index > 0)
            history_index--;
        const char *cmd = g_ptr_array_index(command_history, history_index);
        gtk_entry_set_text(GTK_ENTRY(widget), cmd);
        gtk_editable_set_position(GTK_EDITABLE(widget), -1);
        return TRUE;
    } else if (event->keyval == GDK_KEY_Down) {
        if (history_index < (int)command_history->len - 1) {
            history_index++;
            const char *cmd = g_ptr_array_index(command_history, history_index);
            gtk_entry_set_text(GTK_ENTRY(widget), cmd);
        } else {
            gtk_entry_set_text(GTK_ENTRY(widget), "");
            history_index = command_history->len;
        }
        gtk_editable_set_position(GTK_EDITABLE(widget), -1);
        return TRUE;
    } else if (event->keyval == GDK_KEY_Tab) {
        const char *current_text = gtk_entry_get_text(GTK_ENTRY(widget));
        if (!current_text || strlen(current_text) == 0) return TRUE;

        int cursor_pos = gtk_editable_get_position(GTK_EDITABLE(widget));
        if (cursor_pos <= 0) return TRUE;

        // Duplicate up to cursor position
        char *before_cursor = g_strndup(current_text, cursor_pos);

        // Find last space to extract the word to complete
        char *last_space = strrchr(before_cursor, ' ');
        const char *word_to_complete = last_space ? last_space + 1 : before_cursor;

        char *completed = complete_path(word_to_complete);
        if (completed) {
            GString *new_text = g_string_new_len(current_text, last_space ? (last_space - current_text + 1) : 0);
            g_string_append(new_text, completed);
            g_free(completed);

            gtk_entry_set_text(GTK_ENTRY(widget), new_text->str);
            gtk_editable_set_position(GTK_EDITABLE(widget), -1);
            g_string_free(new_text, TRUE);
        }

        g_free(before_cursor);
        return TRUE; // Handled Tab key
    }

    return FALSE;
}


void on_entry_activate(GtkEntry *entry, gpointer user_data);
void cleanup_args(int argc, char *args[], RedirectionInfo *redir);
int parse_command(char *command_line, char *args[], RedirectionInfo *redir);

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
// --- Helper: Update Prompt/Title ---
void update_prompt_and_title(void) {
    char cwd_buf[PATH_MAX];
    char hostname[HOST_NAME_MAX + 1] = "?"; // Default if gethostname fails
    char username[LOGIN_NAME_MAX + 1] = "?"; // Default if getpwuid fails
    // Allocate enough space: user + @ + host + : + cwd + $ + space + null
    char prompt[LOGIN_NAME_MAX + HOST_NAME_MAX + PATH_MAX + 5];
    char title[PATH_MAX + 20];
    char *cwd_ptr = "?"; // Default if getcwd fails

    // --- Get Username ---
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) {
        strncpy(username, pw->pw_name, sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0'; // Ensure null termination
    } else {
         // Optionally log getpwuid error, but keep username as "?"
         strcpy(username, "user"); // Or provide a simple default
    }

    // --- Get Hostname ---
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostname[sizeof(hostname) - 1] = '\0'; // Ensure null termination
    } else {
         // Optionally log gethostname error, but keep hostname as "?"
         strcpy(hostname, "host"); // Or provide a simple default
    }

    // --- Get Current Directory ---
    if (getcwd(cwd_buf, sizeof(cwd_buf)) != NULL) {
        cwd_ptr = cwd_buf;
        // Set window title based on CWD
        snprintf(title, sizeof(title), "Linux C Shell - %s", cwd_ptr);
    } else {
        perror("getcwd() error");
        cwd_ptr = "?"; // Use "?" if getcwd fails
        // Set window title indicating error
        snprintf(title, sizeof(title), "Linux C Shell - Error getting CWD");
    }

    // --- Format Prompt ---
    snprintf(prompt, sizeof(prompt), "%s@%s : %s$ ", username, hostname, cwd_ptr);

    // --- Update UI ---
    append_output("\n"); // Add newline before prompt
    append_output(prompt);
    gtk_window_set_title(main_window, title);
}

// --- Helper: Parse Command Line ---
// Uses g_strdup now
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


// --- Helper: Free Memory Allocated by parse_command ---
// Uses g_free now
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

// Custom command to reverse a string
gboolean reverse(int argc, char *args[]) {
    if (argc < 2) {
        append_output("Usage: reverse <string>\n");
        return TRUE;
    }

    char *input = args[1];
    int length = strlen(input);
    char reversed[length + 1];

    for (int i = 0; i < length; i++) {
        reversed[i] = input[length - i - 1];
    }
    reversed[length] = '\0'; // Null terminate the string

    append_output("Reversed: ");
    append_output(reversed);
    append_output("\n");

    return TRUE;
}

// Custom command for countdown
gboolean countdown(int argc, char *args[]) {
    if (argc < 2) {
        append_output("Usage: countdown <seconds>\n");
        return TRUE;
    }

    int seconds = atoi(args[1]);
    if (seconds <= 0) {
        append_output("Please provide a positive number of seconds.\n");
        return TRUE;
    }

    append_output("Starting countdown:\n");
    for (int i = seconds; i >= 0; i--) {
        append_output("Time left: ");
        char time_left[10];
        snprintf(time_left, sizeof(time_left), "%d", i);
        append_output(time_left);
        append_output("\n");
        // Allow GTK to update the UI
        while (gtk_events_pending()) {
            gtk_main_iteration();
        }

        sleep(1);
    }

    append_output("Countdown complete!\n");
    return TRUE;
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

    if (strcmp(args[0], "countdown") == 0) {
        countdown(argc,args);
        return TRUE;
    }
    if (strcmp(args[0], "reverse") == 0) {
        reverse(argc,args);
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
void execute_external_command(int argc, char *args[], RedirectionInfo *redir) {
    if (argc <= 0 || args == NULL || args[0] == NULL) return;

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

        // Handle input redirection
        if (redir->input_file) {
            FILE *in = fopen(redir->input_file, "r");
            if (!in) {
                perror("input redirection failed");
                _exit(EXIT_FAILURE);
            }
            dup2(fileno(in), STDIN_FILENO);
            fclose(in);
        }

        // Handle output redirection
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
    } else { // Parent
        close(pipe_fd[1]);
        char buffer[READ_BUF_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            append_output(buffer);
            while (gtk_events_pending()) gtk_main_iteration();
        }
        if (bytes_read == -1) {
            perror("read failed");
            append_output("\nError reading command output.\n");
        }
        close(pipe_fd[0]);

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "\nProcess exited with status %d", WEXITSTATUS(status));
            append_output(msg);
        } else if (WIFSIGNALED(status)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "\nProcess terminated by signal %d", WTERMSIG(status));
            append_output(msg);
        }
    }
}


// --- Signal Handler for Input Entry ---
// Uses g_strdup and g_free now
void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;

    const char *command_const = gtk_entry_get_text(entry);
    if (!command_const) return;

    // Duplicate input safely
    char *command_line = g_strdup(command_const);
    if (!command_line) {
        g_error("g_strdup failed: Out of memory");
        return;
    }

    // Trim whitespace
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
        RedirectionInfo redir = {0};  // Init redirection info

        int argc = parse_command(start, args, &redir);
        if (argc >= 0) {
            gboolean was_clear = (argc > 0 && strcmp(args[0], "clear") == 0);

            if (!handle_builtin(argc, args)) {
                execute_external_command(argc, args, &redir);
            }

            if (was_clear) {
                show_prompt = FALSE;
            }

            cleanup_args(argc, args, &redir);
        }
    } else {
        append_output("\n");
    }
    //?for arrow function
    if (strlen(start) > 0) {
        // Add to in-memory history
        g_ptr_array_add(command_history, g_strdup(start));
        history_index = command_history->len;
    
        // Append to history file
        FILE *history_fp = fopen(HISTORY_FILE, "a");
        if (history_fp) {
            fprintf(history_fp, "%s\n", start);
            fclose(history_fp);
        }
    }
    
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
    // --- Load command history from file ---
       //?for arrow key function
    command_history = g_ptr_array_new_with_free_func(g_free);
        FILE *history_fp = fopen(HISTORY_FILE, "r");
        if (history_fp) {
            char *line = NULL;
            size_t len = 0;
            while (getline(&line, &len, history_fp) != -1) {
            // Remove trailing newline
                line[strcspn(line, "\n")] = '\0';
                g_ptr_array_add(command_history, g_strdup(line));
            }
            free(line);
            fclose(history_fp);
        }

        g_signal_connect(input_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
        g_signal_connect(input_entry, "key-press-event", G_CALLBACK(on_key_press), NULL);
    gtk_main();
    return 0;
}