#include <errno.h>
#include <inttypes.h>
#include <libimobiledevice/afc.h>
#include <libimobiledevice/libimobiledevice.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int track_number;
    char *title;
    char *location;
    sqlite3_int64 file_size;
    int track_count;
    int bitrate;
    int sample_rate;
} Track;

typedef struct {
    char **paths;
    size_t count;
} CandidateList;

typedef struct {
    const char *udid;
    const char *artist;
    const char *album;
    const char *output;
    int track_count;
    int bitrate;
    bool dry_run;
    bool prefer_network;
} Options;

static void usage(const char *program) {
    fprintf(stderr,
        "Usage: %s --udid UDID --artist ARTIST --album ALBUM --output DIR [options]\n"
        "\n"
        "Options:\n"
        "  --track-count N   Select only releases with N tracks when available\n"
        "  --bitrate N       Select only tracks with this bitrate\n"
        "  --dry-run         List matches without copying audio files\n"
        "  --network         Prefer the iPhone Wi-Fi connection\n"
        "  --help            Show this help\n",
        program);
}

static char *duplicate_string(const char *value) {
    if (!value) {
        return NULL;
    }
    size_t length = strlen(value);
    char *copy = malloc(length + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, length + 1);
    return copy;
}

static void free_track(Track *track) {
    if (!track) {
        return;
    }
    free(track->title);
    free(track->location);
    memset(track, 0, sizeof(*track));
}

static void free_tracks(Track *tracks, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free_track(&tracks[i]);
    }
    free(tracks);
}

