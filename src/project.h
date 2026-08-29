#ifndef OVERKILL_PROJECT_H
#define OVERKILL_PROJECT_H

int project_build(const char *build, const char *project);
int project_run(const char *build, const char *project);
int project_test(const char *build, const char *project);
int project_clean(const char *build, const char *project);
int project_files(const char *project);
int project_todo(const char *project);
int project_ports(void);
int project_changed(const char *project);

#endif
