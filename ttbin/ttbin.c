/*****************************************************************************\
** ttbin.c                                                                   **
** TTBIN parsing implementation                                              **
\*****************************************************************************/

#include "ttbin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

/*****************************************************************************/

const OFFLINE_FORMAT OFFLINE_FORMATS[OFFLINE_FORMAT_COUNT] = {
    { OFFLINE_FORMAT_CSV, "csv", 0, export_csv },
    { OFFLINE_FORMAT_FIT, "fit", 1, 0          },
    { OFFLINE_FORMAT_GPX, "gpx", 1, export_gpx },
    { OFFLINE_FORMAT_KML, "kml", 1, export_kml },
    { OFFLINE_FORMAT_PWX, "pwx", 1, 0          },
    { OFFLINE_FORMAT_TCX, "tcx", 1, export_tcx },
};

/*****************************************************************************/

typedef struct __attribute__((packed))
{
    uint8_t tag;
    uint16_t length;
} RECORD_LENGTH;

typedef struct __attribute__((packed))
{
    uint8_t  file_version;
    uint8_t  firmware_version[4];
    uint16_t product_id;
    uint32_t timestamp;     /* local time */
    uint8_t  _unk[96];
    uint32_t timestamp2;    /* local time, duplicate */
    int32_t local_time_offset;  /* seconds from UTC */
    uint8_t  _unk2;
    uint8_t  length_count;  /* number of RECORD_LENGTH objects to follow */
    RECORD_LENGTH lengths[1];
} FILE_HEADER;

typedef struct __attribute__((packed))
{
    uint8_t  activity;
    float    distance;
    uint32_t duration;      /* seconds, after adding 1 */
    uint16_t calories;
} FILE_SUMMARY_RECORD;

typedef struct __attribute__((packed))
{
    int32_t  latitude;      /* degrees * 1e7 */
    int32_t  longitude;     /* degrees * 1e7 */
    uint16_t heading;       /* degrees * 100, N = 0, E = 9000 */
    uint16_t speed;         /* m/s * 100 */
    uint32_t timestamp;     /* gps time (utc) */
    uint16_t calories;
    float    inc_distance;  /* metres */
    float    cum_distance;  /* metres */
    uint8_t  cycles;        /* steps/strokes/cycles etc. */
} FILE_GPS_RECORD;

typedef struct __attribute__((packed))
{
    uint8_t  heart_rate;    /* bpm */
    uint8_t  _unk;
    uint32_t timestamp;     /* local time */
} FILE_HEART_RATE_RECORD;

typedef struct __attribute__((packed))
{
    uint8_t  status;        /* 0 = ready, 1 = active, 2 = paused, 3 = stopped */
    uint8_t  activity;      /* 0 = running, 1 = cycling, 2 = swimming, 7 = treadmill, 8 = freestyle */
    uint32_t timestamp;     /* local time */
} FILE_STATUS_RECORD;

typedef struct __attribute__((packed))
{
    uint32_t timestamp;     /* local time */
    float    distance;      /* metres */
    uint16_t calories;
    uint32_t steps;
    uint16_t _unk;
} FILE_TREADMILL_RECORD;

typedef struct __attribute__((packed))
{
    uint32_t timestamp;         /* local time */
    float    total_distance;    /* metres */
    uint8_t  _unk1;             /* always 0xff */
    uint8_t  _unk2;
    uint32_t strokes;           /* since the last report */
    uint32_t completed_laps;
    uint16_t total_calories;
} FILE_SWIM_RECORD;

typedef struct __attribute__((packed))
{
    uint32_t total_time;        /* seconds since activity start */
    float    total_distance;    /* metres */
    uint16_t total_calories;
} FILE_LAP_RECORD;

typedef struct __attribute__((packed))
{
    uint32_t _unk[4];
    float    distance;  /* metres */
    uint32_t duration;  /* seconds */
    char     name[16];  /* unused characters are zero */
} FILE_RACE_SETUP_RECORD;

typedef struct __attribute__((packed))
{
    uint32_t duration;  /* seconds */
    float    distance;  /* metres */
    uint16_t calories;
} FILE_RACE_RESULT_RECORD;

