#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L
#define _FILE_OFFSET_BITS 64
#endif

#include "imu_converter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

#define IMU_INPUT_CAPACITY 65536u
#define IMU_FRAME_SIZE 50u
#define IMU_HEADER_SIZE 512u
#define IMU_OUTPUT_RECORD_SIZE 32u
#define IMU_IMR_MAGIC "$IMURAW"
#define IMU_IMR_MAGIC_SIZE 7u

static const uint8_t imu_frame_marker[4] = {0xAA, 0x77, 0x02, 0x31};

static FILE *open_file_utf8(const char *path, int write_mode)
{
#ifdef _WIN32
    int wide_length;
    wchar_t *wide_path;
    FILE *file;

    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      path, -1, NULL, 0);
    if (wide_length <= 0)
        return NULL;

    wide_path = (wchar_t *)malloc((size_t)wide_length * sizeof(wchar_t));
    if (wide_path == NULL)
        return NULL;

    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide_path, wide_length) != wide_length) {
        free(wide_path);
        return NULL;
    }

    file = _wfopen(wide_path, write_mode ? L"wb" : L"rb");
    free(wide_path);
    return file;
#else
    return fopen(path, write_mode ? "wb" : "rb");
#endif
}

static uint64_t get_file_size(FILE *file)
{
#ifdef _WIN32
    __int64 end;
    if (_fseeki64(file, 0, SEEK_END) != 0)
        return 0;
    end = _ftelli64(file);
    if (_fseeki64(file, 0, SEEK_SET) != 0 || end < 0)
        return 0;
    return (uint64_t)end;
#else
    off_t end;
    if (fseeko(file, 0, SEEK_END) != 0)
        return 0;
    end = ftello(file);
    if (fseeko(file, 0, SEEK_SET) != 0 || end < 0)
        return 0;
    return (uint64_t)end;
#endif
}

static void store_u64_le(uint8_t *output, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        output[i] = (uint8_t)(value >> (i * 8));
}

static void store_double_le(uint8_t *output, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    store_u64_le(output, bits);
}

static void create_imr_header(uint8_t header[IMU_HEADER_SIZE])
{
    const double ie_version = 8.70;
    const double imu_rate = 200.0;
    const double gyro_scale = 1.0 / 4325376.0;
    const double accel_scale = (1.0 / 262144.0) / 1000.0 * 9.80664772;

    memset(header, 0, IMU_HEADER_SIZE);
    memcpy(header, "$IMURAW", 7);
    store_double_le(header + 9, ie_version);
    store_double_le(header + 25, imu_rate);
    store_double_le(header + 33, gyro_scale);
    store_double_le(header + 41, accel_scale);
    header[49] = 0x02;
    header[53] = 0x02;
    memcpy(header + 65, "xxx", 3);
    memcpy(header + 101, "xxx", 3);
}

