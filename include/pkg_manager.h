#ifndef PKG_MANAGER_H
#define PKG_MANAGER_H

/* Add a package dependency to varian.pkg */
int pkg_add(const char *pkg_name);

/* Generate a Varian wrapper for a foreign library (e.g. python:math) */
int pkg_wrap(const char *target);

#endif /* PKG_MANAGER_H */
