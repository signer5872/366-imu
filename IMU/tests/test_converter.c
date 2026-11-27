#include "imu_converter.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t sample_frame[50] = {
    0xaa, 0x77, 0x02, 0x31, 0x19, 0x0b, 0x11, 0x08, 0x05, 0x11,
    0xd4, 0x05, 0x0d, 0x80, 0xfe, 0xfc, 0xfd, 0x39, 0xa0, 0x00,
    0xff, 0xff, 0x84, 0x41, 0x00, 0x03, 0x21, 0x00, 0xff, 0xfe,
    0x5d, 0x77, 0xff, 0xf7, 0x82, 0x7e, 0xff, 0xfa, 0x6c, 0xea,
    0x0f, 0xa9, 0xe0, 0x80, 0x67, 0x7b, 0x0d, 0xe1, 0x15, 0xee
};

static const uint8_t expected_record[32] = {
    0xa1, 0x10, 0x01, 0x87, 0xfd, 0x34, 0xfc, 0x40,
    0x41, 0x84, 0xff, 0xff, 0x00, 0x21, 0x03, 0x00,
    0x77, 0x5d, 0xfe, 0xff, 0x7e, 0x82, 0xf7, 0xff,
    0xea, 0x6c, 0xfa, 0xff, 0x80, 0xe0, 0xa9, 0x0f
};

static int fail(const char *message)
{
    fprintf(stderr, "测试失败: %s\n", message);
    return 1;
}

