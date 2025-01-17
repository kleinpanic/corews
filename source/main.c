#include <gtk/gtk.h>
#include <vte/vte.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>
#include <glib.h>

#define FILE_PATH_COLUMN 0

static GtkListStore *store;
static GtkWidget *tree_view, *window;
static VteTerminal *terminal;
static int inotify_fd;
static char *base_dir;
static char *current_dir;

// Function declarations
static void show_new_directory_dialog();
static void create_new_directory(const char *dir_name);
static void show_deletion_dialog();
static void delete_selected_item();
// static int get_directory_depth(const char *dir);  // Unused function
static gboolean recursive_delete(const char *path);
static void make_file_executable(const char *path);
static void make_file_not_executable(const char *path);
static const char *get_language_from_path(const char *path);
static void compile_and_run(const char *path, const char *language);
static gboolean show_confirmation_dialog(const char *message);
static void create_new_file(const char *file_name);
static void show_new_file_dialog();
static void display_directory(const char *dir);
static void open_file_with_appropriate_application(const char *filepath);
static gboolean on_key_press(GtkWidget *widget __attribute__((unused)), GdkEventKey *event, gpointer userdata __attribute__((unused)));
static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer userdata __attribute__((unused)));
static void on_row_activated(GtkTreeView *treeview, GtkTreePath *path, GtkTreeViewColumn *col __attribute__((unused)), gpointer userdata __attribute__((unused)));
static GtkWidget* create_tree_view();
static void on_window_destroy(GtkWidget *widget __attribute__((unused)), gpointer data __attribute__((unused)));
static char *strip_html_markup(const char *input);
void run_executable(GtkMenuItem *menu_item __attribute__((unused)), gpointer user_data);
void make_file_executable_menu(GtkMenuItem *menu_item __attribute__((unused)), gpointer user_data);
void make_file_not_executable_menu(GtkMenuItem *menu_item __attribute__((unused)), gpointer user_data);

// static void handle_file_open_error(const char *filepath);  // Unused function

static gboolean has_extension(const char *filename, const char *extension) {
    const char *dot = strrchr(filename, '.');
    return dot && !g_strcmp0(dot + 1, extension);
}

static gboolean show_confirmation_dialog(const char *message) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_OK_CANCEL,
                                               "%s", message);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return (response == GTK_RESPONSE_OK);
}

/*
static void handle_file_open_error(const char *filepath) {
    gchar *msg = g_strdup_printf("\nFailed to open '%s'\n", filepath);
    vte_terminal_feed_child(VTE_TERMINAL(terminal), msg, -1);
    g_free(msg);
}
*/

static void open_file_with_appropriate_application(const char *filepath) {
    struct stat path_stat;
    if (stat(filepath, &path_stat) != 0 || S_ISDIR(path_stat.st_mode)) {
        g_print("Attempted to open a directory or invalid path: %s\n", filepath);
        return;
    }

    gchar *command = NULL;

    if (has_extension(filepath, "jpg") || has_extension(filepath, "png")) {
        command = g_strdup_printf("feh \"%s\"", filepath);
    } else if (has_extension(filepath, "pdf")) {
        command = g_strdup_printf("zathura \"%s\"", filepath);
    } else {
        command = g_strdup_printf("nvim \"%s\"", filepath);
    }

    gchar *argv[] = {"/bin/sh", "-c", command, NULL};

    vte_terminal_spawn_async(
        VTE_TERMINAL(terminal),
        VTE_PTY_DEFAULT,
        NULL,
        argv,
        NULL,
        G_SPAWN_DEFAULT,
        NULL, NULL,
        NULL,
        -1,
        NULL, NULL,
        NULL);

    g_free(command);
}

static void display_directory(const char *dir) {
    DIR *d;
    struct dirent *entry;
    GtkTreeIter iter;
    struct stat statbuf;
    gboolean readme_found = FALSE;
    char *readme_path = NULL;

    if ((d = opendir(dir)) == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n", strerror(errno));
        return;
    }

    gtk_list_store_clear(store);

    while ((entry = readdir(d)) != NULL) {
        char *full_path = g_strdup_printf("%s/%s", dir, entry->d_name);
        if (stat(full_path, &statbuf) == -1) {
            g_free(full_path);
            continue;
        }

        gchar *display_name;
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 && strcmp(entry->d_name, "codeWS") != 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                display_name = g_markup_printf_escaped("<span foreground='blue'>%s</span>", entry->d_name);
            } else if (statbuf.st_mode & S_IXUSR) {
                display_name = g_markup_printf_escaped("<span foreground='red'>%s</span>", entry->d_name);
            } else {
                display_name = g_strdup(entry->d_name);
            }

            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, display_name, 1, entry->d_name, -1);
            g_free(display_name);

            if (g_ascii_strcasecmp(entry->d_name, "readme.md") == 0) {
                readme_found = TRUE;
                readme_path = g_strdup_printf("%s/%s", dir, entry->d_name);
            }
        }
        g_free(full_path);
    }
    closedir(d);

    if (readme_found && readme_path) {
        open_file_with_appropriate_application(readme_path);
        g_free(readme_path);
    }
}

