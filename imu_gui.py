#!/usr/bin/env python3
import ctypes
import os
import platform
import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

FORMAT_IMR = "IMR 二进制"
FORMAT_TEXT = "文本报告"
FORMAT_CSV = "CSV 表格"

FORMAT_LABELS = {
    "imr": FORMAT_IMR,
    "text": FORMAT_TEXT,
    "csv": FORMAT_CSV,
}
FORMAT_KEYS = {label: key for key, label in FORMAT_LABELS.items()}
FORMAT_SUFFIX = {"imr": ".imr", "text": ".txt", "csv": ".csv"}


class ConversionResult(ctypes.Structure):
    _fields_ = [
        ("records_written", ctypes.c_uint64),
        ("bytes_read", ctypes.c_uint64),
        ("truncated_frames", ctypes.c_uint64),
        ("checksum_errors", ctypes.c_uint64),
    ]


class ExportResult(ctypes.Structure):
    _fields_ = [
        ("records_written", ctypes.c_uint64),
        ("bytes_read", ctypes.c_uint64),
        ("start_time", ctypes.c_double),
        ("end_time", ctypes.c_double),
    ]


class HeaderInfo(ctypes.Structure):
    _fields_ = [
        ("ie_version", ctypes.c_double),
        ("imu_rate", ctypes.c_double),
        ("gyro_scale", ctypes.c_double),
        ("accel_scale", ctypes.c_double),
        ("imu_name", ctypes.c_char * 33),
        ("author_name", ctypes.c_char * 33),
    ]


class NativeConverter:
    def __init__(self):
        self._library = ctypes.CDLL(str(self._find_library()))
        self._callback_type = ctypes.CFUNCTYPE(
            None, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p
        )
        self._library.imu_convert_file.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            self._callback_type,
            ctypes.c_void_p,
            ctypes.POINTER(ConversionResult),
        ]
        self._library.imu_convert_file.restype = ctypes.c_int
        self._library.imu_export_text.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_int,
            self._callback_type,
            ctypes.c_void_p,
            ctypes.POINTER(ExportResult),
        ]
        self._library.imu_export_text.restype = ctypes.c_int
        self._library.imu_read_header.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(HeaderInfo),
        ]
        self._library.imu_read_header.restype = ctypes.c_int
        self._library.imu_status_string.argtypes = [ctypes.c_int]
        self._library.imu_status_string.restype = ctypes.c_char_p

    @staticmethod
    def _find_library():
        root = Path(__file__).resolve().parent
        if os.name == "nt":
            relative_paths = [
                "dist/windows/x86_64/imu.dll",
                "build/imu.dll",
                "build/Debug/imu.dll",
                "imu.dll",
            ]
        else:
            machine = platform.machine().lower()
            architecture = {
                "x86_64": "x86_64",
                "amd64": "x86_64",
                "aarch64": "arm64",
                "arm64": "arm64",
            }.get(machine, machine)
            relative_paths = [
                "dist/linux/{}/libimu.so".format(architecture),
                "build/libimu.so",
                "lib/libimu.so",
                "libimu.so",
            ]

        for relative_path in relative_paths:
            candidate = root / relative_path
            if candidate.is_file():
                return candidate
        raise OSError("未找到本地转换库，请确认 dist 目录与程序放在一起。")

    def convert(self, input_path, output_path, progress_callback):
        result = ConversionResult()
        callback = self._callback_type(progress_callback)
        status = self._library.imu_convert_file(
            os.fspath(input_path).encode("utf-8"),
            os.fspath(output_path).encode("utf-8"),
            callback,
            None,
            ctypes.byref(result),
        )
        return status, result

    def export_text(self, input_path, output_path, csv_mode, progress_callback):
        result = ExportResult()
        callback = self._callback_type(progress_callback)
        status = self._library.imu_export_text(
            os.fspath(input_path).encode("utf-8"),
            os.fspath(output_path).encode("utf-8"),
            1 if csv_mode else 0,
            callback,
            None,
            ctypes.byref(result),
        )
        return status, result

    def read_header(self, input_path):
        header = HeaderInfo()
        status = self._library.imu_read_header(
            os.fspath(input_path).encode("utf-8"), ctypes.byref(header)
        )
        return status, header

    def status_string(self, status):
        text = self._library.imu_status_string(status)
        return text.decode("utf-8", errors="replace")


