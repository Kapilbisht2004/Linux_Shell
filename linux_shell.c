#include <gtk/gtk.h>
#include <glib.h>      // For GLib utility functions and data structures
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <errno.h>
#include <pwd.h>       // For user information functions (getpwuid)
#include <sys/types.h> // For type definitions (uid_t)
#include <dirent.h>    // For directory traversal functions

/* 
 * CONSTANTS
 */
#define HISTORY_FILE "cmd_history.txt"  // File where command history is stored
#define MAX_HISTORY 1000                // Maximum number of commands to store in history
#define READ_BUF_SIZE 4096              // Buffer size for reading command output
#define MAX_ARGS 64                     // Maximum number of arguments for a command

/*
 * GLOBAL VARIABLES
 */
// Command history management
GPtrArray *command_history = NULL;      // Stores command history using GLib's pointer array
int history_index = -1;                 // Current position in command history when navigating

// GTK UI components
GtkTextView *output_view;               // Text view widget for displaying command output
GtkTextBuffer *output_buffer;           // Buffer for the output view
GtkEntry *input_entry;                  // Text entry widget for command input
GtkWindow *main_window;                 // Main application window

/*
 * DATA STRUCTURES
 */
// Structure to hold information about input/output redirection
typedef struct {
    char *input_file;                   // Input redirection file (< file)
    char *output_file;                  // Output redirection file (> file or >> file)
    gboolean append_output;             // TRUE if output should be appended (>>)
} RedirectionInfo;

/*
 * FUNCTION PROTOTYPES
 */
// UI functions
void append_output(const char *text);
void update_prompt_and_title(void);
gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
void on_entry_activate(GtkEntry *entry, gpointer user_data);

// Command processing functions
int parse_command(char *command_line, char *args[], RedirectionInfo *redir);
void cleanup_args(int argc, char *args[], RedirectionInfo *redir);
gboolean handle_builtin(int argc, char *args[]);
void execute_external_command(int argc, char *args[], RedirectionInfo *redir);

// Built-in commands
gboolean reverse(int argc, char *args[]);
gboolean countdown(int argc, char *args[]);

// Helper functions
char *complete_path(const char *path_prefix);

/*
 * PATH AUTOCOMPLETION
 * Attempts to autocomplete a path based on a prefix.
 */
char *complete_path(const char *path_prefix) {
    // Find the last slash to separate directory from partial filename
    char *last_slash = strrchr(path_prefix, '/');
    char dir_path[PATH_MAX];
    const char *partial;

    // Determine directory path and partial filename
    if (last_slash) {
        // Extract directory part of the path
        strncpy(dir_path, path_prefix, last_slash - path_prefix);
        dir_path[last_slash - path_prefix] = '\0';
        partial = last_slash + 1;  // Partial filename after slash
    } else {
        // No directory specified, use current directory
        strcpy(dir_path, ".");
        partial = path_prefix;     // Entire input is the partial filename
    }

    // Open the directory
    DIR *dir = opendir(*dir_path ? dir_path : ".");
    if (!dir) return NULL;

    struct dirent *entry;
    char *match = NULL;
    size_t partial_len = strlen(partial);
    
    // Look for matching entries
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, partial, partial_len) == 0) {
            if (!match) {
                // First match found
                match = g_strdup(entry->d_name);
            } else {
                // More than one match, we can't determine which one to use
                g_free(match);
                closedir(dir);
                return NULL;
            }
        }
    }
    closedir(dir);

    // If we found exactly one match, construct the completed path
    if (match) {
        char *completed_path;
        if (last_slash) {
            // Combine the directory part with the matched filename
            size_t prefix_len = last_slash - path_prefix + 1;
            completed_path = g_malloc(prefix_len + strlen(match) + 1);
            strncpy(completed_path, path_prefix, prefix_len);
            completed_path[prefix_len] = '\0';
            strcat(completed_path, match);
        } else {
            // No directory part, just return the matched filename
            completed_path = g_strdup(match);
        }
        g_free(match);
        return completed_path;
    }
    return NULL;
}

/*
 * KEY PRESS EVENT HANDLER
 * Handles special key presses for command history navigation and tab completion.
 *
 * @param widget The widget that received the key press
 * @param event The key event data
 * @param user_data User data passed to the callback (unused)
 * @return TRUE if the event was handled, FALSE otherwise
 */
gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    // Ignore if history is empty
    if (!command_history || command_history->len == 0)
        return FALSE;

    // Handle Up Arrow - Navigate backward in history
    if (event->keyval == GDK_KEY_Up) {
        if (history_index > 0)
            history_index--;
        const char *cmd = g_ptr_array_index(command_history, history_index);
        gtk_entry_set_text(GTK_ENTRY(widget), cmd);
        gtk_editable_set_position(GTK_EDITABLE(widget), -1);  // Move cursor to end
        return TRUE;
    } 
    // Handle Down Arrow - Navigate forward in history
    else if (event->keyval == GDK_KEY_Down) {
        if (history_index < (int)command_history->len - 1) {
            history_index++;
            const char *cmd = g_ptr_array_index(command_history, history_index);
            gtk_entry_set_text(GTK_ENTRY(widget), cmd);
        } else {
            // At the end of history, clear the entry
            gtk_entry_set_text(GTK_ENTRY(widget), "");
            history_index = command_history->len;
        }
        gtk_editable_set_position(GTK_EDITABLE(widget), -1);  // Move cursor to end
        return TRUE;
    } 
    // Handle Tab - Autocomplete paths
    else if (event->keyval == GDK_KEY_Tab) {
        const char *current_text = gtk_entry_get_text(GTK_ENTRY(widget));
        if (!current_text || strlen(current_text) == 0) return TRUE;

        // Get cursor position
        int cursor_pos = gtk_editable_get_position(GTK_EDITABLE(widget));
        if (cursor_pos <= 0) return TRUE;

        // Get text up to cursor position
        char *before_cursor = g_strndup(current_text, cursor_pos);

        // Find last space to extract the word to complete
        char *last_space = strrchr(before_cursor, ' ');
        const char *word_to_complete = last_space ? last_space + 1 : before_cursor;

        char *completed = complete_path(word_to_complete);
        if (completed) {
            // Step 1: Find start of the word to complete
            const char *start_of_word = current_text + strlen(current_text) - strlen(word_to_complete);

            // Step 2: Create a new string with everything before the word to complete
            GString *new_text = g_string_new_len(current_text, start_of_word - current_text);

            // Step 3: Append the completed path
            g_string_append(new_text, completed);

            // Step 4: Update the entry text
            gtk_entry_set_text(GTK_ENTRY(widget), new_text->str);
            gtk_editable_set_position(GTK_EDITABLE(widget), -1);

            // Cleanup
            g_string_free(new_text, TRUE);
            g_free(completed);
        }



        g_free(before_cursor);
        return TRUE; // Handled Tab key
    }

    return FALSE;  // Event not handled
}

/*
 * APPEND TEXT TO OUTPUT
 * Adds text to the output view and scrolls to show the latest content.
 *
 * @param text The text to append
 */
void append_output(const char *text) {
    if (!text) return;
    
    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(output_buffer, &end_iter);
    gtk_text_buffer_insert(output_buffer, &end_iter, text, -1);
    
    // Scroll to show the newly added text
    GtkTextMark *end_mark = gtk_text_buffer_get_insert(output_buffer);
    gtk_text_view_scroll_mark_onscreen(output_view, end_mark);
}

/*
 * UPDATE PROMPT AND WINDOW TITLE
 * Updates the shell prompt with username, hostname and current directory,
 * and updates the window title with the current directory.
 */
void update_prompt_and_title(void) {
    char cwd_buf[PATH_MAX];
    char hostname[HOST_NAME_MAX + 1] = "?";  // Default if gethostname fails
    char username[LOGIN_NAME_MAX + 1] = "?"; // Default if getpwuid fails
    // Allocate enough space: user + @ + host + : + cwd + $ + space + null
    char prompt[LOGIN_NAME_MAX + HOST_NAME_MAX + PATH_MAX + 5];
    char title[PATH_MAX + 20];
    char *cwd_ptr = "?"; // Default if getcwd fails

    // Get username
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) {
        strncpy(username, pw->pw_name, sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0'; // Ensure null termination
    } else {
         // Use a simple default if getpwuid fails
         strcpy(username, "user");
    }

    // Get hostname
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostname[sizeof(hostname) - 1] = '\0'; // Ensure null termination
    } else {
         // Use a simple default if gethostname fails
         strcpy(hostname, "host");
    }

    // Get current directory
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

    // Format prompt
    snprintf(prompt, sizeof(prompt), "%s@%s : %s$ ", username, hostname, cwd_ptr);

    // Update UI
    append_output("\n"); // Add newline before prompt
    append_output(prompt);
    gtk_window_set_title(main_window, title);
}