static void navigate_up_directory() {
    if (strcmp(current_dir, base_dir) == 0) {
        return;
    }

    char *last_slash = strrchr(current_dir, '/');
    if (last_slash != NULL) {
        *last_slash = '\0';
        if (strcmp(current_dir, base_dir) >= 0) {
            display_directory(current_dir);
        } else {
            strcpy(current_dir, base_dir);
            display_directory(current_dir);
        }
    }
}

static char *strip_html_markup(const char *input) {
    if (input == NULL) return NULL;

    GError *error = NULL;
    GRegex *regex = g_regex_new("<[^>]*>", 0, 0, &error);
    if (error != NULL) {
        g_print("Regex error: %s\n", error->message);
        g_error_free(error);
        return NULL;
    }

    gchar *stripped = g_regex_replace_literal(regex, input, -1, 0, "", 0, NULL);
    g_regex_unref(regex);

    return stripped;
}

gboolean is_executable(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_mode & S_IXUSR;
    }
    return FALSE;
}

static gboolean on_key_press(GtkWidget *widget __attribute__((unused)), GdkEventKey *event, gpointer userdata __attribute__((unused))) {
    if (event->keyval == GDK_KEY_BackSpace && !vte_terminal_get_has_selection(VTE_TERMINAL(terminal))) {
        navigate_up_directory();
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_n) {
        show_new_directory_dialog();
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_d) {
        show_deletion_dialog();
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_r) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
        GtkTreeModel *model;
        GtkTreeIter iter;
        gchar *filename;

        if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
            gtk_tree_model_get(model, &iter, 0, &filename, -1);
            char *clean_filename = strip_html_markup(filename);
            char *path = g_strdup_printf("%s/%s", current_dir, clean_filename);
            const char *language = get_language_from_path(path);
            char *clean_path = strip_html_markup(path);

            if (clean_path) {
                g_print("Cleaned path: %s\n", clean_path);  // Debug print
                compile_and_run(clean_path, language);
                g_free(clean_path);
            } else {
                g_print("Failed to clean path: %s\n", path);  // Debug print
            }

            g_free(clean_filename);
            g_free(filename);
            g_free(path);
        }
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_e) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
        GtkTreeModel *model;
        GtkTreeIter iter;
        gchar *filename;

        if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
            gtk_tree_model_get(model, &iter, 0, &filename, -1);
            char *clean_filename = strip_html_markup(filename);
            char *path = g_strdup_printf("%s/%s", current_dir, clean_filename);
            make_file_executable(path);
            display_directory(current_dir);
            g_free(clean_filename);
            g_free(filename);
            g_free(path);
        }
        return TRUE;
    }
    if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_f) {
        show_new_file_dialog();
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer userdata __attribute__((unused))) {
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        GtkTreePath *path;
        GtkTreeView *tree_view = GTK_TREE_VIEW(widget);

        if (gtk_tree_view_get_path_at_pos(tree_view, (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL)) {
            GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gchar *file_name;
                gtk_tree_model_get(model, &iter, 1, &file_name, -1);
                
                // Construct the full path
                gchar *file_path = g_strdup_printf("%s/%s", current_dir, file_name);

                struct stat path_stat;
                if (stat(file_path, &path_stat) == 0 && !S_ISDIR(path_stat.st_mode)) {  // Check if not a directory
                    GtkWidget *menu = gtk_menu_new();

                    // Create and add "Make File Executable" item
                    GtkWidget *make_exec_item = gtk_menu_item_new_with_label("Make File Executable");
                    g_signal_connect(make_exec_item, "activate", G_CALLBACK(make_file_executable_menu), g_strdup(file_path));
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), make_exec_item);

                    // Create and add "Make File Not Executable" item
                    GtkWidget *make_not_exec_item = gtk_menu_item_new_with_label("Make File Not Executable");
                    g_signal_connect(make_not_exec_item, "activate", G_CALLBACK(make_file_not_executable_menu), g_strdup(file_path));
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), make_not_exec_item);

                    // Create and add "Run" item if the file is executable
                    if (is_executable(file_path)) {
                        GtkWidget *run_item = gtk_menu_item_new_with_label("Run");
                        g_signal_connect(run_item, "activate", G_CALLBACK(run_executable), g_strdup(file_path));
                        gtk_menu_shell_append(GTK_MENU_SHELL(menu), run_item);
                    }

                    gtk_widget_show_all(menu);
                    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
                } else {
                    g_print("Right-clicked on a directory or invalid path: %s\n", file_path);
                }

                g_free(file_path);
                g_free(file_name);
            }
            gtk_tree_path_free(path);
        }
        return TRUE;
    }
    return FALSE;
}