/* 公历日期转换为相对 1970-01-01 的天数，避免 mktime 的时区依赖。 */
static int64_t days_from_civil(int64_t year, unsigned int month,
                               unsigned int day)
{
    int64_t era;
    int64_t year_of_era;
    int64_t day_of_year;
    int64_t day_of_era;
    unsigned int shifted_month;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = year - era * 400;
    shifted_month = (unsigned int)((int)month + (month > 2 ? -3 : 9));
    day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100
                 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static double frame_gps_week_seconds(const uint8_t frame[IMU_FRAME_SIZE])
{
    const int64_t gps_epoch = days_from_civil(1980, 1, 6);
    const int64_t week_microseconds = 604800LL * 1000000LL;
    int64_t date = days_from_civil(2000 + frame[4], frame[5], frame[6]);
    int64_t weekday = (date - gps_epoch) % 7;
    int64_t seconds_in_week;
    int64_t microseconds_in_week;
    int64_t whole_seconds;
    int64_t remaining_microseconds;
    uint32_t microseconds = (uint32_t)frame[10]
                            | ((uint32_t)frame[11] << 8)
                            | ((uint32_t)frame[12] << 16);

    if (weekday < 0)
        weekday += 7;

    seconds_in_week = weekday * 86400 + frame[7] * 3600
                      + frame[8] * 60 + frame[9] + 18;
    microseconds_in_week = seconds_in_week * 1000000LL
                           + microseconds - 8000;
    microseconds_in_week %= week_microseconds;
    if (microseconds_in_week < 0)
        microseconds_in_week += week_microseconds;

    whole_seconds = microseconds_in_week / 1000000LL;
    remaining_microseconds = microseconds_in_week % 1000000LL;
    return (double)whole_seconds
           + (double)remaining_microseconds * 0.000001;
}

static void create_output_record(const uint8_t frame[IMU_FRAME_SIZE],
                                 uint8_t output[IMU_OUTPUT_RECORD_SIZE])
{
    unsigned int field;
    unsigned int byte;
    double timestamp = frame_gps_week_seconds(frame);

    store_double_le(output, timestamp);
    for (field = 0; field < 6; ++field) {
        for (byte = 0; byte < 4; ++byte) {
            output[8 + field * 4 + byte] =
                frame[20 + field * 4 + (3 - byte)];
        }
    }
}

static int is_frame_marker(const uint8_t *data)
{
    return memcmp(data, imu_frame_marker, sizeof(imu_frame_marker)) == 0;
}

/* 文档规定累加至第 46 字节；样本帧尾为 EE，故不强制文档示例中的 40。 */
static int has_valid_checksum(const uint8_t frame[IMU_FRAME_SIZE])
{
    uint32_t sum = 0;
    uint16_t stored_checksum;
    unsigned int i;

    for (i = 0; i < 47; ++i)
        sum += frame[i];
    stored_checksum = (uint16_t)frame[47] | ((uint16_t)frame[48] << 8);
    return (sum & 0xFFFFu) == stored_checksum;
}

int imu_convert_file(const char *input_path_utf8,
                     const char *output_path_utf8,
                     ImuProgressCallback progress,
                     void *user_data,
                     ImuConversionResult *result)
{
    FILE *input = NULL;
    FILE *output = NULL;
    ImuConversionResult local_result = {0};
    uint8_t input_buffer[IMU_INPUT_CAPACITY + IMU_FRAME_SIZE - 1];
    uint8_t output_record[IMU_OUTPUT_RECORD_SIZE];
    uint8_t header[IMU_HEADER_SIZE];
    size_t pending_size = 0;
    uint64_t total_bytes;
    int status = IMU_SUCCESS;

    if (result != NULL)
        memset(result, 0, sizeof(*result));

    if (input_path_utf8 == NULL || output_path_utf8 == NULL
        || input_path_utf8[0] == '\0' || output_path_utf8[0] == '\0'
        || strcmp(input_path_utf8, output_path_utf8) == 0)
        return IMU_ERROR_INVALID_ARGUMENT;

    input = open_file_utf8(input_path_utf8, 0);
    if (input == NULL)
        return IMU_ERROR_OPEN_INPUT;

    output = open_file_utf8(output_path_utf8, 1);
    if (output == NULL) {
        fclose(input);
        return IMU_ERROR_OPEN_OUTPUT;
    }

    total_bytes = get_file_size(input);
    create_imr_header(header);
    if (fwrite(header, 1, sizeof(header), output) != sizeof(header))
        status = IMU_ERROR_WRITE;

    if (progress != NULL)
        progress(0, total_bytes, user_data);

    while (status == IMU_SUCCESS) {
        size_t bytes_read = fread(input_buffer + pending_size, 1,
                                  IMU_INPUT_CAPACITY, input);
        size_t available = pending_size + bytes_read;
        size_t position = 0;

        local_result.bytes_read += bytes_read;

        while (position < available) {
            while (position + sizeof(imu_frame_marker) <= available
                   && !is_frame_marker(input_buffer + position))
                ++position;

            if (position + sizeof(imu_frame_marker) > available
                || available - position < IMU_FRAME_SIZE)
                break;

            if (!has_valid_checksum(input_buffer + position)) {
                ++local_result.checksum_errors;
                ++position;
                continue;
            }

            create_output_record(input_buffer + position, output_record);
            if (fwrite(output_record, 1, sizeof(output_record), output)
                != sizeof(output_record)) {
                status = IMU_ERROR_WRITE;
                break;
            }

            ++local_result.records_written;
            position += IMU_FRAME_SIZE;
        }

        pending_size = available - position;
        if (pending_size > 0)
            memmove(input_buffer, input_buffer + position, pending_size);

        if (progress != NULL)
            progress(local_result.bytes_read, total_bytes, user_data);

        if (bytes_read < IMU_INPUT_CAPACITY) {
            if (ferror(input))
                status = IMU_ERROR_READ;
            break;
        }
    }

    if (pending_size >= sizeof(imu_frame_marker)
        && pending_size < IMU_FRAME_SIZE
        && is_frame_marker(input_buffer))
        local_result.truncated_frames = 1;

    fclose(input);
    if (fclose(output) != 0 && status == IMU_SUCCESS)
        status = IMU_ERROR_WRITE;

    if (result != NULL)
        *result = local_result;
    return status;
}

static int seek_file(FILE *file, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static uint64_t read_u64_le(const uint8_t *data)
{
    uint64_t value = 0;
    unsigned int i;
    for (i = 0; i < 8; ++i)
        value |= (uint64_t)data[i] << (i * 8);
    return value;
}

static double read_double_le(const uint8_t *data)
{
    uint64_t bits = read_u64_le(data);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int32_t read_i32_le(const uint8_t *data)
{
    uint32_t value = (uint32_t)data[0]
                     | ((uint32_t)data[1] << 8)
                     | ((uint32_t)data[2] << 16)
                     | ((uint32_t)data[3] << 24);
    return (int32_t)value;
}

static int parse_imr_header(const uint8_t *raw, ImuHeaderInfo *info)
{
    if (memcmp(raw, IMU_IMR_MAGIC, IMU_IMR_MAGIC_SIZE) != 0)
        return 0;

    memset(info, 0, sizeof(*info));
    info->ie_version = read_double_le(raw + 9);
    info->imu_rate = read_double_le(raw + 25);
    info->gyro_scale = read_double_le(raw + 33);
    info->accel_scale = read_double_le(raw + 41);
    memcpy(info->imu_name, raw + 65, 32);
    memcpy(info->author_name, raw + 101, 32);
    info->imu_name[32] = '\0';
    info->author_name[32] = '\0';
    return 1;
}

int imu_read_header(const char *imr_path_utf8, ImuHeaderInfo *header)
{
    FILE *input;
    uint8_t raw[IMU_HEADER_SIZE];

    if (imr_path_utf8 == NULL || header == NULL || imr_path_utf8[0] == '\0')
        return IMU_ERROR_INVALID_ARGUMENT;

    input = open_file_utf8(imr_path_utf8, 0);
    if (input == NULL)
        return IMU_ERROR_OPEN_INPUT;

    if (fread(raw, 1, sizeof(raw), input) != sizeof(raw)) {
        fclose(input);
        return IMU_ERROR_INVALID_FORMAT;
    }
    fclose(input);

    if (!parse_imr_header(raw, header))
        return IMU_ERROR_INVALID_FORMAT;
    return IMU_SUCCESS;
}

static void write_table_header(FILE *output, const ImuHeaderInfo *info,
                               const ImuExportResult *exported)
{
    fprintf(output, "HEADER INFORMATION\n");
    fprintf(output, "------------------\n");
    fprintf(output, "IMU Name         : %s\n", info->imu_name);
    fprintf(output, "Author           : %s\n", info->author_name);
    fprintf(output, "IE Version       : %.6f\n", info->ie_version);
    fprintf(output, "Sample Rate (Hz) : %.6f\n", info->imu_rate);
    fprintf(output, "Gyro Scale       : %.9e\n", info->gyro_scale);
    fprintf(output, "Accel Scale      : %.9e\n", info->accel_scale);
    fprintf(output, "Start Time (s)   : %.6f\n", exported->start_time);
    fprintf(output, "End Time (s)     : %.6f\n", exported->end_time);
    fprintf(output, "Record Count     : %llu\n",
            (unsigned long long)exported->records_written);
    fprintf(output, "\n");
    fprintf(output, "%14s %15s %15s %15s %15s %15s %15s\n",
            "Time(s)", "Gx(rad/s)", "Gy(rad/s)", "Gz(rad/s)",
            "Ax(m/s^2)", "Ay(m/s^2)", "Az(m/s^2)");
}

int imu_export_text(const char *imr_path_utf8,
                    const char *output_path_utf8,
                    int format,
                    ImuProgressCallback progress,
                    void *user_data,
                    ImuExportResult *result)
{
    FILE *input = NULL;
    FILE *output = NULL;
    uint8_t raw_header[IMU_HEADER_SIZE];
    uint8_t record[IMU_OUTPUT_RECORD_SIZE];
    ImuHeaderInfo info;
    ImuExportResult local_result = {0, 0, 0.0, 0.0};
    uint64_t total_bytes;
    uint64_t record_count;
    uint64_t index;
    int status = IMU_SUCCESS;

    if (result != NULL)
        memset(result, 0, sizeof(*result));

    if (imr_path_utf8 == NULL || output_path_utf8 == NULL
        || imr_path_utf8[0] == '\0' || output_path_utf8[0] == '\0'
        || strcmp(imr_path_utf8, output_path_utf8) == 0
        || (format != IMU_TEXT_FORMAT_TABLE && format != IMU_TEXT_FORMAT_CSV))
        return IMU_ERROR_INVALID_ARGUMENT;

    input = open_file_utf8(imr_path_utf8, 0);
    if (input == NULL)
        return IMU_ERROR_OPEN_INPUT;

    total_bytes = get_file_size(input);
    if (fread(raw_header, 1, sizeof(raw_header), input) != sizeof(raw_header)
        || !parse_imr_header(raw_header, &info)) {
        fclose(input);
        return IMU_ERROR_INVALID_FORMAT;
    }

    output = open_file_utf8(output_path_utf8, 1);
    if (output == NULL) {
        fclose(input);
        return IMU_ERROR_OPEN_OUTPUT;
    }

    record_count = total_bytes > IMU_HEADER_SIZE
                   ? (total_bytes - IMU_HEADER_SIZE) / IMU_OUTPUT_RECORD_SIZE
                   : 0;

    if (record_count > 0) {
        if (fread(record, 1, sizeof(record), input) != sizeof(record))
            status = IMU_ERROR_READ;
        else
            local_result.start_time = read_double_le(record);
    }

    if (status == IMU_SUCCESS && record_count > 0
        && seek_file(input, IMU_HEADER_SIZE
                            + (record_count - 1) * IMU_OUTPUT_RECORD_SIZE)) {
        if (fread(record, 1, sizeof(record), input) != sizeof(record))
            status = IMU_ERROR_READ;
        else
            local_result.end_time = read_double_le(record);
    }

    if (status == IMU_SUCCESS && !seek_file(input, IMU_HEADER_SIZE))
        status = IMU_ERROR_READ;

    if (status == IMU_SUCCESS) {
        local_result.records_written = record_count;
        if (format == IMU_TEXT_FORMAT_TABLE)
            write_table_header(output, &info, &local_result);
        else
            fprintf(output,
                    "time_s,gx_rad_s,gy_rad_s,gz_rad_s,"
                    "ax_m_s2,ay_m_s2,az_m_s2\n");
        local_result.records_written = 0;
        if (ferror(output))
            status = IMU_ERROR_WRITE;
    }

    if (progress != NULL)
        progress(IMU_HEADER_SIZE, total_bytes, user_data);

    for (index = 0; status == IMU_SUCCESS && index < record_count; ++index) {
        double time_s;
        double gyro[3];
        double accel[3];
        int field;

        if (fread(record, 1, sizeof(record), input) != sizeof(record)) {
            status = IMU_ERROR_READ;
            break;
        }

        time_s = read_double_le(record);
        for (field = 0; field < 3; ++field) {
            gyro[field] = (double)read_i32_le(record + 8 + field * 4)
                          * info.gyro_scale;
            accel[field] = (double)read_i32_le(record + 20 + field * 4)
                           * info.accel_scale;
        }

        if (format == IMU_TEXT_FORMAT_TABLE) {
            fprintf(output,
                    "%14.6f %15.8f %15.8f %15.8f %15.8f %15.8f %15.8f\n",
                    time_s, gyro[0], gyro[1], gyro[2],
                    accel[0], accel[1], accel[2]);
        } else {
            fprintf(output, "%.6f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n",
                    time_s, gyro[0], gyro[1], gyro[2],
                    accel[0], accel[1], accel[2]);
        }

        if (ferror(output)) {
            status = IMU_ERROR_WRITE;
            break;
        }

        ++local_result.records_written;
        local_result.bytes_read = IMU_HEADER_SIZE
                                  + (index + 1) * IMU_OUTPUT_RECORD_SIZE;

        if (progress != NULL && (index & 0x3FFu) == 0)
            progress(local_result.bytes_read, total_bytes, user_data);
    }

    if (status == IMU_SUCCESS && progress != NULL)
        progress(total_bytes, total_bytes, user_data);

    fclose(input);
    if (fclose(output) != 0 && status == IMU_SUCCESS)
        status = IMU_ERROR_WRITE;

    if (result != NULL)
        *result = local_result;
    return status;
}

const char *imu_status_string(int status)
{
    switch (status) {
    case IMU_SUCCESS:
        return "Success";
    case IMU_ERROR_INVALID_ARGUMENT:
        return "Invalid argument or identical input/output path";
    case IMU_ERROR_OPEN_INPUT:
        return "Cannot open input file";
    case IMU_ERROR_OPEN_OUTPUT:
        return "Cannot open output file";
    case IMU_ERROR_READ:
        return "Failed while reading input file";
    case IMU_ERROR_WRITE:
        return "Failed while writing output file";
    case IMU_ERROR_INVALID_FORMAT:
        return "File is not a valid IMR file";
    default:
        return "Unknown error";
    }
}
