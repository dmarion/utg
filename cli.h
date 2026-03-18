#pragma once

#include "utg.h"

void usage(const char *prog);
int parse_app_args(int argc, char **argv, struct app_config *cfg);
