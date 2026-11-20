#include <stdio.h>
#include "pod_metadata.h"

int main(int argc, char **argv)
{
    if (argc < 2) return 1;
    pod_metadata_t m;
    esp_err_t e = pod_metadata_load(argv[1], &m);
    printf("err=%d title=[%s] artist=[%s] album=[%s] cover=[%s]\n",
           e, m.title, m.artist, m.album, m.cover_path);
    return 0;
}
