/* Minimal libmtp uploader that targets a specific parent folder id — the
 * bundled mtp-sendfile/mtp-connect CLIs can only send to root, which Garmin's
 * CIQ loader ignores (apps must live in GARMIN/Apps).
 *   usage: mtp_send_to_folder <local> <remotename> <parent_folder_id> [device_index]
 * Build: cc mtp_send_to_folder.c -I/opt/homebrew/include -L/opt/homebrew/lib -lmtp -o mtp_send_to_folder
 */
#include <libmtp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s <local> <remotename> <parent_id> [device_index]\n", argv[0]);
        return 2;
    }
    const char *local = argv[1], *remote = argv[2];
    uint32_t parent = (uint32_t)strtoul(argv[3], NULL, 0);
    int dev_index = argc == 5 ? atoi(argv[4]) : 0;

    struct stat st;
    if (stat(local, &st) != 0) { perror("stat local"); return 1; }

    LIBMTP_Init();
    LIBMTP_mtpdevice_t *devlist = NULL;
    LIBMTP_Get_Connected_Devices(&devlist);
    if (!devlist) { fprintf(stderr, "no MTP devices\n"); return 1; }
    LIBMTP_mtpdevice_t *dev = devlist;
    for (int i = 0; i < dev_index && dev->next; i++) dev = dev->next;
    if (!dev) { fprintf(stderr, "device index %d not found\n", dev_index); return 1; }

    uint32_t storage_id = dev->storage ? dev->storage->id : 0; /* primary storage */

    LIBMTP_file_t *f = LIBMTP_new_file_t();
    f->filename   = strdup(remote);
    f->filesize   = (uint64_t)st.st_size;
    f->filetype   = LIBMTP_FILETYPE_UNKNOWN;
    f->parent_id  = parent;
    f->storage_id = storage_id;

    printf("sending %s (%lld bytes) -> parent=%u storage=0x%08x as '%s'\n",
           local, (long long)st.st_size, parent, storage_id, remote);

    int rc = LIBMTP_Send_File_From_File(dev, local, f, NULL, NULL);
    if (rc != 0) {
        LIBMTP_Dump_Errorstack(dev);
        LIBMTP_Clear_Errorstack(dev);
    } else {
        printf("OK: new object id = %u\n", f->item_id);
    }
    LIBMTP_destroy_file_t(f);
    LIBMTP_Release_Device(dev);
    return rc;
}
