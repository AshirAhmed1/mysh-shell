#include <stdlib.h>
#include <string.h>
#include "variables.h"
#include "io_helpers.h"

void init_store(VarStore *store) {
    store->head = NULL;
}

static VarNode *find_node(VarStore *store, const char *key) {
    VarNode *curr = store->head;
    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

int store_set(VarStore *store, const char *key, const char *value) {
    VarNode *existing = find_node(store, key);

    char *new_value = malloc(strlen(value) + 1);
    if (new_value == NULL) {
        display_error("ERROR: ", "memory allocation failed");
        return -1;
    }
    strcpy(new_value, value);

    if (existing != NULL) {
        free(existing->value);
        existing->value = new_value;
        return 0;
    }

    VarNode *node = malloc(sizeof(VarNode));
    if (node == NULL) {
        free(new_value);
        display_error("ERROR: ", "memory allocation failed");
        return -1;
    }

    char *new_key = malloc(strlen(key) + 1);
    if (new_key == NULL) {
        free(new_value);
        free(node);
        display_error("ERROR: ", "memory allocation failed");
        return -1;
    }

    strcpy(new_key, key);

    node->key = new_key;
    node->value = new_value;
    node->next = store->head;
    store->head = node;

    return 0;
}

const char *store_get(VarStore *store, const char *key) {
    VarNode *node = find_node(store, key);
    if (node == NULL) {
        return NULL;
    }
    return node->value;
}

void store_destroy(VarStore *store) {
    VarNode *curr = store->head;
    while (curr != NULL) {
        VarNode *next = curr->next;
        free(curr->key);
        free(curr->value);
        free(curr);
        curr = next;
    }
    store->head = NULL;
}