/*****************************************************************************/

TTBIN_FILE *read_ttbin_file(FILE *file)
{
    uint32_t size = 0;
    uint8_t *data = 0;
    TTBIN_FILE *ttbin;

    while (!feof(file))
    {
        data = realloc(data, size + 1024);
        size += fread(data + size, 1, 1024, file);
    }

    ttbin = parse_ttbin_data(data, size);

    free(data);
    return ttbin;
}

/*****************************************************************************/

TTBIN_RECORD *append_record(TTBIN_FILE *ttbin, uint8_t tag, uint16_t length)
{
    TTBIN_RECORD *record = (TTBIN_RECORD*)malloc(sizeof(TTBIN_RECORD));
    if (ttbin->last)
    {
        record->prev = ttbin->last;
        ttbin->last->next = record;
        ttbin->last = record;
    }
    else
    {
        ttbin->first = ttbin->last = malloc(sizeof(TTBIN_RECORD));
        record->prev = 0;
    }
    record->next = 0;
    record->length = length;
    record->tag = tag;

    return record;
}

#define insert_pointer(ptrs, count, ptr)    do { \
    ptrs = realloc(ptrs, (count + 1) * sizeof(&ptrs[0])); \
    ptrs[count++] = ptr; \
} while (0)

TTBIN_FILE *parse_ttbin_data(uint8_t *data, uint32_t size)
{
    uint8_t *end;
    TTBIN_FILE *file;
    int index;
    unsigned length;

    FILE_HEADER             *file_header = 0;
    FILE_SUMMARY_RECORD     *summary_record;
    FILE_GPS_RECORD         *gps_record;
    FILE_HEART_RATE_RECORD  *heart_rate_record;
    FILE_STATUS_RECORD      *status_record;
    FILE_TREADMILL_RECORD   *treadmill_record;
    FILE_SWIM_RECORD        *swim_record;
    FILE_LAP_RECORD         *lap_record;
    FILE_RACE_SETUP_RECORD  *race_setup_record;
    FILE_RACE_RESULT_RECORD *race_result_record;

    TTBIN_RECORD *record;

    file = malloc(sizeof(TTBIN_FILE));
    memset(file, 0, sizeof(TTBIN_FILE));

    end = data + size;

    while (data < end)
    {
        uint8_t tag = *data++;

        /* find the length of this tag */
        if (tag != TAG_FILE_HEADER)
        {
            index = 0;
            while ((index < file_header->length_count) && (file_header->lengths[index].tag < tag))
                ++index;
            if ((index < file_header->length_count) && (file_header->lengths[index].tag == tag))
                length = file_header->lengths[index].length - 1;
            else
            {
                free_ttbin(file);
                return 0;
            }
        }
        else
            length = 0;

        switch (tag)
        {
        case TAG_FILE_HEADER:
            file_header = (FILE_HEADER*)data;
            data += sizeof(FILE_HEADER) + (file_header->length_count - 1) * sizeof(RECORD_LENGTH);

            file->file_version    = file_header->file_version;
            memcpy(file->firmware_version, file_header->firmware_version, sizeof(file->firmware_version));
            file->product_id      = file_header->product_id;
            file->timestamp_local = file_header->timestamp;
            file->timestamp_utc   = file_header->timestamp - file_header->local_time_offset;
            file->utc_offset      = file_header->local_time_offset;
            break;
        case TAG_SUMMARY:
            summary_record = (FILE_SUMMARY_RECORD*)data;

            file->activity       = summary_record->activity;
            file->total_distance = summary_record->distance;
            file->duration       = summary_record->duration;
            file->total_calories = summary_record->calories;
            break;
        case TAG_STATUS:
            status_record = (FILE_STATUS_RECORD*)data;
            status_record->timestamp -= file->utc_offset;
            index = file->status_record_count;

            record = append_record(file, tag, length);
            record->status = (STATUS_RECORD*)malloc(sizeof(STATUS_RECORD));
            record->status->status = status_record->status;
            record->status->activity = status_record->activity;
            insert_pointer(file->status_records, file->status_record_count, record);
            break;
        case TAG_GPS:
            gps_record = (FILE_GPS_RECORD*)data;

            /* if the GPS signal is lost, 0xffffffff is stored in the file */
            if (gps_record->timestamp == 0xffffffff)
                break;

            record = append_record(file, tag, length);
            record->gps = (GPS_RECORD*)malloc(sizeof(GPS_RECORD));
            record->gps->latitude     = gps_record->latitude * 1e-7f;
            record->gps->longitude    = gps_record->longitude * 1e-7f;
            record->gps->elevation    = 0.0f;
            record->gps->heading      = gps_record->heading / 100.0f;
            record->gps->speed        = gps_record->speed / 100.0f;
            record->gps->timestamp    = gps_record->timestamp;
            record->gps->calories     = gps_record->calories;
            record->gps->inc_distance = gps_record->inc_distance;
            record->gps->cum_distance = gps_record->cum_distance;
            record->gps->cycles       = gps_record->cycles;
            insert_pointer(file->gps_records, file->gps_record_count, record);
            break;
        case TAG_HEART_RATE:
            heart_rate_record = (FILE_HEART_RATE_RECORD*)data;
            heart_rate_record->timestamp -= file->utc_offset;

            record = append_record(file, tag, length);
            record->heart_rate = (HEART_RATE_RECORD*)malloc(sizeof(HEART_RATE_RECORD));
            record->heart_rate->timestamp  = heart_rate_record->timestamp;
            record->heart_rate->heart_rate = heart_rate_record->heart_rate;
            insert_pointer(file->heart_rate_records, file->heart_rate_record_count, record);
            break;
        case TAG_LAP:
            lap_record = (FILE_LAP_RECORD*)data;

            record = append_record(file, tag, length);
            record->lap = (LAP_RECORD*)malloc(sizeof(LAP_RECORD));
            record->lap->total_time     = lap_record->total_time;
            record->lap->total_distance = lap_record->total_distance;
            record->lap->total_calories = lap_record->total_calories;
            insert_pointer(file->lap_records, file->lap_record_count, record);
            break;
        case TAG_TREADMILL:
            treadmill_record = (FILE_TREADMILL_RECORD*)data;
            treadmill_record->timestamp -= file->utc_offset;

            record = append_record(file, tag, length);
            record->treadmill = (TREADMILL_RECORD*)malloc(sizeof(TREADMILL_RECORD));
            record->treadmill->timestamp = treadmill_record->timestamp;
            record->treadmill->distance  = treadmill_record->distance;
            record->treadmill->calories  = treadmill_record->calories;
            record->treadmill->steps     = treadmill_record->steps;
            insert_pointer(file->treadmill_records, file->treadmill_record_count, record);
            break;
        case TAG_SWIM:
            swim_record = (FILE_SWIM_RECORD*)data;
            swim_record->timestamp -= file->utc_offset;

            record = append_record(file, tag, length);
            record->swim = (SWIM_RECORD*)malloc(sizeof(SWIM_RECORD));
            record->swim->timestamp      = swim_record->timestamp;
            record->swim->total_distance = swim_record->total_distance;
            record->swim->strokes        = swim_record->strokes;
            record->swim->completed_laps = swim_record->completed_laps;
            record->swim->total_calories = swim_record->total_calories;
            insert_pointer(file->swim_records, file->swim_record_count, record);
            break;
        case TAG_RACE_SETUP:
            race_setup_record = (FILE_RACE_SETUP_RECORD*)data;

            record = append_record(file, tag, length);
            record->race_setup = (RACE_SETUP_RECORD*)malloc(sizeof(RACE_SETUP_RECORD));
            record->race_setup->distance = race_setup_record->distance;
            record->race_setup->duration = race_setup_record->duration;
            memcpy(record->race_setup->name, race_setup_record->name, sizeof(race_setup_record->name));
            file->race_setup = record->race_setup;
            break;
        case TAG_RACE_RESULT:
            race_result_record = (FILE_RACE_RESULT_RECORD*)data;

            record = append_record(file, tag, length);
            record->race_result = (RACE_RESULT_RECORD*)malloc(sizeof(RACE_RESULT_RECORD));
            record->race_result->distance = race_result_record->distance;
            record->race_result->duration = race_result_record->duration;
            record->race_result->calories = race_result_record->calories;
            file->race_result = record->race_result;
            break;
        default:
            record = append_record(file, tag, length);
            record->data = (uint8_t*)malloc(length);
            memcpy(record->data, data, length);
            break;
        }

        /* we should have got a file header first... */
        if (!file_header)
        {
            free_ttbin(file);
            return 0;
        }

        /* skip to the next record */
        data += length;
    }

    return file;
}