/*
 * PARSE COMMAND LINE
 * Parses a command line into arguments and redirection specifications.
 *
 * @param command_line The command line to parse
 * @param args Array to store the parsed arguments
 * @param redir Structure to store redirection information
 * @return The number of arguments parsed
 */
int parse_command(char *command_line, char *args[], RedirectionInfo *redir) {
    int argc = 0;
    char *token;
    char *saveptr;

    // Initialize redirection info
    redir->input_file = NULL;
    redir->output_file = NULL;
    redir->append_output = FALSE;

    // Tokenize the command line
    token = strtok_r(command_line, " \t\n\r", &saveptr);
    while (token != NULL && argc < MAX_ARGS - 1) {
        if (strcmp(token, "<") == 0) {
            // Input redirection
            token = strtok_r(NULL, " \t\n\r", &saveptr);
            if (token != NULL) redir->input_file = g_strdup(token);
        } else if (strcmp(token, ">") == 0) {
            // Output redirection (overwrite)
            token = strtok_r(NULL, " \t\n\r", &saveptr);
            if (token != NULL) {
                redir->output_file = g_strdup(token);
                redir->append_output = FALSE;
            }
        } else if (strcmp(token, ">>") == 0) {
            // Output redirection (append)
            token = strtok_r(NULL, " \t\n\r", &saveptr);
            if (token != NULL) {
                redir->output_file = g_strdup(token);
                redir->append_output = TRUE;
            }
        } else {
            // Regular argument
            args[argc] = g_strdup(token);
            if (args[argc] == NULL) g_error("g_strdup failed: Out of memory");
            argc++;
        }
        token = strtok_r(NULL, " \t\n\r", &saveptr);
    }
    
    // Null-terminate the argument array
    args[argc] = NULL;
    return argc;
}

/*
 * CLEANUP ARGUMENTS AND REDIRECTION INFO
 * Frees memory allocated by parse_command.
 *
 * @param argc Number of arguments
 * @param args Array of argument strings to free
 * @param redir Redirection info structure to cleanup
 */
void cleanup_args(int argc, char *args[], RedirectionInfo *redir) {
    if (!args) return;
    
    // Free each argument
    for (int i = 0; i < argc; i++) {
        g_free(args[i]);
        args[i] = NULL;
    }
    
    // Free redirection paths
    if (redir) {
        g_free(redir->input_file);
        g_free(redir->output_file);
        redir->input_file = NULL;
        redir->output_file = NULL;
        redir->append_output = FALSE;
    }
}

/*
 * REVERSE STRING (BUILT-IN COMMAND)
 * Reverses the characters in a string.
 *
 * @param argc Number of arguments
 * @param args Array of arguments
 * @return TRUE indicating command was handled
 */
gboolean reverse(int argc, char *args[]) {
    if (argc < 2) {
        append_output("Usage: reverse <string>\n");
        return TRUE;
    }

    char *input = args[1];
    int length = strlen(input);
    char reversed[length + 1];

    // Reverse the string character by character
    for (int i = 0; i < length; i++) {
        reversed[i] = input[length - i - 1];
    }
    reversed[length] = '\0'; // Null terminate the string

    append_output("Reversed: ");
    append_output(reversed);
    append_output("\n");

    return TRUE;
}

/*
 * COUNTDOWN TIMER (BUILT-IN COMMAND)
 * Counts down from a specified number of seconds.
 *
 * @param argc Number of arguments
 * @param args Array of arguments
 * @return TRUE indicating command was handled
 */
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
        
        // Allow GTK to update the UI during countdown
        while (gtk_events_pending()) {
            gtk_main_iteration();
        }

        // Wait 1 second between updates
        sleep(1);
    }

    append_output("Countdown complete!\n");
    return TRUE;
}

/*
 * HANDLE BUILT-IN COMMANDS
 * Checks if a command is built-in and executes it if so.
 *
 * @param argc Number of arguments
 * @param args Array of arguments
 * @return TRUE if command was a built-in and handled, FALSE otherwise
 */
