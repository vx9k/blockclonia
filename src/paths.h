/* Where the game keeps its files, following the XDG Base Directory
 * Specification: settings in $XDG_CONFIG_HOME/blockclonia, worlds in
 * $XDG_DATA_HOME/blockclonia/worlds and the Vulkan pipeline cache in
 * $XDG_CACHE_HOME/blockclonia, each defaulting to its place under $HOME.
 * It only builds paths and creates directories; save.c and settings.c
 * read and write the files. */
#ifndef MC_PATHS_H
#define MC_PATHS_H

#include <stddef.h>

/* Every path built here is shorter than this, the limit --world has, so
 * the column file names under a world still fit save.c's buffers. */
#define PATHS_MAX 200

typedef struct {
    char config[PATHS_MAX];    /* settings file */
    char world[PATHS_MAX];     /* the world played unless --world names another */
    char pipelines[PATHS_MAX]; /* Vulkan pipeline cache */
    /* 1 where the path is the XDG one; 0 where it fell back to the name in
     * the working directory that older builds used (no usable $HOME). */
    int xdg_config, xdg_world, xdg_pipelines;
} paths;

/* "<base>/blockclonia" in out: base is $<var> when it holds an absolute
 * path (the spec says to ignore empty and relative values), else
 * $HOME/<home_rel>. Returns 0, or -1 when neither is usable or the result
 * does not fit in cap. */
int paths_app_dir(const char *var, const char *home_rel, char *out, size_t cap);

/* Resolves all three from the environment. A path that cannot be resolved
 * falls back to its old name in the working directory. */
void paths_resolve(paths *p);

/* Creates dir and any missing parents with mode 0700, as the spec asks;
 * directories that exist keep their permissions. Returns 0, or -1 if a
 * component is not a directory or cannot be created. */
int paths_make_dirs(const char *dir);

/* The directory part of a file path. Returns 0, or -1 if there is none or
 * it does not fit in cap. */
int paths_parent(const char *path, char *out, size_t cap);

#endif