/*****************************************************************************/

void insert_length_record(FILE_HEADER *header, uint8_t tag, uint16_t length)
{
    unsigned i = 0;
    /* look for the position to put the new tag (numerical order) */
    while ((tag < header->lengths[i].tag) && (header->lengths[i].tag != 0))
        ++i;

    /* make sure we don't insert duplicates */
    if (header->lengths[i].tag != tag)
    {
        memmove(header->lengths + i + 1, header->lengths + i, (29 - i) * sizeof(RECORD_LENGTH));
        header->lengths[i].tag = tag;
        header->lengths[i].length = length;
        ++header->length_count;
    }
}

int write_ttbin_file(const TTBIN_FILE *ttbin, FILE *file)
{
    time_t current_time;
    unsigned status_index, gps_index, treadmill_index;
    unsigned swim_index, heart_rate_index, lap_index, unknown_index;
    int more;
    unsigned i;
    TTBIN_RECORD *record;

    /* create and write the file header */
    unsigned size = sizeof(FILE_HEADER) + 29 * sizeof(RECORD_LENGTH);
    FILE_HEADER *header = (FILE_HEADER*)malloc(size);
    memset(header, 0, size);
    header->file_version = ttbin->file_version;
    memcpy(header->firmware_version, ttbin->firmware_version, sizeof(header->firmware_version));
    header->product_id = ttbin->product_id;
    header->timestamp = ttbin->timestamp_local;
    header->timestamp2 = ttbin->timestamp_local;
    header->local_time_offset = ttbin->utc_offset;
    insert_length_record(header, TAG_FILE_HEADER, sizeof(FILE_HEADER) - sizeof(RECORD_LENGTH));
    insert_length_record(header, TAG_SUMMARY, sizeof(FILE_SUMMARY_RECORD));
    for (record = ttbin->first; record; record = record->next)
        insert_length_record(header, record->tag, record->length);
    fwrite(header, 1, sizeof(FILE_HEADER) + (header->length_count - 1) * sizeof(RECORD_LENGTH), file);

    for (record = ttbin->first; record; record = record->next)
    {
        switch (record->tag)
        {
        case TAG_STATUS: {
            FILE_STATUS_RECORD r = {
                record->status->status,
                record->status->activity,
                record->status->timestamp + ttbin->utc_offset
            };
            fwrite(&r, 1, sizeof(FILE_STATUS_RECORD), file);
            break;
        }
        case TAG_GPS: {
            FILE_GPS_RECORD r = {
                record->gps->latitude * 1e7,
                record->gps->longitude * 1e7,
                record->gps->heading,
                record->gps->speed * 100,
                record->gps->timestamp,
                record->gps->calories,
                record->gps->inc_distance,
                record->gps->cum_distance,
                record->gps->cycles
            };
            fwrite(&r, 1, sizeof(FILE_GPS_RECORD), file);
            break;
        }
        case TAG_HEART_RATE: {
            FILE_HEART_RATE_RECORD r = {
                record->heart_rate->heart_rate,
                0,  /* unknown */
                record->heart_rate->timestamp + ttbin->utc_offset
            };
            fwrite(&r, 1, sizeof(FILE_HEART_RATE_RECORD), file);
            break;
        }
        case TAG_LAP: {
            FILE_LAP_RECORD r = {
                record->lap->total_time,
                record->lap->total_distance,
                record->lap->total_calories
            };
            fwrite(&r, 1, sizeof(FILE_LAP_RECORD), file);
            break;
        }
        case TAG_TREADMILL: {
            FILE_TREADMILL_RECORD r = {
                record->treadmill->timestamp + ttbin->utc_offset,
                record->treadmill->distance,
                record->treadmill->calories,
                record->treadmill->steps,
                0
            };
            fwrite(&r, 1, sizeof(FILE_TREADMILL_RECORD), file);
            break;
        }
        case TAG_SWIM: {
            FILE_SWIM_RECORD r = {
                record->swim->timestamp + ttbin->utc_offset,
                record->swim->total_distance,
                0xff,   /* unknown, always 0xff */
                0x00,   /* unknown */
                record->swim->strokes,
                record->swim->completed_laps,
                record->swim->total_calories
            };
            fwrite(&r, 1, sizeof(FILE_SWIM_RECORD), file);
            break;
        }
        case TAG_RACE_SETUP: {
            FILE_RACE_SETUP_RECORD r = {
                { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff },
                record->race_setup->distance,
                record->race_setup->duration,
                {0}
            };
            memcpy(r.name, record->race_setup->name, sizeof(r.name));
            fwrite(&r, 1, sizeof(FILE_RACE_SETUP_RECORD), file);
            break;
        }
        case TAG_RACE_RESULT: {
            FILE_RACE_RESULT_RECORD r = {
                record->race_result->distance,
                record->race_result->duration,
                record->race_result->calories
            };
            fwrite(&r, 1, sizeof(FILE_RACE_RESULT_RECORD), file);
            break;
        }
        }
    }
}

