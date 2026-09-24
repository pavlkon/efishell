#ifndef OS_FEATURES_H
#define OS_FEATURES_H
#include "kernel.h"
void os_feature_tests(void (*report)(const char *, BOOLEAN));
BOOLEAN os_feature_command(const char *);
void os_feature_help(void);
#endif