gboolean handle_builtin(int argc, char *args[]) {
    if (argc <= 0 || args == NULL || args[0] == NULL) {
        return FALSE;
    }
    
    // exit - Terminate the shell
    if (strcmp(args[0], "exit") == 0) {
        gtk_main_quit();
        return TRUE;
    }

    // clear - Clear the output view
    if (strcmp(args[0], "clear") == 0) {
        gtk_text_buffer_set_text(output_buffer, "", 0);
        return TRUE;
    }

    // countdown - Run the countdown timer
    if (strcmp(args[0], "countdown") == 0) {
        countdown(argc, args);
        return TRUE;
    }
    
    // reverse - Reverse a string
    if (strcmp(args[0], "reverse") == 0) {
        reverse(argc, args);
        return TRUE;
    }
    
    // cd - Change directory
    if (strcmp(args[0], "cd") == 0) {
        const char *target_dir = NULL;
        
        // If no directory specified, go to home directory
        if (argc > 1) {
            target_dir = args[1];
        } else {
            target_dir = getenv("HOME");
            if (!target_dir) {
                target_dir = "/";  // Fallback to root if HOME not set
            }
        }

        // Try to change directory
        if (chdir(target_dir) != 0) {
            char error_msg[PATH_MAX + 64];
            snprintf(error_msg, sizeof(error_msg), "cd: %s: %s\n", target_dir, strerror(errno));
            append_output(error_msg);
        }
        return TRUE;
    }
    
    // Not a built-in command
    return FALSE;
}

/*
 * EXECUTE EXTERNAL COMMAND
 * Forks a child process to execute an external command.
 *
 * @param argc Number of arguments
 * @param args Array of arguments
 * @param redir Redirection information
 */
void execute_external_command(int argc, char *args[], RedirectionInfo *redir) {
    if (argc <= 0 || args == NULL || args[0] == NULL) return;

    int pipe_fd[2];  // Pipe for reading command output
    pid_t pid;

    // Create pipe for communication between parent and child
    if (pipe(pipe_fd) == -1) {
        perror("pipe failed");
        append_output("Error: Failed to create pipe.\n");
        return;
    }

    // Fork a child process
    pid = fork();
    if (pid == -1) {
        perror("fork failed");
        append_output("Error: Failed to fork process.\n");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return;
    }

    if (pid == 0) { // Child process
        close(pipe_fd[0]);  // Close unused read end

        // Handle input redirection
        if (redir->input_file) {
            FILE *in = fopen(redir->input_file, "r");
            if (!in) {
                perror("input redirection failed");
                _exit(EXIT_FAILURE);
            }
            dup2(fileno(in), STDIN_FILENO);  // Redirect stdin to file
            fclose(in);
        }

        // Handle output redirection
        if (redir->output_file) {
            FILE *out = fopen(redir->output_file, redir->append_output ? "a" : "w");
            if (!out) {
                perror("output redirection failed");
                _exit(EXIT_FAILURE);
            }
            dup2(fileno(out), STDOUT_FILENO);  // Redirect stdout to file
            dup2(fileno(out), STDERR_FILENO);  // Redirect stderr to file
            fclose(out);
        } else {
            // If no output redirection, redirect stdout and stderr to pipe
            dup2(pipe_fd[1], STDOUT_FILENO);
            dup2(pipe_fd[1], STDERR_FILENO);
        }

        close(pipe_fd[1]);  // Close pipe after dup
        
        // Execute the command
        execvp(args[0], args);
        
        // If execvp returns, it failed
        perror(args[0]);
        _exit(EXIT_FAILURE);
    } else { // Parent process
        close(pipe_fd[1]);  // Close unused write end
        
        // Read output from the child process
        char buffer[READ_BUF_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';  // Null-terminate the string
            append_output(buffer);
            
            // Process GTK events to update UI
            while (gtk_events_pending()) gtk_main_iteration();
        }
        
        if (bytes_read == -1) {
            perror("read failed");
            append_output("\nError reading command output.\n");
        }
        close(pipe_fd[0]);

        // Wait for child process to finish and check its exit status
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            // Command exited with non-zero status
            char msg[64];
            snprintf(msg, sizeof(msg), "\nProcess exited with status %d\n", WEXITSTATUS(status));
            append_output(msg);
        } else if (WIFSIGNALED(status)) {
            // Command was terminated by a signal
            char msg[64];
            snprintf(msg, sizeof(msg), "\nProcess terminated by signal %d\n", WTERMSIG(status));
            append_output(msg);
        }
    }
}