/*****************************************************************************/

TTBIN_RECORD *insert_before(TTBIN_FILE *ttbin, TTBIN_RECORD *record)
{
    TTBIN_RECORD *r = (TTBIN_RECORD*)malloc(sizeof(TTBIN_RECORD));
    if (record == ttbin->first)
    {
        r->next = ttbin->first;
        r->prev = 0;
        r->next->prev = r;
        ttbin->first = r;
    }
    else
    {
        r->next = record;
        r->prev = record->prev;
        r->next->prev = r;
        r->prev->next = r;
    }
    return r;
}

/*****************************************************************************/

TTBIN_RECORD *insert_after(TTBIN_FILE *ttbin, TTBIN_RECORD *record)
{
   TTBIN_RECORD *r = (TTBIN_RECORD*)malloc(sizeof(TTBIN_RECORD));
   if (record == ttbin->last)
   {
       r->next = 0;
       r->prev = ttbin->last;
       r->prev->next = r;
       ttbin->last = r;
   }
   else
   {
       r->next = record->next;
       r->prev = record;
       r->next->prev = r;
       r->prev->next = r;
   }
   return r;
}

/*****************************************************************************/

const char *create_filename(TTBIN_FILE *ttbin, const char *ext)
{
    static char filename[32];
    struct tm *time = gmtime(&ttbin->timestamp_local);
    const char *type = "Unknown";

    switch (ttbin->activity)
    {
    case ACTIVITY_RUNNING:   type = "Running"; break;
    case ACTIVITY_CYCLING:   type = "Cycling"; break;
    case ACTIVITY_SWIMMING:  type = "Pool_swim"; break;
    case ACTIVITY_TREADMILL: type = "Treadmill"; break;
    case ACTIVITY_FREESTYLE: type = "Freestyle"; break;
    }
    sprintf(filename, "%s_%02d-%02d-%02d.%s", type, time->tm_hour, time->tm_min, time->tm_sec, ext);

    return filename;
}