void run_executable(GtkMenuItem *menu_item __attribute__((unused)), gpointer user_data) {
    const char *file_path = (const char *)user_data;
    gchar *clean_path = g_strdup(file_path); // Duplicate the file path
    gchar *command = g_strdup_printf("clear && '%s'", clean_path); // Use single quotes to handle spaces in paths
    gchar *argv[] = {"bash", "-c", command, NULL};

    g_print("Running executable: %s\n", clean_path); // Debug print
    g_print("Command: %s\n", command); // Print the command for debugging

    vte_terminal_spawn_async(
        VTE_TERMINAL(terminal),
        VTE_PTY_DEFAULT,
        current_dir,  // Set the working directory
        argv,
        NULL,
        G_SPAWN_DEFAULT,
        NULL, NULL, NULL, -1,
        NULL, NULL, NULL);

    g_free(command);
    g_free(clean_path);
    g_free((char *)file_path);  // Free the strdup-ed file path
}

void make_file_executable_menu(GtkMenuItem *menu_item __attribute__((unused)), gpointer user_data) {
    const char *file_path = (const char *)user_data;
    make_file_executable(file_path);
    display_directory(current_dir);  // Update the directory view immediately
    g_free((char *)file_path);  // Free the strdup-ed file path
}

void make_file_not_executable_menu(GtkMenuItem *menu_item __attribute__((unused)), gpointer user_data) {
    const char *file_path = (const char *)user_data;
    make_file_not_executable(file_path);
    display_directory(current_dir);  // Update the directory view immediately
    g_free((char *)file_path);  // Free the strdup-ed file path
}

static void on_row_activated(GtkTreeView *treeview, GtkTreePath *path, GtkTreeViewColumn *col __attribute__((unused)), gpointer userdata __attribute__((unused))) {
    GtkTreeModel *model = gtk_tree_view_get_model(treeview);
    GtkTreeIter iter;
    gchar *actual_name;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gtk_tree_model_get(model, &iter, 1, &actual_name, -1);

        char *new_path = g_strdup_printf("%s/%s", current_dir, actual_name);
        struct stat path_stat;
        if (stat(new_path, &path_stat) == 0) {
            if (S_ISDIR(path_stat.st_mode)) {
                g_free(current_dir);
                current_dir = new_path;
                display_directory(current_dir);
            } else {
                open_file_with_appropriate_application(new_path);
                g_free(new_path);
            }
        } else {
            g_printerr("Failed to access %s: %s\n", new_path, strerror(errno));
            g_free(new_path);
        }
        g_free(actual_name);
    }
}

static GtkWidget* create_tree_view() {
    tree_view = gtk_tree_view_new();
    store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_view), GTK_TREE_MODEL(store));

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Entries", renderer, "markup", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

    g_signal_connect(tree_view, "row-activated", G_CALLBACK(on_row_activated), NULL);
    g_signal_connect(tree_view, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(tree_view, "button-press-event", G_CALLBACK(on_button_press), NULL);

    return tree_view;
}

static void on_window_destroy(GtkWidget *widget __attribute__((unused)), gpointer data __attribute__((unused))) {
    close(inotify_fd);
    g_free(base_dir);
    g_free(current_dir);
    gtk_main_quit();
}

/*
static int get_directory_depth(const char *dir) {
    const char *base = base_dir;
    const char *tmp = dir;

    while (*base != '\0' && *tmp == *base) {
        base++;
        tmp++;
    }

    int depth = 0;
    while (*tmp != '\0') {
        if (*tmp == '/') {
            depth++;
        }
        tmp++;
    }

    return depth;
}
*/

static char *extract_first_directory(const char *path) {
    if (!path) return NULL;
    path += strlen(base_dir);
    if (*path == '/') path++;

    const char *end = strchr(path, '/');
    if (!end) end = path + strlen(path);

    size_t len = end - path;
    char *dir = malloc(len + 1);
    if (dir) {
        strncpy(dir, path, len);
        dir[len] = '\0';
    }
    return dir;
}