/*
 * HANDLE COMMAND ENTRY
 * Processes a command when the user presses Enter in the input field.
 *
 * @param entry The input entry widget
 * @param user_data User data passed to the callback (unused)
 */
void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;  // Unused parameter

    // Get command text from entry
    const char *command_const = gtk_entry_get_text(entry);
    if (!command_const) return;

    // Duplicate input safely
    char *command_line = g_strdup(command_const);
    if (!command_line) {
        g_error("g_strdup failed: Out of memory");
        return;
    }

    // Trim whitespace from command
    char *start = command_line;
    while (g_ascii_isspace(*start)) start++;
    
    // If there's non-whitespace content
    if (*start != '\0') {
        char *end = start + strlen(start) - 1;
        while (end > start && g_ascii_isspace(*end)) end--;
        *(end + 1) = '\0';
    }

    gboolean show_prompt = TRUE;

    // Process non-empty command
    if (strlen(start) > 0) {
        // Echo command to output
        append_output("\n");
        append_output(start);
        append_output("\n");

        // Parse and execute the command
        char *args[MAX_ARGS];
        RedirectionInfo redir = {0};  // Initialize redirection info

        int argc = parse_command(start, args, &redir);
        if (argc >= 0) {
            gboolean was_clear = (argc > 0 && strcmp(args[0], "clear") == 0);

            if (!handle_builtin(argc, args)) {
                execute_external_command(argc, args, &redir);
            }

            // Don't show prompt after 'clear' as it would be redundant
            if (was_clear) {
                show_prompt = FALSE;
            }

            // Free allocated memory
            cleanup_args(argc, args, &redir);
        }

        // Add to command history
        g_ptr_array_add(command_history, g_strdup(start));
        history_index = command_history->len;
    
        // Append to history file
        FILE *history_fp = fopen(HISTORY_FILE, "a");
        if (history_fp) {
            fprintf(history_fp, "%s\n", start);
            fclose(history_fp);
        }
    } else {
        // Empty command, just add a newline
        append_output("\n");
    }
    
    // Free memory and clear entry
    g_free(command_line);
    gtk_entry_set_text(entry, "");

    // Show new prompt if needed
    if (show_prompt) {
        update_prompt_and_title();
    }
}

/*
 * MAIN FUNCTION
 * Program entry point - initializes GTK and creates the shell UI.
 */
int main(int argc, char *argv[]) {
    // Initialize GTK
    gtk_init(&argc, &argv);
    
    // Create main window
    main_window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    gtk_window_set_title(main_window, "Linux C Shell");
    gtk_window_set_default_size(main_window, 800, 600);
    g_signal_connect(main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    
    // Create vertical layout box
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);
    
    // Create scrolled window for output
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), 
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);
    
    // Create text view for output
    output_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(output_view, FALSE);
    gtk_text_view_set_cursor_visible(output_view, FALSE);
    gtk_text_view_set_wrap_mode(output_view, GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled_window), GTK_WIDGET(output_view));
    output_buffer = gtk_text_view_get_buffer(output_view);
    
    // Create entry field for command input
    input_entry = GTK_ENTRY(gtk_entry_new());
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(input_entry), FALSE, FALSE, 0);
    
    // Initialize command history
    command_history = g_ptr_array_new_with_free_func(g_free);
    
    // Load command history from file
    FILE *history_fp = fopen(HISTORY_FILE, "r");
    if (history_fp) {
        char *line = NULL;
        size_t len = 0;
        while (getline(&line, &len, history_fp) != -1) {
            // Remove trailing newline
            line[strcspn(line, "\n")] = '\0';
            g_ptr_array_add(command_history, g_strdup(line));
        }
        free(line);  // Free the buffer allocated by getline
        fclose(history_fp);
    }
    
    // Set history index to end of history
    history_index = command_history ? command_history->len : 0;
    
    // Connect signals
    g_signal_connect(input_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    g_signal_connect(input_entry, "key-press-event", G_CALLBACK(on_key_press), NULL);
    
    // Show welcome message
    append_output("Welcome to Linux C Shell!\n");
    update_prompt_and_title();
    
    // Show UI and focus on input entry
    gtk_widget_show_all(GTK_WIDGET(main_window));
    gtk_widget_grab_focus(GTK_WIDGET(input_entry));
    
    // Start GTK main loop
    gtk_main();
    
    // Clean up command history before exit
    if (command_history) {
        g_ptr_array_free(command_history, TRUE);
    }
    
    return 0;
}