/*****************************************************************************/

typedef struct
{
    TTBIN_RECORD **data;
    uint32_t max_count;
    uint32_t current_count;

    float elev;
    float mult;
} ELEV_DATA_INFO;

static size_t curl_write_data(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    ELEV_DATA_INFO *info = (ELEV_DATA_INFO*)userdata;
    char *s1;

    size_t length = size * nmemb;

    /* this is a simple float-parser that maintains state between
       invocations incase we get a single number split between
       multiple buffers */
    for (s1 = ptr; s1 < (ptr + length); ++s1)
    {
        if (isdigit(*s1))
        {
            if (info->mult > 0.5f)
                info->elev = (info->elev * 10.0f) + (*s1 - '0');
            else
            {
                info->elev += info->mult * (*s1 - '0');
                info->mult /= 10.0f;
            }
        }
        else if (*s1 == '.')
            info->mult = 0.1f;
        else if ((*s1 == ',') || (*s1 == ']'))
        {
            if (info->current_count < info->max_count)
            {
                (*info->data)->gps->elevation = info->elev;
                ++info->current_count;
                ++info->data;
            }
            info->elev = 0.0f;
            info->mult = 1.0f;
        }
    }

    return length;
}

void download_elevation_data(TTBIN_FILE *ttbin)
{
    CURL *curl;
    struct curl_slist *headers;
    char *post_data;
    char *response_data;
    char *str;
    uint32_t i;
    ELEV_DATA_INFO info = {0};
    int result;

    /* only download elevation data if we have GPS records */
    if (!ttbin || !ttbin->gps_record_count || !ttbin->gps_records)
        return;

    curl = curl_easy_init();
    if (!curl)
    {
        fprintf(stderr, "Unable to initialise libcurl\n");
        return;
    }

    /* create the post string to send to the server */
    post_data = malloc(ttbin->gps_record_count * 52 + 10);
    str = post_data;
    str += sprintf(str, "[\n");
    for (i = 0; i < ttbin->gps_record_count; ++i)
    {
        if (i != (ttbin->gps_record_count - 1))
        {
            str += sprintf(str, "   [ %f, %f ],\n",
                ttbin->gps_records[i]->gps->latitude,
                ttbin->gps_records[i]->gps->longitude);
        }
        else
        {
            str += sprintf(str, "   [ %f, %f ]\n",
                ttbin->gps_records[i]->gps->latitude,
                ttbin->gps_records[i]->gps->longitude);
        }
    }
    str += sprintf(str, "]\n");

    headers = curl_slist_append(NULL, "Content-Type:text/plain");

    /* setup the callback function data structure */
    info.mult = 1.0;
    info.elev = 0.0;
    info.data = ttbin->gps_records;
    info.max_count = ttbin->gps_record_count;
    info.current_count = 0;

    /* setup the transaction */
    curl_easy_setopt(curl, CURLOPT_URL, "https://mysports.tomtom.com/tyne/dem/fixmodel");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, str - post_data);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TomTom");
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &info);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_data);

    /* perform the transaction */
    result = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK)
        fprintf(stderr, "Unable to download elevation data: %d\n", result);
}