static int ensure_directory(const char *path) {
    struct stat info;
    if (stat(path, &info) == 0) {
        return S_ISDIR(info.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int remote_file_size(afc_client_t afc, const char *path, uint64_t *size_out) {
    char **info = NULL;
    if (afc_get_file_info(afc, path, &info) != AFC_E_SUCCESS) {
        return -1;
    }

    int found = -1;
    for (size_t i = 0; info[i] && info[i + 1]; i += 2) {
        if (strcmp(info[i], "st_size") == 0) {
            *size_out = strtoull(info[i + 1], NULL, 10);
            found = 0;
            break;
        }
    }
    afc_dictionary_free(info);
    return found;
}

static int copy_remote_file(afc_client_t afc, const char *remote_path, const char *local_path) {
    uint64_t remote_handle = 0;
    if (afc_file_open(afc, remote_path, AFC_FOPEN_RDONLY, &remote_handle) != AFC_E_SUCCESS) {
        fprintf(stderr, "Could not open on the iPhone: %s\n", remote_path);
        return -1;
    }

    FILE *output = fopen(local_path, "wb");
    if (!output) {
        fprintf(stderr, "Could not create %s: %s\n", local_path, strerror(errno));
        afc_file_close(afc, remote_handle);
        return -1;
    }

    char buffer[64 * 1024];
    int result = 0;
    for (;;) {
        uint32_t bytes_read = 0;
        afc_error_t error = afc_file_read(afc, remote_handle, buffer,
                                           (uint32_t)sizeof(buffer), &bytes_read);
        if (error != AFC_E_SUCCESS) {
            fprintf(stderr, "Error reading %s\n", remote_path);
            result = -1;
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        if (fwrite(buffer, 1, bytes_read, output) != bytes_read) {
            fprintf(stderr, "Error writing %s: %s\n", local_path, strerror(errno));
            result = -1;
            break;
        }
    }

    fclose(output);
    afc_file_close(afc, remote_handle);

    if (result != 0) {
        unlink(local_path);
    }
    return result;
}

static int download_database_files(afc_client_t afc, const char *directory, char *database_path,
                                   size_t database_path_size) {
    const char *database_remote =
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
    const char *sidecars[] = {
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-wal",
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-shm",
    };

    snprintf(database_path, database_path_size, "%s/MediaLibrary.sqlitedb", directory);
    if (copy_remote_file(afc, database_remote, database_path) != 0) {
        fprintf(stderr, "Could not download the iPhone music library.\n");
        return -1;
    }

    for (size_t i = 0; i < sizeof(sidecars) / sizeof(sidecars[0]); i++) {
        uint64_t ignored_size = 0;
        if (remote_file_size(afc, sidecars[i], &ignored_size) == 0) {
            const char *basename = strrchr(sidecars[i], '/');
            basename = basename ? basename + 1 : sidecars[i];

            char local_path[PATH_MAX];
            snprintf(local_path, sizeof(local_path), "%s/%s", directory, basename);
            if (copy_remote_file(afc, sidecars[i], local_path) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int append_track(Track **tracks, size_t *count, size_t *capacity, const Track *source) {
    if (*count == *capacity) {
        size_t new_capacity = *capacity ? *capacity * 2 : 16;
        Track *expanded = realloc(*tracks, new_capacity * sizeof(**tracks));
        if (!expanded) {
            return -1;
        }
        *tracks = expanded;
        *capacity = new_capacity;
    }

    Track *destination = &(*tracks)[*count];
    memset(destination, 0, sizeof(*destination));
    destination->track_number = source->track_number;
    destination->file_size = source->file_size;
    destination->track_count = source->track_count;
    destination->bitrate = source->bitrate;
    destination->sample_rate = source->sample_rate;
    destination->title = duplicate_string(source->title);
    destination->location = duplicate_string(source->location);

    if (!destination->title || !destination->location) {
        free_track(destination);
        return -1;
    }
    (*count)++;
    return 0;
}

static int query_tracks(sqlite3 *database, const Options *options, Track **tracks_out,
                        size_t *count_out) {
    const char *sql =
        "SELECT i.track_number, "
        "       COALESCE(e.title, ''), "
        "       COALESCE(a.item_artist, ''), "
        "       COALESCE(al.album, ''), "
        "       COALESCE(e.location, ''), "
        "       COALESCE(e.file_size, 0), "
        "       COALESCE(e.track_count, 0), "
        "       COALESCE(p.bit_rate, 0), "
        "       COALESCE(p.sample_rate, 0) "
        "FROM item AS i "
        "JOIN item_extra AS e ON e.item_pid = i.item_pid "
        "LEFT JOIN item_artist AS a ON a.item_artist_pid = i.item_artist_pid "
        "LEFT JOIN album AS al ON al.album_pid = i.album_pid "
        "LEFT JOIN item_playback AS p ON p.item_pid = i.item_pid "
        "WHERE a.item_artist = ?1 COLLATE NOCASE "
        "  AND al.album = ?2 COLLATE NOCASE "
        "ORDER BY i.track_number, e.title;";

    sqlite3_stmt *statement = NULL;
    int error = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (error != SQLITE_OK) {
        fprintf(stderr, "Could not query MediaLibrary.sqlitedb: %s\n",
                sqlite3_errmsg(database));
        return -1;
    }

    sqlite3_bind_text(statement, 1, options->artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, options->album, -1, SQLITE_TRANSIENT);

    Track *tracks = NULL;
    size_t count = 0;
    size_t capacity = 0;

    while ((error = sqlite3_step(statement)) == SQLITE_ROW) {
        Track source = {
            .track_number = sqlite3_column_int(statement, 0),
            .title = (char *)sqlite3_column_text(statement, 1),
            .location = (char *)sqlite3_column_text(statement, 4),
            .file_size = sqlite3_column_int64(statement, 5),
            .track_count = sqlite3_column_int(statement, 6),
            .bitrate = sqlite3_column_int(statement, 7),
            .sample_rate = sqlite3_column_int(statement, 8),
        };

        if (options->track_count > 0 &&
            source.track_count > 0 &&
            source.track_count != options->track_count) {
            continue;
        }

        if (options->bitrate > 0 && source.bitrate != options->bitrate) {
            continue;
        }

        if (append_track(&tracks, &count, &capacity, &source) != 0) {
            free_tracks(tracks, count);
            sqlite3_finalize(statement);
            return -1;
        }
    }

    if (error != SQLITE_DONE) {
        fprintf(stderr, "Error iterating over results: %s\n", sqlite3_errmsg(database));
        free_tracks(tracks, count);
        sqlite3_finalize(statement);
        return -1;
    }

    sqlite3_finalize(statement);
    *tracks_out = tracks;
    *count_out = count;
    return 0;
}

static const char *basename_from_location(const char *location) {
    const char *slash = strrchr(location, '/');
    return slash ? slash + 1 : location;
}

static int add_candidate(CandidateList *list, const char *path) {
    char **expanded = realloc(list->paths, (list->count + 1) * sizeof(*list->paths));
    if (!expanded) {
        return -1;
    }
    list->paths = expanded;
    list->paths[list->count] = duplicate_string(path);
    if (!list->paths[list->count]) {
        return -1;
    }
    list->count++;
    return 0;
}

static void free_candidates(CandidateList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->paths[i]);
    }
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
}

static int find_candidates(afc_client_t afc, const char *filename, uint64_t expected_size,
                           CandidateList *result) {
    char **folders = NULL;
    if (afc_read_directory(afc, "/iTunes_Control/Music", &folders) != AFC_E_SUCCESS) {
        fprintf(stderr, "Could not read /iTunes_Control/Music on the iPhone.\n");
        return -1;
    }

    for (size_t i = 0; folders[i]; i++) {
        if (folders[i][0] != 'F') {
            continue;
        }

        char directory[PATH_MAX];
        snprintf(directory, sizeof(directory), "/iTunes_Control/Music/%s", folders[i]);

        char **files = NULL;
        if (afc_read_directory(afc, directory, &files) != AFC_E_SUCCESS) {
            continue;
        }

        for (size_t j = 0; files[j]; j++) {
            if (strcmp(files[j], filename) != 0) {
                continue;
            }

            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", directory, files[j]);

            uint64_t actual_size = 0;
            if (remote_file_size(afc, path, &actual_size) == 0 &&
                actual_size == expected_size) {
                if (add_candidate(result, path) != 0) {
                    afc_dictionary_free(files);
                    afc_dictionary_free(folders);
                    return -1;
                }
            }
        }
        afc_dictionary_free(files);
    }

    afc_dictionary_free(folders);
    return 0;
}

static void sanitize_filename(char *value) {
    for (char *cursor = value; *cursor; cursor++) {
        if (*cursor == '/' || *cursor == ':' || *cursor == '\n' ||
            *cursor == '\r' || *cursor == '\t') {
            *cursor = '_';
        }
    }
}

static const char *extension_from_location(const char *location) {
    const char *basename = basename_from_location(location);
    const char *dot = strrchr(basename, '.');
    return dot ? dot : ".mp3";
}

static int copy_tracks(afc_client_t afc, const Options *options, Track *tracks, size_t count) {
    size_t copied = 0;
    size_t skipped = 0;
    size_t missing = 0;

    for (size_t i = 0; i < count; i++) {
        const char *filename = basename_from_location(tracks[i].location);
        CandidateList candidates = {0};

        if (find_candidates(afc, filename, (uint64_t)tracks[i].file_size, &candidates) != 0) {
            free_candidates(&candidates);
            return -1;
        }

        printf("%02d. %s | %lld bytes | %d kbps | %d Hz\n",
               tracks[i].track_number,
               tracks[i].title,
               (long long)tracks[i].file_size,
               tracks[i].bitrate,
               tracks[i].sample_rate);

        if (candidates.count == 0) {
            printf("    No remote copy with the same name and size was found.\n");
            missing++;
            free_candidates(&candidates);
            continue;
        }

        if (candidates.count > 1) {
            printf("    Warning: %zu candidates have the same name and size; using the first one.\n",
                   candidates.count);
        }

        if (options->dry_run) {
            printf("    Candidato: %s\n", candidates.paths[0]);
            free_candidates(&candidates);
            continue;
        }

        char title[PATH_MAX];
        snprintf(title, sizeof(title), "%02d - %s%s",
                 tracks[i].track_number,
                 tracks[i].title[0] ? tracks[i].title : filename,
                 extension_from_location(tracks[i].location));
        sanitize_filename(title);

        char local_path[PATH_MAX];
        snprintf(local_path, sizeof(local_path), "%s/%s", options->output, title);

        if (access(local_path, F_OK) == 0) {
            printf("    Already exists; skipping: %s\n", local_path);
            skipped++;
            free_candidates(&candidates);
            continue;
        }

        if (copy_remote_file(afc, candidates.paths[0], local_path) == 0) {
            printf("    Copied: %s\n", local_path);
            copied++;
        } else {
            free_candidates(&candidates);
            return -1;
        }
        free_candidates(&candidates);
    }

    if (options->dry_run) {
        printf("\nDry run: %zu tracks with a candidate, %zu without a candidate.\n",
               count - missing, missing);
    } else {
        printf("\nRecovered: %zu; skipped: %zu; without a candidate: %zu.\n",
               copied, skipped, missing);
    }
    return missing ? 1 : 0;
}

static void cleanup_temp_directory(const char *directory) {
    const char *files[] = {
        "MediaLibrary.sqlitedb",
        "MediaLibrary.sqlitedb-wal",
        "MediaLibrary.sqlitedb-shm",
    };

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        unlink(path);
    }
    rmdir(directory);
}

static int parse_options(int argc, char **argv, Options *options) {
    memset(options, 0, sizeof(*options));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--udid") == 0 && i + 1 < argc) {
            options->udid = argv[++i];
        } else if (strcmp(argv[i], "--artist") == 0 && i + 1 < argc) {
            options->artist = argv[++i];
        } else if (strcmp(argv[i], "--album") == 0 && i + 1 < argc) {
            options->album = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            options->output = argv[++i];
        } else if (strcmp(argv[i], "--track-count") == 0 && i + 1 < argc) {
            options->track_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            options->bitrate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            options->dry_run = true;
        } else if (strcmp(argv[i], "--network") == 0) {
            options->prefer_network = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            return -1;
        }
    }

    if (!options->udid || !options->artist || !options->album ||
        (!options->output && !options->dry_run)) {
        usage(argv[0]);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    Options options;
    if (parse_options(argc, argv, &options) != 0) {
        return 2;
    }

    if (!options.dry_run && ensure_directory(options.output) != 0) {
        fprintf(stderr, "Could not use output directory %s: %s\n",
                options.output, strerror(errno));
        return 2;
    }

    idevice_t device = NULL;
    idevice_error_t device_error;
    if (options.prefer_network) {
        device_error = idevice_new_with_options(
            &device, options.udid,
            IDEVICE_LOOKUP_NETWORK | IDEVICE_LOOKUP_PREFER_NETWORK);
    } else {
        device_error = idevice_new(&device, options.udid);
    }

    if (device_error != IDEVICE_E_SUCCESS || !device) {
        fprintf(stderr, "Could not connect to iPhone %s.\n", options.udid);
        return 3;
    }

    afc_client_t afc = NULL;
    if (afc_client_start_service(device, &afc, "iphone-audio-recover") != AFC_E_SUCCESS ||
        !afc) {
        fprintf(stderr, "Could not start AFC on the iPhone.\n");
        idevice_free(device);
        return 3;
    }

    char temp_template[] = "/tmp/iphone-audio-recover-XXXXXX";
    char *temp_directory = mkdtemp(temp_template);
    if (!temp_directory) {
        fprintf(stderr, "Could not create the temporary directory: %s\n", strerror(errno));
        afc_client_free(afc);
        idevice_free(device);
        return 4;
    }

    char database_path[PATH_MAX];
    int result = download_database_files(
        afc, temp_directory, database_path, sizeof(database_path));
    if (result != 0) {
        cleanup_temp_directory(temp_directory);
        afc_client_free(afc);
        idevice_free(device);
        return 4;
    }

    sqlite3 *database = NULL;
    if (sqlite3_open_v2(database_path, &database, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "Could not open the temporary database copy.\n");
        sqlite3_close(database);
        cleanup_temp_directory(temp_directory);
        afc_client_free(afc);
        idevice_free(device);
        return 4;
    }

    Track *tracks = NULL;
    size_t track_count = 0;
    result = query_tracks(database, &options, &tracks, &track_count);
    sqlite3_close(database);

    if (result != 0) {
        cleanup_temp_directory(temp_directory);
        afc_client_free(afc);
        idevice_free(device);
        return 5;
    }

    if (track_count == 0) {
        fprintf(stderr, "No tracks found for artist='%s' album='%s'.\n",
                options.artist, options.album);
        free_tracks(tracks, track_count);
        cleanup_temp_directory(temp_directory);
        afc_client_free(afc);
        idevice_free(device);
        return 6;
    }

    printf("Matches: %zu\n", track_count);
    result = copy_tracks(afc, &options, tracks, track_count);

    free_tracks(tracks, track_count);
    cleanup_temp_directory(temp_directory);
    afc_client_free(afc);
    idevice_free(device);
    return result < 0 ? 7 : result;
}
