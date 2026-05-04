//
// Created by mete on 29.04.2026.
//

#include "../include/plugin.h"
#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Static state */
static void   *handles[MAX_PLUGINS];   /* dlopen handles */
static Plugin *plugins[MAX_PLUGINS];   /* plugin structs */
static int     plugin_count = 0;

void plugins_init(const char *plugin_dir) {
    DIR *dir = opendir(plugin_dir);
    if (!dir) return; /* return silently */

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Check if file ends with ".so" */
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 3, ".so") != 0) continue;
        
        if (plugin_count >= MAX_PLUGINS) break;

        /* build full path */
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", plugin_dir, entry->d_name);

        /* load plugin */
        void *handle = dlopen(path, RTLD_LAZY);
        if (!handle) {
            fprintf(stderr, "plugin: failed to load %s: %s\n",
                    entry->d_name, dlerror());
            continue;
        }

        /* look up plugin_register symbol */
        Plugin *(*plugin_register)(void) = dlsym(handle, "plugin_register");
        if (!plugin_register) {
            fprintf(stderr, "plugin: no plugin_register in %s\n",
                    entry->d_name);
            dlclose(handle);
            continue;
        }

        Plugin *p = plugin_register();
        if (!p) {
            dlclose(handle);
            continue;
        }

        handles[plugin_count] = handle;
        plugins[plugin_count] = p;
        plugin_count++;
    }

    closedir(dir);
}

void plugins_unload(void) {
    for (int i = 0; i < plugin_count; i++) {
        if (handles[i]) dlclose(handles[i]);
    }
    plugin_count = 0;
    memset(handles, 0, sizeof(handles));
    memset(plugins, 0, sizeof(plugins));
}

char *hook_prompt_left(void) {
    /* Collect all non-NULL results, concatenate with space */
    char result[1024] = {0};
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i]->on_prompt_left) {
            char *s = plugins[i]->on_prompt_left();
            if (s) {
                if (result[0]) strncat(result, " ", sizeof(result)-strlen(result)-1);
                strncat(result, s, sizeof(result)-strlen(result)-1);
                free(s);
            }
        }
    }
    if (result[0]) return strdup(result);
    return NULL;
}

char *hook_prompt_right(void) {
    /* Same pattern as hook_prompt_left */
    char result[256] = {0};
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i]->on_prompt_right) {
            char *s = plugins[i]->on_prompt_right();
            if (s) {
                if (result[0]) strncat(result, " ", sizeof(result)-strlen(result)-1);
                strncat(result, s, sizeof(result)-strlen(result)-1);
                free(s);
            }
        }
    }
    if (result[0]) return strdup(result);
    return NULL;
}

void hook_pre_exec(const char *cmd) {
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i]->on_pre_exec) {
            plugins[i]->on_pre_exec(cmd);
        }
    }
}

void hook_post_exec(const char *cmd, int exit_status) {
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i]->on_post_exec) {
            plugins[i]->on_post_exec(cmd, exit_status);
        }
    }
}