/*****************************************************************************/

uint32_t export_formats(TTBIN_FILE *ttbin, uint32_t formats)
{
    unsigned i;
    FILE *f;

    for (i = 0; i < OFFLINE_FORMAT_COUNT; ++i)
    {
        if ((formats & OFFLINE_FORMATS[i].mask) && OFFLINE_FORMATS[i].producer)
        {
            if (!OFFLINE_FORMATS[i].requires_gps || ttbin->gps_record_count)
            {
                f = fopen(create_filename(ttbin, OFFLINE_FORMATS[i].name), "w");
                if (f)
                {
                    (*OFFLINE_FORMATS[i].producer)(ttbin, f);
                    fclose(f);
                }
                else
                    formats &= ~OFFLINE_FORMATS[i].mask;
            }
            else
                formats &= ~OFFLINE_FORMATS[i].mask;
        }
        else
            formats &= ~OFFLINE_FORMATS[i].mask;
    }

    return formats;
}

/*****************************************************************************/

void free_ttbin(TTBIN_FILE *ttbin)
{
    if (ttbin)
    {
        TTBIN_RECORD *record;
        for (record = ttbin->first; record; record = record->next)
        {
            free(record->data);
            if (record->prev)
                free(record->prev);
        }
        if (ttbin->last)
            free(ttbin->last);
        if (ttbin->race_setup)
            free(ttbin->race_setup);
        if (ttbin->race_result)
            free(ttbin->race_result);
        if (ttbin->gps_records)
            free(ttbin->gps_records);
        if (ttbin->status_records)
            free(ttbin->status_records);
        if (ttbin->treadmill_records)
            free(ttbin->treadmill_records);
        if (ttbin->swim_records)
            free(ttbin->swim_records);
        if (ttbin->lap_records)
            free(ttbin->lap_records);
        if (ttbin->heart_rate_records)
            free(ttbin->heart_rate_records);
        free(ttbin);
    }
}
