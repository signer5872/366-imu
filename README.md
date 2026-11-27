**图形操作界面**
**第一步 进入解压后的文件夹**
cd IMU
**第二步 **
sudo apt install python3 python3-tk
//更新python库，没有就等待下载完成，更新完成后，后续使用过程中就无需再次发送此步骤
**第三步**
python3 imu_gui.py
//打开操作页面后第一个框可以解析原始数据和imr数据，将原始数据放入后可以解析出imr文件，再将imr文件放入其中就可以解析出字符串了



**命令行操作**
cd IMU
chmod +x dist/linux/x86_64/imu_convert
./dist/linux/x86_64/imu_convert *.txt
//将*.txt换成需要转换的原始文件，如366.dat/txt等，可以将其转换为imr文件
./dist/linux/x86_64/imu_convert *.imr
//将*.imr换成需要转换的imr文件，就可以将其转为.csv结尾的字符串数据文件了
./dist/linux/x86_64/imu_convert *.imr out.txt
//在*.imr后面在加上需要的命名即可自定义名称和后缀