static const char *get_language_from_path(const char *path) {
    if (!path) {
        printf("Error: Path is NULL\n");
        return NULL;
    }

    char *language_dir = extract_first_directory(path);
    if (!language_dir) {
        printf("No language directory found or error in path processing.\n");
        return NULL;
    }

    const char *language = NULL;
    if (strcmp(language_dir, "C") == 0) {
        language = "C";
    } else if (strcmp(language_dir, "Python3") == 0) {
        language = "Python3";
    } else if (strcmp(language_dir, "Asm") == 0) {
        language = "Assembly";
    }

    if (language) {
        printf("Detected language: %s\n", language);
    } else {
        printf("No recognized language found in the directory name.\n");
    }

    free(language_dir);
    return language;
}

static void compile_and_run(const char *path, const char *language) {
    if (path == NULL) {
        g_print("Error: Path is NULL\n");
        return;
    }

    char *clean_path = strip_html_markup(path);
    if (clean_path == NULL) {
        g_print("Failed to clean path from HTML markup.\n");
        return;
    }

    gchar *compile_cmd = NULL;
    gchar *cmd = g_strdup_printf("Are you sure you want to compile and run this %s program?", language);
    if (show_confirmation_dialog(cmd)) {
        gchar *dir_path = g_path_get_dirname(clean_path);
        if (dir_path == NULL) {
            g_print("Error: Unable to determine directory from path: %s\n", clean_path);
            g_free(cmd);
            g_free(clean_path);
            return;
        }

        if (strcasecmp(language, "C") == 0) {
            compile_cmd = g_strdup_printf("clear && cd '%s' && make clean && make", dir_path);
        } else if (strcasecmp(language, "Python3") == 0) {
            compile_cmd = g_strdup_printf("clear && cd %s && python3 %s\n", dir_path, clean_path);
        } else if (strcasecmp(language, "Assembly") == 0) {
            compile_cmd = g_strdup_printf("clear && cd %s && nasm -f elf64 %s -o %s.o && ld %s.o -o %s && ./%s\n", dir_path, clean_path, clean_path, clean_path, clean_path, clean_path);
        }

        g_free(dir_path);

        if (compile_cmd) {
            g_print("Compile command: %s\n", compile_cmd);
            vte_terminal_spawn_async(
                VTE_TERMINAL(terminal),
                VTE_PTY_DEFAULT,
                NULL,
                (char *[]){ "bash", "-c", compile_cmd, NULL },
                NULL,
                G_SPAWN_DEFAULT,
                NULL, NULL, NULL, -1,
                NULL, NULL, NULL);
            g_free(compile_cmd);
        } else {
            g_print("Unsupported language: %s\n", language);
        }
    }
    g_free(cmd);
    g_free(clean_path);
}

static void make_file_executable(const char *path) {
    if (path == NULL) return;
    gchar *command = g_strdup_printf("chmod +x \"%s\"", path);
    gchar *argv[] = {"bash", "-c", command, NULL};
    VteTerminal *vte_terminal = VTE_TERMINAL(terminal);

    vte_terminal_spawn_async(
        vte_terminal,
        VTE_PTY_DEFAULT,
        current_dir,
        argv,
        NULL,
        G_SPAWN_DEFAULT,
        NULL, NULL, NULL, -1,
        NULL, NULL, NULL);

    g_free(command);
}

static void make_file_not_executable(const char *path) {
    if (path == NULL) return;
    gchar *command = g_strdup_printf("chmod -x \"%s\"", path);
    gchar *argv[] = {"bash", "-c", command, NULL};
    VteTerminal *vte_terminal = VTE_TERMINAL(terminal);

    vte_terminal_spawn_async(
        vte_terminal,
        VTE_PTY_DEFAULT,
        current_dir,
        argv,
        NULL,
        G_SPAWN_DEFAULT,
        NULL, NULL, NULL, -1,
        NULL, NULL, NULL);

    g_free(command);
}

static void create_new_file(const char *file_name) {
    if (file_name == NULL) return;

    gchar *full_path = g_strdup_printf("%s/%s", current_dir, file_name);
    FILE *file = fopen(full_path, "w");
    if (file) {
        fclose(file);
    } else {
        g_print("Error creating file: %s\n", strerror(errno));
    }
    g_free(full_path);
}

