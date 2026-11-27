#ifndef IMU_CONVERTER_H
#define IMU_CONVERTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(IMU_BUILD)
#    define IMU_API __declspec(dllexport)
#  else
#    define IMU_API __declspec(dllimport)
#  endif
#else
#  define IMU_API __attribute__((visibility("default")))
#endif

typedef enum ImuStatus {
    IMU_SUCCESS = 0,
    IMU_ERROR_INVALID_ARGUMENT = 1,
    IMU_ERROR_OPEN_INPUT = 2,
    IMU_ERROR_OPEN_OUTPUT = 3,
    IMU_ERROR_READ = 4,
    IMU_ERROR_WRITE = 5,
    IMU_ERROR_INVALID_FORMAT = 6
} ImuStatus;

typedef struct ImuConversionResult {
    uint64_t records_written;
    uint64_t bytes_read;
    uint64_t truncated_frames;
    uint64_t checksum_errors;
} ImuConversionResult;

typedef struct ImuHeaderInfo {
    double ie_version;
    double imu_rate;
    double gyro_scale;
    double accel_scale;
    char imu_name[33];
    char author_name[33];
} ImuHeaderInfo;

typedef enum ImuTextFormat {
    IMU_TEXT_FORMAT_TABLE = 0,
    IMU_TEXT_FORMAT_CSV = 1
} ImuTextFormat;

typedef struct ImuExportResult {
    uint64_t records_written;
    uint64_t bytes_read;
    double start_time;
    double end_time;
} ImuExportResult;

typedef void (*ImuProgressCallback)(uint64_t processed_bytes,
                                    uint64_t total_bytes,
                                    void *user_data);

/*
 * 将 AA 77 02 31 格式的 IMU 原始数据转换为 IMR 文件。
 * 仅输出完整的 50 字节且累加和校验通过的帧。
 * 路径使用 UTF-8 编码；progress 和 result 可以为 NULL。
 * 函数不修改全局状态，可在不同线程中并发转换不同文件。
 */
IMU_API int imu_convert_file(const char *input_path_utf8,
                             const char *output_path_utf8,
                             ImuProgressCallback progress,
                             void *user_data,
                             ImuConversionResult *result);

/* 读取 IMR 文件头；路径使用 UTF-8 编码。 */
IMU_API int imu_read_header(const char *imr_path_utf8,
                            ImuHeaderInfo *header);

/*
 * 将 IMR 文件导出为文本。
 * format 为 IMU_TEXT_FORMAT_TABLE 时输出带表头的对齐报告，
 * 为 IMU_TEXT_FORMAT_CSV 时输出逗号分隔数据。
 * 陀螺仪单位为 rad/s，加速度单位为 m/s^2。
 */
IMU_API int imu_export_text(const char *imr_path_utf8,
                            const char *output_path_utf8,
                            int format,
                            ImuProgressCallback progress,
                            void *user_data,
                            ImuExportResult *result);

/* 返回静态错误文本，调用方无需释放。 */
IMU_API const char *imu_status_string(int status);

#ifdef __cplusplus
}
#endif

#endif
