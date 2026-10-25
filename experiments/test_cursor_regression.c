#include "blinkhash_hybrid.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    bhh_table *table = bhh_create(16);
    unsigned char a = 'a', b = 'b', val = 'v';
    bhh_handle ha = BHH_HANDLE_NULL, hb = BHH_HANDLE_NULL;
    if (!table ||
        bhh_set(table, &a, 1, &val, 1, 200, &ha) != 0 ||
        bhh_set(table, &b, 1, &val, 1, 200, &hb) != 0) {
        fprintf(stderr, "SET/CREATE failed\n");
        return EXIT_FAILURE;
    }
    size_t first = bhh_purge_limit(table, 201, 1);
    if (first != 1 || bhh_size(table) != 1 || !table->purge_record_cursor) {
        fprintf(stderr, "Test setup failed\n");
        bhh_free(table);
        return EXIT_FAILURE;
    }
    uint32_t idx = table->purge_record_cursor;
    bhh_handle pending = bhh_handle_make(table->arena[idx].generation, idx);
    if (bhh_del_handle(table, pending) != 0 || bhh_size(table) != 0) {
        fprintf(stderr, "Manual deletion failed\n");
        bhh_free(table);
        return EXIT_FAILURE;
    }
    size_t second = bhh_purge_limit(table, 201, 1);
    printf("Purge #1: %zu; purge #2 (empty table): %zu; active keys: %zu\n",
           first, second, bhh_size(table));
    int ok = (second == 0);
    bhh_free(table);
    if (!ok) {
        fprintf(stderr, "FAIL: purge falsely reports reclamation of a deleted key.\n");
        return EXIT_FAILURE;
    }
    puts("PASS");
    return EXIT_SUCCESS;
}