static void show_new_file_dialog() {
    GtkWidget *dialog, *content_area, *entry;
    dialog = gtk_dialog_new_with_buttons("New File", GTK_WINDOW(window),
                                         GTK_DIALOG_MODAL,
                                         "_OK", GTK_RESPONSE_OK,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         NULL);
    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(content_area), entry);
    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        const char *file_name = gtk_entry_get_text(GTK_ENTRY(entry));
        create_new_file(file_name);
        display_directory(current_dir);
    }
    gtk_widget_destroy(dialog);
}

static void show_new_directory_dialog() {
    GtkWidget *dialog, *content_area, *entry;
    dialog = gtk_dialog_new_with_buttons("New Directory", GTK_WINDOW(window),
                                         GTK_DIALOG_MODAL,
                                         "_OK", GTK_RESPONSE_OK,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         NULL);
    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(content_area), entry);
    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        const char *dir_name = gtk_entry_get_text(GTK_ENTRY(entry));
        create_new_directory(dir_name);
        display_directory(current_dir);
    }
    gtk_widget_destroy(dialog);
}

static void create_new_directory(const char *dir_name) {
    if (dir_name == NULL) return;

    gchar *full_path = g_strdup_printf("%s/%s", current_dir, dir_name);
    if (mkdir(full_path, 0755) == -1) {
        g_print("Error creating directory: %s\n", strerror(errno));
    } else {
        display_directory(current_dir);
    }
    g_free(full_path);
}

/*
static int get_directory_depth(const char *dir) {
    const char *base = base_dir;
    const char *tmp = dir;

    while (*base != '\0' && *tmp == *base) {
        base++;
        tmp++;
    }

    int depth = 0;
    while (*tmp != '\0') {
        if (*tmp == '/') {
            depth++;
        }
        tmp++;
    }

    return depth;
}
*/

static gboolean recursive_delete(const char *path) {
    DIR *d = opendir(path);
    if (d == NULL) {
        g_printerr("Failed to open directory for deletion: %s\n", strerror(errno));
        return FALSE;
    }

    struct dirent *entry;
    gboolean result = TRUE;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || strcmp(entry->d_name, "codeWS") == 0) {
            continue;
        }

        char *full_path = g_strdup_printf("%s/%s", path, entry->d_name);
        if (entry->d_type == DT_DIR) {
            if (!recursive_delete(full_path)) {
                result = FALSE;
                g_free(full_path);
                break;
            }
        } else {
            if (remove(full_path) != 0) {
                g_printerr("Failed to delete file: %s\n", strerror(errno));
                result = FALSE;
                g_free(full_path);
                break;
            }
        }
        g_free(full_path);
    }

    closedir(d);

    if (result) {
        if (rmdir(path) != 0) {
            g_printerr("Failed to delete directory: %s\n", strerror(errno));
            result = FALSE;
        }
    }

    return result;
}

static void show_deletion_dialog() {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_WARNING,
                                               GTK_BUTTONS_OK_CANCEL,
                                               "Are you sure you want to delete the selected item?");
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_OK) {
        delete_selected_item();
    }
}

static void delete_selected_item() {
    GtkTreeModel *model;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    GtkTreeIter iter;
    gchar *actual_name;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_tree_model_get(model, &iter, 1, &actual_name, -1);
        char *full_path = g_strdup_printf("%s/%s", current_dir, actual_name);

        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            if (!recursive_delete(full_path)) {
                g_printerr("Failed to delete directory: %s\n", strerror(errno));
            }
        } else {
            if (remove(full_path) != 0) {
                g_printerr("Failed to delete file: %s\n", strerror(errno));
            }
        }

        g_free(full_path);
        g_free(actual_name);
        display_directory(current_dir);
    }
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    char *home_dir = getenv("HOME");
    if (home_dir == NULL) {
        fprintf(stderr, "Environment variable HOME is not set.\n");
        return EXIT_FAILURE;
    }

    base_dir = g_strdup_printf("%s/codeWS", home_dir);
    current_dir = strdup(base_dir);

    inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        perror("inotify_init");
        return EXIT_FAILURE;
    }

    inotify_add_watch(inotify_fd, current_dir, IN_CREATE | IN_DELETE | IN_MODIFY);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    gtk_window_set_title(GTK_WINDOW(window), "Code Workshop");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 600);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_add(GTK_CONTAINER(window), hbox);

    GtkWidget *view = create_tree_view();
    gtk_box_pack_start(GTK_BOX(hbox), view, FALSE, FALSE, 5);

    terminal = VTE_TERMINAL(vte_terminal_new());
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(terminal), TRUE, TRUE, 5);

    display_directory(current_dir);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