int main(void)
{
    const char *input_path = "imu_test_input.dat";
    const char *output_path = "imu_test_output.imr";
    const uint8_t noise[2] = {0x00, 0x12};
    uint8_t output[544];
    ImuConversionResult result;
    FILE *file;
    size_t output_size;
    int status;

    file = fopen(input_path, "wb");
    if (file == NULL)
        return fail("无法创建测试输入");
    if (fwrite(noise, 1, sizeof(noise), file) != sizeof(noise)
        || fwrite(sample_frame, 1, sizeof(sample_frame), file) != sizeof(sample_frame)
        || fwrite(sample_frame, 1, 10, file) != 10
        || fclose(file) != 0)
        return fail("无法写入测试输入");

    status = imu_convert_file(input_path, output_path, NULL, NULL, &result);
    if (status != IMU_SUCCESS)
        return fail(imu_status_string(status));
    if (result.records_written != 1 || result.bytes_read != 62
        || result.truncated_frames != 1 || result.checksum_errors != 0)
        return fail("转换统计不正确");

    file = fopen(output_path, "rb");
    if (file == NULL)
        return fail("无法打开测试输出");
    output_size = fread(output, 1, sizeof(output), file);
    fclose(file);
    if (output_size != sizeof(output))
        return fail("输出文件大小不正确");
    if (memcmp(output, "$IMURAW", 7) != 0)
        return fail("IMR 文件头不正确");
    if (output[49] != 2 || output[53] != 2)
        return fail("IMR 类型字段不正确");
    if (memcmp(output + 512, expected_record, sizeof(expected_record)) != 0)
        return fail("首条输出记录与原程序不兼容");

    {
        uint8_t boundary_frame[50];
        uint32_t checksum = 0;
        unsigned int i;
        double timestamp;
        double expected_timestamp = 17.0 + 991999.0 * 0.000001;

        memcpy(boundary_frame, sample_frame, sizeof(boundary_frame));
        boundary_frame[6] = 0x16;
        boundary_frame[7] = 0x17;
        boundary_frame[8] = 0x3b;
        boundary_frame[9] = 0x3b;
        boundary_frame[10] = 0x3f;
        boundary_frame[11] = 0x42;
        boundary_frame[12] = 0x0f;
        for (i = 0; i < 47; ++i)
            checksum += boundary_frame[i];
        boundary_frame[47] = (uint8_t)checksum;
        boundary_frame[48] = (uint8_t)(checksum >> 8);

        file = fopen(input_path, "wb");
        if (file == NULL)
            return fail("无法创建跨周测试输入");
        if (fwrite(boundary_frame, 1, sizeof(boundary_frame), file)
                != sizeof(boundary_frame)
            || fclose(file) != 0)
            return fail("无法写入跨周测试输入");
        if (imu_convert_file(input_path, output_path, NULL, NULL, &result)
            != IMU_SUCCESS)
            return fail("跨周时间转换失败");
        file = fopen(output_path, "rb");
        if (file == NULL)
            return fail("无法打开跨周测试输出");
        if (fread(output, 1, sizeof(output), file) != sizeof(output)) {
            fclose(file);
            return fail("无法读取跨周测试输出");
        }
        fclose(file);
        memcpy(&timestamp, output + 512, sizeof(timestamp));
        if (timestamp != expected_timestamp)
            return fail("GPS 跨周时间换算不正确");
    }

    {
        uint8_t invalid_frame[50];
        memcpy(invalid_frame, sample_frame, sizeof(invalid_frame));
        invalid_frame[20] ^= 0x01;
        file = fopen(input_path, "wb");
        if (file == NULL)
            return fail("无法创建坏校验帧输入");
        if (fwrite(invalid_frame, 1, sizeof(invalid_frame), file)
                != sizeof(invalid_frame)
            || fclose(file) != 0)
            return fail("无法写入坏校验帧输入");
        if (imu_convert_file(input_path, output_path, NULL, NULL, &result)
            != IMU_SUCCESS)
            return fail("坏校验帧转换调用失败");
        if (result.records_written != 0 || result.checksum_errors != 1)
            return fail("坏校验帧未被正确排除");
        file = fopen(output_path, "rb");
        if (file == NULL)
            return fail("无法打开坏校验帧输出");
        output_size = fread(output, 1, sizeof(output), file);
        fclose(file);
        if (output_size != 512)
            return fail("坏校验帧不应生成数据记录");
    }

    {
        const char *csv_path = "imu_test_output.csv";
        ImuExportResult exported;
        ImuHeaderInfo header;
        char line[256];
        int lines = 0;

        file = fopen(input_path, "wb");
        if (file == NULL)
            return fail("无法创建导出测试输入");
        if (fwrite(sample_frame, 1, sizeof(sample_frame), file) != sizeof(sample_frame)
            || fwrite(sample_frame, 1, sizeof(sample_frame), file) != sizeof(sample_frame)
            || fclose(file) != 0)
            return fail("无法写入导出测试输入");

        if (imu_convert_file(input_path, output_path, NULL, NULL, &result)
            != IMU_SUCCESS)
            return fail("导出测试的 IMR 生成失败");

        if (imu_read_header(output_path, &header) != IMU_SUCCESS)
            return fail("无法读取 IMR 文件头");
        if (strcmp(header.imu_name, "xxx") != 0)
            return fail("IMR 文件头 IMU 名称不正确");
        if (header.imu_rate != 200.0)
            return fail("IMR 文件头采样率不正确");

        if (imu_export_text(output_path, csv_path, IMU_TEXT_FORMAT_CSV,
                            NULL, NULL, &exported) != IMU_SUCCESS)
            return fail("CSV 导出失败");
        if (exported.records_written != 2)
            return fail("CSV 导出记录数不正确");
        if (exported.start_time <= 0.0 || exported.end_time < exported.start_time)
            return fail("CSV 导出时间范围不正确");

        file = fopen(csv_path, "rb");
        if (file == NULL)
            return fail("无法打开 CSV 输出");
        while (fgets(line, sizeof(line), file) != NULL)
            ++lines;
        fclose(file);
        if (lines != 3)
            return fail("CSV 行数应为表头加两条记录");

        if (imu_export_text(output_path, "imu_test_output.txt",
                            IMU_TEXT_FORMAT_TABLE, NULL, NULL, &exported)
            != IMU_SUCCESS)
            return fail("文本报告导出失败");
        file = fopen("imu_test_output.txt", "rb");
        if (file == NULL)
            return fail("无法打开文本报告");
        if (fgets(line, sizeof(line), file) == NULL
            || strncmp(line, "HEADER INFORMATION", 18) != 0) {
            fclose(file);
            return fail("文本报告缺少头信息");
        }
        fclose(file);

        remove(csv_path);
        remove("imu_test_output.txt");
    }

    if (imu_convert_file("does-not-exist.imu", output_path,
                         NULL, NULL, NULL) != IMU_ERROR_OPEN_INPUT)
        return fail("缺失输入文件错误码不正确");
    if (imu_convert_file(input_path, input_path,
                         NULL, NULL, NULL) != IMU_ERROR_INVALID_ARGUMENT)
        return fail("输入输出路径冲突检查不正确");

    remove(input_path);
    remove(output_path);
    puts("全部测试通过。");
    return 0;
}
