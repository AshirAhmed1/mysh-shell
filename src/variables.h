#ifndef VARIABLES_H
#define VARIABLES_H

#include <stddef.h>

typedef struct VarNode {
    char *key;
    char *value;
    struct VarNode *next;
} VarNode;

typedef struct {
    VarNode *head;
} VarStore;

void init_store(VarStore *store);
int store_set(VarStore *store, const char *key, const char *value);
const char *store_get(VarStore *store, const char *key);
void store_destroy(VarStore *store);

#endif