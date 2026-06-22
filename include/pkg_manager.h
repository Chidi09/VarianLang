#ifndef PKG_MANAGER_H
#define PKG_MANAGER_H

#include <stdbool.h>

typedef struct {
    char name[64];
    char version[32];
    int kind; // 0 = unset, 1 = zenith, 2 = lumen, 3 = aurora
    
    // Dependencies
    struct {
        char name[64];
        char val[256];
        bool is_git;
        char git_url[256];
        char git_tag[64];
    } deps[64];
    int dep_count;
    
    // Capabilities
    bool cap_ffi;
    bool cap_python;
    bool cap_net;
    bool cap_fs;
    
    // Build
    char build_script[128];
} ConstellationManifest;

#define MANIFEST_KIND_UNSET 0
#define MANIFEST_KIND_ZENITH 1
#define MANIFEST_KIND_LUMEN 2
#define MANIFEST_KIND_AURORA 3

/* Add a package dependency to constellation.toml */
int pkg_add(const char *pkg_name);

/* Install all dependencies from constellation.toml */
int pkg_install(bool frozen);

/* Remove a package dependency and delete its vendored files */
int pkg_remove(const char *pkg_name);

/* Update package dependencies and re-resolve latest Git commits */
int pkg_update(void);

/* Search the registry index for packages matching a query */
int pkg_search(const char *query);

/* Publish package to the registry */
int pkg_publish(void);

/* Generate a Varian wrapper for a foreign library (e.g. python:math) */
int pkg_wrap(const char *target);

/* Manifest utilities */
int pkg_migrate_manifest(void);
bool pkg_manifest_load(ConstellationManifest *manifest, const char *path);
bool pkg_manifest_save(const ConstellationManifest *manifest, const char *path);

#endif /* PKG_MANAGER_H */