class ImuConverterApp:
    def __init__(self, root, converter):
        self.root = root
        self.converter = converter
        self.events = queue.Queue()
        self.worker = None
        self.input_path = tk.StringVar()
        self.output_path = tk.StringVar()
        self.format_label = tk.StringVar(value=FORMAT_LABELS["imr"])
        self.status_text = tk.StringVar(value="请选择 IMU 原始数据或 IMR 文件。")
        self.result_text = tk.StringVar(value="尚无转换记录")
        self.info_text = tk.StringVar(value="")

        root.title("IMU 数据转换与导出")
        root.geometry("720x390")
        root.minsize(640, 370)
        root.protocol("WM_DELETE_WINDOW", self._close)

        container = ttk.Frame(root, padding=20)
        container.pack(fill=tk.BOTH, expand=True)
        container.columnconfigure(1, weight=1)

        ttk.Label(
            container,
            text="IMU → IMR → 文本",
            font=("Microsoft YaHei UI", 16, "bold"),
        ).grid(row=0, column=0, columnspan=3, sticky=tk.W, pady=(0, 16))

        ttk.Label(container, text="输入文件：").grid(row=1, column=0, sticky=tk.W, pady=5)
        self.input_entry = ttk.Entry(container, textvariable=self.input_path)
        self.input_entry.grid(row=1, column=1, sticky=tk.EW, padx=8, pady=5)
        self.input_button = ttk.Button(container, text="浏览…", command=self._select_input)
        self.input_button.grid(row=1, column=2, pady=5)

        ttk.Label(container, text="输出文件：").grid(row=2, column=0, sticky=tk.W, pady=5)
        self.output_entry = ttk.Entry(container, textvariable=self.output_path)
        self.output_entry.grid(row=2, column=1, sticky=tk.EW, padx=8, pady=5)
        self.output_button = ttk.Button(container, text="浏览…", command=self._select_output)
        self.output_button.grid(row=2, column=2, pady=5)

        ttk.Label(container, text="输出格式：").grid(row=3, column=0, sticky=tk.W, pady=5)
        self.format_box = ttk.Combobox(
            container,
            textvariable=self.format_label,
            values=[FORMAT_IMR, FORMAT_TEXT, FORMAT_CSV],
            state="readonly",
        )
        self.format_box.grid(row=3, column=1, sticky=tk.W, padx=8, pady=5)
        self.format_box.bind("<<ComboboxSelected>>", self._on_format_change)

        ttk.Label(container, textvariable=self.info_text, foreground="#555555").grid(
            row=4, column=0, columnspan=3, sticky=tk.W, pady=(6, 0)
        )

        self.progress = ttk.Progressbar(container, maximum=100, mode="determinate")
        self.progress.grid(row=5, column=0, columnspan=3, sticky=tk.EW, pady=(12, 8))
        ttk.Label(container, textvariable=self.status_text).grid(
            row=6, column=0, columnspan=3, sticky=tk.W
        )
        ttk.Label(container, textvariable=self.result_text).grid(
            row=7, column=0, columnspan=3, sticky=tk.W, pady=(6, 14)
        )

        self.convert_button = ttk.Button(container, text="开始处理", command=self._start)
        self.convert_button.grid(row=8, column=0, columnspan=3)
        root.after(100, self._poll_events)

    def _current_mode(self):
        return FORMAT_KEYS.get(self.format_label.get(), "imr")

    def _set_mode(self, mode):
        self.format_label.set(FORMAT_LABELS[mode])

    def _replace_suffix(self, path, suffix):
        if not path:
            return path
        return str(Path(path).with_suffix(suffix))

    def _select_input(self):
        selected = filedialog.askopenfilename(
            title="选择输入文件",
            filetypes=[
                ("IMU 数据", "*.imu *.dat *.imr"),
                ("IMR 文件", "*.imr"),
                ("所有文件", "*.*"),
            ],
        )
        if not selected:
            return

        self.input_path.set(selected)
        status, header = self.converter.read_header(selected)
        if status == 0:
            name = header.imu_name.split(b"\x00")[0].decode("utf-8", errors="replace")
            self.info_text.set(
                "IMR 文件：{}  采样率 {:.3f} Hz".format(name or "未命名", header.imu_rate)
            )
            mode = "csv"
        else:
            self.info_text.set("原始 IMU 数据，将转换为 IMR。")
            mode = "imr"

        self._set_mode(mode)
        self.output_path.set(self._replace_suffix(selected, FORMAT_SUFFIX[mode]))
        self.status_text.set("已选择输入文件。")

    def _select_output(self):
        mode = self._current_mode()
        selected = filedialog.asksaveasfilename(
            title="选择输出位置",
            defaultextension=FORMAT_SUFFIX[mode],
            filetypes=[
                ("IMR 数据", "*.imr"),
                ("文本报告", "*.txt"),
                ("CSV 表格", "*.csv"),
                ("所有文件", "*.*"),
            ],
        )
        if selected:
            self.output_path.set(selected)

    def _on_format_change(self, _event=None):
        mode = self._current_mode()
        self.output_path.set(
            self._replace_suffix(self.output_path.get(), FORMAT_SUFFIX[mode])
        )

    def _start(self):
        source = self.input_path.get().strip()
        destination = self.output_path.get().strip()
        mode = self._current_mode()

        if not source or not destination:
            messagebox.showwarning("路径不完整", "请选择输入文件和输出文件。")
            return
        if not Path(source).is_file():
            messagebox.showerror("输入无效", "输入文件不存在或不可读取。")
            return
        if os.path.abspath(source) == os.path.abspath(destination):
            messagebox.showerror("路径冲突", "输出文件不能覆盖输入文件。")
            return
        if Path(destination).exists() and not messagebox.askyesno(
            "确认覆盖", "输出文件已存在，是否覆盖？"
        ):
            return

        self._set_running(True)
        self.progress["value"] = 0
        self.status_text.set("正在准备处理…")
        self.result_text.set("处理过程中可以最小化窗口，请等待完成。")
        self.worker = threading.Thread(
            target=self._worker, args=(source, destination, mode)
        )
        self.worker.start()

    def _worker(self, source, destination, mode):
        last_percent = [-1]

        def report(processed, total, _user_data):
            if total == 0:
                percent = 100 if processed == 0 else 0
            else:
                percent = int(processed * 100 / total)
            if percent != last_percent[0]:
                last_percent[0] = percent
                self.events.put(("progress", percent))

        try:
            if mode == "imr":
                status, result = self.converter.convert(source, destination, report)
                self.events.put(("done", "imr", status, result, destination, None))
            else:
                status, result = self.converter.export_text(
                    source, destination, mode == "csv", report
                )
                self.events.put(("done", mode, status, result, destination, None))
        except Exception as error:
            self.events.put(("done", mode, -1, None, destination, str(error)))

    def _poll_events(self):
        try:
            while True:
                event = self.events.get_nowait()
                if event[0] == "progress":
                    percent = event[1]
                    self.progress["value"] = percent
                    self.status_text.set("正在处理… {}%".format(percent))
                else:
                    self._finish(event)
        except queue.Empty:
            pass
        self.root.after(100, self._poll_events)

    def _finish(self, event):
        _tag, mode, status, result, destination, exception_text = event
        self._set_running(False)
        self.worker = None

        if status != 0:
            detail = exception_text or self.converter.status_string(status)
            self.status_text.set("处理失败。")
            self.result_text.set(detail)
            messagebox.showerror("处理失败", detail)
            return

        self.progress["value"] = 100
        self.status_text.set("处理完成：{}".format(destination))

        if mode == "imr":
            summary = (
                "写入 {:,} 条记录；读取 {:,} 字节；不完整帧 {} 个；校验失败 {} 个。".format(
                    result.records_written,
                    result.bytes_read,
                    result.truncated_frames,
                    result.checksum_errors,
                )
            )
            message = "已写入 {:,} 条记录。".format(result.records_written)
        else:
            summary = "导出 {:,} 条记录；时间范围 {:.6f} ~ {:.6f} s。".format(
                result.records_written, result.start_time, result.end_time
            )
            message = "已导出 {:,} 条记录。".format(result.records_written)

        self.result_text.set(summary)
        messagebox.showinfo("处理完成", "{}\n输出文件：{}".format(message, destination))

    def _set_running(self, running):
        state = tk.DISABLED if running else tk.NORMAL
        for widget in (
            self.input_entry,
            self.output_entry,
            self.input_button,
            self.output_button,
            self.format_box,
        ):
            widget.configure(state=state)
        self.convert_button.configure(state=state)

    def _close(self):
        if self.worker is not None:
            messagebox.showwarning("处理进行中", "请等待当前任务完成后再退出。")
            return
        self.root.destroy()


def main():
    root = tk.Tk()
    try:
        converter = NativeConverter()
    except (OSError, AttributeError) as error:
        root.withdraw()
        messagebox.showerror("无法启动", str(error))
        root.destroy()
        return 1
    ImuConverterApp(root, converter)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
